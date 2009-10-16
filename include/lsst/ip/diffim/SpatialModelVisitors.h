// -*- lsst-c++ -*-
/**
 * @file SpatialModelVisitors.h
 *
 * @brief Declaration and implementation of CandidateVisitors for SpatialModelKernel
 *
 * @author Andrew Becker, University of Washington
 *
 * @note These might more naturally be declared in an anonymous
 * namespace but then they could not be unit tested.  These are
 * implementation and not API, and will not be swigged.
 *
 * @ingroup ip::diffim
 */
#include <boost/timer.hpp> 

#include <lsst/afw/image/Image.h>
#include <lsst/afw/image/ImagePca.h>
#include <lsst/afw/math/SpatialCell.h>
#include <lsst/afw/math/Kernel.h>
#include <lsst/afw/math/FunctionLibrary.h>

#include <lsst/pex/exceptions/Runtime.h>
#include <lsst/pex/policy/Policy.h>
#include <lsst/pex/logging/Trace.h>

#include <lsst/ip/diffim/SpatialModelKernel.h>

#include <Eigen/Core>
#include <Eigen/Cholesky>
#include <Eigen/LU>
#include <Eigen/QR>

#define DEBUG_MATRIX 0

namespace afwMath        = lsst::afw::math;
namespace afwImage       = lsst::afw::image;
namespace pexLogging     = lsst::pex::logging; 
namespace pexExcept      = lsst::pex::exceptions; 
namespace pexPolicy      = lsst::pex::policy; 

namespace lsst {
namespace ip {
namespace diffim {
namespace detail {

/**
 * @class KernelSumVisitor
 * @ingroup ip_diffim
 *
 * @brief A class to accumulate kernel sums across SpatialCells 
 *
 * @code
    Policy::Ptr policy(new Policy);
    policy->set("kernelSumClipping", false);
    policy->set("maxKsumSigma", 3.0);

    detail::KernelSumVisitor<PixelT> kernelSumVisitor(*policy);
    kernelSumVisitor.reset();
    kernelSumVisitor.setMode(detail::KernelSumVisitor<PixelT>::AGGREGATE);
    kernelCells.visitCandidates(&kernelSumVisitor, nStarPerCell);
    kernelSumVisitor.processKsumDistribution();
    kernelSumVisitor.setMode(detail::KernelSumVisitor<PixelT>::REJECT);
    kernelCells.visitCandidates(&kernelSumVisitor, nStarPerCell);
    int nRejected = kernelSumVisitor.getNRejected();
 * @endcode
 *

 * @note The class has 2 processing modes; the first AGGREGATES kernel sums
 * across all candidates.  You must the process the distribution to set member
 * variables representing the mean and standard deviation of the kernel sums.
 * The second mode then REJECTs candidates with kernel sums outside the
 * acceptable range (set by the policy).  It does this by setting candidate
 * status to afwMath::SpatialCellCandidate::BAD.  In this mode it also
 * accumulates the number of candidates it sets as bad.
 *
 * @note The statistics call calculates sigma-clipped values (afwMath::MEANCLIP,
 * afwMath::STDEVCLIP)
 *
 */
template<typename PixelT>
class KernelSumVisitor : public afwMath::CandidateVisitor {
    typedef afwImage::Image<afwMath::Kernel::Pixel> ImageT;
public:
    enum Mode {AGGREGATE = 0, REJECT = 1};
    
    KernelSumVisitor(
        lsst::pex::policy::Policy const& policy ///< Policy file directing behavior
        ) :
        afwMath::CandidateVisitor(),
        _mode(AGGREGATE),
        _kSums(std::vector<double>()),
        _kSumMean(0.),
        _kSumStd(0.),
        _dkSumMax(0.),
        _kSumNpts(0),
        _nRejected(0),
        _policy(policy) {}
    
    void setMode(Mode mode) {_mode = mode;}

    int    getNRejected() {return _nRejected;}
    double getkSumMean()  {return _kSumMean;}
    double getkSumStd()   {return _kSumStd;}
    double getdkSumMax()  {return _dkSumMax;}
    double getkSumNpts()  {return _kSumNpts;}

    /* NOTE - reset() is an overridden method of the base class that gets called
     * before every visitCandidates().  Since we use this class to visit each
     * candidate twice, and need to retain the information of the first visit
     * for the second, we can't clear the values in reset().  Call our mode
     * resetDerived().
     */
    void resetDerived() {
        _kSums.clear();
        _kSumMean =  0.;
        _kSumStd  =  0.;
        _dkSumMax =  0.;
        _kSumNpts =  0;
        _nRejected = 0;
    }

    void processCandidate(afwMath::SpatialCellCandidate *candidate) {
        KernelCandidate<PixelT> *kCandidate = dynamic_cast<KernelCandidate<PixelT> *>(candidate);
        if (kCandidate == NULL) {
            throw LSST_EXCEPT(lsst::pex::exceptions::LogicErrorException,
                              "Failed to cast SpatialCellCandidate to KernelCandidate");
        }
        pexLogging::TTrace<6>("lsst.ip.diffim.KernelSumVisitor.processCandidate", 
                              "Processing candidate %d, mode %d", kCandidate->getId(), _mode);
        
        /* Grab all kernel sums and look for outliers */
        if (_mode == AGGREGATE) {
            _kSums.push_back(kCandidate->getKsum());
        }
        else if (_mode == REJECT) {
            if (_policy.getBool("kernelSumClipping")) {
                if (fabs(kCandidate->getKsum() - _kSumMean) > (_dkSumMax)) {
                    kCandidate->setStatus(afwMath::SpatialCellCandidate::BAD);
                    pexLogging::TTrace<4>("lsst.ip.diffim.KernelSumVisitor.processCandidate", 
                                          "Rejecting candidate %d due to bad source kernel sum : (%.2f %.2f %.2f)",
                                          kCandidate->getId(),
                                          kCandidate->getKsum(), _kSumMean, _dkSumMax);
                    _nRejected += 1;
                }
            }
            else {
                pexLogging::TTrace<6>("lsst.ip.diffim.KernelSumVisitor.processCandidate", 
                                      "Sigma clipping not enabled");
            }
        }
    }
    
