// -*- lsst-c++ -*-
/**
 * @file
 *
 * @brief Implementation of image subtraction functions declared in ImageSubtract.h
 *
 * @author Andrew Becker, University of Washington
 *
 * @ingroup ip_diffim
 */
#include <iostream>
#include <limits>
#include <boost/timer.hpp> 

#include <Eigen/Cholesky>
#include <Eigen/Core>
#include <Eigen/LU>
#include <Eigen/QR>

// NOTE -  trace statements >= 6 can ENTIRELY kill the run time
// #define LSST_MAX_TRACE 5

#include <lsst/ip/diffim/ImageSubtract.h>
#include <lsst/afw/image.h>
#include <lsst/afw/math.h>
#include <lsst/pex/exceptions/Exception.h>
#include <lsst/pex/logging/Trace.h>
#include <lsst/pex/logging/Log.h>
#include <lsst/afw/detection/Footprint.h>
#include <lsst/afw/math/ConvolveImage.h>

#define DEBUG_MATRIX 0

namespace exceptions = lsst::pex::exceptions; 
namespace logging    = lsst::pex::logging; 
namespace image      = lsst::afw::image;
namespace math       = lsst::afw::math;
namespace detection  = lsst::afw::detection;
namespace diffim     = lsst::ip::diffim;

//
// Constructors
//
template <typename PixelT, typename VarT>
diffim::PsfMatchingFunctor<PixelT, VarT>::PsfMatchingFunctor(
    lsst::afw::math::KernelList const &basisList
    ) :
    _basisList(basisList),
    _M(),
    _B(),
    _Soln(),
    _H(),
    _initialized(false),
    _regularize(false)
{;}

template <typename PixelT, typename VarT>
diffim::PsfMatchingFunctor<PixelT, VarT>::PsfMatchingFunctor(
    lsst::afw::math::KernelList const &basisList,
    boost::shared_ptr<Eigen::MatrixXd> const &H
    ) :
    _basisList(basisList),
    _M(),
    _B(),
    _Soln(),
    _H(H),
    _initialized(false),
    _regularize(true)
{;}

template <typename PixelT, typename VarT>
diffim::PsfMatchingFunctor<PixelT, VarT>::PsfMatchingFunctor(
    const PsfMatchingFunctor<PixelT,VarT> &rhs
    ) :
    _basisList(rhs._basisList),
    _M(),
    _B(),
    _Soln(),
    _H(rhs._H),
    _initialized(false),
    _regularize(rhs._regularize)
{;}

//
// Public Member Functions
//

/** Create PSF matching kernel
 */
template <typename PixelT>
Eigen::MatrixXd diffim::imageToEigenMatrix(
    lsst::afw::image::Image<PixelT> const &img
    ) {
    unsigned int rows = img.getHeight();
    unsigned int cols = img.getWidth();
    Eigen::MatrixXd M = Eigen::MatrixXd::Zero(rows, cols);
    for (int y = 0; y != img.getHeight(); ++y) {
        int x = 0;
        for (typename lsst::afw::image::Image<PixelT>::x_iterator ptr = img.row_begin(y); ptr != img.row_end(y); ++ptr, ++x) {
            // M is addressed row, col
            M(y,x) = *ptr;
        }
    }
    return M;
}
    

