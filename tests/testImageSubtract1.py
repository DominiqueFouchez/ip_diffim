import pdb
import unittest
import lsst.fw.Core.fwLib as fw
from lsst.mwi.policy import Policy
import lsst.detection.detectionLib as detection
import lsst.imageproc.imageprocLib as imageproc
import sys
import numpy as num

###########
#
# Get directives from policy
#

policy = Policy.createPolicy('tests/ImageSubtract_policy.paf')
convolveThreshold = policy.get('convolveThreshold')
edgeMaskBit = policy.get('edgeMaskBit')
kernelRows = policy.get('kernelRows')
kernelCols = policy.get('kernelCols')
kernelSpatialOrder = policy.get('kernelSpatialOrder')
backgroundSpatialOrder = policy.get('backgroundSpatialOrder')
DEBUG_IO = 0

###########
#
# Get objects from clipboard or read them in
#
scienceMaskedImage  = fw.MaskedImageF()
scienceMaskedImage.readFits(sys.argv[1])

templateMaskedImage = fw.MaskedImageF()
templateMaskedImage.readFits(sys.argv[2])

###########
#
# Generate objects from policy directives
#

# create basis vectors
kernelBasisVec = fw.vectorKernelPtrD()
imageproc.generateDeltaFunctionKernelSet_D(kernelRows, kernelCols, kernelBasisVec)

# create output kernel pointer
kernelPtr = imageproc.LinearCombinationKernelPtrTypeD(fw.LinearCombinationKernelD())

# and its function for spatial variation
kernelFunctionPtr = fw.Function2PtrTypeD(fw.PolynomialFunction2D(kernelSpatialOrder))

# and background function
backgroundFunctionPtr = fw.Function2PtrTypeD(fw.PolynomialFunction2D(backgroundSpatialOrder))

###########
#
# Get good footprints
#
footprintList = detection.FootprintContainerT()
if 1:
    imageproc.getCollectionOfMaskedImagesForPsfMatching(footprintList)
else:
    # set detection policy
    policy.set('getCollectionOfFootprintsForPsfMatching.footprintDetectionThreshold', 15000.0)
    imageproc.getCollectionOfFootprintsForPsfMatching_F(templateMaskedImage, scienceMaskedImage, footprintList, policy)

if 0:
    # use c-code
    imageproc.computePsfMatchingKernelForMaskedImage_FDD(templateMaskedImage,
                                                         scienceMaskedImage,
                                                         kernelBasisVec,
                                                         footprintList,
                                                         kernelPtr,
                                                         kernelFunctionPtr,
                                                         backgroundFunctionPtr,
                                                         policy)
