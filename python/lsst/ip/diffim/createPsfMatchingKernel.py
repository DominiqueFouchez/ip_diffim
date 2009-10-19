# all the c++ level classes and routines
import diffimLib

# all the other diffim routines
from createKernelFunctor import createKernelFunctor

# all the other LSST packages
import lsst.afw.image.imageLib as afwImage
import lsst.afw.math.mathLib as afwMath

# Most general routine
def createPsfMatchingKernel(templateMaskedImage,
                            scienceMaskedImage,
                            policy,
                            footprints=None):

    # Object to store the KernelCandidates for spatial modeling
    kernelCellSet = afwMath.SpatialCellSet(afwImage.BBox(afwImage.PointI(templateMaskedImage.getX0(),
                                                                         templateMaskedImage.getY0()),
                                                         templateMaskedImage.getWidth(),
                                                         templateMaskedImage.getHeight()),
                                           policy.getInt("sizeCellX"),
                                           policy.getInt("sizeCellY"))
    
    # Object to perform the Psf matching on a source-by-source basis
    kFunctor = createKernelFunctor(policy)

    # Candidate source footprints to use for Psf matching
    if footprints == None:
        footprints = diffimLib.getCollectionOfFootprintsForPsfMatching(templateMaskedImage,
                                                                       scienceMaskedImage,
                                                                       policy)

    # Place candidate footprints within the spatial grid
    for fp in footprints:
        bbox = fp.getBBox()
        xC   = 0.5 * ( bbox.getX0() + bbox.getX1() )
        yC   = 0.5 * ( bbox.getY0() + bbox.getY1() )
        tmi  = afwImage.MaskedImageF(templateMaskedImage,  bbox)
        smi  = afwImage.MaskedImageF(scienceMaskedImage, bbox)
        
        cand = diffimLib.makeKernelCandidate(xC, yC, tmi, smi)
        kernelCellSet.insertCandidate(cand)

    # Create the Psf matching kernel
    KB = diffimLib.fitSpatialKernelFromCandidates(kFunctor, kernelCellSet, policy)
    spatialKernel = KB.first
    spatialBg = KB.second

    return spatialKernel, spatialBg, kernelCellSet


# Specialized routines where I tweak the policy based on what you want done
def createMeanPsfMatchingKernel(templateMaskedImage,
                                scienceMaskedImage,
                                policy):

    policy.set("spatialKernelOrder", 0)
    policy.set("singleKernelClipping", True)
    policy.set("kernelSumClipping", True)
    policy.set("spatialKernelClipping", False)

    return createPsfMatchingKernel(templateMaskedImage, scienceMaskedImage, policy)