    void processKsumDistribution() {
        try {
            afwMath::Statistics stats = afwMath::makeStatistics(_kSums, afwMath::NPOINT | afwMath::MEANCLIP | afwMath::STDEVCLIP); 
            _kSumMean = stats.getValue(afwMath::MEANCLIP);
            _kSumStd  = stats.getValue(afwMath::STDEVCLIP);
            _kSumNpts = static_cast<int>(stats.getValue(afwMath::NPOINT));
        } catch (lsst::pex::exceptions::Exception &e) {
            LSST_EXCEPT_ADD(e, "Kernel Sum Statistics");
            throw e;
        }
        _dkSumMax = _policy.getDouble("maxKsumSigma") * _kSumStd;
        pexLogging::TTrace<2>("lsst.ip.diffim.KernelSumVisitor.processCandidate", 
                              "Kernel Sum Distribution : %.3f +/- %.3f (%d points)", _kSumMean, _kSumStd, _kSumNpts);
    }

private:
    Mode _mode;
    std::vector<double> _kSums;
    double _kSumMean;
    double _kSumStd;
    double _dkSumMax;
    int    _kSumNpts;
    int    _nRejected;
    lsst::pex::policy::Policy _policy;
};    


/**
 * @class SetPcaImageVisitor
 * @ingroup ip_diffim
 *
 * @brief A class to run a PCA on all candidate kernels (represented as Images)
 *
 * @code
    afwImage::ImagePca<ImageT> imagePca;
    detail::SetPcaImageVisitor<PixelT> importStarVisitor(&imagePca);
    kernelCells.visitCandidates(&importStarVisitor, nStarPerCell);
    importStarVisitor.subtractMean();
    imagePca.analyze();
    std::vector<typename ImageT::Ptr> eigenImages = imagePca.getEigenImages();
    afwMath::KernelList kernelListRaw;
    kernelListRaw.push_back(afwMath::Kernel::Ptr(
                                new afwMath::FixedKernel(
                                    afwImage::Image<afwMath::Kernel::Pixel>
                                    (*(importStarVisitor.returnMean()), true))));
    int const ncomp = static_cast<int>(eigenImages.size()) - 1; // -1 since we have subtracted mean
    for (int j = 0; j != ncomp; ++j) {
        kernelListRaw.push_back(afwMath::Kernel::Ptr(
                                    new afwMath::FixedKernel(
                                        afwImage::Image<afwMath::Kernel::Pixel>
                                        (*eigenImages[j], true))));
    }
 * @endcode
 *
 * @note Works in concert with a afwMath::SpatialCellSet and afwImage::ImagePca
 * to create a Karhunen-Loeve basis from all the good KernelCandidates.  This
 * class adds the extra functionality to subtract off the mean kernel from all
 * entries in afwImage::ImagePca, which makes the resulting basis more compact.
 * The user needs to manually add this mean image into the resulting basis list
 * after imagePca.analyze() is called.
 *
 * @note afwImage::ImagePca weights objects of different brightness differently.
 * However we don't necessarily want images with larger kernel sums to have more
 * weight.  Each kernel should have constant weight in the Pca.  For simplicity
 * we scale them to have the same kernel sum, 1.0, and send to ImagePca that the
 * flux (weight) is 1.0.
 * 
 */
template<typename PixelT>
class SetPcaImageVisitor : public afwMath::CandidateVisitor {
    typedef afwImage::Image<lsst::afw::math::Kernel::Pixel> ImageT;
public:
    
    SetPcaImageVisitor(
        afwImage::ImagePca<ImageT> *imagePca ///< Set of Images to initialise
        ) :
        afwMath::CandidateVisitor(),
        _imagePca(imagePca),
        _mean() {}
    
    void processCandidate(afwMath::SpatialCellCandidate *candidate) {
        KernelCandidate<PixelT> *kCandidate = dynamic_cast<KernelCandidate<PixelT> *>(candidate);
        if (kCandidate == NULL) {
            throw LSST_EXCEPT(lsst::pex::exceptions::LogicErrorException,
                              "Failed to cast SpatialCellCandidate to KernelCandidate");
        }
        
        try {
            /* Normalize to unit sum */
            ImageT::Ptr kImage = kCandidate->copyImage();
            *kImage           /= kCandidate->getKsum();
            /* Tell imagePca they have the same weighting in the Pca */
            _imagePca->addImage(kImage, 1.0);
        } catch(lsst::pex::exceptions::LengthErrorException &e) {
            return;
        }
    }

    void subtractMean() {
        /* 
           If we don't subtract off the mean before we do the Pca, the
           subsequent terms carry less of the power than if you do subtract
           off the mean.  Explicit example:

           With mean subtraction:
             DEBUG: Eigenvalue 0 : 0.010953 (0.373870 %)
             DEBUG: Eigenvalue 1 : 0.007927 (0.270604 %)
             DEBUG: Eigenvalue 2 : 0.001393 (0.047542 %)
             DEBUG: Eigenvalue 3 : 0.001092 (0.037261 %)
             DEBUG: Eigenvalue 4 : 0.000829 (0.028283 %)
           
           Without mean subtraction:
             DEBUG: Eigenvalue 0 : 0.168627 (0.876046 %)
             DEBUG: Eigenvalue 1 : 0.007935 (0.041223 %)
             DEBUG: Eigenvalue 2 : 0.006049 (0.031424 %)
             DEBUG: Eigenvalue 3 : 0.001188 (0.006173 %)
             DEBUG: Eigenvalue 4 : 0.001050 (0.005452 %)

           After the first term above, which basically represents the mean,
           the remaining terms carry less of the power than if you do
           subtract off the mean.  (0.041223/(1-0.876046) < 0.373870).
         */
        _mean = _imagePca->getMean();
        afwImage::ImagePca<ImageT>::ImageList imageList = _imagePca->getImageList();
        for (typename afwImage::ImagePca<ImageT>::ImageList::const_iterator ptr = imageList.begin(), end = imageList.end(); ptr != end; ++ptr) {
            **ptr -= *_mean;
        }
    } 
    
