// -*- lsst-c++ -*-
/**
 * @file
 *
 * @brief Implementation of SpatialModelKernel class
 *
 * @author Andrew Becker, University of Washington
 *
 * @ingroup ip_diffim
 */
#include <boost/timer.hpp> 

#include <lsst/afw/image/Image.h>
#include <lsst/afw/image/ImagePca.h>
#include <lsst/afw/math/Kernel.h>
#include <lsst/afw/math/FunctionLibrary.h>
#include <lsst/afw/detection/Footprint.h>

#include <lsst/pex/exceptions/Runtime.h>
#include <lsst/pex/policy/Policy.h>
#include <lsst/pex/logging/Trace.h>

#include <Eigen/Core>
#include <Eigen/Cholesky>
#include <Eigen/LU>
#include <Eigen/QR>

#include <lsst/ip/diffim/SpatialModelKernel.h>

#define DEBUG_MATRIX 0

namespace afwMath        = lsst::afw::math;
namespace afwImage       = lsst::afw::image;
namespace pexLogging     = lsst::pex::logging; 
namespace pexExcept      = lsst::pex::exceptions; 
namespace pexPolicy      = lsst::pex::policy; 

namespace lsst {
namespace ip {
namespace diffim {

template <typename PixelT>
KernelCandidate<PixelT>::ImageT::ConstPtr KernelCandidate<PixelT>::getImage() const {
    if (!_haveKernel) {
        throw LSST_EXCEPT(pexExcept::Exception, "No Kernel to make KernelCandidate Image from");
    }
    return _image;
}

template <typename PixelT>
KernelCandidate<PixelT>::ImageT::Ptr KernelCandidate<PixelT>::copyImage() const {
    return typename KernelCandidate<PixelT>::ImageT::Ptr(new typename KernelCandidate<PixelT>::ImageT(*getImage(), true));
    /*
    typename KernelCandidate<PixelT>::ImageT::Ptr imcopy(
        new typename KernelCandidate<PixelT>::ImageT(*_image, true)
        );
    return imcopy;
    */
}

  
template <typename PixelT>
void KernelCandidate<PixelT>::setKernel(lsst::afw::math::Kernel::Ptr kernel) {
    _kernel     = kernel; 
    _haveKernel = true;

    setWidth(_kernel->getWidth());
    setHeight(_kernel->getHeight());
    
    typename KernelCandidate<PixelT>::ImageT::Ptr image (
        new typename KernelCandidate<PixelT>::ImageT(_kernel->getDimensions())
        );
    _kSum  = _kernel->computeImage(*image, false);                    
    _image = image;
}

template <typename PixelT>
afwMath::Kernel::Ptr KernelCandidate<PixelT>::getKernel() const {
    if (!_haveKernel) {
        throw LSST_EXCEPT(pexExcept::Exception, "No Kernel for KernelCandidate");
    }
    return _kernel;
}

template <typename PixelT>
double KernelCandidate<PixelT>::getBackground() const {
    if (!_haveKernel) {
        throw LSST_EXCEPT(pexExcept::Exception, "No Kernel for KernelCandidate");
    }
    return _background;
}

template <typename PixelT>
double KernelCandidate<PixelT>::getKsum() const {
    if (!_haveKernel) {
        throw LSST_EXCEPT(pexExcept::Exception, "No Kernel for KernelCandidate");
    }
    return _kSum;
}

template <typename PixelT>
lsst::afw::image::MaskedImage<PixelT> KernelCandidate<PixelT>::returnDifferenceImage() {
    if (!_haveKernel) {
        throw LSST_EXCEPT(pexExcept::Exception, "No Kernel for KernelCandidate");
    }
    return returnDifferenceImage(_kernel, _background);
}

template <typename PixelT>
lsst::afw::image::MaskedImage<PixelT> KernelCandidate<PixelT>::returnDifferenceImage(
    lsst::afw::math::Kernel::Ptr kernel,
    double background
    ) {
    if (!_haveKernel) {
        throw LSST_EXCEPT(pexExcept::Exception, "No Kernel for KernelCandidate");
    }
    
    /* Make diffim and set chi2 from result */
    lsst::afw::image::MaskedImage<PixelT> diffim = convolveAndSubtract(*_miToConvolvePtr,
                                                                       *_miToNotConvolvePtr,
                                                                       *kernel,
                                                                       background);
    return diffim;

}

namespace {
    template<typename PixelT>
    class SetPcaImageVisitor : public afwMath::CandidateVisitor {
        typedef afwImage::Image<lsst::afw::math::Kernel::Pixel> ImageT;
    public:
        SetPcaImageVisitor(afwImage::ImagePca<ImageT> *imagePca // Set of Images to initialise
            ) :
            afwMath::CandidateVisitor(),
            _imagePca(imagePca) {}
        