template <typename PixelT, typename VarT>
void diffim::PsfMatchingFunctor<PixelT, VarT>::apply(
    lsst::afw::image::Image<PixelT> const &imageToConvolve,    ///< Image to apply kernel to
    lsst::afw::image::Image<PixelT> const &imageToNotConvolve, ///< Image whose PSF you want to match to
    lsst::afw::image::Image<VarT>   const &varianceEstimate,   ///< Estimate of the variance per pixel
    lsst::pex::policy::Policy       const &policy              ///< Policy file
    ) {
    
    unsigned int const nKernelParameters     = _basisList.size();
    unsigned int const nBackgroundParameters = 1;
    unsigned int const nParameters           = nKernelParameters + nBackgroundParameters;
    std::vector<boost::shared_ptr<math::Kernel> >::const_iterator kiter = _basisList.begin();
    
    // Ignore buffers around edge of convolved images :
    //
    // If the kernel has width 5, it has center pixel 2.  The first good pixel
    // is the (5-2)=3rd pixel, which is array index 2, and ends up being the
    // index of the central pixel.
    //
    // You also have a buffer of unusable pixels on the other side, numbered
    // width-center-1.  The last good usable pixel is N-width+center+1.

    // Example : the kernel is width = 5, center = 2
    //
    //     ---|---|-c-|---|---|
    //          
    //           the image is width = N
    //           convolve this with the kernel, and you get
    //
    //    |-x-|-x-|-g-|---|---| ... |---|---|-g-|-x-|-x-|
    //
    //           g = first/last good pixel
    //           x = bad
    // 
    //           the first good pixel is the array index that has the value "center", 2
    //           the last good pixel has array index N-(5-2)+1
    //           eg. if N = 100, you want to use up to index 97
    //               100-3+1 = 98, and the loops use i < 98, meaning the last
    //               index you address is 97.
    unsigned int const startCol = (*kiter)->getCtrX();
    unsigned int const startRow = (*kiter)->getCtrY();
    unsigned int const endCol   = imageToConvolve.getWidth()  - ((*kiter)->getWidth()  - (*kiter)->getCtrX()) + 1;
    unsigned int const endRow   = imageToConvolve.getHeight() - ((*kiter)->getHeight() - (*kiter)->getCtrY()) + 1;
    
    boost::timer t;
    t.restart();
    
    /* Least squares matrices */
    Eigen::MatrixXd M = Eigen::MatrixXd::Zero(nParameters, nParameters);
    Eigen::VectorXd B = Eigen::VectorXd::Zero(nParameters);
    /* Eigen representation of input images; only the pixels that are unconvolved in cimage below */
    Eigen::MatrixXd eigenToConvolve    = diffim::imageToEigenMatrix(imageToConvolve).block(startRow, startCol, 
                                                                                           endRow-startRow, endCol-startCol);
    Eigen::MatrixXd eigenToNotConvolve = diffim::imageToEigenMatrix(imageToNotConvolve).block(startRow, startCol, 
                                                                                              endRow-startRow, endCol-startCol);
    Eigen::MatrixXd eigeniVariance     = diffim::imageToEigenMatrix(varianceEstimate).block(startRow, startCol, 
                                                                                            endRow-startRow, endCol-startCol).cwise().inverse();
    /* Resize into 1-D for later usage */
    eigenToConvolve.resize(eigenToConvolve.rows()*eigenToConvolve.cols(), 1);
    eigenToNotConvolve.resize(eigenToNotConvolve.rows()*eigenToNotConvolve.cols(), 1);
    eigeniVariance.resize(eigeniVariance.rows()*eigeniVariance.cols(), 1);

    /* Holds image convolved with basis function */
    image::Image<PixelT> cimage(imageToConvolve.getDimensions());
    
    /* Holds eigen representation of image convolved with all basis functions */
    std::vector<boost::shared_ptr<Eigen::MatrixXd> > convolvedEigenList(nKernelParameters);
    
    /* Iterators over convolved image list and basis list */
    typename std::vector<boost::shared_ptr<Eigen::MatrixXd> >::iterator eiter = convolvedEigenList.begin();
    /* Create C_i in the formalism of Alard & Lupton */
    for (; kiter != _basisList.end(); ++kiter, ++eiter) {
        math::convolve(cimage, imageToConvolve, **kiter, false); /* cimage stores convolved image */
	boost::shared_ptr<Eigen::MatrixXd> cmat (
            new Eigen::MatrixXd(diffim::imageToEigenMatrix(cimage).block(startRow, startCol, endRow-startRow, endCol-startCol))
            );
	cmat->resize(cmat->rows()*cmat->cols(), 1);
	*eiter = cmat;
    } 
    
    double time = t.elapsed();
    logging::TTrace<5>("lsst.ip.diffim.PsfMatchingFunctor.apply", 
                       "Total compute time to do basis convolutions : %.2f s", time);
    t.restart();
    
    /* 
     * 
     * NOTE - 
     * 
     * Below is the original Eigen representation of the matrix math needed.
     * Its a bit more readable but 5-10% slower than the as-implemented Eigen
     * math.  Left here for reference as it nicely and simply outlines the math
     * that goes into the construction of M and B.
     * 

    typename std::vector<boost::shared_ptr<Eigen::VectorXd> >::iterator eiteri = convolvedEigenList.begin();
    typename std::vector<boost::shared_ptr<Eigen::VectorXd> >::iterator eiterE = convolvedEigenList.end();
    for (unsigned int kidxi = 0; eiteri != eiterE; eiteri++, kidxi++) {
        Eigen::VectorXd eiteriDotiVariance = (*eiteri)->cwise() * eigeniVarianceV;

        typename std::vector<boost::shared_ptr<Eigen::VectorXd> >::iterator eiterj = eiteri;
        for (unsigned int kidxj = kidxi; eiterj != eiterE; eiterj++, kidxj++) {
            M(kidxi, kidxj) = (eiteriDotiVariance.cwise() * (**eiterj)).sum();
            M(kidxj, kidxi) = M(kidxi, kidxj);
        }
	B(kidxi)                 = (eiteriDotiVariance.cwise() * eigenToNotConvolveV).sum();
	M(kidxi, nParameters-1)  = eiteriDotiVariance.sum();
	M(nParameters-1, kidxi)  = M(kidxi, nParameters-1);
    }
    B(nParameters-1)                = (eigenToNotConvolveV.cwise() * eigeniVarianceV).sum();
    M(nParameters-1, nParameters-1) = eigeniVarianceV.sum();

    */
    
    /* 
       Load matrix with all values from convolvedEigenList : all images
       (eigeniVariance, convolvedEigenList) must be the same size
    */
    Eigen::MatrixXd C(eigeniVariance.col(0).size(), nParameters);
    typename std::vector<boost::shared_ptr<Eigen::MatrixXd> >::iterator eiterj = convolvedEigenList.begin();
    typename std::vector<boost::shared_ptr<Eigen::MatrixXd> >::iterator eiterE = convolvedEigenList.end();
    for (unsigned int kidxj = 0; eiterj != eiterE; eiterj++, kidxj++) {
        C.col(kidxj) = (*eiterj)->col(0);
    }
    /* Treat the last "image" as all 1's to do the background calculation. */
    C.col(nParameters-1).fill(1.);
    
    /* Caculate the variance-weighted pixel values */
    Eigen::MatrixXd VC = eigeniVariance.col(0).asDiagonal() * C;
    
    /* Calculate M as the variance-weighted inner product of C */
    M = C.transpose() * VC;
    B = VC.transpose() * eigenToNotConvolve.col(0);

    if (DEBUG_MATRIX) {
        std::cout << "M " << std::endl;
        std::cout << M << std::endl;
        std::cout << "B " << std::endl;
        std::cout << B << std::endl;
    }

    time = t.elapsed();
    logging::TTrace<5>("lsst.ip.diffim.PsfMatchingFunctor.apply", 
                       "Total compute time to step through pixels : %.2f s", time);
    t.restart();

    /* If the regularization matrix is here and not null, we use it by default */
    if (_regularize) {
        double regularizationScaling = policy.getDouble("regularizationScaling");        
        /* 
           See N.R. 18.5 equation 18.5.8 for the solution to the regularized
           normal equations.  For M.x = b, and solving for x, 

           M -> (Mt.M + lambda*H)
           B -> (Mt.B)

           An estimate of lambda is NR 18.5.16

           lambda = Trace(Mt.M) / Tr(H)

         */

        Eigen::MatrixXd Mt = M.transpose();
        M = Mt * M;

        double lambda = M.trace() / _H->trace();
        lambda *= regularizationScaling;

        M = M + lambda * *_H;
        B = Mt * B;
        logging::TTrace<5>("lsst.ip.diffim.PsfMatchingFunctor.apply", 
                           "Applying kernel regularization with lambda = %.2e", lambda);
        
    }
    

    // To use Cholesky decomposition, the matrix needs to be symmetric (M is, by
    // design) and positive definite.  
    //
    // Eventually put a check in here to make sure its positive definite
    //
    Eigen::VectorXd Soln = Eigen::VectorXd::Zero(nParameters);;
    if (!(M.ldlt().solve(B, &Soln))) {
        logging::TTrace<5>("lsst.ip.diffim.PsfMatchingFunctor.apply", 
                           "Unable to determine kernel via Cholesky LDL^T");
        if (!(M.llt().solve(B, &Soln))) {
            logging::TTrace<5>("lsst.ip.diffim.PsfMatchingFunctor.apply", 
                               "Unable to determine kernel via Cholesky LL^T");
            if (!(M.lu().solve(B, &Soln))) {
                logging::TTrace<5>("lsst.ip.diffim.PsfMatchingFunctor.apply", 
                                   "Unable to determine kernel via LU");
                // LAST RESORT
                try {
                    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> eVecValues(M);
                    Eigen::MatrixXd const& R = eVecValues.eigenvectors();
                    Eigen::VectorXd eValues  = eVecValues.eigenvalues();
                    
                    for (int i = 0; i != eValues.rows(); ++i) {
                        if (eValues(i) != 0.0) {
                            eValues(i) = 1.0/eValues(i);
                        }
                    }
                    
                    Soln = R*eValues.asDiagonal()*R.transpose()*B;
                } catch (exceptions::Exception& e) {
                    logging::TTrace<5>("lsst.ip.diffim.PsfMatchingFunctor.apply", 
                                       "Unable to determine kernel via eigen-values");
                    
                    throw LSST_EXCEPT(exceptions::Exception, "Unable to determine kernel solution in PsfMatchingFunctor::apply");
                }
            }
        }
    }

    /* Save matrices as they are expensive to calculate.
     * 
     * NOTE : one might consider saving the VC and B vectors instead of M and B;
     * however then we would not be able to maintain the regularization of M
     * even though the stored B would be regularized.
     * 
     * ANOTHER NOTE : we might also consider *not* solving for Soln here, in the
     * case that we don't care about the results of the single-kernel fit.  That
     * is, if we decide to only do sigma clipping on the spatial results.
     * 
     */
    _M    = boost::shared_ptr<Eigen::MatrixXd>(new Eigen::MatrixXd(M));
    _B    = boost::shared_ptr<Eigen::VectorXd>(new Eigen::VectorXd(B));
    _Soln = boost::shared_ptr<Eigen::VectorXd>(new Eigen::VectorXd(Soln));
    _initialized = true;
    time = t.elapsed();
    logging::TTrace<5>("lsst.ip.diffim.PsfMatchingFunctor.apply", 
                       "Total compute time to do matrix math : %.2f s", time);
    
}