    ImageT::Ptr returnMean() {return _mean;}

private:
    afwImage::ImagePca<ImageT> *_imagePca; 
    ImageT::Ptr _mean;
};


/**
 * @class BuildSingleKernelVisitor
 * @ingroup ip_diffim
 *
 * @brief Builds the convolution kernel for a given candidate
 *
 * @code
    Policy::Ptr policy(new Policy);
    policy->set("constantVarianceWeighting", false);
    policy->set("iterateSingleKernel", false);
    policy->set("singleKernelClipping", true);
    policy->set("candidateResidualMeanMax", 0.25);
    policy->set("candidateResidualStdMax", 1.25);

    detail::BuildSingleKernelVisitor<PixelT> singleKernelFitter(kFunctor, *policy);
    int nRejected = -1;
    while (nRejected != 0) {
        singleKernelFitter.reset();
        kernelCells.visitCandidates(&singleKernelFitter, nStarPerCell);
        nRejected = singleKernelFitter.getNRejected();
    }
 * @endcode
 *
 * @note Visits each current candidate in a afwMath::SpatialCellSet, and builds
 * its kernel using member object kFunctor.  We don't build the kernel for
 * *every* candidate since this is computationally expensive, only when its the
 * current candidate in the cell.  During the course of building the kernel, it
 * also assesses the quality of the difference image.  If it is determined to be
 * bad (based on the Policy paramters) the candidate is flagged as
 * afwMath::SpatialCellCandidate::BAD; otherwise its marked as
 * afwMath::SpatialCellCandidate::GOOD.  Keeps a running sample of all the new
 * candidates it visited that turned out to be bad.
 *
 * @note Because this visitor does not have access to the next candidate in the
 * cell, it must be called iteratively until no candidates are rejected.  This
 * ensures that the current candidate of every cell has an initialized Kernel.
 * This also requires that this class re-Visit all the cells after any other
 * Visitors with the ability to mark something as BAD.
 *
 * @note Because we are frequently re-Visiting entirely GOOD candidates during
 * these iterations, the option of _skipBuilt=true will enable the user to *not*
 * rebuilt the kernel on every visit.
 *
 * @note For the particular use case of creating a Pca basis from the raw
 * kernels, we want to re-Visit each candidate and re-fit the kernel using this
 * Pca basis.  However, we don't want to overwrite the original raw kernel,
 * since that is what is used to create the Pca basis in the first place.  Thus
 * the user has the option to setCandidateKernel(false), which will not override
 * the candidates original kernel, but will override its _M and _B matrices for
 * use in the spatial modeling.  This also requires the user to
 * setSkipBuilt(false) so that the candidate is reprocessed with this new basis.
 * 
 * @note When sending data to _kFunctor, this class uses an estimate of the
 * variance which is the straight difference of the 2 images.  If requested in
 * the Policy ("iterateSingleKernel"), the kernel will be rebuilt using the
 * variance of the difference image resulting from this first approximate step.
 * This is particularly useful when convolving a single-depth science image; the
 * variance (and thus resulting kernel) generally converges after 1 iteration.
 * If "constantVarianceWeighting" is requested in the Policy, no iterations will
 * be performed even if requested.
 * 
 */
template<typename PixelT>
class BuildSingleKernelVisitor : public afwMath::CandidateVisitor {
    typedef afwImage::MaskedImage<PixelT> MaskedImageT;
public:
    BuildSingleKernelVisitor(
        PsfMatchingFunctor<PixelT> &kFunctor,   ///< Functor that builds the kernels
        lsst::pex::policy::Policy const& policy ///< Policy file directing behavior
        ) :
        afwMath::CandidateVisitor(),
        _kFunctor(kFunctor),
        _policy(policy),
        _imstats(ImageStatistics<PixelT>()),
        _setCandidateKernel(true),
        _skipBuilt(true),
        _nRejected(0)
        {}
    
    /* 
     * This functionality allows the user to not set the "kernel" and thus
     * "image" values of the KernelCandidate.  When running a PCA fit on the
     * kernels, we want to keep the delta-function representation of the raw
     * kernel which are used to derive the eigenBases, while still being
     * able to modify the _M and _B matrices with linear fits to the
     * eigenBases themselves.
     */
    void setCandidateKernel(bool set) {_setCandidateKernel = set;}
    
    /* 
       Don't reprocess candidate if its already been build.  The use
       case for this functionality is : when iterating over all Cells
       and rejecting bad Kernels, we need to re-visit *all* Cells to
       build the next candidate in the list.  Without this flag we would
       unncessarily re-build all the good Kernels.
    */
    void setSkipBuilt(bool skip)      {_skipBuilt = skip;}
    
    void reset()          {_nRejected = 0;}

