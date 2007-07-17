// -*- lsst-c++ -*-
/**
 * \file
 *
 * Implementation of image subtraction
 *
 * Implementation of image subtraction
 *
 * \author Andrew Becker
 *
 * \ingroup imageproc
 */

#include <lsst/fw/MaskedImage.h>
#include <lsst/fw/Kernel.h>
#include <lsst/fw/KernelFunctions.h>
#include <lsst/fw/PixelAccessors.h>
#include <lsst/fw/Source.h>
#include <vw/Math/Matrix.h> 
#include <vw/Math/Vector.h> 
#include <vw/Math/LinearAlgebra.h> 
#include <vw/Math/Functions.h> 
#include <boost/shared_ptr.hpp>
#include <PCA.h>

using namespace std;

/**
 * Computes spatially varying PSF matching kernel for image subtraction
 *
 * Note: Longer description here
 *
 * \return This describes the return value if there is one
 * \throw Any exceptions thrown must be described here
 * \throw Here too
 * \ingroup imageproc
 */
template <typename PixelT, typename MaskT, typename KernelT>
void lsst::imageproc::computePSFMatchingKernelForMaskedImage(
    lsst::fw::MaskedImage<PixelT,MaskT> const &imageToConvolve, ///< Template image; convolved
    lsst::fw::MaskedImage<PixelT,MaskT> const &imageToNotConvolve, ///< Science image; not convolved
    vector<boost::shared_ptr<lsst::fw::Kernel<KernelT> > > const &kernelBasisVec ///< Input set of basis kernels
    ) {

    vector<lsst::fw::Source> sourceCollection;
    getCollectionOfMaskedImagesForPSFMatching(sourceCollection);

    // Reusable view around each source
    typename lsst::fw::MaskedImage<PixelT,MaskT>::MaskedImagePtrT imageToConvolvePtr;
    typename lsst::fw::MaskedImage<PixelT,MaskT>::MaskedImagePtrT imageToNotConvolvePtr;

    // Collection of output kernels
    vector<lsst::fw::LinearCombinationKernel<KernelT> > kernelVec(sourceCollection.size());

    // Iterate over source
    typename vector<lsst::fw::Source>::iterator siter = sourceCollection.begin();
    typename vector<lsst::fw::LinearCombinationKernel<KernelT> >::iterator kiter = kernelVec.begin();

    for (; siter != sourceCollection.end(); ++siter, ++kiter) {
        lsst::fw::Source diffImSource = *siter;
        
        // grab view around each source
        // do i really want a new stamp or just a view?
        BBox2i stamp(boost::any_cast<const int>(diffImSource.getRowc() - diffImSource.getDrow()), 
                     boost::any_cast<const int>(diffImSource.getRowc() + diffImSource.getDrow()),
                     boost::any_cast<const int>(diffImSource.getColc() - diffImSource.getDcol()), 
                     boost::any_cast<const int>(diffImSource.getColc() + diffImSource.getDcol()));
        imageToConvolvePtr    = imageToConvolve.getSubImage(stamp);
        imageToNotConvolvePtr = imageToNotConvolve.getSubImage(stamp);

        vector<double> kernelCoeffs(kernelBasisVec.size());

        // Find best single kernel for this stamp
        lsst::imageproc::computePSFMatchingKernelForPostageStamp
            (*imageToConvolvePtr, *imageToNotConvolvePtr, kernelBasisVec, kernelCoeffs);
        
        // Create a linear combination kernel from this and append to kernelVec
        lsst::fw::LinearCombinationKernel<KernelT> sourceKernel(kernelBasisVec, kernelCoeffs);
        *kiter = sourceKernel;
    }
    // Hold output PCA kernel
    vector<lsst::fw::Kernel<KernelT> > kernelPCABasisVec;
    lsst::imageproc::computePCAKernelBasis(kernelVec, kernelPCABasisVec);
}

/**
 * Single line description with no period
 *
 * Note: Long description
 *
 * \return This describes the return value if there is one
 * \throw Any exceptions thrown must be described here
 * \throw Here too
 * \ingroup I guess imageproc for me
 */