        // Called by SpatialCellSet::visitCandidates for each Candidate
        void processCandidate(afwMath::SpatialCellCandidate *candidate) {
            KernelCandidate<PixelT> *kCandidate = dynamic_cast<KernelCandidate<PixelT> *>(candidate);
            if (kCandidate == NULL) {
                throw LSST_EXCEPT(lsst::pex::exceptions::LogicErrorException,
                                  "Failed to cast SpatialCellCandidate to KernelCandidate");
            }
            
            try {
                _imagePca->addImage(kCandidate->copyImage(), kCandidate->getCandidateRating());
            } catch(lsst::pex::exceptions::LengthErrorException &e) {
                return;
            }
        }
    private:
        afwImage::ImagePca<ImageT> *_imagePca; 
    };
}

namespace {
    /* Lets assume this steps over the bad footprints */
    template<typename PixelT>
    class BuildSingleKernelVisitor : public afwMath::CandidateVisitor {
        typedef afwImage::MaskedImage<PixelT> MaskedImageT;
    public:
        BuildSingleKernelVisitor(PsfMatchingFunctor<PixelT> &kFunctor,
                                 lsst::pex::policy::Policy const& policy) :
            afwMath::CandidateVisitor(),
            _kFunctor(kFunctor),
            _policy(policy),
            _imstats(ImageStatistics<PixelT>()){}
        
