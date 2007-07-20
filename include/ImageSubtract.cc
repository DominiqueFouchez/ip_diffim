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
#include <lsst/fw/Trace.h>
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
template <typename ImageT, typename MaskT, typename KernelT>
void lsst::imageproc::computePSFMatchingKernelForMaskedImage(
    lsst::fw::MaskedImage<ImageT,MaskT> const &imageToConvolve, ///< Template image; convolved
    lsst::fw::MaskedImage<ImageT,MaskT> const &imageToNotConvolve, ///< Science image; not convolved
    vector<boost::shared_ptr<lsst::fw::Kernel<KernelT> > > const &kernelBasisVec ///< Input set of basis kernels
    ) {

    vector<lsst::fw::Source> sourceCollection;
    getCollectionOfMaskedImagesForPSFMatching(sourceCollection);

    lsst::fw::Trace("lsst.imageproc.computePSFMatchingKernelForMaskedImage", 2, "Entering subroutine computePSFMatchingKernelForMaskedImage");

    // Reusable view around each source
    typename lsst::fw::MaskedImage<ImageT,MaskT>::MaskedImagePtrT imageToConvolvePtr;
    typename lsst::fw::MaskedImage<ImageT,MaskT>::MaskedImagePtrT imageToNotConvolvePtr;

    // Collection of output kernels
    vector<lsst::fw::LinearCombinationKernel<KernelT> > kernelVec(sourceCollection.size());

    // Iterate over source
    typename vector<lsst::fw::Source>::iterator siter = sourceCollection.begin();
    typename vector<lsst::fw::LinearCombinationKernel<KernelT> >::iterator kiter = kernelVec.begin();

    for (; siter != sourceCollection.end(); ++siter, ++kiter) {
        lsst::fw::Source diffImSource = *siter;
        
        // grab view around each source
        // do i really want a new stamp or just a view?
        // Bbox2i has x y cols rows
        // NOTE : we need to make sure we get the centering right with these +1, etc...
        BBox2i stamp(floor(diffImSource.getColc() - diffImSource.getDcol()), 
                     floor(diffImSource.getRowc() - diffImSource.getDrow()), 
                     ceil(2 * diffImSource.getDcol() + 1),
                     ceil(2 * diffImSource.getDrow() + 1)
                     );

        imageToConvolvePtr    = imageToConvolve.getSubImage(stamp);
        imageToNotConvolvePtr = imageToNotConvolve.getSubImage(stamp);

        vector<double> kernelCoeffs(kernelBasisVec.size());

        imageToConvolvePtr->writeFits( (boost::format("iFits_%d") % diffImSource.getId()).str() );

        // Find best single kernel for this stamp
        lsst::imageproc::computePSFMatchingKernelForPostageStamp
            (*imageToConvolvePtr, *imageToNotConvolvePtr, kernelBasisVec, kernelCoeffs);

        // Create a linear combination kernel from this and append to kernelVec
        lsst::fw::LinearCombinationKernel<KernelT> sourceKernel(kernelBasisVec, kernelCoeffs);
        *kiter = sourceKernel;

        // DEBUGGING
        lsst::fw::Image<KernelT> kImage = sourceKernel.getImage(0, 0, false);
        kImage.writeFits( (boost::format("kFits_%d.fits") % diffImSource.getId()).str() );
        

    }
    // Hold output PCA kernel
    vector<boost::shared_ptr<lsst::fw::Kernel<KernelT> > > kernelPCABasisVec;
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
template <typename ImageT, typename MaskT, typename KernelT>
void lsst::imageproc::computePSFMatchingKernelForPostageStamp(
    lsst::fw::MaskedImage<ImageT, MaskT> const &imageToConvolve, ///< Goes with the code
    lsst::fw::MaskedImage<ImageT, MaskT> const &imageToNotConvolve, ///< This is for doxygen
    vector<boost::shared_ptr<lsst::fw::Kernel<KernelT> > > const &kernelBasisVec, ///< Input kernel basis set
    vector<double> &kernelCoeffs ///< Output kernel basis coefficients
    ) { 

    int nKernelParameters=0, nBackgroundParameters=0, nParameters=0;
    const KernelT threshold = 0.0;

    lsst::fw::Trace("lsst.imageproc.computePSFMatchingKernelForPostageStamp", 2, "Entering subroutine computePSFMatchingKernelForPostageStamp");
    
    // We assume that each kernel in the Set has 1 parameter you fit for
    nKernelParameters = kernelBasisVec.size();
    // Or, we just assume that across a single kernel, background 0th order.  This quite makes sense.
    nBackgroundParameters = 1;
    // Total number of parameters
    nParameters = nKernelParameters + nBackgroundParameters;
    
    vw::math::Vector<double> B(nParameters);
    vw::math::Matrix<double> M(nParameters, nParameters);
    for (unsigned int i = nParameters; i--;) {
        B(i) = 0;
        for (unsigned int j = nParameters; j--;) {
            M(i,j) = 0;
        }
    }

    // convolve creates a MaskedImage, push it onto the back of the Vector
    // need to use shared pointers because MaskedImage copy does not work
    vector<boost::shared_ptr<lsst::fw::MaskedImage<ImageT, MaskT> > > convolvedImageVec(nKernelParameters);
    // and an iterator over this
    typename vector<boost::shared_ptr<lsst::fw::MaskedImage<ImageT, MaskT> > >::iterator citer = convolvedImageVec.begin();
    
    // iterator for input kernel basis
    typename vector<boost::shared_ptr<lsst::fw::Kernel<KernelT> > >::const_iterator kiter = kernelBasisVec.begin();
    // Buffer around source images
    // The convolved images will be reduced in size by the kernel extent, half on either side.  the extra pixel gets taken from the top/right
    // NOTE : we assume here that all kernels are the same size so that grabbing the size of the first one suffices
    unsigned int startColBuffer = (*kiter)->getCtrCol();
    unsigned int startRowBuffer = (*kiter)->getCtrRow();

    int kId = 0;
    for (; kiter != kernelBasisVec.end(); ++kiter, ++citer, ++kId) {

        lsst::fw::Trace("lsst.imageproc.computePSFMatchingKernelForPostageStamp", 3, "Convolving an Object with Basis");

        boost::shared_ptr<lsst::fw::MaskedImage<ImageT, MaskT> > imagePtr(
            new lsst::fw::MaskedImage<ImageT, MaskT>(lsst::fw::kernel::convolve(imageToConvolve, **kiter, threshold, vw::NoEdgeExtension(), -1))
            );

        lsst::fw::Trace("lsst.imageproc.computePSFMatchingKernelForPostageStamp", 3, "Convolved an Object with Basis");

        *citer = imagePtr;
        
        imagePtr->writeFits( (boost::format("cFits_%d") % kId).str() );
    } 

    // Figure out how big the convolved images are
    // NOTE : getCtrRow() pixels are subtracted from the left side, and getRows()-getCtrRow() from the right side
    //        so we start at startRowBuffer = getCtrRow() and go getRows() pixels
    //        this method will effectively accomplish grabbing the right subset of data from the images
    citer = convolvedImageVec.begin();
    unsigned int cCols = (*citer)->getCols();
    unsigned int cRows = (*citer)->getRows();

    // An accessor for each convolution plane
    // NOTE : MaskedPixelAccessor has no empty constructor, therefore we need to push_back()
    vector<lsst::fw::MaskedPixelAccessor<ImageT, MaskT> > convolvedAccessorRowVec;
    for (citer = convolvedImageVec.begin(); citer != convolvedImageVec.end(); ++citer) {
        convolvedAccessorRowVec.push_back(lsst::fw::MaskedPixelAccessor<ImageT, MaskT>(**citer));
    }

    // An accessor for each input image; address rows and cols separately
    lsst::fw::MaskedPixelAccessor<ImageT, MaskT> imageToConvolveRow(imageToConvolve);
    lsst::fw::MaskedPixelAccessor<ImageT, MaskT> imageToNotConvolveRow(imageToNotConvolve);

    // Take into account buffer for kernel images
    imageToConvolveRow.advance(startColBuffer, startRowBuffer);
    imageToNotConvolveRow.advance(startColBuffer, startRowBuffer);

    // integral over image's dx and dy
    // need to take into account that the convolution has reduced the size of the convovled image somewhat
    // therefore we'll use the kernels to drive the size
    for (unsigned int row = 0; row < cRows; row++) {

        // An accessor for each convolution plane
        vector<lsst::fw::MaskedPixelAccessor<ImageT, MaskT> > convolvedAccessorColVec = convolvedAccessorRowVec;

        // An accessor for each input image; places the col accessor at the correct row
        lsst::fw::MaskedPixelAccessor<ImageT, MaskT> imageToConvolveCol = imageToConvolveRow;
        lsst::fw::MaskedPixelAccessor<ImageT, MaskT> imageToNotConvolveCol = imageToNotConvolveRow;

        for (unsigned int col = 0; col < cCols; col++) {

            lsst::fw::Trace("lsst.imageproc.computePSFMatchingKernelForPostageStamp", 5, 
                            boost::format("Accessing image row %d col %d") % (row+startRowBuffer) % (col+startColBuffer));
            lsst::fw::Trace("lsst.imageproc.computePSFMatchingKernelForPostageStamp", 5, 
                            boost::format("Accessing convolved row %d col %d") % row % col);
            
            ImageT ncCamera = *imageToNotConvolveCol.image;
            ImageT ncVariance = *imageToNotConvolveCol.variance;

            // Its an approximation to use this since you don't really know the kernel yet
            // You really want the post-convolved variance but this is close enough
            ImageT cVariance = *imageToConvolveCol.variance;
            
            // Quicker computation
            //double iVariance = 1.0 / (ncVariance + cVariance);
            // NOTE : ignore variance for now
            double iVariance = 1.0;

            // kernel index i
            typename vector<lsst::fw::MaskedPixelAccessor<ImageT, MaskT> >::iterator kiteri = convolvedAccessorColVec.begin();
            for (int kidxi = 0; kiteri != convolvedAccessorColVec.end(); kiteri++, kidxi++) {
                ImageT cdCamerai = *kiteri->image;
                B[kidxi] += ncCamera * cdCamerai * iVariance;
                cout << "B " << kidxi << " " << ncCamera << " " << cdCamerai << " " << endl;
                
                // kernel index j 
                typename vector<lsst::fw::MaskedPixelAccessor<ImageT, MaskT> >::iterator kiterj = kiteri;
                for (int kidxj = kidxi; kiterj != convolvedAccessorColVec.end(); kiterj++, kidxj++) {
                    ImageT cdCameraj = *kiterj->image;
                    M[kidxi][kidxj] += cdCamerai * cdCameraj * iVariance;
                } 
            } 

            // Here we have a single background term
            B[nParameters-1] += ncCamera * iVariance;
            cout << "B " << nParameters << " " << ncCamera << " " << endl;

            M[nParameters-1][nParameters-1] += 1.0 * iVariance;

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
        
    } // row

    // Fill in rest of M
    for (int kidxi=0; kidxi < nKernelParameters; kidxi++) 
        for (int kidxj=kidxi+1; kidxj < nKernelParameters; kidxj++) 
            M[kidxj][kidxi] = M[kidxi][kidxj];

    cout << "B : " << B << endl;
    cout << "M : " << M << endl;

    // Invert M
    vw::math::Matrix<double> Minv = vw::math::inverse(M);

    // Solve for x in Mx = B
    vw::math::Vector<double> Soln = Minv * B;

    // Worry about translating here...
    for (int ki = 0; ki < nKernelParameters; ki++) {
        kernelCoeffs[ki] = Soln[ki];
    }
}

void lsst::imageproc::getCollectionOfMaskedImagesForPSFMatching(
    vector<lsst::fw::Source> &sourceCollection ///< Vector of sources to use for diffim kernel
    ) {

    // Hack some positions in for /lsst/becker/lsst_devel/DC2/fw/tests/data/871034p_1_MI_img.fits
    //lsst::fw::Source src1(1, 1010.345, 2375.548, 10., 10.); 
    //lsst::fw::Source src2(2, 404.248, 573.398, 10., 10.);
    //lsst::fw::Source src3(3, 1686.743, 1880.935, 10., 10.);

    // HACK EVEN MORE; i'm using convolved images and the centers have shifted
    lsst::fw::Source src1(1, 1010.345-3, 2375.548-3, 10., 10.); 
    lsst::fw::Source src2(2, 404.248-3, 573.398-3, 10., 10.);
    lsst::fw::Source src3(3, 1686.743-3, 1880.935-3, 10., 10.);

    sourceCollection.push_back(src1);
    sourceCollection.push_back(src2);
    sourceCollection.push_back(src3);
}

template <typename KernelT>
void lsst::imageproc::computePCAKernelBasis(
    vector<lsst::fw::LinearCombinationKernel<KernelT> > const &kernelVec, ///< Original input kernel basis set
    vector<boost::shared_ptr<lsst::fw::Kernel<KernelT> > > &kernelPCABasisVec ///< Output principal components as kernel images
    ) {
    
    //typedef double CalcT;
    //typedef float ImageT;  // Watch out with multiple ImageT definitions!

    const int nKernel = kernelVec.size();
    const int nCols = kernelVec[0].getCols();
    const int nRows = kernelVec[0].getRows();
    const int nPixels = nCols * nRows;

    lsst::fw::Trace("lsst.imageproc.computePCAKernelBasis", 2, "Entering subroutine computePCAKernelBasis");

    // Matrix to invert.  Number of rows = number of pixels; number of columns = number of kernels
    // All calculations here are in double
    vw::Matrix<double> M(nPixels, nKernel); 

    typedef typename vw::ImageView<KernelT>::pixel_accessor imageAccessorType;

    // iterator over kernels
    typename vector<lsst::fw::LinearCombinationKernel<KernelT> >::const_iterator kiter = kernelVec.begin();

    // fill up matrix for PCA
    // does it matter if i use kiter++ or ++kiter?
    for (int ki = 0; kiter != kernelVec.end(); ki++, kiter++) {
        lsst::fw::Image<KernelT> kImage = kiter->getImage();

        //assert(nRows == kImage.getRows());
        //assert(nCols == kImage.getCols());
        int mIdx = 0;

        imageAccessorType imageAccessorCol(kImage.origin());
        for (int col = 0; col < nCols; col++) {

            imageAccessorType imageAccessorRow(imageAccessorCol);
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

                M(mIdx, ki) = *imageAccessorRow;
                imageAccessorRow.next_row();
            }
            imageAccessorCol.next_col();
        }
    }

    vw::math::Matrix<double> eVec(nPixels, nKernel);
    vw::math::Vector<double> eVal(nKernel);
    vw::math::Vector<double> mMean(nPixels);

    lsst::fw::Trace("lsst.imageproc.computePCAKernelBasis", 4, "Computing pricipal components");
    lsst::imageproc::computePCA(M, mMean, eVal, eVec, true);
    lsst::fw::Trace("lsst.imageproc.computePCAKernelBasis", 4, "Computed pricipal components");

    // turn each eVec into an Image and then into a Kernel
    for (unsigned int ki = 0; ki < eVec.cols(); ki++) {
        lsst::fw::Image<KernelT> basisImage(nCols, nRows);

        // Not sure how to bulk load information into Image
        int kIdx = 0;

        imageAccessorType imageAccessorCol(basisImage.origin());
        for (int col = 0; col < nCols; col++) {
            
            imageAccessorType imageAccessorRow(imageAccessorCol);
            for (int row = 0; row < nRows; row++, kIdx++) {

                *imageAccessorRow = eVec(kIdx, ki);
                imageAccessorRow.next_row();
            }
            imageAccessorCol.next_col();
        }
        // debugging info
        basisImage.writeFits( (boost::format("eFits_%d.fits") % ki).str() );

        kernelPCABasisVec.push_back(boost::shared_ptr<lsst::fw::Kernel<KernelT> > (new lsst::fw::FixedKernel<KernelT>(basisImage)));
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


