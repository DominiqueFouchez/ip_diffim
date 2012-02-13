import eups
import sys
import os
import optparse
import re
import numpy as num

import lsst.daf.base as dafBase
import lsst.afw.image as afwImage

from lsst.pex.logging import Trace
from lsst.pex.logging import Log
import lsst.meas.algorithms as measAlg

import lsst.ip.diffim as ipDiffim
import lsst.ip.diffim.diffimTools as diffimTools

def main():
    defDataDir   = eups.productDir('afwdata')
    if defDataDir == None:
        print 'Error: afwdata not set up'
        sys.exit(1)
    imageProcDir = eups.productDir('ip_diffim')
    if imageProcDir == None:
        print 'Error: could not set up ip_diffim'
        sys.exit(1)

    defSciencePath  = os.path.join(defDataDir, "DC3a-Sim", "sci", "v26-e0", "v26-e0-c011-a10.sci")
    defTemplatePath = os.path.join(defDataDir, "DC3a-Sim", "sci", "v5-e0", "v5-e0-c011-a10.sci")

    defOutputPath   = 'diffExposure.fits'
    defVerbosity    = 5
    defFwhm         = 3.5
    sigma2fwhm      = 2. * num.sqrt(2. * num.log(2.))

    usage = """usage: %%prog [options] [scienceExposure [templateExposure [outputExposure]]]]

Notes:
- image arguments are paths to Expoure fits files
- image arguments must NOT include the final _img.fits
- the result is science image - template image
- the template exposure is convolved, the science exposure is not
- default scienceExposure=%s
- default templateExposure=%s
- default outputExposure=%s 
""" % (defSciencePath, defTemplatePath, defOutputPath)
    
    parser = optparse.OptionParser(usage)
    parser.add_option('-v', '--verbosity', type=int, default=defVerbosity,
                      help='verbosity of Trace messages')
    parser.add_option('-d', '--display', action='store_true', default=False,
                      help='display the images')
    parser.add_option('-b', '--bg', action='store_true', default=False,
                      help='subtract backgrounds using afw')
    parser.add_option('--fwhmS', type=float,
                      help='Science Image Psf Fwhm (pixel)')
    parser.add_option('--fwhmT', type=float,
                      help='Template Image Psf Fwhm (pixel)')

    (options, args) = parser.parse_args()
    
    def getArg(ind, defValue):
        if ind < len(args):
            return args[ind]
        return defValue
    
    sciencePath     = getArg(0, defSciencePath)
    templatePath    = getArg(1, defTemplatePath)
    outputPath      = getArg(2, defOutputPath)
    
    if sciencePath == None or templatePath == None:
        parser.print_help()
        sys.exit(1)

    print 'Science exposure: ', sciencePath
    print 'Template exposure:', templatePath
    print 'Output exposure:  ', outputPath

    templateExposure = afwImage.ExposureF(templatePath)
    scienceExposure  = afwImage.ExposureF(sciencePath)
    config           = ipDiffim.PsfMatchConfigAL()
    
    fwhmS = defFwhm
    if options.fwhmS:
        if scienceExposure.hasPsf():
            width, height = scienceExposure.getPsf().getKernel().getDimensions()
            psfAttr = measAlg.PsfAttributes(scienceExposure.getPsf(), width//2, height//2)
            s = psfAttr.computeGaussianWidth(psfAttr.ADAPTIVE_MOMENT) # gaussian sigma in pixels
            fwhm = s * sigma2fwhm
            print 'NOTE: Embedded Psf has FwhmS =', fwhm
        print 'USING: FwhmS =', options.fwhmS
        fwhmS = options.fwhmS

    fwhmT = defFwhm
    if options.fwhmT:
        if templateExposure.hasPsf():
            width, height = templateExposure.getPsf().getKernel().getDimensions()
            psfAttr = measAlg.PsfAttributes(templateExposure.getPsf(), width//2, height//2)
            s = psfAttr.computeGaussianWidth(psfAttr.ADAPTIVE_MOMENT) # gaussian sigma in pixels
            fwhm = s * sigma2fwhm
            print 'NOTE: Embedded Psf has FwhmT =', fwhm
        print 'USING: FwhmT =', options.fwhmT
        fwhmT = options.fwhmT

    display = False
    if options.display:
        print 'Display =', options.display
        display = True

    bgSub = False
    if options.bg:
        print 'Background subtract =', options.bg
        bgSub = True

    if options.verbosity > 0:
        print 'Verbosity =', options.verbosity
        Trace.setVerbosity('lsst.ip.diffim', options.verbosity)

    ####
        
    if bgSub:
        diffimTools.backgroundSubtract(config.afwBackgroundConfig,
                                       [templateExposure.getMaskedImage(),
                                        scienceExposure.getMaskedImage()])
    else:
        if config.fitForBackground == False:
            print 'NOTE: no background subtraction at all is requested'

    psfmatch = ipDiffim.ImagePsfMatch(config)
    results  = psfmatch.subtractExposures(templateExposure, scienceExposure,
                                          psfFwhmPixTc = fwhmT, psfFwhmPixTnc = fwhmS)

    differenceExposure = results[0]
    differenceExposure.writeFits(outputPath)

    if False:
        psfMatchingKernel = results[1]
        backgroundModel   = results[2]
        kernelCellSet     = results[3]
        
        diffimTools.writeKernelCellSet(kernelCellSet, psfMatchingKernel, backgroundModel,
                                       re.sub('.fits', '', outputPath))
        
def run():
    Log.getDefaultLog()
    memId0 = dafBase.Citizen_getNextMemId()
    main()
    # check for memory leaks
    if dafBase.Citizen_census(0, memId0) != 0:
        print dafBase.Citizen_census(0, memId0), 'Objects leaked:'
        print dafBase.Citizen_census(dafBase.cout, memId0)

if __name__ == '__main__':
    run()