template <typename PixelT, typename MaskT, typename KernelT>
void lsst::imageproc::computePSFMatchingKernelForPostageStamp(
    lsst::fw::MaskedImage<PixelT, MaskT> const &imageToConvolve, ///< Goes with the code
    lsst::fw::MaskedImage<PixelT, MaskT> const &imageToNotConvolve, ///< This is for doxygen
    vector<boost::shared_ptr<lsst::fw::Kernel<KernelT> > > const &kernelBasisVec, ///< Input kernel basis set
    vector<double> &kernelCoeffs ///< Output kernel basis coefficients
    ) { 

    int nKernelParameters=0, nBackgroundParameters=0, nParameters=0;
    const float threshold = 0.0;
    
    // We assume that each kernel in the Set has 1 parameter you fit for
    nKernelParameters = kernelBasisVec.size();
    // Or, we just assume that across a single kernel, background 0th order.  This quite makes sense.
    nBackgroundParameters = 1;
    // Total number of parameters
    nParameters = nKernelParameters + nBackgroundParameters;
    
    vw::math::Vector<double> B(nParameters);
    vw::math::Matrix<double> M(nParameters, nParameters);

    // convolve creates a MaskedImage, push it onto the back of the Vector
    // need to use shared pointers because MaskedImage copy does not work
    vector<boost::shared_ptr<lsst::fw::MaskedImage<PixelT, MaskT> > > convolvedImageVec;

    typename vector<boost::shared_ptr<lsst::fw::Kernel<KernelT> > >::const_iterator kiter = kernelBasisVec.begin();
    typename vector<boost::shared_ptr<lsst::fw::MaskedImage<PixelT, MaskT> > >::iterator citer = convolvedImageVec.begin();

    for (; kiter != kernelBasisVec.end(); ++kiter, ++citer) {

        boost::shared_ptr<lsst::fw::MaskedImage<PixelT, MaskT> > imagePtr(
            new lsst::fw::MaskedImage<PixelT, MaskT>(lsst::fw::kernel::convolve(imageToConvolve, **kiter, threshold, vw::NoEdgeExtension(), -1))
            );

        *citer = imagePtr;
    } 

    // An accessor for each convolution plane
    // NOTE : MaskedPixelAccessor has no empty constructor, therefore we need to push_back()
    vector<lsst::fw::MaskedPixelAccessor<PixelT, MaskT> > convolvedAccessorRowVec;
    for (citer = convolvedImageVec.begin(); citer != convolvedImageVec.end(); ++citer) {
        lsst::fw::MaskedPixelAccessor<PixelT, MaskT> convolvedAccessorRow(**citer);
        convolvedAccessorRowVec.push_back(convolvedAccessorRow);
    }

    // An accessor for each input image
    lsst::fw::MaskedPixelAccessor<PixelT, MaskT> imageToConvolveRow(imageToConvolve);
    lsst::fw::MaskedPixelAccessor<PixelT, MaskT> imageToNotConvolveRow(imageToNotConvolve);

    // integral over image's dx and dy
    for (int row = 0; row < imageToConvolve.getRows(); row++) {

        // An accessor for each convolution plane
        vector<lsst::fw::MaskedPixelAccessor<PixelT, MaskT> > convolvedAccessorColVec;
        for (int ki = 0; ki < nKernelParameters; ki++) {
            lsst::fw::MaskedPixelAccessor<PixelT, MaskT> convolvedAccessorCol = convolvedAccessorRowVec[ki];
            convolvedAccessorColVec.push_back(convolvedAccessorCol);
        }

        // An accessor for each input image
        lsst::fw::MaskedPixelAccessor<PixelT, MaskT> imageToConvolveCol = imageToConvolveRow;
        lsst::fw::MaskedPixelAccessor<PixelT, MaskT> imageToNotConvolveCol = imageToNotConvolveRow;

        for (int col = 0; col < imageToConvolve.getCols(); col++) {
            
            PixelT ncCamera = *imageToNotConvolveCol.image;
            PixelT ncVariance = *imageToNotConvolveCol.variance;

            // Its an approximation to use this since you don't really know the kernel yet
            // You really want the post-convolved variance but this is close enough
            PixelT cVariance = *imageToConvolveCol.variance;
            
            // Quicker computation
            double iVariance = 1.0 / (ncVariance + cVariance);

            // kernel index i
            for (int kidxi = 0; kidxi < nKernelParameters; kidxi++) {
                PixelT cdCamerai = *convolvedAccessorColVec[kidxi].image;
                B[kidxi] += ncCamera * cdCamerai * iVariance;
                
                // kernel index j 
                for (int kidxj = kidxi; kidxj < nKernelParameters; kidxj++) {
                    PixelT cdCameraj = *convolvedAccessorColVec[kidxj].image;
                    M[kidxi][kidxj] += cdCamerai * cdCameraj * iVariance;
                } // kidxj
            } // kidxi

            // Here we have a single background term
            B[nParameters] += ncCamera * iVariance;
            M[nParameters][nParameters] += 1.0 * iVariance;

            // Step each accessor in column
            imageToConvolveCol.nextCol();
            imageToNotConvolveCol.nextCol();
            for (int ki = 0; ki < nKernelParameters; ki++) {
                convolvedAccessorColVec[ki].nextCol();
            }
        } // col
        
        // Step each accessor in row
        imageToConvolveRow.nextRow();
        imageToNotConvolveRow.nextRow();
        for (int ki = 0; ki < nKernelParameters; ki++) {
            convolvedAccessorRowVec[ki].nextRow();
        }

        // clean up
        convolvedAccessorColVec.~vector<lsst::fw::MaskedPixelAccessor<PixelT, MaskT> >();
    } // row

    // Fill in rest of M
    for (int kidxi=0; kidxi < nKernelParameters; kidxi++) 
        for (int kidxj=kidxi+1; kidxj < nKernelParameters; kidxj++) 
            M[kidxj][kidxi] = M[kidxi][kidxj];
    
    // Invert M
    vw::math::Matrix<double> Minv = vw::math::inverse(M);
    // Solve for x in Mx = B
    //vw::math::Vector<double> Soln = B * Minv;  // uh, this does not work for B
    vw::math::Vector<double> Soln = Minv * B; // will this at least compile, '*' is a method for Minv

    // Worry about translating here...
    //kernelCoeffs = Soln;  // somehow
    for (int ki = 0; ki < nKernelParameters; ki++) {
        kernelCoeffs[ki] = Soln[ki];
    }

    // clean up
    //delete B;
    //delete M;
    convolvedImageVec.~vector<boost::shared_ptr<lsst::fw::MaskedImage<PixelT, MaskT> > >();
    convolvedAccessorRowVec.~vector<lsst::fw::MaskedPixelAccessor<PixelT, MaskT> >();
    //delete Minv;
    //delete Soln;
}

