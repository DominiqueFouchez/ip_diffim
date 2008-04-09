#!/usr/bin/env python
"""
Test lsst.ip.diffim.ip_diffimLib.wcsMatch

Author: Nicole M. Silvestri, University of Washington
Contact: nms@astro.washington.edu
Created on: Thu Sep 20, 2007
"""

import os
import math
import pdb # we may want to say pdb.set_trace()
import unittest

import numpy

import eups
import lsst.afw.Core.afwLib as afw
import lsst.utils.tests as tests
import lsst.pex.logging as logging
import lsst.ip.diffim.ip_diffimLib as improc

try:
    type(verbose)
except NameError:
    verbose = 0
    logging.Trace_setVerbosity("ip.diffim", verbose)

dataDir = os.environ.get("FWDATA_DIR")
if not dataDir:
    raise RuntimeError("Must set up fwData to run these tests")

InputOrigMaskedImageName = "871034p_1_MI"
InputSciMaskedImageName = "871034p_1_MI"
SwarpedMaskedImageName = "small_MISwarp"
RemappedMaskedImageName = "871034p_1_MIRemapped"

inFilePathOrig = os.path.join(dataDir, InputOrigMaskedImageName)
inFilePathSci = os.path.join(dataDir, InputSciMaskedImageName)
inFilePathSwarp = os.path.join(dataDir, SwarpedMaskedImageName)
outFilePathRemap = os.path.join(dataDir, RemappedMaskedImageName)
#-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