        void processCandidate(afwMath::SpatialCellCandidate *candidate) {
            KernelCandidate<PixelT> *kCandidate = dynamic_cast<KernelCandidate<PixelT> *>(candidate);
            if (kCandidate == NULL) {
                throw LSST_EXCEPT(lsst::pex::exceptions::LogicErrorException,
                                  "Failed to cast SpatialCellCandidate to KernelCandidate");
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
                LSST_EXCEPT_ADD(e, "Unable to calculate Kernel");
                throw e;
            }
            

            /* Update the candidate with derived products, need to get Kernel
             * first.  This calls for some redesign */
            std::pair<boost::shared_ptr<lsst::afw::math::Kernel>, double> KB = _kFunctor.getKernel();
            kCandidate->setKernel(KB.first);
            kCandidate->setBackground(KB.second);

            std::pair<boost::shared_ptr<Eigen::MatrixXd>, boost::shared_ptr<Eigen::VectorXd> > MB = _kFunctor.getAndClearMB();
            kCandidate->setM(MB.first);
            kCandidate->setB(MB.second);

            /* Make diffim and set chi2 from result */
            MaskedImageT diffim = kCandidate->returnDifferenceImage();
            
            /* Remake the kernel using the first iteration difference image
             * variance as a better estimate of the true diffim variance */
            if (_policy.getBool("iterateSingleKernel")) {
                try {
                    _kFunctor.apply(*(kCandidate->getMiToConvolvePtr()->getImage()),
                                    *(kCandidate->getMiToNotConvolvePtr()->getImage()),
                                    *(diffim.getVariance()),
                                    _policy);
                } catch (lsst::pex::exceptions::Exception &e) {
                    LSST_EXCEPT_ADD(e, "Unable to recalculate Kernel");
                    throw e;
                }
                KB = _kFunctor.getKernel();
                kCandidate->setKernel(KB.first);
                kCandidate->setBackground(KB.second);
                MB = _kFunctor.getAndClearMB();
                kCandidate->setM(MB.first);
                kCandidate->setB(MB.second);
                diffim = kCandidate->returnDifferenceImage();                
            }

            _imstats.apply(diffim);
            kCandidate->setChi2(_imstats.getVariance());

            pexLogging::TTrace<4>("lsst.ip.diffim.BuildSingleKernelVisitor.processCandidate", 
                                  "Chi2 = %.2f", kCandidate->getChi2());
            pexLogging::TTrace<5>("lsst.ip.diffim.BuildSingleKernelVisitor.processCandidate",
                                  "X = %.2f Y = %.2f",
                                  kCandidate->getXCenter(), 
                                  kCandidate->getYCenter());
            pexLogging::TTrace<5>("lsst.ip.diffim.BuildSingleKernelVisitor.processCandidate",
                                  "Kernel Sum = %.3f", kCandidate->getKsum());
            pexLogging::TTrace<5>("lsst.ip.diffim.BuildSingleKernelVisitor.processCandidate",
                                  "Background = %.3f", kCandidate->getBackground());
            pexLogging::TTrace<5>("lsst.ip.diffim.BuildSingleKernelVisitor.processCandidate",
                                  "Diffim residuals = %.2f +/- %.2f sigma",
                                  _imstats.getMean(),
                                  _imstats.getRms());

            if (_imstats.getMean() > _policy.getDouble("candidateResidualMeanMax")) {
                kCandidate->setStatus(afwMath::SpatialCellCandidate::BAD);
                pexLogging::TTrace<5>("lsst.ip.diffim.BuildSingleKernelVisitor.processCandidate", 
                                      "Rejecting due to bad source kernel mean residuals : %.2f > %.2f",
                                      _imstats.getMean(),
                                      _policy.getDouble("candidateResidualMeanMax"));
            }
            else if (_imstats.getRms() > _policy.getDouble("candidateResidualStdMax")) {
                kCandidate->setStatus(afwMath::SpatialCellCandidate::BAD);
                pexLogging::TTrace<5>("lsst.ip.diffim.BuildSingleKernelVisitor.processCandidate", 
                                      "Rejecting due to bad source kernel residual rms : %.2f > %.2f",
                                      _imstats.getRms(),
                                      _policy.getDouble("candidateResidualStdMax"));
            }
            else {
                kCandidate->setStatus(afwMath::SpatialCellCandidate::GOOD);
                pexLogging::TTrace<5>("lsst.ip.diffim.BuildSingleKernelVisitor.processCandidate", 
                                      "Source kernel OK");
            }
        }
    private:
        PsfMatchingFunctor<PixelT> _kFunctor;
        lsst::pex::policy::Policy _policy;
        ImageStatistics<PixelT> _imstats;
    };
}

namespace {
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
            _nRejected(0) {}
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
            
            /* Make diffim and set chi2 from result */

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
                kernelPtr( new afwMath::FixedKernel(kImage) );
            /* </hack> */

            double background = (*_spatialBackground)(kCandidate->getXCenter(), kCandidate->getYCenter());
            
            MaskedImageT diffim = kCandidate->returnDifferenceImage(kernelPtr, background);
            _imstats.apply(diffim);
            kCandidate->setChi2(_imstats.getVariance());
            
            pexLogging::TTrace<4>("lsst.ip.diffim.AssessSpatialKernelVisitor.processCandidate", 
                                  "Chi2 = %.2f", kCandidate->getChi2());
            pexLogging::TTrace<5>("lsst.ip.diffim.AssessSpatialKernelVisitor.processCandidate",
                                  "X = %.2f Y = %.2f",
                                  kCandidate->getXCenter(), 
                                  kCandidate->getYCenter());
            pexLogging::TTrace<5>("lsst.ip.diffim.AssessSpatialKernelVisitor.processCandidate",
                                  "Kernel Sum = %.3f", kSum);
            pexLogging::TTrace<5>("lsst.ip.diffim.AssessSpatialKernelVisitor.processCandidate",
                                  "Background = %.3f", background);
            pexLogging::TTrace<5>("lsst.ip.diffim.AssessSpatialKernelVisitor.processCandidate",
                                  "Diffim residuals = %.2f +/- %.2f sigma",
                                  _imstats.getMean(),
                                  _imstats.getRms());
            
