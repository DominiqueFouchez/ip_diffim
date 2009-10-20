// -*- lsst-c++ -*-
/**
 * @file SpatialModelKernel.h
 *
 * @brief Class used by SpatialModelCell for spatial Kernel fitting
 *
 * @author Andrew Becker, University of Washington
 *
 * @ingroup ip_diffim
 */

#ifndef LSST_IP_DIFFIM_SPATIALMODELKERNEL_H
#define LSST_IP_DIFFIM_SPATIALMODELKERNEL_H

#include <boost/shared_ptr.hpp>
#include <Eigen/Core>

#include <lsst/afw/math/SpatialCell.h>
#include <lsst/afw/math/Kernel.h>
#include <lsst/afw/math/KernelFunctions.h>
#include <lsst/afw/math/Statistics.h>
#include <lsst/pex/policy/Policy.h>
#include <lsst/afw/detection/Footprint.h>
#include <lsst/sdqa/SdqaRating.h>

#include <lsst/ip/diffim/ImageSubtract.h>
#include <lsst/ip/diffim/PsfMatchingFunctor.h>

namespace lsst { namespace ip { namespace diffim {

    /** 
     * @brief Class stored in SpatialCells for spatial Kernel fitting
     * 
     * @note KernelCandidate is a single Kernel derived around a source.  We'll assign
     * them to sets of SpatialCells; these sets will then be used to fit a
     * spatial model to the Kernel.
     *
     * @ingroup ip_diffim
     */    
    template <typename _PixelT>
    class KernelCandidate : public lsst::afw::math::SpatialCellImageCandidate<
        lsst::afw::image::Image<lsst::afw::math::Kernel::Pixel> 
        > {
    public: 
        typedef lsst::afw::image::Image<lsst::afw::math::Kernel::Pixel> ImageT;
        typedef _PixelT PixelT;         // _after_ using lsst::afw::math::Kernel::Pixel

    private:
        using lsst::afw::math::SpatialCellImageCandidate<ImageT>::_image;

    public:
        typedef boost::shared_ptr<KernelCandidate> Ptr;
        typedef boost::shared_ptr<lsst::afw::image::MaskedImage<PixelT> > MaskedImagePtr;

        /**
	 * @brief Constructor
         *
         * @param xCenter Col position of object
         * @param yCenter Row position of object
         * @param miToConvolvePtr  Pointer to template image
         * @param miToNotConvolvePtr  Pointer to science image
         */
        KernelCandidate(float const xCenter,
                        float const yCenter, 
                        MaskedImagePtr const& miToConvolvePtr,
                        MaskedImagePtr const& miToNotConvolvePtr) :
            lsst::afw::math::SpatialCellImageCandidate<ImageT>(xCenter, yCenter),
            _miToConvolvePtr(miToConvolvePtr),
            _miToNotConvolvePtr(miToNotConvolvePtr),
            _templateFlux(),
            _kernel(),
            _kSum(0.),
            _background(0.),
            _M(),
            _B(),
            _haveKernel(false) {

            /* Rank by brightness in template */
            lsst::afw::math::Statistics stats = lsst::afw::math::makeStatistics(*_miToConvolvePtr, 
                                                                                lsst::afw::math::SUM);             
            _templateFlux = stats.getValue(lsst::afw::math::SUM);
        }
        
        /// Destructor
        ~KernelCandidate() {};

        /**
         * Return Cell rating
         * 
         * @note Required method for use by SpatialCell; e.g. total flux
         */
        double getCandidateRating() const { return _templateFlux; }

        MaskedImagePtr getMiToConvolvePtr() {return _miToConvolvePtr;}
        MaskedImagePtr getMiToNotConvolvePtr() {return _miToNotConvolvePtr;}

        /** 
         * @brief Calculate associated difference image using member _kernel/_background
	 */
        lsst::afw::image::MaskedImage<PixelT> returnDifferenceImage();
	
        /** 
         * @brief Calculate associated difference image using provided kernel and background
	 */
        lsst::afw::image::MaskedImage<PixelT> returnDifferenceImage(
            lsst::afw::math::Kernel::Ptr kernel,
            double background
            );


        typename ImageT::ConstPtr getImage() const;
        typename ImageT::Ptr copyImage() const;
        double getKsum() const;
        lsst::afw::math::Kernel::Ptr getKernel() const;
        double getBackground() const;
        boost::shared_ptr<Eigen::MatrixXd> const getM()  {return _M;}
        boost::shared_ptr<Eigen::VectorXd> const getB()  {return _B;}
        bool hasKernel() {return _haveKernel;}
        
        void setKernel(lsst::afw::math::Kernel::Ptr kernel);
        void setBackground(double background) {_background = background;}

        void setM(boost::shared_ptr<Eigen::MatrixXd> M) {_M = M;}
        void setB(boost::shared_ptr<Eigen::VectorXd> B) {_B = B;}

    private:
        MaskedImagePtr _miToConvolvePtr;                    ///< Subimage around which you build kernel
        MaskedImagePtr _miToNotConvolvePtr;                 ///< Subimage around which you build kernel
        double _templateFlux;                               ///< Brightness in the template image; used to rank candidates

        lsst::afw::math::Kernel::Ptr _kernel;               ///< Derived single-object convolution kernel
        double _kSum;                                       ///< Derived kernel sum
        double _background;                                 ///< Derived differential background estimate

        boost::shared_ptr<Eigen::MatrixXd> _M;              ///< Derived least squares matrix
        boost::shared_ptr<Eigen::VectorXd> _B;              ///< Derived least squares vector

        bool mutable _haveKernel;                           ///< do we have a Kernel to return?
    };

    /**
     * @brief Return a KernelCandidate pointer of the right sort
     *
     * @param xCenter  X-center of candidate
     * @param yCenter  Y-center of candidate
     * @param miToConvolvePtr  Template subimage 
     * @param miToNotConvolvePtr  Science image subimage
     *
     * @ingroup ip_diffim
     */
    template <typename PixelT>
    typename KernelCandidate<PixelT>::Ptr
    makeKernelCandidate(float const xCenter,
                        float const yCenter, 
                        boost::shared_ptr<lsst::afw::image::MaskedImage<PixelT> > const& miToConvolvePtr,
                        boost::shared_ptr<lsst::afw::image::MaskedImage<PixelT> > const& miToNotConvolvePtr) {
        
        return typename KernelCandidate<PixelT>::Ptr(new KernelCandidate<PixelT>(xCenter, yCenter,
                                                                                 miToConvolvePtr,
                                                                                 miToNotConvolvePtr));
    }

    /**
     * @brief Fit for a spatial model of the kernel and background
     *
     * @param kFunctor  Functor used for the single-kernel Psf matching
     * @param kernelCells  SpatialCellSet containing the candidate kernels
     * @param policy  Policy for configuration
     *
     * @ingroup ip_diffim
     */
    template<typename PixelT>
    std::pair<lsst::afw::math::LinearCombinationKernel::Ptr, lsst::afw::math::Kernel::SpatialFunctionPtr>
    fitSpatialKernelFromCandidates(
        PsfMatchingFunctor<PixelT> &kFunctor,
        lsst::afw::math::SpatialCellSet const& kernelCells,
        lsst::pex::policy::Policy const& policy);
    
    
}}}

#endif

