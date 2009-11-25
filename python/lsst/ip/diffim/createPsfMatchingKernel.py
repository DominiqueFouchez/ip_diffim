# all the c++ level classes and routines
import diffimLib

# all the other diffim routines
from createKernelFunctor import createKernelFunctor

# all the other LSST packages
import lsst.afw.image.imageLib as afwImage
import lsst.afw.math.mathLib as afwMath
import lsst.pex.logging as pexLog
import lsst.pex.exceptions as pexExcept
import lsst.meas.algorithms as measAlgorithms

display = True
if display:
    import lsst.afw.display.ds9 as ds9
    import diffimTools

# Most general routine
def createPsfMatchingKernel(maskedImageToConvolve,
                            maskedImageToNotConvolve,
                            policy,
                            footprints=None):

    
    # Object to store the KernelCandidates for spatial modeling
    kernelCellSet = afwMath.SpatialCellSet(afwImage.BBox(afwImage.PointI(maskedImageToConvolve.getX0(),
                                                                         maskedImageToConvolve.getY0()),
                                                         maskedImageToConvolve.getWidth(),
                                                         maskedImageToConvolve.getHeight()),
                                           policy.getInt("sizeCellX"),
                                           policy.getInt("sizeCellY"))
    
    # Candidate source footprints to use for Psf matching
    if footprints == None:
        footprints = diffimLib.getCollectionOfFootprintsForPsfMatching(maskedImageToConvolve,
                                                                       maskedImageToNotConvolve,
                                                                       policy)

    # Place candidate footprints within the spatial grid
    for fp in footprints:
        bbox = fp.getBBox()
        xC   = 0.5 * ( bbox.getX0() + bbox.getX1() )
        yC   = 0.5 * ( bbox.getY0() + bbox.getY1() )
        tmi  = afwImage.MaskedImageF(maskedImageToConvolve,  bbox)
        smi  = afwImage.MaskedImageF(maskedImageToNotConvolve, bbox)

        cand = diffimLib.makeKernelCandidate(xC, yC, tmi, smi)
        kernelCellSet.insertCandidate(cand)

    # Object to perform the Psf matching on a source-by-source basis
    kFunctor = createKernelFunctor(policy)

    # Create the Psf matching kernel
    try:
        KB = diffimLib.fitSpatialKernelFromCandidates(kFunctor, kernelCellSet, policy)
    except pexExcept.LsstCppException, e:
        pexLog.Trace("lsst.ip.diffim.createPsfMatchingKernel", 1,
                     "ERROR: Unable to calculate psf matching kernel")
        pexLog.Trace("lsst.ip.diffim.createPsfMatchingKernel", 2,
                     e.args[0].what())
        raise
    else:
        spatialKernel = KB.first
        spatialBg     = KB.second

    # What is the status of the processing?
    nGood = 0
    for cell in kernelCellSet.getCellList():
        for cand in cell.begin(True):
            cand = diffimLib.cast_KernelCandidateF(cand)
            if cand.getStatus() == afwMath.SpatialCellCandidate.GOOD:
                nGood += 1
    if nGood == 0:
        pexLog.Trace("lsst.ip.diffim.createPsfMatchingKernel", 1, "WARNING")
    pexLog.Trace("lsst.ip.diffim.createPsfMatchingKernel", 1,
                 "Used %d kernels for spatial fit" % (nGood))
                 
    return spatialKernel, spatialBg, kernelCellSet