class wcsMatchTestCase(unittest.TestCase):
    """
    A test case for wcsMatch utility Function
    """

    def setUp(self):
        # setup original (template) MaskedImage and WCS
        origMaskedImage = fw.MaskedImageD()
        origMaskedImage.readFits(inFilePathOrig)
        self.origWcs = fw.WCS(origMaskedImage.getImage().getMetaData())
        origCols = origMaskedImage.getCols()
        origRows = origMaskedImage.getRows()

        # setup science MaskedImage and WCS. The science WCS will
        # serve as the remapped WCS       
        sciMaskedImage = fw.MaskedImageD()
        sciMaskedImage.readFits(inFilePathSci)
        self.sciWcs = fw.WCS(sciMaskedImage.getImage().getMetaData())

        # setup the remapped and original Exposure.  The remapped
        # MaskedImage is 1/2 kernel width larger than the original
        # Exposure's MaskedImage 
        remapMaskedImage = fw.MaskedImageD(origCols + 1,origRows + 1)
        self.origExposure = fw.ExposureD(origMaskedImage, self.origWcs)
        self.remapExposure = fw.ExposureD(remapMaskedImage, self.sciWcs)

        # input parameters to generate the analytic remapping kernel
        self.kernelType = "lanczos"
        self.kernelCols = 2
        self.kernelRows = 2
        self.threshold = 1


    def tearDown(self):
        del self.origExposure 
        del self.remapExposure
        del self.origWcs
        del self.sciWcs
        del self.kernelType
        del self.kernelCols
        del self.kernelRows
        del self.threshold


    def testWcsMatchVoidEasy(self):
        """
        Test that wcsMatch utility function simply returns a remapped
        Exposure.  In this case, since the input (original and
        science) MaskedImages are the same, the remaped Exposure
        should have the same WCS as the original Exposure.

        Check that the same pixel position on the original MaskedImage
        and the remapped MaskedImage yield the same RA/Decl on the
        sky. 
        """
        improc.wcsMatch(self.remapExposure, self.origExposure, self.kernelType, self.kernelCols, self.kernelRows)

        # try the origin
        colRow = fw.Coord2D(0,0)
        origRaDec = self.origWcs.colRowToRaDec(colRow)
        remapWcs = self.remapExposure.getWcs()
        remapRaDec = remapWcs.colRowToRaDec(colRow)

        self.assertAlmostEqual(origRaDec.x(), remapRaDec.x())
        self.assertAlmostEqual(origRaDec.y(), remapRaDec.y())

        # try a random location
        colRow1 = fw.Coord2D(0,21)
        origRaDec1 = self.origWcs.colRowToRaDec(colRow1)
        remapRaDec1 = remapWcs.colRowToRaDec(colRow1)

        self.assertAlmostEqual(origRaDec1.x(), remapRaDec1.x())
        self.assertAlmostEqual(origRaDec1.y(), remapRaDec1.y())


    def xtestWcsMatchVoidSwarp(self):
        """
        Test that wcsMatch utility function returns a remapped
        Exposure that is equivalent to an Exposure that has been
        remapped using the SWARP routine frm Terapix.  In this case,
        the remaped Exposure should have the same WCS as the SWARPed
        Exposure.

        Check that the same pixel position on the SWARPed MaskedImage
        and the remapped MaskedImage yield the same RA/Decl on the
        sky.  
        """
        improc.wcsMatch(self.remapExposure, self.origExposure, self.kernelType, self.kernelCols, self.kernelRows)

        swarpMaskedImage = fw.MaskedImageD()
        swarpMaskedImage.readFits(inFilePathSwarp)
        swarpWcs = fw.WCS(swarpMaskedImage.getImage().getMetaData())

        remapWcs = remapExposure.getWcs()
        remapRaDec = remapWcs.colRowToRaDec(colRow)

        # try the origin
        colRow = fw.Coord2D(0,0)
        swarpRaDec = swarpWcs.colRowToRaDec(colRow)
        remapRaDec = remapWcs.colRowToRaDec(colRow)

        self.assertAlmostEqual(swarpRaDec.x(), remapRaDec.x())
        self.assertAlmostEqual(swarpRaDec.y(), remapRaDec.y())

        # try a random location
        colRow1 = fw.Coord2D(12,21)
        swarpRaDec1 = swarpWcs.colRowToRaDec(colRow1)
        remapRaDec1 = remapWcs.colRowToRaDec(colRow1)

        self.assertAlmostEqual(swarpRaDec1.x(), remapRaDec1.x())
        self.assertAlmostEqual(swarpRaDec1.y(), remapRaDec1.y())

                    
    def xtestWcsMatchOverloadEasy(self):
        """        
        Test that wcsMatch overloaded utility function simply returns
        a remapped Exposure.  In this case, since the input (original
        and science) MaskedImages are the same, the remaped Exposure
        should have the same WCS as the original Exposure.

        Check that the same pixel position on the original MaskedImage
        and the remapped MaskedImage yield the same RA/Decl on the
        sky. 
        """
        remapWcs = self.remapExposure.getWcs()
        remapMaskedImage = self.remapExposure.getMaskedImage()
        remapCols = remapMaskedImage.getCols()
        remapRows = remapMaskedImage.getRows()

        newRemapExposure = improc.wcsMatch(remapWcs, remapCols, remapRows, self.origExposure,
                                           self.kernelType, self.kernelCols, self.kernelRows)

        # try the origin
        colRow = fw.Coord2D(0,0)
        origRaDec = origWcs.colRowToRaDec(colRow)
        newRemapWcs = newRemapExposure.getWcs()
        newRemapRaDec = newRemapWcs.colRowToRaDec(colRow)

        self.assertAlmostEqual(origRaDec.x(), newRemapRaDec.x())
        self.assertAlmostEqual(origRaDec.y(), newRemapRaDec.y())

        # try a random location
        colRow1 = fw.Coord2D(21,12)
        origRaDec1 = origWcs.colRowToRaDec(colRow1)
        newRemapRaDec1 = newRemapWcs.colRowToRaDec(colRow1)

        self.assertAlmostEqual(origRaDec1.x(), newRemapRaDec1.x())
        self.assertAlmostEqual(origRaDec1.y(), newRemapRaDec1.y())


    def xtestWcsMatchOverloadSwarp(self):
        """
        Test that wcsMatch overload utility function returns a
        remapped Exposure that is equivalent to an Exposure that has
        been remapped using the SWARP routine frm Terapix.  In this
        case, the remaped Exposure should have the same WCS as the
        SWARPed Exposure.

        Check that the same pixel position on the SWARPed MaskedImage
        and the remapped MaskedImage yield the same RA/Decl on the
        sky.      
        """
        remapWcs = self.remapExposure.getWcs()
        remapMaskedImage = self.remapExposure.getMaskedImage()
        remapCols = remapMaskedImage.getCols()
        remapRows = remapMaskedImage.getRows()

        newRemapExposure = improc.wcsMatch(remapWcs, remapCols, remapRows, self.origExposure, self.kernelType, self.kernelCols, self.kernelRows)

        swarpMaskedImage = fw.MaskedImageD()
        swarpMaskedImage.readFits(inFilePathSwarp)
        swarpWcs = fw.WCS(swarpMaskedImage.getImage().getMetaData())

        newRemapWcs = newRemapExposure.getWcs()
        newRemapRaDec = newRemapWcs.colRowToRaDec(colRow)

        # try the origin
        colRow = fw.Coord2D(0,0)
        swarpRaDec = swarpWcs.colRowToRaDec(colRow)
        newRemapRaDec = newRemapWcs.colRowToRaDec(colRow)

        self.assertAlmostEqual(swarpRaDec.x(), newRemapRaDec.x())
        self.assertAlmostEqual(swarpRaDec.y(), newRemapRaDec.y())

        # try a random location
        colRow1 = fw.Coord2D(12,21)
        swarpRaDec1 = swarpWcs.colRowToRaDec(colRow1)
        newRemapRaDec1 = newRemapWcs.colRowToRaDec(colRow1)

        self.assertAlmostEqual(swarpRaDec1.x(), newRemapRaDec1.x())
        self.assertAlmostEqual(swarpRaDec1.y(), newRemapRaDec1.y())
        
#-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

def suite():
    """
    Returns a suite containing all the test cases in this module.
    """
    tests.init()

    suites = []
    suites += unittest.makeSuite(wcsMatchTestCase)
    suites += unittest.makeSuite(tests.MemoryTestCase)

    return unittest.TestSuite(suites)

def run(exit=False):
    """Run the tests"""
    tests.run(suite(), exit)

if __name__ == "__main__":
    run(True)
