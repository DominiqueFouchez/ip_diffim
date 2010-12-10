from .subtractMaskedImages import subtractMaskedImages
from .warpTemplateExposure import warpTemplateExposure

import lsst.pex.logging as pexLog
import lsst.pex.exceptions as pexExcept
import lsst.afw.image as afwImage
import lsst.afw.display.ds9 as ds9

def subtractExposures(exposureToConvolve, exposureToNotConvolve, policy, display=False, frame=0, doWarping = True):
    # Make sure they end up the same dimensions on the sky
    templateWcs    = exposureToConvolve.getWcs() 
    scienceWcs     = exposureToNotConvolve.getWcs()

    # LLC
    templateOrigin = templateWcs.pixelToSky(0, 0)
    scienceOrigin  = scienceWcs.pixelToSky(0, 0)
    # URC
    templateLimit  = templateWcs.pixelToSky(exposureToConvolve.getWidth(),
                                            exposureToConvolve.getHeight())
    scienceLimit   = scienceWcs.pixelToSky(exposureToNotConvolve.getWidth(),
                                           exposureToNotConvolve.getHeight())

    pexLog.Trace("lsst.ip.diffim.subtractExposure", 1,
                 "Template limits : %f,%f -> %f,%f" %
                 (templateOrigin[0], templateOrigin[1],
                  templateLimit[0], templateLimit[1]))
    pexLog.Trace("lsst.ip.diffim.subtractExposure", 1,
                 "Science limits : %f,%f -> %f,%f" %
                 (scienceOrigin[0], scienceOrigin[1],
                  scienceLimit[0], scienceLimit[1]))
                  

    # Remap if they do not line up in size on the sky, and size in pixels
    if ( (templateOrigin != scienceOrigin) or \
         (templateLimit  != scienceLimit)  or \
         (exposureToConvolve.getHeight() != exposureToNotConvolve.getHeight()) or \
         (exposureToConvolve.getWidth()  != exposureToNotConvolve.getWidth()) ):
        if doWarping:
            pexLog.Trace("lsst.ip.diffim.subtractExposure", 1,
                         "Astrometrically registering template to science image")
            exposureToConvolve = warpTemplateExposure(exposureToConvolve,
                                                      exposureToNotConvolve,
                                                      policy.getPolicy("warpingPolicy"))
        else:
            pexLog.Trace("lsst.ip.diffim.subtractExposure", 1,
                         "ERROR: Input images not registered")
            raise RuntimeError, "Input images not registered"
            
    
    maskedImageToConvolve = exposureToConvolve.getMaskedImage()
    maskedImageToNotConvolve = exposureToNotConvolve.getMaskedImage()

    if display:
        ds9.mtv(maskedImageToConvolve, frame=frame)
        ds9.mtv(maskedImageToNotConvolve, frame=frame+1)

    # Subtract their MaskedImages
    try:
        result = subtractMaskedImages(maskedImageToConvolve,
                                      maskedImageToNotConvolve,
                                      policy)
    except:
        pexLog.Trace("lsst.ip.diffim.subtractExposure", 1,
                     "ERROR: Unable to calculate psf matching kernel")
        raise
    else:
        differenceMaskedImage, spatialKernel, spatialBg, kernelCellSet = result
        
    if display:
        ds9.mtv(differenceMaskedImage, frame=frame+2)
        
    
    # Generate an exposure from the results
    differenceExposure = afwImage.ExposureF(differenceMaskedImage, scienceWcs)


    return differenceExposure, spatialKernel, spatialBg, kernelCellSet