            if (_imstats.getMean() > _policy.getDouble("candidateResidualMeanMax")) {
                kCandidate->setStatus(afwMath::SpatialCellCandidate::BAD);
                pexLogging::TTrace<5>("lsst.ip.diffim.AssessSpatialKernelVisitor.processCandidate", 
                                      "Rejecting due to bad spatial kernel mean residuals : %.2f > %.2f",
                                      _imstats.getMean(),
                                      _policy.getDouble("candidateResidualMeanMax"));
                _nRejected += 1;
            }
            else if (_imstats.getRms() > _policy.getDouble("candidateResidualStdMax")) {
                kCandidate->setStatus(afwMath::SpatialCellCandidate::BAD);
                pexLogging::TTrace<5>("lsst.ip.diffim.AssessSpatialKernelVisitor.processCandidate", 
                                      "Rejecting due to bad spatial kernel residual rms : %.2f > %.2f",
                                      _imstats.getRms(),
                                      _policy.getDouble("candidateResidualStdMax"));
                _nRejected += 1;
            }
            else {
                kCandidate->setStatus(afwMath::SpatialCellCandidate::GOOD);
                pexLogging::TTrace<5>("lsst.ip.diffim.AssessSpatialKernelVisitor.processCandidate", 
                                      "Spatial kernel OK");
            }
        }

        int getNRejected() {
            return _nRejected;
        }