    int  getNRejected()   {return _nRejected;}
    
    
    void processCandidate(afwMath::SpatialCellCandidate *candidate) {
        KernelCandidate<PixelT> *kCandidate = dynamic_cast<KernelCandidate<PixelT> *>(candidate);
        if (kCandidate == NULL) {
            throw LSST_EXCEPT(lsst::pex::exceptions::LogicErrorException,
                              "Failed to cast SpatialCellCandidate to KernelCandidate");
        }
        
        if (_skipBuilt and kCandidate->hasKernel()) {
            return;
        }
        
        pexLogging::TTrace<3>("lsst.ip.diffim.BuildSingleKernelVisitor.processCandidate", 
                              "Processing candidate %d", kCandidate->getId());
        
        /* Estimate of the variance */
        MaskedImageT var = MaskedImageT(*(kCandidate->getMiToNotConvolvePtr()), true);
        if (_policy.getBool("constantVarianceWeighting")) {
            /* Constant variance weighting */
            *var.getVariance() = 1;
        }
        else {
            /* Variance estimate is the straight difference */
            var -= *(kCandidate->getMiToConvolvePtr());
        }
        
        /* Build its kernel here */
        try {
            _kFunctor.apply(*(kCandidate->getMiToConvolvePtr()->getImage()),
                            *(kCandidate->getMiToNotConvolvePtr()->getImage()),
                            *(var.getVariance()),
                            _policy);
        } catch (lsst::pex::exceptions::Exception &e) {
            kCandidate->setStatus(afwMath::SpatialCellCandidate::BAD);
            pexLogging::TTrace<4>("lsst.ip.diffim.BuildSingleKernelVisitor.processCandidate", 
                                  "Unable to process candidate; exception caught (%s)", e.what());
            _nRejected += 1;
            return;
        }
        
        /* 
           Sometimes you do not want to override the kernel; e.g. on a
           second fitting loop after the results of the first fitting loop
           are used to define a PCA basis
        */
        std::pair<boost::shared_ptr<lsst::afw::math::Kernel>, double> KB;
        try {
            KB = _kFunctor.getSolution();
        } catch (lsst::pex::exceptions::Exception &e) {
            kCandidate->setStatus(afwMath::SpatialCellCandidate::BAD);
            pexLogging::TTrace<4>("lsst.ip.diffim.BuildSingleKernelVisitor.processCandidate", 
                                  "Unable to process candidate; exception caught (%s)", e.what());
            _nRejected += 1;
            return;
        }

        if (_setCandidateKernel) {
            kCandidate->setKernel(KB.first);
            kCandidate->setBackground(KB.second);
        }
        
        /* 
         * However you *always* need to reset M and B since these are used *
         * in the spatial fitting
         */
        std::pair<boost::shared_ptr<Eigen::MatrixXd>, boost::shared_ptr<Eigen::VectorXd> > MB = _kFunctor.getAndClearMB();
        kCandidate->setM(MB.first);
        kCandidate->setB(MB.second);
        
        /* 
         * Make diffim and set chi2 from result.  Note that you need to send
         * the newly-derived kernel and background in the case that
         * _setCandidateKernel = false.
         */
        MaskedImageT diffim = kCandidate->returnDifferenceImage(KB.first, KB.second);
        
        /* 
         * Remake the kernel using the first iteration difference image
         * variance as a better estimate of the true diffim variance.  If
         * you are setting "constantVarianceWeighting" it makes no sense to
         * do this
         */
        if (_policy.getBool("iterateSingleKernel") && (!(_policy.getBool("constantVarianceWeighting")))) {
            try {
                _kFunctor.apply(*(kCandidate->getMiToConvolvePtr()->getImage()),
                                *(kCandidate->getMiToNotConvolvePtr()->getImage()),
                                *(diffim.getVariance()),
                                _policy);
            } catch (lsst::pex::exceptions::Exception &e) {
                LSST_EXCEPT_ADD(e, "Unable to recalculate Kernel");
                throw e;
            }

            try {
                KB = _kFunctor.getSolution();
            } catch (lsst::pex::exceptions::Exception &e) {
                kCandidate->setStatus(afwMath::SpatialCellCandidate::BAD);
                pexLogging::TTrace<4>("lsst.ip.diffim.BuildSingleKernelVisitor.processCandidate", 
                                      "Unable to process candidate; exception caught (%s)", e.what());
                _nRejected += 1;
                return;
            }

            if (_setCandidateKernel) {
                kCandidate->setKernel(KB.first);
                kCandidate->setBackground(KB.second);
            }

            MB = _kFunctor.getAndClearMB();
            kCandidate->setM(MB.first);
            kCandidate->setB(MB.second);
            diffim = kCandidate->returnDifferenceImage(KB.first, KB.second);                
        }
        
        _imstats.apply(diffim);
        kCandidate->setChi2(_imstats.getVariance());
        
        /* When using a Pca basis, we don't reset the kernel or background,
           so we need to evaluate these locally for the Trace */
        afwImage::Image<double> kImage(KB.first->getDimensions());
        double kSum = KB.first->computeImage(kImage, false);
        double background = KB.second;
        
        pexLogging::TTrace<5>("lsst.ip.diffim.BuildSingleKernelVisitor.processCandidate", 
                              "Chi2 = %.2f", kCandidate->getChi2());
        pexLogging::TTrace<5>("lsst.ip.diffim.BuildSingleKernelVisitor.processCandidate",
                              "X = %.2f Y = %.2f",
                              kCandidate->getXCenter(), 
                              kCandidate->getYCenter());
        pexLogging::TTrace<5>("lsst.ip.diffim.BuildSingleKernelVisitor.processCandidate",
                              "Kernel Sum = %.3f", kSum);
        pexLogging::TTrace<5>("lsst.ip.diffim.BuildSingleKernelVisitor.processCandidate",
                              "Background = %.3f", background);
        pexLogging::TTrace<4>("lsst.ip.diffim.BuildSingleKernelVisitor.processCandidate",
                              "Diffim residuals = %.2f +/- %.2f sigma",
                              _imstats.getMean(),
                              _imstats.getRms());

        if ((std::isnan(_imstats.getMean())) || (std::isnan(_imstats.getRms()))) {
            kCandidate->setStatus(afwMath::SpatialCellCandidate::BAD);
            pexLogging::TTrace<4>("lsst.ip.diffim.BuildSingleKernelVisitor.processCandidate", 
                                  "Rejecting candidate, encountered NaN");
            _nRejected += 1;
            return;
        }

        if (_policy.getBool("singleKernelClipping")) {
            if (fabs(_imstats.getMean()) > _policy.getDouble("candidateResidualMeanMax")) {
                kCandidate->setStatus(afwMath::SpatialCellCandidate::BAD);
                pexLogging::TTrace<4>("lsst.ip.diffim.BuildSingleKernelVisitor.processCandidate", 
                                      "Rejecting due to bad source kernel mean residuals : |%.2f| > %.2f",
                                      _imstats.getMean(),
                                      _policy.getDouble("candidateResidualMeanMax"));
                _nRejected += 1;
            }
            else if (_imstats.getRms() > _policy.getDouble("candidateResidualStdMax")) {
                kCandidate->setStatus(afwMath::SpatialCellCandidate::BAD);
                pexLogging::TTrace<4>("lsst.ip.diffim.BuildSingleKernelVisitor.processCandidate", 
                                      "Rejecting due to bad source kernel residual rms : %.2f > %.2f",
                                      _imstats.getRms(),
                                      _policy.getDouble("candidateResidualStdMax"));
                _nRejected += 1;
            }
            else {
                kCandidate->setStatus(afwMath::SpatialCellCandidate::GOOD);
                pexLogging::TTrace<5>("lsst.ip.diffim.BuildSingleKernelVisitor.processCandidate", 
                                      "Source kernel OK");
            }
        }
        else {
            kCandidate->setStatus(afwMath::SpatialCellCandidate::GOOD);
            pexLogging::TTrace<5>("lsst.ip.diffim.BuildSingleKernelVisitor.processCandidate", 
                                  "Sigma clipping not enabled");
        }

    }
private:
    PsfMatchingFunctor<PixelT> _kFunctor;
    lsst::pex::policy::Policy _policy;
    ImageStatistics<PixelT> _imstats;
    bool _setCandidateKernel;
    bool _skipBuilt;
    int _nRejected;
};


/**
 * @class BuildSpatialKernelVisitor
 * @ingroup ip_diffim
 *
 * @brief Creates a spatial kernel and background from a list of candidates
 *
 * @code
    Policy::Ptr policy(new Policy);
    policy->set("spatialKernelOrder", spatialKernelOrder);
    policy->set("spatialBgOrder", spatialBgOrder);
    policy->set("kernelBasisSet", "delta-function");
    policy->set("usePcaForSpatialKernel", true);

    detail::BuildSpatialKernelVisitor<PixelT> spatialKernelFitter(*basisListToUse, spatialKernelOrder, spatialBgOrder, *policy);
    kernelCells.visitCandidates(&spatialKernelFitter, nStarPerCell);
    spatialKernelFitter.solveLinearEquation();
    std::pair<afwMath::LinearCombinationKernel::Ptr, 
        afwMath::Kernel::SpatialFunctionPtr> KB = spatialKernelFitter.getSpatialModel();
    spatialKernel     = KB.first;
    spatialBackground = KB.second; 
 * @endcode
 *
 * @note After visiting all candidates, solveLinearEquation() must be called to
 * trigger the matrix math.
 *
 * @note The user has the option to enfore conservation of the kernel sum across
 * the image through the policy.  In this case, all terms but the first are fit
 * for spatial variation.  This requires a little extra code to make sure the
 * matrices are the correct size, and that it is accessing the appropriate terms
 * in the matrices when creating the spatial models.
 * 
 */
template<typename PixelT>
class BuildSpatialKernelVisitor : public afwMath::CandidateVisitor {
public:
    BuildSpatialKernelVisitor(
        afwMath::KernelList basisList,   ///< Basis functions used in the fit
        int const spatialKernelOrder,    ///< Order of spatial kernel variation (cf. lsst::afw::math::PolynomialFunction2)
        int const spatialBgOrder,        ///< Order of spatial bg variation (cf. lsst::afw::math::PolynomialFunction2)
        lsst::pex::policy::Policy policy ///< Policy file directing behavior
        ) :
        afwMath::CandidateVisitor(),
        _basisList(basisList),
        _M(Eigen::MatrixXd()),
        _B(Eigen::VectorXd()),
        _Soln(Eigen::VectorXd()),
        _spatialKernelOrder(spatialKernelOrder),
        _spatialBgOrder(spatialBgOrder),
        _spatialKernelFunction(new afwMath::PolynomialFunction2<double>(spatialKernelOrder)),
        _spatialBgFunction(new afwMath::PolynomialFunction2<double>(spatialBgOrder)),
        _nbases(basisList.size()),
        _policy(policy),
        _constantFirstTerm(false){
        
        /* 
           NOTE : The variable _constantFirstTerm allows that the first
           component of the basisList has no spatial variation.  This is useful
           to conserve the kernel sum across the image.  There are 2 ways to
           implement this : we can make the input matrices/vectors smaller by
           (_nkt-1), or we can create _M and _B with empty values for the first
           component's spatial terms.  The latter could cause problems for the
           matrix math even though its probably more readable, so we go with the
           former.
        */
        if ((_policy.getString("kernelBasisSet") == "alard-lupton") || _policy.getBool("usePcaForSpatialKernel")) {
            _constantFirstTerm = true;
        }
        
        /* Bookeeping terms */
        _nkt = _spatialKernelFunction->getParameters().size();
        _nbt = _spatialBgFunction->getParameters().size();
        if (_constantFirstTerm) {
            _nt = (_nbases-1)*_nkt+1 + _nbt;
        } else {
            _nt = _nbases*_nkt + _nbt;
        }
        _M.resize(_nt, _nt);
        _B.resize(_nt);
        _M.setZero();
        _B.setZero();
        
        pexLogging::TTrace<5>("lsst.ip.diffim.LinearSpatialFitVisitor", 
                              "Initializing with size %d %d %d and constant first term = %s",
                              _nkt, _nbt, _nt,
                              _constantFirstTerm ? "true" : "false");
    }
    
