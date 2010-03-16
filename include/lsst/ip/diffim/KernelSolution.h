// -*- lsst-c++ -*-
/**
 * @file KernelSolution.h
 *
 * @brief Declaration of classes to store the solution for convolution kernels
 *
 * @author Andrew Becker, University of Washington
 *
 * @ingroup ip_diffim
 */

#ifndef LSST_IP_DIFFIM_KERNELSOLUTION_H
#define LSST_IP_DIFFIM_KERNELSOLUTION_H

#include "boost/shared_ptr.hpp"
#include "Eigen/Core"

#include "lsst/afw/math.h"
#include "lsst/afw/image.h"

namespace lsst { 
namespace ip { 
namespace diffim {

    /* 
     * @brief Method used to solve for M and B
     */
    enum KernelSolvedBy {
        NONE          = 0,
        CHOLESKY_LDLT = 1,
        CHOLESKY_LLT  = 2,
        LU            = 3,
        EIGENVECTOR   = 4
    };

    class KernelSolution {
    public:
        typedef boost::shared_ptr<KernelSolution> Ptr;
        explicit KernelSolution(boost::shared_ptr<Eigen::MatrixXd> mMat,
                                boost::shared_ptr<Eigen::VectorXd> bVec);
        virtual ~KernelSolution() {};

        void solve(bool calculateErrors=false);
        inline boost::shared_ptr<Eigen::MatrixXd> getM() {return _mMat;}
        inline boost::shared_ptr<Eigen::VectorXd> getB() {return _bVec;}

    private:
        boost::shared_ptr<Eigen::MatrixXd> _mMat;               ///< Derived least squares M matrix
        boost::shared_ptr<Eigen::VectorXd> _bVec;               ///< Derived least squares B vector
        boost::shared_ptr<Eigen::VectorXd> _sVec;               ///< Derived least squares solution matrix
        KernelSolvedBy _solvedBy;                               ///< Type of algorithm used to make solution
    };

    class StaticKernelSolution : public KernelSolution {
    public:
        typedef boost::shared_ptr<StaticKernelSolution> Ptr;

        StaticKernelSolution(boost::shared_ptr<Eigen::MatrixXd> mMat,
                             boost::shared_ptr<Eigen::VectorXd> bVec,
                             boost::shared_ptr<lsst::afw::math::KernelList> const& basisList);
        virtual ~StaticKernelSolution() {};

        lsst::afw::math::Kernel::Ptr getKernel();
        double getBackground();
        double getKsum();

        std::pair<boost::shared_ptr<lsst::afw::math::Kernel>, double> getKernelSolution();
        std::pair<boost::shared_ptr<lsst::afw::math::Kernel>, double> getKernelUncertainty();
    private:
        boost::shared_ptr<lsst::afw::math::KernelList> _basisList;   ///< List of Basis Kernels

        lsst::afw::math::Kernel::Ptr _kernel;                   ///< Derived single-object convolution kernel
        double _background;                                     ///< Derived differential background estimate
        double _kSum;                                           ///< Derived kernel sum

        lsst::afw::math::Kernel::Ptr _kernelErr;                ///< Uncertainty on the kernel values
        double _backgroundErr;                                  ///< Uncertainty on the background values
        bool _errCalculated;                                    ///< Has the uncertainty been calculated?
    };

    class SpatialKernelSolution : public KernelSolution {
    public:
        typedef boost::shared_ptr<SpatialKernelSolution> Ptr;

        SpatialKernelSolution(boost::shared_ptr<Eigen::MatrixXd> mMat,
                              boost::shared_ptr<Eigen::VectorXd> bVec,
                              boost::shared_ptr<lsst::afw::math::KernelList> const& basisList,
                              lsst::afw::math::Kernel::SpatialFunctionPtr spatialKernelFunction,
                              lsst::afw::math::Kernel::SpatialFunctionPtr spatialBgFunction,
                              bool constantFirstTerm);

        virtual ~SpatialKernelSolution() {};

        std::pair<lsst::afw::math::LinearCombinationKernel::Ptr,
                  lsst::afw::math::Kernel::SpatialFunctionPtr> getKernelSolution();
        std::pair<lsst::afw::math::LinearCombinationKernel::Ptr,
                  lsst::afw::math::Kernel::SpatialFunctionPtr> getKernelUncertainty();
    private:
        boost::shared_ptr<lsst::afw::math::KernelList> _basisList;          ///< List of Basis Kernels
        lsst::afw::math::Kernel::SpatialFunctionPtr _spatialKernelFunction; ///< Spatial function for Kernel
        lsst::afw::math::Kernel::SpatialFunctionPtr _spatialBgFunction;     ///< Spatial function for Bg
        bool _constantFirstTerm;                                            ///< Is the first term constant

        lsst::afw::math::LinearCombinationKernel::Ptr _kernel;   ///< Spatial convolution kernel
        lsst::afw::math::Kernel::SpatialFunctionPtr _background; ///< Spatial background model

        lsst::afw::math::LinearCombinationKernel::Ptr _kernelErr;   ///< Kernel uncertainty
        lsst::afw::math::Kernel::SpatialFunctionPtr _backgroundErr; ///< Background uncertainty
        bool _errCalculated;                                        ///< Has the uncertainty been calculated?
    };

}}} // end of namespace lsst::ip::diffim

#endif