# Matching an image to a Gaussian model
# This requires sending footprints since we can only accept stars
# In this case, maskedImageToConvolve is played by the science image
def createPsfMatchingKernelToGaussian(maskedImageToConvolve,
                                      sigGauss,
                                      policy,
                                      footprints):

    # We must use alard-lupton here because the image we are applying
    # the kernel to is noisy
    #policy.set("kernelBasisSet", "alard-lupton")
    # This is for debugging
    policy.set("spatialKernelClipping", False)

    # TO BE SET IN POLICY LATER
    centroidAlgorithm = "SDSS"
    policy.set("centroidAlgorithm", centroidAlgorithm)

    photometryAlgorithm = "SINC"
    policy.set("photometryAlgorithm", photometryAlgorithm)

    photometryAperture = 10.
    policy.set("photometryAperture", photometryAperture)

    lanczOrder  = 5
    policy.set("lanczOrder", lanczOrder)

    maxCentroidShift  = 0.05
    policy.set("maxCentroidShift", maxCentroidShift)


    # Idealized function you will match all the stars to
    gaussFunc   = afwMath.GaussianFunction2D(sigGauss, sigGauss)

    # Centroider to be able to shift this Gaussian to align at the
    # sub-pixel level with the star
    centroider  = measAlgorithms.createMeasureCentroid(policy.get("centroidAlgorithm"))
    lanczOrder  = policy.get("lanczOrder")
    lanczSize   = 2 * lanczOrder + 1

    # Need to measure the flux of the star to scale the gaussian
    # PSF IS A HACK FOR NOW
    psf        = measAlgorithms.createPSF("DoubleGaussian", 15, 15, 5.)
    photometer = measAlgorithms.createMeasurePhotometry(policy.get("photometryAlgorithm"),
                                                        policy.get("photometryAperture"))

    # Object to store the KernelCandidates for spatial modeling
    kernelCellSet = afwMath.SpatialCellSet(afwImage.BBox(afwImage.PointI(maskedImageToConvolve.getX0(),
                                                                         maskedImageToConvolve.getY0()),
                                                         maskedImageToConvolve.getWidth(),
                                                         maskedImageToConvolve.getHeight()),
                                           policy.getInt("sizeCellX"),
                                           policy.getInt("sizeCellY"))

    # Place candidate footprints within the spatial grid
    for fp in footprints:
        bbox = fp.getBBox()
        xC   = 0.5 * ( bbox.getX0() + bbox.getX1() )
        yC   = 0.5 * ( bbox.getY0() + bbox.getY1() )
        tmi  = afwImage.MaskedImageF(maskedImageToConvolve,  bbox)

        # Find object flux so we can center the Gaussian
        phot  = photometer.apply(maskedImageToConvolve, xC, yC, psf, 0.0)
        flux  = phot.getApFlux()
        
        # The object is not centered on a pixel
        # Offsets are defined such that afwMath.offsetImage will shift the pixel
        #   along the positive x/y axis for positive dx/dy
        cen   = centroider.apply(maskedImageToConvolve.getImage(), int(xC), int(yC))
        dx    = cen.getX() - xC
        dy    = cen.getY() - yC

        # Evaluate the gaussian slightly shifted
        kImageG     = afwImage.ImageD(tmi.getWidth(), tmi.getHeight())
        for y in range(tmi.getHeight()):
            yg = tmi.getHeight()//2 - y
            
            for x in range(tmi.getWidth()):
                xg = tmi.getWidth()//2 - x
                
                kImageG.set(x, y, gaussFunc(xg+dx, yg+dy))

        # Ticket 1040        
        # kImage2 = kImage.convertFloat() 
        # kImage  = kImage2
        foo = diffimTools.vectorFromImage(kImageG)
        foo2 = diffimTools.imageFromVector(foo, kImageG.getWidth(), kImageG.getHeight())
        kImage = foo2
    
        # Did our centroiding work out?
        cTest1 = afwImage.ImageF(tmi.getImage(), True)
        cTest1.setXY0(afwImage.PointI(0, 0))
        cen1   = centroider.apply(cTest1, cTest1.getWidth()//2, cTest1.getHeight()//2)
        cTest2 = afwImage.ImageF(kImage, True)
        cTest2.setXY0(afwImage.PointI(0, 0))
        cen2   = centroider.apply(cTest2, cTest2.getWidth()//2, cTest2.getHeight()//2)
        pexLog.Trace("lsst.ip.diffim.createPsfMatchingKernelToGaussian", 5,
                     "Resulting centroids : %.2f,%.2f vs. %.2f,%.2f" % (cen1.getX(), cen1.getY(),
                                                                        cen2.getX(), cen2.getY()))
        if abs(cen1.getX() - cen2.getX()) > maxCentroidShift:
            pexLog.Trace("lsst.ip.diffim.createPsfMatchingKernelToGaussian", 3,
                         "X offset too large (%.2f > %.2f), rejecting" % (abs(cen1.getX() - cen2.getX()),
                                                                          maxCentroidShift))
            continue
                        
        if abs(cen1.getY() - cen2.getY()) > maxCentroidShift:
            pexLog.Trace("lsst.ip.diffim.createPsfMatchingKernelToGaussian", 3,
                         "Y offset too large (%.2f > %.2f), rejecting" % (abs(cen1.getY() - cen2.getY()),
                                                                          maxCentroidShift))
            continue
                         
            
        
        # Create a masked image that your input image will be matched to
        # Empty mask and zero variance
        smi  = afwImage.MaskedImageF(kImage)

        # Scale the Gaussian to have the same flux as the star
        #
        # Yikes, this is a bit scary.  We are asking that the flux of
        # the Gaussian in N pixels is the same as the flux of the star
        # in those same N pixels.  When they are different FWHMs.  Hmm...
        
        phot = photometer.apply(smi, smi.getWidth()//2, smi.getHeight()//2, psf, 0.0)
        smi *= flux / phot.getApFlux()
        pexLog.Trace("lsst.ip.diffim.createPsfMatchingKernelToGaussian", 5,
                     "Scaling gaussian by %.2f" % (flux / phot.getApFlux()))


        cand = diffimLib.makeKernelCandidate(xC, yC, tmi, smi)
        kernelCellSet.insertCandidate(cand)

        if display:
            ds9.mtv(tmi, frame=1)
            ds9.mtv(smi, frame=2)
            ds9.mtv(kImageG, frame=3)


    # Object to perform the Psf matching on a source-by-source basis
    kFunctor = createKernelFunctor(policy)

    # Create the Psf matching kernel
    try:
        KB = diffimLib.fitSpatialKernelFromCandidates(kFunctor, kernelCellSet, policy)
    except pexExcept.LsstCppException, e:
        pexLog.Trace("lsst.ip.diffim.createPsfMatchingKernel", 1,
                     "ERROR: Unable to calculate psf matching kernel")
        pexLog.Trace("lsst.ip.diffim.createPsfMatchingKernel", 2,
                     e.args[0].what())
        raise
    else:
        spatialKernel = KB.first
        spatialBg     = KB.second

    # What is the status of the processing?
    nGood = 0
    for cell in kernelCellSet.getCellList():
        for cand in cell.begin(True):
            cand = diffimLib.cast_KernelCandidateF(cand)
            if cand.getStatus() == afwMath.SpatialCellCandidate.GOOD:
                nGood += 1
    if nGood == 0:
        pexLog.Trace("lsst.ip.diffim.createPsfMatchingKernel", 1, "WARNING")
    pexLog.Trace("lsst.ip.diffim.createPsfMatchingKernel", 1,
                 "Used %d kernels for spatial fit" % (nGood))

    if display:
        diffimTools.displayCandidateMosaic(kernelCellSet, 1)
        diffimTools.displayBasisMosaic(spatialKernel, 2)
        diffimTools.displaySpatialKernelMosaic(spatialKernel,
                                               maskedImageToConvolve.getWidth(),
                                               maskedImageToConvolve.getHeight(), 3)
        diffimTools.displaySpatialKernelQuality(kernelCellSet, spatialKernel, spatialBg, frame=4)
                 
    return spatialKernel, spatialBg, kernelCellSet


# Specialized routines where I tweak the policy based on what you want done
def createMeanPsfMatchingKernel(maskedImageToConvolve,
                                maskedImageToNotConvolve,
                                policy):

    policy.set("spatialKernelOrder", 0)
    policy.set("singleKernelClipping", True)
    policy.set("kernelSumClipping", True)
    policy.set("spatialKernelClipping", False)

    return createPsfMatchingKernel(maskedImageToConvolve, maskedImageToNotConvolve, policy)
