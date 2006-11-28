#!/usr/bin/env python
"""
test7.fic
                                                                                
Description
    Seventh test of basic python module access within WCS pipeline.
    Testing star match and WCS build on one CCD within MEF image.
                                                                                
"""
import os
import sys
from lsst.apps.fw.Image import MosaicImage
from lsst.apps.fw.Collection import *
from lsst.apps.imageproc.WCS import *
import RO.DS9

print "Test star match and WCS build on one CCD within FIC 642538p.fic.\nStart test7.fic..."

# Acquire the test image from system directory or local directory
try:
    testData = os.environ['LSSTProto'] +\
                          '/tests/WCS/data/642538p.fic'
except:
    testData = "./642538p.fic"

# Acquire mosaic configuration file from system or local directory
#
#    N O T E: both the mosaic and ccd conf files need to be tuned to the
#             detector specifics
#
#             The mosaic conf file defines the CCD conf file to be used.
try:
    mosaicConfFile = os.environ['LSSTProto'] +\
                          '/etc/CFHT12K_Mosaic.conf'
    ccdConfFile = os.environ['LSSTProto'] +\
                          '/etc/CFHT12K_CCD.conf'
except:
    mosaicConfFile = "./CFHT12K_Mosaic.conf"
    ccdConfFile = "./CFHT12K_CCD.conf"

# Acquire the CCD Image (and relevant HDU info)
immef=MosaicImage(testData,mosaicConfFile,ccdConfFile)


# Select CCD to process
try:
    ccd7=immef.GetCCDImage(4)
except:
    print 'Test7.fic: ',sys.exc_type,"\n Cause: ",sys.exc_value,"\n"
    sys.exit(1)

# Extract sources from that CCD
try:
    sc7=ccd7.ExtractSources()
except:
    print 'test7.fic: ',sys.exc_type,"\n Cause: ",sys.exc_value,"\n"
    print 'test7.fic FAILED.'
    sys.exit(1)


# Extract fiducial stars from GSC catalog
try:
    scat7=StarCollection(ccd7.GetSkyRegion(), \
    'gsc', nStarMax=500)
except:
    print 'Test7.fic: ',sys.exc_type,"\nCause: ",sys.exc_value,"\n"
    sys.exit(1)
                                                                                
# Display if desired
#ds9=RO.DS9.DS9Win()
#ccd7.Display(ds9)
#sc7.DisplaySources(ds9,flagMask=0, starGalCut=0)

# Initialize  the match class with the source and fiducial star collections
match=StarSourceMatchCollection(scat7, sc7, ccd7.GetMetaData())

# Perform the star match and acquire the transformation parameters
# match the source and fiducial stars; results remain within match instance
try:
    match.StarMatch()
except:
    print 'Test7.fic: ',sys.exc_type,"\nCause: ",sys.exc_value,"\n"
    print ("Test7.fic: Failure: no WCS constructed\n")
    sys.exit(1)
print "End test7.fic"