    void processCandidate(afwMath::SpatialCellCandidate *candidate) {
        KernelCandidate<PixelT> *kCandidate = dynamic_cast<KernelCandidate<PixelT> *>(candidate);
        if (kCandidate == NULL) {
            throw LSST_EXCEPT(lsst::pex::exceptions::LogicErrorException,
                              "Failed to cast SpatialCellCandidate to KernelCandidate");
        }
        if (!(kCandidate->hasKernel())) {
            pexLogging::TTrace<3>("lsst.ip.diffim.BuildSpatialKernelVisitor.processCandidate", 
                                  "Cannot process candidate %d, continuing", kCandidate->getId());
            return;
        }
        
        pexLogging::TTrace<6>("lsst.ip.diffim.BuildSpatialKernelVisitor.processCandidate", 
                              "Processing candidate %d", kCandidate->getId());
        
        /* Calculate P matrices */
        /* Pure kernel terms */
        std::vector<double> paramsK = _spatialKernelFunction->getParameters();
        for (unsigned int idx = 0; idx < _nkt; idx++) { paramsK[idx] = 0.0; }
        Eigen::VectorXd Pk(_nkt);
        for (unsigned int idx = 0; idx < _nkt; idx++) {
            paramsK[idx] = 1.0;
            _spatialKernelFunction->setParameters(paramsK);
            Pk(idx) = (*_spatialKernelFunction)(kCandidate->getXCenter(), 
                                                kCandidate->getYCenter());
            paramsK[idx] = 0.0;
        }
        Eigen::MatrixXd PkPkt = (Pk * Pk.transpose());
        
        /* Pure background terms */
        std::vector<double> paramsB = _spatialBgFunction->getParameters();
        for (unsigned int idx = 0; idx < _nbt; idx++) { paramsB[idx] = 0.0; }
        Eigen::VectorXd Pb(_nbt);
        for (unsigned int idx = 0; idx < _nbt; idx++) {
            paramsB[idx] = 1.0;
            _spatialBgFunction->setParameters(paramsB);
            Pb(idx) = (*_spatialBgFunction)(kCandidate->getXCenter(), 
                                            kCandidate->getYCenter());
            paramsB[idx] = 0.0;
        }
        Eigen::MatrixXd PbPbt = (Pb * Pb.transpose());
        
        /* Cross terms */
        Eigen::MatrixXd PkPbt = (Pk * Pb.transpose());
        
        if (DEBUG_MATRIX) {
            std::cout << "Spatial weights" << std::endl;
            std::cout << "PkPkt " << PkPkt << std::endl;
            std::cout << "PbPbt " << PbPbt << std::endl;
            std::cout << "PkPbt " << PkPbt << std::endl;
        }
        
        /* Add each candidate to the M, B matrix */
        boost::shared_ptr<Eigen::MatrixXd> Q = kCandidate->getM();
        boost::shared_ptr<Eigen::VectorXd> W = kCandidate->getB();
        
        if (DEBUG_MATRIX) {
            std::cout << "Spatial matrix inputs" << std::endl;
            std::cout << "M " << (*Q) << std::endl;
            std::cout << "B " << (*W) << std::endl;
        }
        
        /* first index to start the spatial blocks; default=0 for non-constant first term */
        unsigned int m0 = 0; 
        /* how many rows/cols to adjust the matrices/vectors; default=0 for non-constant first term */
        unsigned int dm = 0; 
        /* where to start the background terms; this is always true */
        unsigned int mb = _nt - _nbt;
        
        if (_constantFirstTerm) {
            m0 = 1;       /* we need to manually fill in the first (non-spatial) terms below */
            dm = _nkt-1;  /* need to shift terms due to lack of spatial variation in first term */
            
            _M(0, 0) += (*Q)(0,0);
            for(unsigned int m2 = 1; m2 < _nbases; m2++)  {
                _M.block(0, m2*_nkt-dm, 1, _nkt) += (*Q)(0,m2) * Pk.transpose();
            }
            
            _M.block(0, mb, 1, _nbt) += (*Q)(0,_nbases) * Pb.transpose();
            _B(0) += (*W)(0);
        }
        
        /* Fill in the spatial blocks */
        for(unsigned int m1 = m0; m1 < _nbases; m1++)  {
            /* Diagonal kernel-kernel term; only use upper triangular part of PkPkt */
            _M.block(m1*_nkt-dm, m1*_nkt-dm, _nkt, _nkt) += (*Q)(m1,m1) * PkPkt.part<Eigen::UpperTriangular>();
            
            /* Kernel-kernel terms */
            for(unsigned int m2 = m1+1; m2 < _nbases; m2++)  {
                _M.block(m1*_nkt-dm, m2*_nkt-dm, _nkt, _nkt) += (*Q)(m1,m2) * PkPkt;
            }
            
            /* Kernel cross terms with background */
            _M.block(m1*_nkt-dm, mb, _nkt, _nbt) += (*Q)(m1,_nbases) * PkPbt;
            
            /* B vector */
            _B.segment(m1*_nkt-dm, _nkt) += (*W)(m1) * Pk;
        }
        
        /* Background-background terms only */
        _M.block(mb, mb, _nbt, _nbt) += (*Q)(_nbases,_nbases) * PbPbt.part<Eigen::UpperTriangular>();
        _B.segment(mb, _nbt)         += (*W)(_nbases) * Pb;
        
        if (DEBUG_MATRIX) {
            std::cout << "Spatial matrix outputs" << std::endl;
            std::cout << "_M " << _M << std::endl;
            std::cout << "_B " << _B << std::endl;
        }
        
    }
    