void lsst::imageproc::getCollectionOfMaskedImagesForPSFMatching(
    vector<lsst::fw::Source> &sourceCollection ///< Vector of sources to use for diffim kernel
    ) {

    // Hack some positions in for /lsst/becker/lsst_devel/DC2/fw/tests/data/871034p_1_MI_img.fits
    lsst::fw::Source src1(1010.345, 2375.548, 10., 10.); 
    lsst::fw::Source src2(404.248, 573.398, 10., 10.);
    lsst::fw::Source src3(1686.743, 1880.935, 10., 10.);
    sourceCollection.push_back(src1);
    sourceCollection.push_back(src2);
    sourceCollection.push_back(src3);
}

template <typename KernelT>
void lsst::imageproc::computePCAKernelBasis(
    vector<lsst::fw::LinearCombinationKernel<KernelT> > const &kernelVec, ///< Original input kernel basis set
    vector<lsst::fw::Kernel<KernelT> > &kernelPCABasisVec ///< Output principal components as kernel images
    ) {
    
    typedef double ImageT;

    const int nKernel = kernelVec.size();
    const int nCols = kernelVec[0].getCols();
    const int nRows = kernelVec[0].getRows();
    const int nPixels = nCols * nRows;

    int xEval=0;
    int yEval=0;
    bool doNormalize = false;

    // Matrix to invert.  Number of rows = number of pixels; number of columns = number of kernels
    vw::Matrix<ImageT> M(nPixels, nKernel); 

    typedef typename vw::ImageView<ImageT>::pixel_accessor imageAccessorType;

    // iterator over kernels
    typename vector<lsst::fw::LinearCombinationKernel<KernelT> >::const_iterator kiter = kernelVec.begin();

    // fill up matrix for PCA
    // does it matter if i use kiter++ or ++kiter?
    for (int ki = 0; kiter != kernelVec.end(); ki++, kiter++) {
        lsst::fw::Image<ImageT> kImage = kiter->getImage(xEval, yEval, doNormalize);
        imageAccessorType imageAccessor(kImage.origin());

        kImage.writeFits( (boost::format("kFits_%d.fits\n") % ki).str() );
        
        //assert(nRows == kImage.getRows());
        //assert(nCols == kImage.getCols());
        int mIdx = 0;
        for (int col = 0; col < nCols; col++) {
            for (int row = 0; row < nRows; row++, mIdx++) {

                // NOTE : 
                //   arguments to matrix-related functions are given in row,col
                //   arguments to image-related functions are given in col,row
                // HOWEVER :
                //   it doesn't matter which order we put the kernel elements into the PCA
                //   since they are not correlated 
                //   as long as we do the same thing when extracting the components
                // UNLESS :
                //   we want to put in some weighting/regularlization into the PCA
                //   not sure if that is even possible...

                M(mIdx, ki) = *imageAccessor;
                imageAccessor.next_row();
            }
            imageAccessor.next_col();
        }
    }

    vw::math::Matrix<ImageT> eVec(nPixels, nKernel);
    vw::math::Vector<ImageT> eVal(nKernel);
    vw::math::Vector<ImageT> mMean(nPixels);
    
    lsst::imageproc::computePCA(M, mMean, eVal, eVec, true);
    
    // turn each eVec into an Image and then into a Kernel
    for (int i = 0; i < eVec.cols(); i++) {
        lsst::fw::Image<ImageT> basisImage(nCols, nRows);
        imageAccessorType imageAccessor(basisImage.origin());

        // Not sure how to bulk load information into Image
        int kIdx = 0;
        for (int col = 0; col < nCols; col++) {
            for (int row = 0; row < nRows; row++, kIdx++) {
                *imageAccessor = eVec(kIdx, col);
                imageAccessor.next_row();
            }
            imageAccessor.next_col();
        }
        // debugging info
        basisImage.writeFits( (boost::format("eFits_%d.fits\n") % i).str() );

        lsst::fw::FixedKernel<ImageT> basisKernel(basisImage);
        //kernelPCABasisVec.append( basisKernel );
    }
    // I need to pass the eigenvalues back as well
    
}

// TODO BELOW

void lsst::imageproc::getTemplateChunkExposureFromTemplateExposure() {
}
void lsst::imageproc::wcsMatchExposure() {
}
void lsst::imageproc::computeSpatiallyVaryingPSFMatchingKernel() {
}