template <typename PixelT, typename VarT>
std::pair<boost::shared_ptr<lsst::afw::math::Kernel>, double>
diffim::PsfMatchingFunctor<PixelT, VarT>::getKernel() {

    if (!(_initialized)) {
        throw LSST_EXCEPT(exceptions::Exception, "Kernel not initialized");
    }

    unsigned int const nKernelParameters     = _basisList.size();
    unsigned int const nBackgroundParameters = 1;
    unsigned int const nParameters           = nKernelParameters + nBackgroundParameters;

    /* Fill in the kernel results */
    std::vector<double> kValues(nKernelParameters);
    for (unsigned int idx = 0; idx < nKernelParameters; idx++) {
        if (std::isnan((*_Soln)(idx))) {
            throw LSST_EXCEPT(exceptions::Exception, 
                              str(boost::format("Unable to determine kernel solution %d (nan)") % idx));
        }
        kValues[idx] = (*_Soln)(idx);
    }
    boost::shared_ptr<lsst::afw::math::Kernel> kernel( 
        new math::LinearCombinationKernel(_basisList, kValues) 
        );
    
    if (std::isnan((*_Soln)(nParameters-1))) {
        throw LSST_EXCEPT(exceptions::Exception, 
                          str(boost::format("Unable to determine background solution %d (nan)") % (nParameters-1)));
    }
    double background = (*_Soln)(nParameters-1);

    return std::make_pair(kernel, background);
}

template <typename PixelT, typename VarT>
std::pair<boost::shared_ptr<lsst::afw::math::Kernel>, double>
diffim::PsfMatchingFunctor<PixelT, VarT>::getKernelUncertainty() {

    if (!(_initialized)) {
        throw LSST_EXCEPT(exceptions::Exception, "Kernel not initialized");
    }

    unsigned int const nKernelParameters     = _basisList.size();
    unsigned int const nBackgroundParameters = 1;
    unsigned int const nParameters           = nKernelParameters + nBackgroundParameters;

    // Estimate of parameter uncertainties comes from the inverse of the
    // covariance matrix (noise spectrum).  
    // N.R. 15.4.8 to 15.4.15
    // 
    // Since this is a linear problem no need to use Fisher matrix
    // N.R. 15.5.8
    
    // Although I might be able to take advantage of the solution above.
    // Since this now works and is not the rate limiting step, keep as-is for DC3a.
    
    // Use Cholesky decomposition again.
    // Cholkesy:
    // Cov       =  L L^t
    // Cov^(-1)  = (L L^t)^(-1)
    //           = (L^T)^-1 L^(-1)
    Eigen::MatrixXd             Cov    = (*_M).transpose() * (*_M);
    Eigen::LLT<Eigen::MatrixXd> llt    = Cov.llt();
    Eigen::MatrixXd             Error2 = llt.matrixL().transpose().inverse() * llt.matrixL().inverse();
        
    std::vector<double> kErrValues(nKernelParameters);
    for (unsigned int idx = 0; idx < nKernelParameters; idx++) {
        // Insanity checking
        if (std::isnan(Error2(idx, idx))) {
            throw LSST_EXCEPT(exceptions::Exception, 
                              str(boost::format("Unable to determine kernel uncertainty %d (nan)") % idx));
        }
        if (Error2(idx, idx) < 0.0) {
            throw LSST_EXCEPT(exceptions::Exception,
                              str(boost::format("Unable to determine kernel uncertainty, negative variance %d (%.3e)") % 
                                  idx % Error2(idx, idx)));
        }
        kErrValues[idx] = sqrt(Error2(idx, idx));
    }
    boost::shared_ptr<lsst::afw::math::Kernel> kernelErr( 
        new math::LinearCombinationKernel(_basisList, kErrValues) 
        );
 
    // Estimate of Background and Background Error */
    if (std::isnan(Error2(nParameters-1, nParameters-1))) {
        throw LSST_EXCEPT(exceptions::Exception, "Unable to determine background uncertainty (nan)");
    }
    if (Error2(nParameters-1, nParameters-1) < 0.0) {
        throw LSST_EXCEPT(exceptions::Exception, 
                          str(boost::format("Unable to determine background uncertainty, negative variance (%.3e)") % 
                              Error2(nParameters-1, nParameters-1) 
                              ));
    }
    double backgroundErr = sqrt(Error2(nParameters-1, nParameters-1));

    return std::make_pair(kernelErr, backgroundErr);
}

template <typename PixelT, typename VarT>
std::pair<boost::shared_ptr<Eigen::MatrixXd>, boost::shared_ptr<Eigen::VectorXd> >
diffim::PsfMatchingFunctor<PixelT, VarT>::getAndClearMB() {

    boost::shared_ptr<Eigen::MatrixXd> Mout = _M;
    boost::shared_ptr<Eigen::VectorXd> Bout = _B;
    _M.reset();
    _B.reset();
    _Soln.reset();
    _initialized=false;
    return std::make_pair(Mout, Bout);
}
        
    


//
// Subroutines
//

/** 
 * @brief Generate a basis set of delta function Kernels.
 *
 * Generates a vector of Kernels sized nCols * nRows, where each Kernel has
 * a unique pixel set to value 1.0 with the other pixels valued 0.0.  This
 * is the "delta function" basis set.
 * 
 * @return Vector of orthonormal delta function Kernels.
 *
 * @throw lsst::pex::exceptions::DomainError if nRows or nCols not positive
 *
 * @ingroup diffim
 */