    void solveLinearEquation() {
        boost::timer t;
        t.restart();

        pexLogging::TTrace<2>("lsst.ip.diffim.SpatialModelKernel.solveLinearEquation", 
                              "Solving for spatial model");
        
        /* Fill in the other half of _M */
        for (unsigned int i = 0; i < _nt; i++) {
            for (unsigned int j = i+1; j < _nt; j++) {
                _M(j,i) = _M(i,j);
            }
        }
        _Soln = Eigen::VectorXd::Zero(_nt);
        
        if (DEBUG_MATRIX) {
            std::cout << "Solving for _M:" << std::endl;
            std::cout << _M << std::endl;
            std::cout << _B << std::endl;
        }
        
        if (!(_M.ldlt().solve(_B, &_Soln))) {
            pexLogging::TTrace<5>("lsst.ip.diffim.SpatialModelKernel.solveLinearEquation", 
                                  "Unable to determine kernel via Cholesky LDL^T");
            if (!(_M.llt().solve(_B, &_Soln))) {
                pexLogging::TTrace<5>("lsst.ip.diffim.SpatialModelKernel.solveLinearEquation", 
                                      "Unable to determine kernel via Cholesky LL^T");
                if (!(_M.lu().solve(_B, &_Soln))) {
                    pexLogging::TTrace<5>("lsst.ip.diffim.SpatialModelKernel.solveLinearEquation", 
                                          "Unable to determine kernel via LU");
                    // LAST RESORT
                    try {
                        Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> eVecValues(_M);
                        Eigen::MatrixXd const& R = eVecValues.eigenvectors();
                        Eigen::VectorXd eValues  = eVecValues.eigenvalues();
                        
                        for (int i = 0; i != eValues.rows(); ++i) {
                            if (eValues(i) != 0.0) {
                                eValues(i) = 1.0/eValues(i);
                            }
                        }
                        
                        _Soln = R*eValues.asDiagonal()*R.transpose()*_B;
                    } catch (pexExcept::Exception& e) {
                        pexLogging::TTrace<5>("lsst.ip.diffim.SpatialModelKernel.solveLinearEquation", 
                                              "Unable to determine kernel via eigen-values");
                        
                        throw LSST_EXCEPT(pexExcept::Exception, 
                                          "Unable to determine kernel solution in SpatialModelKernel::solveLinearEquation");
                    }
                }
            }
        }
        
        if (DEBUG_MATRIX) {
            std::cout << "Solution:" << std::endl;
            std::cout << _Soln << std::endl;
        }
        
        double time = t.elapsed();
        pexLogging::TTrace<3>("lsst.ip.diffim.SpatialModelKernel.solveLinearEquation", 
                              "Compute time to do spatial matrix math : %.2f s", time);
    }
    
    
    std::pair<afwMath::LinearCombinationKernel::Ptr, afwMath::Kernel::SpatialFunctionPtr> getSpatialModel() {
        /* Set up kernel */
        std::vector<afwMath::Kernel::SpatialFunctionPtr> spatialFunctionList;
        for (unsigned int i = 0; i < _nbases; i++) {
            afwMath::Kernel::SpatialFunctionPtr spatialFunction(_spatialKernelFunction->copy());
            spatialFunctionList.push_back(spatialFunction);
        }
        afwMath::LinearCombinationKernel::Ptr spatialKernel(new afwMath::LinearCombinationKernel(_basisList, spatialFunctionList));
        
        /* Set up background */
        afwMath::Kernel::SpatialFunctionPtr bgFunction(_spatialBgFunction->copy());
        
        /* Set the kernel coefficients */
        std::vector<std::vector<double> > kCoeffs;
        kCoeffs.reserve(_nbases);
        for (unsigned int i = 0, idx = 0; i < _nbases; i++) {
            kCoeffs.push_back(std::vector<double>(_nkt));
            
            /* Deal with the possibility the first term doesn't vary spatially */
            if ((i == 0) && (_constantFirstTerm)) {
                kCoeffs[i][0] = _Soln[idx++];
            }
            else {
                for (unsigned int j = 0; j < _nkt; j++) {
                    kCoeffs[i][j] = _Soln[idx++];
                }
            }
        }
        
        /* Set the background coefficients */
        std::vector<double> bgCoeffs(_nbt);
        for (unsigned int i = 0; i < _nbt; i++) {
            bgCoeffs[i] = _Soln[_nt - _nbt + i];
        }
        
        spatialKernel->setSpatialParameters(kCoeffs);
        bgFunction->setParameters(bgCoeffs);
        
        return std::make_pair(spatialKernel, bgFunction);
    }
    
private:
    afwMath::KernelList _basisList;
    Eigen::MatrixXd _M;       ///< Least squares matrix
    Eigen::VectorXd _B;       ///< Least squares vector
    Eigen::VectorXd _Soln;    ///< Least squares solution
    int const _spatialKernelOrder;
    int const _spatialBgOrder;
    afwMath::Kernel::SpatialFunctionPtr _spatialKernelFunction;
    afwMath::Kernel::SpatialFunctionPtr _spatialBgFunction;
    unsigned int _nbases;     ///< Number of bases being fit for
    unsigned int _nkt;        ///< Number of kernel terms in spatial model
    unsigned int _nbt;        ///< Number of backgruond terms in spatial model
    unsigned int _nt;         ///< Total number of terms in the solution; also dimensions of matrices
    lsst::pex::policy::Policy _policy;
    bool _constantFirstTerm;  ///< Is the first term spatially invariant?
};

/**
 * @class AssessSpatialKernelVisitor
 * @ingroup ip_diffim
 *
 * @brief Asseses the quality of a candidate given a spatial kernel and background model
 *
 * @code
    detail::AssessSpatialKernelVisitor<PixelT> spatialKernelAssessor(spatialKernel, spatialBackground, policy);
    spatialKernelAssessor.reset();
    kernelCells.visitCandidates(&spatialKernelAssessor, nStarPerCell);
    nRejected = spatialKernelAssessor.getNRejected();
 * @endcode
 *
 * @note Evaluates the spatial kernel and spatial background at the location of
 * each candidate, and computes the resulting difference image.  Sets candidate
 * as afwMath::SpatialCellCandidate::GOOD/BAD if requested by the Policy.
 * 
 */
template<typename PixelT>
class AssessSpatialKernelVisitor : public afwMath::CandidateVisitor {
    typedef afwImage::MaskedImage<PixelT> MaskedImageT;
public:
    AssessSpatialKernelVisitor(
        afwMath::LinearCombinationKernel::Ptr spatialKernel,   ///< Spatially varying kernel model
        afwMath::Kernel::SpatialFunctionPtr spatialBackground, ///< Spatially varying backgound model
        lsst::pex::policy::Policy const& policy                ///< Policy file directing behavior
        ) : 
        afwMath::CandidateVisitor(),
        _spatialKernel(spatialKernel),
        _spatialBackground(spatialBackground),
        _policy(policy),
        _imstats(ImageStatistics<PixelT>()),
        _nGood(0),
        _nRejected(0) {}