    private:
        afwMath::LinearCombinationKernel::Ptr _spatialKernel;
        afwMath::Kernel::SpatialFunctionPtr _spatialBackground;
        lsst::pex::policy::Policy _policy;
        ImageStatistics<PixelT> _imstats;
        int _nRejected;
    };
}

namespace {
    template<typename PixelT>
    class BuildSpatialKernelVisitor : public afwMath::CandidateVisitor {
    public:
        BuildSpatialKernelVisitor(
            PsfMatchingFunctor<PixelT> &kFunctor, ///< Basis functions used in the fit
            int const spatialKernelOrder,  ///< Order of spatial kernel variation (cf. lsst::afw::math::PolynomialFunction2)
            int const spatialBgOrder,       ///< Order of spatial bg variation (cf. lsst::afw::math::PolynomialFunction2)
            lsst::pex::policy::Policy policy
            ) :
            afwMath::CandidateVisitor(),
            _kFunctor(kFunctor),
            _M(Eigen::MatrixXd()),
            _B(Eigen::VectorXd()),
            _Soln(Eigen::VectorXd()),
            _spatialKernelOrder(spatialKernelOrder),
            _spatialBgOrder(spatialBgOrder),
            _spatialKernelFunction( new afwMath::PolynomialFunction2<double>(spatialKernelOrder) ),
            _spatialBgFunction( new afwMath::PolynomialFunction2<double>(spatialBgOrder) ),
            _nbases(kFunctor.getBasisList().size()),
            _policy(policy),
            _constantFirstTerm(false){

            /* Bookeeping terms */
            _nkt = _spatialKernelFunction->getParameters().size();
            _nbt = _spatialBgFunction->getParameters().size();

            /* Nbases + 1 term for background */
            _M.resize(_nbases*_nkt + _nbt, _nbases*_nkt + _nbt);
            _B.resize(_nbases*_nkt + _nbt);
            _M.setZero();
            _B.setZero();

            if (_policy.getBool("useAlardKernel")) {
                _constantFirstTerm = true;
            }
                
            pexLogging::TTrace<5>("lsst.ip.diffim.LinearSpatialFitVisitor", 
                                  "Initializing with size %d %d %d %d %d",
                                  _nkt, _nbt, _M.rows(), _M.cols(), _B.size());
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

            pexLogging::TTrace<3>("lsst.ip.diffim.BuildSpatialKernelVisitor.processCandidate", 
                                  "Processing candidate %d", kCandidate->getId());
            
            /* Calculate P matrices */
            /* Pure kernel terms */
            std::vector<double> paramsK = _spatialKernelFunction->getParameters();
            for (unsigned int idx = 0; idx < _nkt; idx++) { paramsK[idx] = 0.0; }
            Eigen::VectorXd Pk(_nkt);
            for (unsigned int idx = 0; idx < _nkt; idx++) {
                paramsK[idx] = 1.0;
                _spatialKernelFunction->setParameters(paramsK);
                Pk(idx) = (*_spatialKernelFunction)( kCandidate->getXCenter(), 
                                                     kCandidate->getYCenter() );
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
                Pb(idx) = (*_spatialBgFunction)( kCandidate->getXCenter(), 
                                                 kCandidate->getYCenter() );
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
            
            /* Add 'em to the M, B matrix */
            boost::shared_ptr<Eigen::MatrixXd> Q = kCandidate->getM();
            boost::shared_ptr<Eigen::VectorXd> W = kCandidate->getB();

            if (DEBUG_MATRIX) {
                std::cout << "Spatial matrix inputs" << std::endl;
                std::cout << "M " << (*Q) << std::endl;
                std::cout << "B " << (*W) << std::endl;
            }

            /* Fill in matrices */
            unsigned int m0 = _nkt*_nbases;
            for(unsigned int m1 = 0; m1 < _nbases; m1++)  {
                
                /* Kernel-kernel terms */
                for(unsigned int m2 = m1; m2 < _nbases; m2++)  {
                    if (m1 == m2) {
                        /* Diagonal kernel-kernel term; only use upper triangular part of PkPkt */
                        _M.block(m1*_nkt, m2*_nkt, _nkt, _nkt) += (*Q)(m1,m1) * PkPkt.part<Eigen::UpperTriangular>();
                    }
                    else {
                        _M.block(m1*_nkt, m2*_nkt, _nkt, _nkt) += (*Q)(m1,m2) * PkPkt;
                    }
                }
                
                /* Kernel cross terms with background */
                _M.block(m1*_nkt, m0, _nkt, _nbt) += (*Q)(m1,_nbases) * PkPbt;

                /* B vector */
                _B.segment(m1*_nkt, _nkt) += (*W)(m1) * Pk;
            } 

            /* Background-background terms only */
            _M.block(m0, m0, _nbt, _nbt) += (*Q)(_nbases,_nbases) * PbPbt.part<Eigen::UpperTriangular>();
            _B.segment(m0, _nbt)         += (*W)(_nbases) * Pb;

            if (DEBUG_MATRIX) {
                std::cout << "Spatial matrix outputs" << std::endl;
                std::cout << "_M " << _M << std::endl;
                std::cout << "_B " << _B << std::endl;
            }

        }
        
        void solveLinearEquation() {
            boost::timer t;
            t.restart();

            /* Fill in the other half of _M */
            /*
            for (int i = 0; i < _M.rows(); i++) {
                for (int j = i+1; j < _M.cols(); j++) {
                    _M(j,i) = _M(i,j);
                }
            }
            */

            _Soln = Eigen::VectorXd::Zero(_nbases*_nkt + _nbt);
            
            if (DEBUG_MATRIX) {
                std::cout << "Solving for _M:" << std::endl;
                std::cout << _M << std::endl;
                std::cout << _B << std::endl;
            }

            if (!( _M.ldlt().solve(_B, &_Soln) )) {
                pexLogging::TTrace<5>("lsst.ip.diffim.SpatialModelKernel.solveLinearEquation", 
                                      "Unable to determine kernel via Cholesky LDL^T");
                if (!( _M.llt().solve(_B, &_Soln) )) {
                    pexLogging::TTrace<5>("lsst.ip.diffim.SpatialModelKernel.solveLinearEquation", 
                                          "Unable to determine kernel via Cholesky LL^T");
                    if (!( _M.lu().solve(_B, &_Soln) )) {
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
            pexLogging::TTrace<5>("lsst.ip.diffim.SpatialModelKernel.solveLinearEquation", 
                                  "Compute time to do spatial matrix math : %.2f s", time);
        }

        
        Eigen::VectorXd getSolution() {
            return _Soln; 
        }
        
        std::pair<afwMath::LinearCombinationKernel::Ptr, afwMath::Kernel::SpatialFunctionPtr> getSpatialModel() {
            /* Bases are needed to make the kernel */
            afwMath::KernelList kernelList = _kFunctor.getBasisList();

            /* Set up kernel */
            std::vector<afwMath::Kernel::SpatialFunctionPtr> spatialFunctionList;
            for (unsigned int i = 0; i < _nbases; i++) {
                afwMath::Kernel::SpatialFunctionPtr spatialFunction(_spatialKernelFunction->copy());
                spatialFunctionList.push_back(spatialFunction);
            }
            afwMath::LinearCombinationKernel::Ptr spatialKernel(new afwMath::LinearCombinationKernel(kernelList, spatialFunctionList));
            
            /* Set up background */
            afwMath::Kernel::SpatialFunctionPtr bgFunction(_spatialBgFunction->copy());
            
            /* Set the kernel coefficients */
            std::vector<std::vector<double> > kCoeffs;
            kCoeffs.reserve(_nbases);
            for (unsigned int i = 0, idx = 0; i < _nbases; i++) {
                kCoeffs.push_back(std::vector<double>(_nkt));
                for (unsigned int j = 0; j < _nkt; j++, idx++) {
                    kCoeffs[i][j] = _Soln[idx];
                }
            }
            
            /* Set the background coefficients */
            std::vector<double> bgCoeffs(_nbt);
            for (unsigned int i = 0; i < _nbt; i++) {
                bgCoeffs[i] = _Soln[i + _nbases*_nkt];
            }
            
            spatialKernel->setSpatialParameters(kCoeffs);
            bgFunction->setParameters(bgCoeffs);
            
            return std::make_pair(spatialKernel, bgFunction);
        }

    private:
        PsfMatchingFunctor<PixelT> _kFunctor;
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
        lsst::pex::policy::Policy _policy;
        bool _constantFirstTerm;  ///< Is the first term spatially variable?
    };
}

/************************************************************************************************************/

template<typename PixelT>
std::pair<afwMath::LinearCombinationKernel::Ptr, afwMath::Kernel::SpatialFunctionPtr>
fitSpatialKernelFromCandidates(
    PsfMatchingFunctor<PixelT> &kFunctor,      ///< kFunctor used to build the kernels
    afwMath::SpatialCellSet const& psfCells,   ///< A SpatialCellSet containing PsfCandidates
    pexPolicy::Policy const& policy            ///< Policy to control the processing
                                 ) {
    int const maxSpatialIterations = policy.getInt("maxSpatialIterations");
    int const nStarPerCell         = policy.getInt("nStarPerCell");
    int const spatialKernelOrder   = policy.getInt("spatialKernelOrder");
    int const spatialBgOrder       = policy.getInt("spatialBgOrder");

    afwMath::LinearCombinationKernel::Ptr spatialKernel;
    afwMath::Kernel::SpatialFunctionPtr spatialBackground;
    
    for (int i=0; i < maxSpatialIterations; i++) {
        /* Visitor for the single kernel fit */
        BuildSingleKernelVisitor<PixelT> singleKernelFitter(kFunctor, policy);
        
        /* Visitor for the spatial kernel fit */
        BuildSpatialKernelVisitor<PixelT> spatialKernelFitter(kFunctor, spatialKernelOrder, spatialBgOrder, policy);
        
        psfCells.visitCandidates(&singleKernelFitter, nStarPerCell);
        psfCells.visitCandidates(&spatialKernelFitter, nStarPerCell);
        spatialKernelFitter.solveLinearEquation();
        
        std::pair<afwMath::LinearCombinationKernel::Ptr, 
            afwMath::Kernel::SpatialFunctionPtr> KB = spatialKernelFitter.getSpatialModel();
        
        spatialKernel     = KB.first;
        spatialBackground = KB.second;
        
        /* Visitor for the spatial kernel result */
        AssessSpatialKernelVisitor<PixelT> spatialKernelAssessor(spatialKernel, spatialBackground, policy);
        psfCells.visitCandidates(&spatialKernelAssessor, nStarPerCell);
        int nRej = spatialKernelAssessor.getNRejected();

        pexLogging::TTrace<5>("lsst.ip.diffim.fitSpatialKernelFromCandidates", 
                              "Spatial Kernel iteration %d, %d rejected", i, nRej);
        if (nRej == 0) {
            break;
        }
    }
    return std::make_pair(spatialKernel, spatialBackground);
}

/************************************************************************************************************/

template<typename PixelT>
std::pair<afwMath::LinearCombinationKernel::Ptr, std::vector<double> > createPcaBasisFromCandidates(
    afwMath::SpatialCellSet const& psfCells, ///< A SpatialCellSet containing PsfCandidates
    pexPolicy::Policy const& policy  ///< Policy to control the processing
    ) {
    typedef typename afwImage::Image<lsst::afw::math::Kernel::Pixel> ImageT;

    int const nEigenComponents   = policy.getInt("nEigenComponents");   // number of eigen components to keep; <= 0 => infty
    int const nStarPerCell       = policy.getInt("nStarPerCell");       // order of spatial variation
    int const spatialKernelOrder = policy.getInt("spatialKernelOrder"); // max no. of stars per cell; <= 0 => infty
    
    afwImage::ImagePca<ImageT> imagePca;
    SetPcaImageVisitor<PixelT> importStarVisitor(&imagePca);
    psfCells.visitCandidates(&importStarVisitor, nStarPerCell);
    imagePca.analyze();
    
    std::vector<typename ImageT::Ptr> eigenImages = imagePca.getEigenImages();
    std::vector<double> eigenValues               = imagePca.getEigenValues();
    int const nEigen = static_cast<int>(eigenValues.size());
    int const ncomp  = (nEigenComponents <= 0 || nEigen < nEigenComponents) ? nEigen : nEigenComponents;
    
    //
    // Now build our LinearCombinationKernel; build the lists of basis functions
    // and spatial variation, then assemble the Kernel
    //
    afwMath::KernelList kernelList;
    std::vector<afwMath::Kernel::SpatialFunctionPtr> spatialFunctionList;
    
    for (int i = 0; i != ncomp; ++i) {
        kernelList.push_back(afwMath::Kernel::Ptr(
                                 new afwMath::FixedKernel(afwImage::Image<afwMath::Kernel::Pixel>(*eigenImages[i], true)))
            );
        
        afwMath::Kernel::SpatialFunctionPtr spatialFunction(new afwMath::PolynomialFunction2<double>(spatialKernelOrder));
        if (i == 0) 
            spatialFunction->setParameter(0, 1.0); // the constant term = mean kernel; all others are 0
        spatialFunctionList.push_back(spatialFunction);
    }
    
    afwMath::LinearCombinationKernel::Ptr kernel(new afwMath::LinearCombinationKernel(kernelList, spatialFunctionList));
    return std::make_pair(kernel, eigenValues);
}

/************************************************************************************************************/
//
// Explicit instantiations
//
/// \cond
    typedef float PixelT;
    template class KernelCandidate<PixelT>;

    template
    std::pair<lsst::afw::math::LinearCombinationKernel::Ptr, std::vector<double> >
    createPcaBasisFromCandidates<PixelT>(lsst::afw::math::SpatialCellSet const&,
                                         lsst::pex::policy::Policy const&);

    template
    std::pair<afwMath::LinearCombinationKernel::Ptr, afwMath::Kernel::SpatialFunctionPtr>
    fitSpatialKernelFromCandidates<PixelT>(PsfMatchingFunctor<PixelT> &,
                                           lsst::afw::math::SpatialCellSet const&,
                                           lsst::pex::policy::Policy const&);
    
/// \endcond

}}} // end of namespace lsst::ip::diffim