math::KernelList
diffim::generateDeltaFunctionBasisSet(
    unsigned int width,                 ///< number of columns in the set
    unsigned int height                 ///< number of rows in the set
    ) {
    if ((width < 1) || (height < 1)) {
        throw LSST_EXCEPT(exceptions::Exception, "nRows and nCols must be positive");
    }
    const int signedWidth = static_cast<int>(width);
    const int signedHeight = static_cast<int>(height);
    math::KernelList kernelBasisList;
    for (int row = 0; row < signedHeight; ++row) {
        for (int col = 0; col < signedWidth; ++col) {
            boost::shared_ptr<math::Kernel> 
                kernelPtr(new math::DeltaFunctionKernel(width, height, image::PointI(col,row)));
            kernelBasisList.push_back(kernelPtr);
        }
    }
    return kernelBasisList;
}

boost::shared_ptr<Eigen::MatrixXd>
diffim::generateFiniteDifferenceRegularization(
    unsigned int width,
    unsigned int height,
    unsigned int order,
    unsigned int boundary_style,  // 0 = unwrapped, 1 = wrapped, 2 = order-tappered ('order' is highest used)
    unsigned int difference_style, // 0 = forward, 1 = central
    bool printB // a debug flag ... remove when done.
					    ) {

    if ((order < 0) || (order > 2)) throw LSST_EXCEPT(exceptions::Exception, "Only orders 0..2 allowed");
    if ((width < 0))  throw LSST_EXCEPT(exceptions::Exception, "Width < 0");
    if ((height < 0)) throw LSST_EXCEPT(exceptions::Exception, "Height < 0");

    if ((boundary_style < 0) || (boundary_style > 2)) { 
	throw LSST_EXCEPT(exceptions::Exception, "Boundary styles 0..2 defined");
    }
    if ((difference_style < 0) || (difference_style > 1)) {
	throw LSST_EXCEPT(exceptions::Exception, "Only forward (0), and central (1) difference styles defined.");
    }

    /* what works, and what doesn't */
    // == good job == 
    // - order 0, wrapped, forward
    // - order 1, wrapped or tapered, central or forward
    // - order 2, wrapped or tapered, forward
    // == bad job (usually diagongal stripes) ==
    // - all others


    /* 
       Instead of Taylor expanding the forward difference approximation of
       derivatives (N.R. section 18.5) lets just hard code in the expansion of
       the 1st through 3rd derivatives, which will try and enforce smoothness of
       0 through 2nd derivatives.

       A property of the basic "finite difference regularization" is that their
       rows (column multipliers) sum to 0.

       Another consideration is to use *multiple* finite difference operators as
       a constraint.

    */


    // ===========================================================================
    // Get the coeffs for the finite differencing
    // note: The coeffs are stored 2D although they are essentially 1D entities.
    //       The 2d design was chosen to allow cross-terms to be included,
    //         though they are not yet implemented.
    //
    std::vector<std::vector<std::vector<float> > > 
	coeffs(3, std::vector<std::vector<float> >(5, std::vector<float>(5,0)));
    unsigned int x_cen = 0,  y_cen = 0;  // center of reqested order coeffs
    unsigned int x_cen1 = 0, y_cen1 = 0; // center of order 1 coeffs
    unsigned int x_cen2 = 0, y_cen2 = 0; // center of order 2 coeffs
    unsigned int x_size = 0, y_size = 0;

    // forward difference coefficients
    if (difference_style == 0) {
	
	y_cen  = x_cen  = 0;
	x_cen1 = y_cen1 = 0;
	x_cen2 = y_cen2 = 0;

	x_size = y_size = order + 2;

	// default forward difference suggested in NR chap 18
	// 0th order
	coeffs[0][0][0] = -2; coeffs[0][0][1] = 1; 
	coeffs[0][1][0] = 1;  coeffs[0][1][1] = 0;

	// 1st 2
	coeffs[1][0][0] = -2; coeffs[1][0][1] = 2;  coeffs[1][0][2] = -1; 
	coeffs[1][1][0] = 2;  coeffs[1][1][1] = 0;  coeffs[1][1][2] =  0; 
	coeffs[1][2][0] = -1; coeffs[1][2][1] = 0;  coeffs[1][2][2] =  0; 

	// 2nd 2
	coeffs[2][0][0] = -2; coeffs[2][0][1] = 3;  coeffs[2][0][2] = -3; coeffs[2][0][3] = 1; 
	coeffs[2][1][0] = 3;  coeffs[2][1][1] = 0;  coeffs[2][1][2] =  0; coeffs[2][1][3] = 0; 
	coeffs[2][2][0] = -3; coeffs[2][2][1] = 0;  coeffs[2][2][2] =  0; coeffs[2][2][3] = 0; 
	coeffs[2][3][0] = 1;  coeffs[2][3][1] = 0;  coeffs[2][3][2] =  0; coeffs[2][3][3] = 0; 

    }

    // central difference coefficients
    if (difference_style == 1) {

	// this is asymmetric and produces diagonal banding in the kernel
	// from: http://www.holoborodko.com/pavel/?page_id=239
	if (order == 0) { 
	    y_cen = x_cen = 1;
	    x_size = y_size = 3;
	}
	coeffs[0][0][0] =  0; coeffs[0][0][1] = -1;  coeffs[0][0][2] =  0; 
	coeffs[0][1][0] = -1; coeffs[0][1][1] =  0;  coeffs[0][1][2] =  1; 
	coeffs[0][2][0] =  0; coeffs[0][2][1] =  1;  coeffs[0][2][2] =  0; 

	// this works well and is largely the same as order=1 forward-diff.
	// from: http://www.holoborodko.com/pavel/?page_id=239
	if (order == 1) { 
	    y_cen = x_cen = 1;
	    x_size = y_size = 3;
	}
	y_cen1 = x_cen1 = 1;
	coeffs[1][0][0] =  0; coeffs[1][0][1] =  1;  coeffs[1][0][2] =  0;  
	coeffs[1][1][0] =  1; coeffs[1][1][1] = -4;  coeffs[1][1][2] =  1; 
	coeffs[1][2][0] =  0; coeffs[1][2][1] =  1;  coeffs[1][2][2] =  0;  

	// asymmetric and produces diagonal banding in the kernel
	// from http://www.holoborodko.com/pavel/?page_id=239
	if (order == 2) { 
	    y_cen = x_cen = 2;
	    x_size = y_size = 5;
	}
	y_cen2 = x_cen2 = 2;
	coeffs[2][0][0] =  0; coeffs[2][0][1] =  0;  coeffs[2][0][2] = -1; coeffs[2][0][3] =  0; coeffs[2][0][4] =  0; 
	coeffs[2][1][0] =  0; coeffs[2][1][1] =  0;  coeffs[2][1][2] =  2; coeffs[2][1][3] =  0; coeffs[2][1][4] =  0; 
	coeffs[2][2][0] = -1; coeffs[2][2][1] =  2;  coeffs[2][2][2] =  0; coeffs[2][2][3] = -2; coeffs[2][2][4] =  1; 
	coeffs[2][3][0] =  0; coeffs[2][3][1] =  0;  coeffs[2][3][2] = -2; coeffs[2][3][3] =  0; coeffs[2][3][4] =  0; 
	coeffs[2][4][0] =  0; coeffs[2][4][1] =  0;  coeffs[2][4][2] =  1; coeffs[2][4][3] =  0; coeffs[2][4][4] =  0; 
	
    }


    /* Note we have to add 1 extra (empty) term here because of the differential
     * background fitting */
    Eigen::MatrixXd B = Eigen::MatrixXd::Zero(width*height+1, width*height+1);

    /* Forward difference approximation */
    for (unsigned int i = 0; i < width*height; i++) {

	unsigned int const x0 = i % width;  // the x coord in the kernel image
	unsigned int const y0 = i / width;  // the y coord in the kernel image
	
	unsigned int x_edge_distance = (x0 > (width - x0 - 1))  ? width - x0 - 1  : x0;
	unsigned int y_edge_distance = (y0 > (height - y0 - 1)) ? height - y0 - 1 : y0;
	unsigned int edge_distance = (x_edge_distance < y_edge_distance) ? x_edge_distance : y_edge_distance;

        for (unsigned int dx = 0; dx < x_size; dx++) {
	    for (unsigned int dy = 0; dy < y_size; dy++) {

		// determine where to put this coeff

		// handle the boundary condition
		// note: adding width and height in the sum prevents negatives
		unsigned int x = 0;
		unsigned int y = 0; 
		double this_coeff = 0;

		// no-wrapping at edges
		if (boundary_style == 0) {
		    x = x0 + dx - x_cen;
		    y = y0 + dy - y_cen;
		    if ((y < 0) || (y > height - 1) || (x < 0) || (x > width - 1)) { continue; }
		    this_coeff = coeffs[order][dx][dy];

		// wrapping at edges
		} else if (boundary_style == 1) {
		    x = (width  + x0 + dx - x_cen) % width;
		    y = (height + y0 + dy - y_cen) % height;
		    this_coeff = coeffs[order][dx][dy];

		// order tapering to the edge (just clone wrapping for now)
		// - use the lowest order possible
		} else if (boundary_style == 2) {

		    // edge rows and columns ... set to constant
		    if (edge_distance == 0) {
			x = x0;
			y = y0;
			this_coeff = 1;
		    }
		    // in one from edge, use 1st order
		    else if (edge_distance == 1 && order > 0) {
			x = (width  + x0 + dx - x_cen1) % width;
			y = (height + y0 + dy - y_cen1) % height;
			if ((dx < 3) && (dy < 3)) { this_coeff = coeffs[1][dx][dy]; } 
		    }
		    // in two from edge, use 2st order if order > 1
		    else if (edge_distance == 2 && order > 1){
			x = (width  + x0 + dx - x_cen2) % width;
			y = (height + y0 + dy - y_cen2) % height;
			if ((dx < 5) && (dy < 5)) { this_coeff = coeffs[2][dx][dy]; } 
		    } 
		    // if we're somewhere in the middle
		    else if (edge_distance > order) {
			x = (width  + x0 + dx - x_cen) % width;
			y = (height + y0 + dy - y_cen) % height;
		    	this_coeff = coeffs[order][dx][dy];
		    }

		} 

		B(i, y*width + x) = this_coeff;
		
	    }

        }

    }

    if (printB)  {
	std::cout << B << std::endl;
    }
    
    boost::shared_ptr<Eigen::MatrixXd> H (new Eigen::MatrixXd(B.transpose() * B));
    return H;
}