else:
    ###########
    #
    # Calculate all individual Kernels
    #

    # This is effectively use case computePsfMatchingKernelForMaskedImage.  Grab it's information from the Policy.
    maximumFootprintResidualMean = policy.get('maximumFootprintResidualMean')
    maximumFootprintResidualVariance = policy.get('maximumFootprintResidualVariance')
    
    diffImContainerList = imageproc.vectorDiffImContainer_D()
    nFootprint = 0
    for iFootprintPtr in footprintList:
        footprintBBox = iFootprintPtr.getBBox()
        print '# Stamp %d = %d,%d -> %d,%d' % (nFootprint,
                                               footprintBBox.min().x(),
                                               footprintBBox.min().y(),
                                               footprintBBox.max().x(),
                                               footprintBBox.max().y())

        templateMaskedImageStampPtr = templateMaskedImage.getSubImage(footprintBBox)
        scienceMaskedImageStampPtr  = scienceMaskedImage.getSubImage(footprintBBox)
        print 'DONE'

        if DEBUG_IO:
            templateMaskedImageStampPtr.writeFits('tFits_%d' % (nFootprint))
            scienceMaskedImageStampPtr.writeFits('sFits_%d' % (nFootprint))


        # have to initialize this with a size here
        kernelCoeffs = fw.vectorD( len(kernelBasisVec) )

        # background is a single number; SWIG returns it here.
        background = imageproc.computePsfMatchingKernelForPostageStamp_FD(templateMaskedImageStampPtr.get(),
                                                                          scienceMaskedImageStampPtr.get(),
                                                                          kernelBasisVec,
                                                                          kernelCoeffs,
                                                                          policy)

        print '# Background %d = %.3f' % (nFootprint, background)
        
        # Best kernel for this footprint
        footprintKernel    = fw.LinearCombinationKernelD(kernelBasisVec, kernelCoeffs)
        footprintKernelPtr = imageproc.LinearCombinationKernelPtrTypeD(footprintKernel)
        # Every time you put something in a pointer you have to do this
        footprintKernel.this.disown()

        # Structure holding information about this footprint and its fit to a kernel
        diffImFootprintContainer = imageproc.DiffImContainer_D()
        diffImFootprintContainer.id = nFootprint
        diffImFootprintContainer.isGood = True
        diffImFootprintContainer.diffImFootprintPtr = iFootprintPtr
        diffImFootprintContainer.diffImKernelPtr = footprintKernelPtr
        diffImFootprintContainer.background = background

        # renormalize the coordinates between -1 and 1?
        # for now, no
        # NOTE - check and make sure these are the correct coords
        center = footprintBBox.center()
        diffImFootprintContainer.colcNorm = center.y()
        diffImFootprintContainer.rowcNorm = center.x()

        # calculate the residual of the subtracted image here
        convolvedImageStamp = fw.convolve(templateMaskedImageStampPtr.get(),
                                          footprintKernelPtr.get(),
                                          convolveThreshold,
                                          edgeMaskBit)

        differenceImageStamp  = scienceMaskedImageStampPtr.get()
    if 0:
        differenceImageStamp -= convolvedImageStamp
        differenceImageStamp -= background


        nGoodPixels = 0
        meanOfResiduals = 0.0
        varianceOfResiduals = 0.0
        nGoodPixels, meanOfResiduals, varianceOfResiduals = imageproc.calculateMaskedImageResiduals_F(differenceImageStamp)
        print '# Diffim stamp %d residual : mean residual = %.3f sigma; RMS = %.3f in sigma; npixels = %d' % (nFootprint,
                                                                                                              meanOfResiduals,
                                                                                                              varianceOfResiduals,
                                                                                                              nGoodPixels)
        

        diffImFootprintContainer.footprintResidualMean = meanOfResiduals
        diffImFootprintContainer.footprintResidualVariance = varianceOfResiduals

        if abs(meanOfResiduals) > maximumFootprintResidualMean:
            diffImFootprintContainer.isGood = False
        if varianceOfResiduals > maximumFootprintResidualVariance:
            diffImFootprintContainer.isGood = False
            
        diffImContainerList.append(diffImFootprintContainer)
        
        if DEBUG_IO:
            kImage, imSum = footprintKernelPtr.get().computeNewImage()
            kImage.writeFits('kFits_%d.fits' % (nFootprint))
            differenceImageStamp.writeFits('dFits_%d' % (nFootprint))

        nFootprint += 1

    print '# Debugging done'
    sys.exit(1)
    
    #
    # Calculate all individual Kernels
    #
    ###########
    #
    # Calculate the basis kernels and their spatial variation
    #

    # Test if you do PCA from Policy
    # For now just do it
    kernelOutBasisList = fw.vectorKernelPtrD()
    imageproc.computePcaKernelBasis_D(diffImContainerList, kernelOutBasisList, policy)
    
    # Compute spatial variation of the kernel
    computeSpatiallyVaryingPsfMatchingKernel_DD(diffImContainerList,
                                                kernelOutBasisList,
                                                kernelPtr,
                                                kernelFunctionPtr,
                                                policy)
    
    #
    # Calculate the basis kernels and their spatial variation
    #
    ###########
    #
    # Compute spatial variation of the background
    #
    backgrounds = []
    variances = []
    position1 = []
    position2 = []

    nGood = 0
    backgroundSum = 0.0
    for iDiffImContainer in diffImContainerList:
        if iDiffImContainer.isGood == False:
            continue

        backgrounds.append(iDiffImContainer.background)
        variances.append(iDiffImContainer.footprintResidualVariance) # This is not entirely correct
        position1.append(iDiffImContainer.colcNorm)
        position2.append(iDiffImContainer.rowcNorm)

        backgroundSum += iDiffImContainer.background
        nGood         += 1

    if nGood == 0:
        # Throw execption; oops!
        sys.exit(1)

    backgrounds = num.array(backgrounds)
    variances   = num.array(variances)
    position1   = num.array(position1)
    position2   = num.array(position2)

    # Set up object to fit
    nSigmaSquared = 1.0
    backgroundFunction = fw.MinimizerFunctionBase2_D(backgrounds, variances, position1, position2, nSigmaSquared, backgroundFunctionPtr)

    # Set up initial guesses; 0th order term is average background, spatial terms zero
    nParameters = backgroundFunctionPtr.getNParameters()
    parameters = num.zeros((nParameters,))
    parameters[0] = backgroundSum / nGood

    # Step size
    stepsize  = num.ones((nParameters,))
    stepsize *= 0.1
    
    # Set up error matrix
    errors = num.zeros((nParameters, 2))
        
    # Minimize!
    fw.minimize_D(backgroundFunction, parameters, stepsize, errors)

    # Set actual function parameters
    backgroundFunctionPtr.setParameters(parameters)

    # Debugging info
    for i in range(len(parameters)):
        utils.Trace(4, 'Fit Background Parameter %d : %f (%f,%f)' % (i, parameters[i], errors[i][0], errors[i][1]))
        
    #
    # Compute spatial variation of the background
    #
    ###########
    #
    # Create final difference image
    #
    if type(spatialKernelPtr) == type(fw.LinearCombinationKernel()):
        differenceImage  = inputImage - fw.convolveLinearD(templateImage, spatialKernelPtr, edgeMaskBit)
    else:
        differenceImage  = inputImage - fw.convolveD(templateImage, spatialKernelPtr, convolveThreshold, edgeMaskBit)
            
    #differenceImage -= backgroundFunctionPtr
