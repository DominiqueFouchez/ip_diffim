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

    template<typename PixelT>
    class KernelSumVisitor : public afwMath::CandidateVisitor {
        typedef afwImage::Image<afwMath::Kernel::Pixel> ImageT;
    public:
        enum Mode {AGGREGATE = 0, REJECT = 1};
        
        KernelSumVisitor(lsst::pex::policy::Policy const& policy) :
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

        void reset() {
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
                                              "Rejecting candidate %d due to bad source kernel sum : (%.2f)",
                                              kCandidate->getId(),
                                              kCandidate->getKsum());
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
            afwMath::Statistics stats = afwMath::makeStatistics(_kSums, afwMath::NPOINT | afwMath::MEANCLIP | afwMath::STDEVCLIP); 
            _kSumMean = stats.getValue(afwMath::MEANCLIP);
            _kSumStd  = stats.getValue(afwMath::STDEVCLIP);
            _kSumNpts = static_cast<int>(stats.getValue(afwMath::NPOINT));
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
    
    template<typename PixelT>
    class SetPcaImageVisitor : public afwMath::CandidateVisitor {
        typedef afwImage::Image<lsst::afw::math::Kernel::Pixel> ImageT;
    public:
        
        SetPcaImageVisitor(afwImage::ImagePca<ImageT> *imagePca // Set of Images to initialise
            ) :
            afwMath::CandidateVisitor(),
            _imagePca(imagePca),
            _mean() {}
        
        // Called by SpatialCellSet::visitCandidates for each Candidate
        void processCandidate(afwMath::SpatialCellCandidate *candidate) {
            KernelCandidate<PixelT> *kCandidate = dynamic_cast<KernelCandidate<PixelT> *>(candidate);
            if (kCandidate == NULL) {
                throw LSST_EXCEPT(lsst::pex::exceptions::LogicErrorException,
                                  "Failed to cast SpatialCellCandidate to KernelCandidate");
            }
            
            try {
                /* 
                   We don't necessarily want images with larger kernel sums to
                   have more weight.  Each kernel should have constant weight in
                   the Pca.  For simplicity we will also scale them to have the
                   same kernel sum, 1.0, and send to ImagePca that the flux is
                   1.0.
                */
                ImageT::Ptr kImage = kCandidate->copyImage();
                *kImage           /= kCandidate->getKsum();
                
                _imagePca->addImage(kImage, 1.0);
            } catch(lsst::pex::exceptions::LengthErrorException &e) {
                return;
            }
        }

        void subtractMean() {
            /* 
               If we don't subtract off the mean before we do the Pca, the
               subsequent terms carry less of the power than if you do subtract
               off the means.  Explicit example:

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
               subtract off the mean.  (0.041223/(1-0.876046) < 0.373870).  Not
               by much, but still significant.
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
    
    
    template<typename PixelT>
    class BuildSingleKernelVisitor : public afwMath::CandidateVisitor {
        typedef afwImage::MaskedImage<PixelT> MaskedImageT;
    public:
        BuildSingleKernelVisitor(PsfMatchingFunctor<PixelT> &kFunctor,
                                 lsst::pex::policy::Policy const& policy) :
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
        
        /* 
           Since this is the base class that builds a kernel, we need to make
           sure that the current Kernel in the Cell is initialized.  To do this,
           if we set something afwMath::SpatialCellCandidate::BAD we have to go
           back over the Cells and build the next Candidate, until we are out of
           Candidates (in which case this will get called on no Cells and
           nRejected=0) or all current Candidates are
           afwMath::SpatialCellCandidate::GOOD.
        */
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
    
    
    template<typename PixelT>
    class AssessSpatialKernelVisitor : public afwMath::CandidateVisitor {
        typedef afwImage::MaskedImage<PixelT> MaskedImageT;
    public:
        AssessSpatialKernelVisitor(afwMath::LinearCombinationKernel::Ptr spatialKernel,
                                   afwMath::Kernel::SpatialFunctionPtr spatialBackground,
                                   lsst::pex::policy::Policy const& policy) :
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
    
    
    template<typename PixelT>
    class BuildSpatialKernelVisitor : public afwMath::CandidateVisitor {
    public:
        BuildSpatialKernelVisitor(
            afwMath::KernelList basisList,  ///< Basis functions used in the fit
            int const spatialKernelOrder,   ///< Order of spatial kernel variation (cf. lsst::afw::math::PolynomialFunction2)
            int const spatialBgOrder,       ///< Order of spatial bg variation (cf. lsst::afw::math::PolynomialFunction2)
            lsst::pex::policy::Policy policy
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
               component of the basisList has no spatial variation.  This is
               useful to conserve the kernel sum across the image.  There are 2
               options to enable this : we can make the input matrices/vectors
               smaller by (_nkt-1), or we can create _M and _B with empty values
               for the first component's spatial terms.  The latter could cause
               problems for the matrix math even though its probably more
               readable, so we go with the former.
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
}}}}