/** 
 * @brief Rescale an input set of kernels 
 *
 * @return Vector of renormalized kernels
 *
 * @ingroup diffim
 */
math::KernelList
diffim::renormalizeKernelList(
    math::KernelList const &kernelListIn
    ) {
    typedef lsst::afw::math::Kernel::Pixel PixelT;
    typedef image::Image<PixelT> ImageT;

    /* 
       
    We want all the bases except for the first to sum to 0.0.  This allows
    us to achieve kernel flux conservation (Ksum) across the image since all
    the power will be in the first term, which will not vary spatially.
    
    K(x,y) = Ksum * B_0 + Sum_i : a(x,y) * B_i
    
    To do this, normalize all Kernels to sum = 1. and subtract B_0 from all
    subsequent kenrels.  
    
    To get an idea of the relative contribution of each of these basis
    functions later on down the line, lets also normalize them such that 
    
    Sum(B_i)  == 0.0   *and*
    B_i * B_i == 1.0
    
    For completeness 
    
    Sum(B_0)  == 1.0
    B_0 * B_0 != 1.0
    
    */
    math::KernelList kernelListOut;
    if (kernelListIn.size() == 0) {
        return kernelListOut;
    }

    ImageT image0(kernelListIn[0]->getDimensions());
    ImageT image(kernelListIn[0]->getDimensions());
    
    for (unsigned int i = 0; i < kernelListIn.size(); i++) {
        if (i == 0) {
            /* Make sure that it is normalized to kSum 1. */
            (void)kernelListIn[i]->computeImage(image0, true);
            boost::shared_ptr<math::Kernel> 
                kernelPtr(new math::FixedKernel(image0));
            kernelListOut.push_back(kernelPtr);
            continue;
        }

        /* For the rest, normalize to kSum 1. and subtract off image0 */
        (void)kernelListIn[i]->computeImage(image, true);
        image -= image0;

        /* Finally, rescale such that the inner product is 1 */
        double ksum = 0.;
        for (int y = 0; y < image.getHeight(); y++) {
            for (ImageT::xy_locator ptr = image.xy_at(0, y), end = image.xy_at(image.getWidth(), y); ptr != end; ++ptr.x()) {
                ksum += *ptr * *ptr;
            }
        }
        image /= sqrt(ksum);

        boost::shared_ptr<math::Kernel> 
            kernelPtr(new math::FixedKernel(image));
        kernelListOut.push_back(kernelPtr);
    }
    return kernelListOut;
}