    void reset() {
        _nGood = 0;
        _nRejected = 0;
    }

    int getNGood() {return _nGood;}

    int getNRejected() {return _nRejected;}

    void processCandidate(afwMath::SpatialCellCandidate *candidate) {
        KernelCandidate<PixelT> *kCandidate = dynamic_cast<KernelCandidate<PixelT> *>(candidate);
        if (kCandidate == NULL) {
            throw LSST_EXCEPT(lsst::pex::exceptions::LogicErrorException,
                              "Failed to cast SpatialCellCandidate to KernelCandidate");
        }
        if (!(kCandidate->hasKernel())) {
            pexLogging::TTrace<3>("lsst.ip.diffim.AssessSpatialKernelVisitor.processCandidate", 
                                  "Cannot process candidate %d, continuing", kCandidate->getId());
            return;
        }
        
        pexLogging::TTrace<3>("lsst.ip.diffim.AssessSpatialKernelVisitor.processCandidate", 
                              "Processing candidate %d", kCandidate->getId());
        
        /* 
           Note - this is a hack until the Kernel API is upgraded by the
           Davis crew.  I need a "local" version of the spatially varying
           Kernel
        */
        afwImage::Image<double> kImage(_spatialKernel->getDimensions());
        double kSum = _spatialKernel->computeImage(kImage, false, 
                                                   kCandidate->getXCenter(),
                                                   kCandidate->getYCenter());
        boost::shared_ptr<afwMath::Kernel>
            kernelPtr(new afwMath::FixedKernel(kImage));
        /* </hack> */
        
        double background = (*_spatialBackground)(kCandidate->getXCenter(), kCandidate->getYCenter());
        
        MaskedImageT diffim = kCandidate->returnDifferenceImage(kernelPtr, background);

        kImage.writeFits("/tmp/kernel2.fits");
        diffim.writeFits("/tmp/diffim");

        _imstats.apply(diffim);
        kCandidate->setChi2(_imstats.getVariance());
        
        pexLogging::TTrace<5>("lsst.ip.diffim.AssessSpatialKernelVisitor.processCandidate", 
                              "Chi2 = %.2f", kCandidate->getChi2());
        pexLogging::TTrace<5>("lsst.ip.diffim.AssessSpatialKernelVisitor.processCandidate",
                              "X = %.2f Y = %.2f",
                              kCandidate->getXCenter(), 
                              kCandidate->getYCenter());
        pexLogging::TTrace<5>("lsst.ip.diffim.AssessSpatialKernelVisitor.processCandidate",
                              "Kernel Sum = %.3f", kSum);
        pexLogging::TTrace<5>("lsst.ip.diffim.AssessSpatialKernelVisitor.processCandidate",
                              "Background = %.3f", background);
        pexLogging::TTrace<4>("lsst.ip.diffim.AssessSpatialKernelVisitor.processCandidate",
                              "Diffim residuals = %.2f +/- %.2f sigma",
                              _imstats.getMean(),
                              _imstats.getRms());

        if ((std::isnan(_imstats.getMean())) || (std::isnan(_imstats.getRms()))) {
            kCandidate->setStatus(afwMath::SpatialCellCandidate::BAD);
            pexLogging::TTrace<4>("lsst.ip.diffim.AssessSpatialKernelVisitor.processCandidate", 
                                  "Rejecting candidate, encountered NaN");
            _nRejected += 1;
            return;
        }

        if (_policy.getBool("spatialKernelClipping")) {            
            if (fabs(_imstats.getMean()) > _policy.getDouble("candidateResidualMeanMax")) {
                kCandidate->setStatus(afwMath::SpatialCellCandidate::BAD);
                pexLogging::TTrace<4>("lsst.ip.diffim.AssessSpatialKernelVisitor.processCandidate", 
                                      "Rejecting due to bad spatial kernel mean residuals : |%.2f| > %.2f",
                                      _imstats.getMean(),
                                      _policy.getDouble("candidateResidualMeanMax"));
                _nRejected += 1;
            }
            else if (_imstats.getRms() > _policy.getDouble("candidateResidualStdMax")) {
                kCandidate->setStatus(afwMath::SpatialCellCandidate::BAD);
                pexLogging::TTrace<4>("lsst.ip.diffim.AssessSpatialKernelVisitor.processCandidate", 
                                      "Rejecting due to bad spatial kernel residual rms : %.2f > %.2f",
                                      _imstats.getRms(),
                                      _policy.getDouble("candidateResidualStdMax"));
                _nRejected += 1;
            }
            else {
                kCandidate->setStatus(afwMath::SpatialCellCandidate::GOOD);
                pexLogging::TTrace<5>("lsst.ip.diffim.AssessSpatialKernelVisitor.processCandidate", 
                                      "Spatial kernel OK");
                _nGood += 1;
            }
        }
        else {
            kCandidate->setStatus(afwMath::SpatialCellCandidate::GOOD);
            pexLogging::TTrace<5>("lsst.ip.diffim.AssessSpatialKernelVisitor.processCandidate", 
                                  "Sigma clipping not enabled");
            _nGood += 1;
        }
    }
    
private:
    afwMath::LinearCombinationKernel::Ptr _spatialKernel;
    afwMath::Kernel::SpatialFunctionPtr _spatialBackground;
    lsst::pex::policy::Policy _policy;
    ImageStatistics<PixelT> _imstats;
    int _nGood;
    int _nRejected;
};

}}}}