/** 
 * @brief Generate an Alard-Lupton basis set of Kernels.
 *
 * @note Should consider implementing as SeparableKernels for additional speed,
 * but this will make the normalization a bit more complicated
 * 
 * @return Vector of Alard-Lupton Kernels.
 *
 * @ingroup diffim
 */
math::KernelList
diffim::generateAlardLuptonBasisSet(
    unsigned int halfWidth,                ///< size is 2*N + 1
    unsigned int nGauss,                   ///< number of gaussians
    std::vector<double> const &sigGauss,   ///< width of the gaussians
    std::vector<int>    const &degGauss    ///< local spatial variation of gaussians
    ) {
    typedef lsst::afw::math::Kernel::Pixel PixelT;
    typedef image::Image<PixelT> ImageT;

    if (halfWidth < 1) {
        throw LSST_EXCEPT(exceptions::Exception, "halfWidth must be positive");
    }
    if (nGauss != sigGauss.size()) {
        throw LSST_EXCEPT(exceptions::Exception, "sigGauss does not have enough entries");
    }
    if (nGauss != degGauss.size()) {
        throw LSST_EXCEPT(exceptions::Exception, "degGauss does not have enough entries");
    }
    int fullWidth = 2 * halfWidth + 1;
    ImageT image(fullWidth, fullWidth);
    
    math::KernelList kernelBasisList;
    for (unsigned int i = 0; i < nGauss; i++) {
        /* 
           sigma = FWHM / ( 2 * sqrt(2 * ln(2)) )
        */
        double sig        = sigGauss[i];
        unsigned int deg  = degGauss[i];

        math::GaussianFunction2<PixelT> gaussian(sig, sig);
        math::AnalyticKernel kernel(fullWidth, fullWidth, gaussian);
        math::PolynomialFunction2<PixelT> polynomial(deg);

        for (unsigned int j = 0, n = 0; j <= deg; j++) {
            for (unsigned int k = 0; k <= (deg - j); k++, n++) {
                /* for 0th order term, skip polynomial */
                (void)kernel.computeImage(image, true);
                if (n == 0) {
                    boost::shared_ptr<math::Kernel> 
                        kernelPtr(new math::FixedKernel(image));
                    kernelBasisList.push_back(kernelPtr);
                    continue;
                }
                
                /* gaussian to be modified by this term in the polynomial */
                polynomial.setParameter(n, 1.);
                (void)kernel.computeImage(image, true);
                for (int y = 0, v = -halfWidth; y < image.getHeight(); y++, v++) {
                    int u = -halfWidth;
                    for (ImageT::xy_locator ptr = image.xy_at(0, y), end = image.xy_at(image.getWidth(), y); ptr != end; ++ptr.x(), u++) {
                        /* Evaluate from -1 to 1 */
                        *ptr  = *ptr * polynomial(u/static_cast<double>(halfWidth), v/static_cast<double>(halfWidth));
                    }
                }
                boost::shared_ptr<math::Kernel> 
                    kernelPtr(new math::FixedKernel(image));
                kernelBasisList.push_back(kernelPtr);
                polynomial.setParameter(n, 0.);
            }
        }
    }
    return renormalizeKernelList(kernelBasisList);
}

/************************************************************************************************************/
/*
 * Adds a Function to an Image
 *
 * @note This routine assumes that the pixel coordinates start at (0, 0) which is
 * in general not true
 *
 * @node this function was renamed from addFunctionToImage to addSomethingToImage to allow generic programming
 */
template <typename PixelT, typename FunctionT>
void diffim::addSomethingToImage(image::Image<PixelT> &image,
                                 FunctionT const &function
    ) {
    
    // Set the pixels row by row, to avoid repeated checks for end-of-row
    for (int y = 0; y != image.getHeight(); ++y) {
        double yPos = image::positionToIndex(y);
        
        double xPos = image::positionToIndex(0);
        for (typename image::Image<PixelT>::x_iterator ptr = image.row_begin(y), end = image.row_end(y);
             ptr != end; ++ptr, ++xPos) {            
            *ptr += function(xPos, yPos);
        }
    }
}
//
// Add a scalar.
//
template <typename PixelT>
void diffim::addSomethingToImage(image::Image<PixelT> &image,
                                 double value
    ) {
    if (value != 0.0) {
        image += value;
    }
}

/** 
 * @brief Implement fundamental difference imaging step of convolution and
 * subtraction : D = I - (K*T + bg) where * denotes convolution
 * 
 * @note If you convolve the science image, D = (K*I + bg) - T, set invert=False
 *
 * @note The template is taken to be an MaskedImage; this takes c 1.6 times as long
 * as using an Image
 *
 * @return Difference image
 *
 * @ingroup diffim
 */
template <typename PixelT, typename BackgroundT>
image::MaskedImage<PixelT> diffim::convolveAndSubtract(
    lsst::afw::image::MaskedImage<PixelT> const &imageToConvolve,    ///< Image T to convolve with Kernel
    lsst::afw::image::MaskedImage<PixelT> const &imageToNotConvolve, ///< Image I to subtract convolved template from
    lsst::afw::math::Kernel const &convolutionKernel,                ///< PSF-matching Kernel used for convolution
    BackgroundT background,                               ///< Differential background function or scalar
    bool invert                                           ///< Invert the output difference image
    ) {

    boost::timer t;
    t.restart();

    image::MaskedImage<PixelT> convolvedMaskedImage(imageToConvolve.getDimensions());
    convolvedMaskedImage.setXY0(imageToConvolve.getXY0());
    math::convolve(convolvedMaskedImage, imageToConvolve, convolutionKernel, false);
    
    /* Add in background */
    addSomethingToImage(*(convolvedMaskedImage.getImage()), background);
    
    /* Do actual subtraction */
    convolvedMaskedImage -= imageToNotConvolve;

    /* Invert */
    if (invert) {
        convolvedMaskedImage *= -1.0;
    }

    double time = t.elapsed();
    logging::TTrace<5>("lsst.ip.diffim.convolveAndSubtract", 
                       "Total compute time to convolve and subtract : %.2f s", time);

    return convolvedMaskedImage;
}

/** 
 * @brief Implement fundamental difference imaging step of convolution and
 * subtraction : D = I - (K.x.T + bg)
 *
 * @note The template is taken to be an Image, not a MaskedImage; it therefore
 * has neither variance nor bad pixels
 *
 * @note If you convolve the science image, D = (K*I + bg) - T, set invert=False
 * 
 * @return Difference image
 *
 * @ingroup diffim
 */
template <typename PixelT, typename BackgroundT>
image::MaskedImage<PixelT> diffim::convolveAndSubtract(
    lsst::afw::image::Image<PixelT> const &imageToConvolve,          ///< Image T to convolve with Kernel
    lsst::afw::image::MaskedImage<PixelT> const &imageToNotConvolve, ///< Image I to subtract convolved template from
    lsst::afw::math::Kernel const &convolutionKernel,                ///< PSF-matching Kernel used for convolution
    BackgroundT background,                                          ///< Differential background function or scalar
    bool invert                                                      ///< Invert the output difference image
    ) {
    
    boost::timer t;
    t.restart();

    image::MaskedImage<PixelT> convolvedMaskedImage(imageToConvolve.getDimensions());
    convolvedMaskedImage.setXY0(imageToConvolve.getXY0());
    math::convolve(*convolvedMaskedImage.getImage(), imageToConvolve, convolutionKernel, false);
    
    /* Add in background */
    addSomethingToImage(*convolvedMaskedImage.getImage(), background);
    
    /* Do actual subtraction */
    *convolvedMaskedImage.getImage() -= *imageToNotConvolve.getImage();

    /* Invert */
    if (invert) {
        *convolvedMaskedImage.getImage() *= -1.0;
    }
    *convolvedMaskedImage.getMask() <<= *imageToNotConvolve.getMask();
    *convolvedMaskedImage.getVariance() <<= *imageToNotConvolve.getVariance();
    
    double time = t.elapsed();
    logging::TTrace<5>("lsst.ip.diffim.convolveAndSubtract", 
                       "Total compute time to convolve and subtract : %.2f s", time);

    return convolvedMaskedImage;
}

/** 
 * @brief Runs Detection on a single image for significant peaks, and checks
 * returned Footprints for Masked pixels.
 *
 * Accepts two MaskedImages, one of which is to be convolved to match the
 * other.  The Detection package is run on the image to be convolved
 * (assumed to be higher S/N than the other image).  The subimages
 * associated with each returned Footprint in both images are checked for
 * Masked pixels; Footprints containing Masked pixels are rejected.  The
 * Footprints are grown by an amount specified in the Policy.  The
 * acceptible Footprints are returned in a vector.
 *
 * @return Vector of "clean" Footprints around which Image Subtraction
 * Kernels will be built.
 *
 * @ingroup diffim
 */
template <typename PixelT>
std::vector<lsst::afw::detection::Footprint::Ptr> diffim::getCollectionOfFootprintsForPsfMatching(
    lsst::afw::image::MaskedImage<PixelT> const &imageToConvolve,    
    lsst::afw::image::MaskedImage<PixelT> const &imageToNotConvolve, 
    lsst::pex::policy::Policy             const &policy                                       
    ) {
    
    // Parse the Policy
    unsigned int fpNpixMin      = policy.getInt("fpNpixMin");
    unsigned int fpNpixMax      = policy.getInt("fpNpixMax");

    int const kCols             = policy.getInt("kernelCols");
    int const kRows             = policy.getInt("kernelRows");
    double fpGrowKsize          = policy.getDouble("fpGrowKsize");

    int minCleanFp              = policy.getInt("minCleanFp");
    double detThreshold         = policy.getDouble("detThreshold");
    double detThresholdScaling  = policy.getDouble("detThresholdScaling");
    double detThresholdMin      = policy.getDouble("detThresholdMin");
    std::string detThresholdType = policy.getString("detThresholdType");

    // New mask plane that tells us which pixels are already in sources
    // Add to both images so mask planes are aligned
    int diffimMaskPlane = imageToConvolve.getMask()->addMaskPlane(diffim::diffimStampCandidateStr);
    (void)imageToNotConvolve.getMask()->addMaskPlane(diffim::diffimStampCandidateStr);
    image::MaskPixel const diffimBitMask = imageToConvolve.getMask()->getPlaneBitMask(diffim::diffimStampCandidateStr);

    // Add in new plane that will tell us which ones are used
    (void)imageToConvolve.getMask()->addMaskPlane(diffim::diffimStampUsedStr);
    (void)imageToNotConvolve.getMask()->addMaskPlane(diffim::diffimStampUsedStr);

    // Number of pixels to grow each Footprint, based upon the Kernel size
    int fpGrowPix = int(fpGrowKsize * ((kCols > kRows) ? kCols : kRows));

    // List of Footprints
    std::vector<detection::Footprint::Ptr> footprintListIn;
    std::vector<detection::Footprint::Ptr> footprintListOut;

    // Functors to search through the images for masked pixels within candidate footprints
    diffim::FindSetBits<image::Mask<image::MaskPixel> > itcFunctor(*(imageToConvolve.getMask())); 
    diffim::FindSetBits<image::Mask<image::MaskPixel> > itncFunctor(*(imageToNotConvolve.getMask())); 
 
    int nCleanFp = 0;
    while ((nCleanFp < minCleanFp) and (detThreshold > detThresholdMin)) {
        imageToConvolve.getMask()->clearMaskPlane(diffimMaskPlane);
        imageToNotConvolve.getMask()->clearMaskPlane(diffimMaskPlane);

        footprintListIn.clear();
        footprintListOut.clear();
        
        // Find detections
        detection::Threshold threshold = 
                detection::createThreshold(detThreshold, detThresholdType);
        detection::FootprintSet<PixelT> footprintSet(
                imageToConvolve, 
                threshold,
                "",
                fpNpixMin);
        
        // Get the associated footprints
        footprintListIn = footprintSet.getFootprints();
        logging::TTrace<4>("lsst.ip.diffim.getCollectionOfFootprintsForPsfMatching", 
                           "Found %d total footprints above threshold %.3f",
                           footprintListIn.size(), detThreshold);

        // Iterate over footprints, look for "good" ones
        nCleanFp = 0;
        for (std::vector<detection::Footprint::Ptr>::iterator i = footprintListIn.begin(); i != footprintListIn.end(); ++i) {
            // footprint has too many pixels
            if (static_cast<unsigned int>((*i)->getNpix()) > fpNpixMax) {
                logging::TTrace<6>("lsst.ip.diffim.getCollectionOfFootprintsForPsfMatching", 
                               "Footprint has too many pix: %d (max =%d)", 
                               (*i)->getNpix(), fpNpixMax);
                continue;
            } 
            
            logging::TTrace<8>("lsst.ip.diffim.getCollectionOfFootprintsForPsfMatching", 
                               "Footprint in : %d,%d -> %d,%d",
                               (*i)->getBBox().getX0(), (*i)->getBBox().getX1(), 
                               (*i)->getBBox().getY0(), (*i)->getBBox().getY1());

            logging::TTrace<8>("lsst.ip.diffim.getCollectionOfFootprintsForPsfMatching", 
                               "Grow by : %d pixels", fpGrowPix);

            /* Grow the footprint
               flag true  = isotropic grow   = slow
               flag false = 'manhattan grow' = fast
               
               The manhattan masks are rotated 45 degree w.r.t. the coordinate
               system.  They intersect the vertices of the rectangle that would
               connect pixels (X0,Y0) (X1,Y0), (X0,Y1), (X1,Y1).
               
               The isotropic masks do take considerably longer to grow and are
               basically elliptical.  X0, X1, Y0, Y1 delimit the extent of the
               ellipse.

               In both cases, since the masks aren't rectangles oriented with
               the image coordinate system, when we DO extract such rectangles
               as subimages for kernel fitting, some corner pixels can be found
               in multiple subimages.

            */
            detection::Footprint::Ptr fpGrow = 
                detection::growFootprint(*i, fpGrowPix, false);
            
            logging::TTrace<6>("lsst.ip.diffim.getCollectionOfFootprintsForPsfMatching", 
                               "Footprint out : %d,%d -> %d,%d (center %d,%d)",
                               (*fpGrow).getBBox().getX0(), (*fpGrow).getBBox().getY0(),
			       (*fpGrow).getBBox().getX1(), (*fpGrow).getBBox().getY1(),
			       int(0.5 * ((*i)->getBBox().getX0()+(*i)->getBBox().getX1())),
			       int(0.5 * ((*i)->getBBox().getY0()+(*i)->getBBox().getY1())));


            // Ignore if its too close to the edge of the amp image 
            // Note we need to translate to pixel coordinates here
            image::BBox fpBBox = (*fpGrow).getBBox();
            fpBBox.shift(-imageToConvolve.getX0(), -imageToConvolve.getY0());
            if (((*fpGrow).getBBox().getX0() < 0) ||
                ((*fpGrow).getBBox().getY0() < 0) ||
                ((*fpGrow).getBBox().getX1() > imageToConvolve.getWidth()) ||
                ((*fpGrow).getBBox().getY1() > imageToConvolve.getHeight()))
                continue;


            // Grab a subimage; report any exception
            try {
                image::MaskedImage<PixelT> subImageToConvolve(imageToConvolve, fpBBox);
                image::MaskedImage<PixelT> subImageToNotConvolve(imageToNotConvolve, fpBBox);
            } catch (exceptions::Exception& e) {
                logging::TTrace<6>("lsst.ip.diffim.getCollectionOfFootprintsForPsfMatching",
                                   "Exception caught extracting Footprint");
                logging::TTrace<7>("lsst.ip.diffim.getCollectionOfFootprintsForPsfMatching",
                                   e.what());
                continue;
            }

            // Search for any masked pixels within the footprint
            itcFunctor.apply(*fpGrow);
            if (itcFunctor.getBits() > 0) {
                logging::TTrace<6>("lsst.ip.diffim.getCollectionOfFootprintsForPsfMatching", 
                                   "Footprint has masked pix (val=%d) in image to convolve", itcFunctor.getBits()); 
                continue;
            }

            itncFunctor.apply(*fpGrow);
            if (itncFunctor.getBits() > 0) {
                logging::TTrace<6>("lsst.ip.diffim.getCollectionOfFootprintsForPsfMatching", 
                                   "Footprint has masked pix (val=%d) in image not to convolve", itncFunctor.getBits());
                continue;
            }

            // If we get this far, we have a clean footprint
            footprintListOut.push_back(fpGrow);
            (void)detection::setMaskFromFootprint(&(*imageToConvolve.getMask()), *fpGrow, diffimBitMask);
            (void)detection::setMaskFromFootprint(&(*imageToNotConvolve.getMask()), *fpGrow, diffimBitMask);
            nCleanFp += 1;
        }
        detThreshold *= detThresholdScaling;
    }
    imageToConvolve.getMask()->clearMaskPlane(diffimMaskPlane);
    imageToNotConvolve.getMask()->clearMaskPlane(diffimMaskPlane);

    if (footprintListOut.size() == 0) {
      throw LSST_EXCEPT(exceptions::Exception, 
			"Unable to find any footprints for Psf matching");
    }

    logging::TTrace<1>("lsst.ip.diffim.getCollectionOfFootprintsForPsfMatching", 
                       "Found %d clean footprints above threshold %.3f",
                       footprintListOut.size(), detThreshold/detThresholdScaling);
    
    return footprintListOut;
}

// Explicit instantiations
template class diffim::PsfMatchingFunctor<float, float>;
template class diffim::PsfMatchingFunctor<double, float>;

template class diffim::FindSetBits<image::Mask<> >;

template class diffim::ImageStatistics<float>;
template class diffim::ImageStatistics<double>;

/* */

#define p_INSTANTIATE_convolveAndSubtract(TEMPLATE_IMAGE_T, TYPE)     \
    template \
    image::MaskedImage<TYPE> diffim::convolveAndSubtract( \
        image::TEMPLATE_IMAGE_T<TYPE> const& imageToConvolve, \
        image::MaskedImage<TYPE> const& imageToNotConvolve, \
        math::Kernel const& convolutionKernel, \
        double background, \
        bool invert);      \
    \
    template \
    image::MaskedImage<TYPE> diffim::convolveAndSubtract( \
        image::TEMPLATE_IMAGE_T<TYPE> const& imageToConvolve, \
        image::MaskedImage<TYPE> const& imageToNotConvolve, \
        math::Kernel const& convolutionKernel, \
        math::Function2<double> const& backgroundFunction, \
        bool invert); \

#define INSTANTIATE_convolveAndSubtract(TYPE) \
p_INSTANTIATE_convolveAndSubtract(Image, TYPE) \
p_INSTANTIATE_convolveAndSubtract(MaskedImage, TYPE)
/*
 * Here are the instantiations.
 *
 * Do we really need double diffim code?  It isn't sufficient to remove it here; you'll have to also remove at
 * least SpatialModelKernel<double> and swig instantiations thereof
 */
INSTANTIATE_convolveAndSubtract(float);
INSTANTIATE_convolveAndSubtract(double);

/* */


template
std::vector<detection::Footprint::Ptr> diffim::getCollectionOfFootprintsForPsfMatching(
    image::MaskedImage<float> const &,
    image::MaskedImage<float> const &,
    lsst::pex::policy::Policy const &);

template
std::vector<detection::Footprint::Ptr> diffim::getCollectionOfFootprintsForPsfMatching(
    image::MaskedImage<double> const &,
    image::MaskedImage<double> const &,
    lsst::pex::policy::Policy  const &);

template 
void diffim::addSomethingToImage(
    image::Image<float> &,
    math::PolynomialFunction2<double> const &
    );
template 
void diffim::addSomethingToImage(
    image::Image<double> &,
    math::PolynomialFunction2<double> const &
    );

template 
void diffim::addSomethingToImage(
    image::Image<float> &,
    double
    );
template 
void diffim::addSomethingToImage(
    image::Image<double> &,
    double
    );
