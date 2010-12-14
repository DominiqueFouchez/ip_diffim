#!/usr/bin/env python
import os
import pdb
import sys
import unittest
import lsst.utils.tests as tests
 
import eups
import lsst.afw.image as afwImage
import lsst.afw.math as afwMath
import lsst.ip.diffim as ipDiffim
import lsst.ip.diffim.diffimTools as diffimTools
import lsst.pex.logging as pexLog

import lsst.afw.display.ds9 as ds9

verbosity = 3
pexLog.Trace_setVerbosity('lsst.ip.diffim', verbosity)

display   = False
writefits = False

defDataDir = eups.productDir('afwdata')
if defDataDir:
    defTemplatePath = os.path.join(defDataDir, "DC3a-Sim", "sci", "v5-e0",
                                   "v5-e0-c011-a00.sci")
    defSciencePath = os.path.join(defDataDir, "DC3a-Sim", "sci", "v26-e0",
                                  "v26-e0-c011-a00.sci")

# THIS IS "LEAVE-ONE-OUT" CROSS VALIDATION OF THE SPATIAL KERNEL

class DiffimTestCases(unittest.TestCase):
    def setUp(self):
        if not defDataDir:
            return
        
        self.policy = ipDiffim.createDefaultPolicy()
        self.scienceImage   = afwImage.ExposureF(defSciencePath)
        self.templateImage  = afwImage.ExposureF(defTemplatePath)
        # takes forever to remap the CFHT images
        if defSciencePath.find('CFHT') == -1:
            self.templateImage  = ipDiffim.warpTemplateExposure(self.templateImage,
                                                                self.scienceImage,
                                                                self.policy.getPolicy("warpingPolicy"))
            
        self.scienceMaskedImage = self.scienceImage.getMaskedImage()
        self.templateMaskedImage = self.templateImage.getMaskedImage()
        self.dStats = ipDiffim.ImageStatisticsF()
        
        diffimTools.backgroundSubtract(self.policy.getPolicy("afwBackgroundPolicy"),
                                       [self.templateMaskedImage,
                                        self.scienceMaskedImage])

    def stats(self, cid, diffim, size=5):
        bbox = afwImage.BBox(afwImage.PointI((diffim.getWidth() - size)//2,
                                             (diffim.getHeight() - size)//2),
                             afwImage.PointI((diffim.getWidth() + size)//2,
                                             (diffim.getHeight() + size)//2))
        self.dStats.apply(diffim)
        pexLog.Trace("lsst.ip.diffim.JackknifeResampleKernel", 1,
                     "Candidate %d : Residuals all (%d px): %.3f +/- %.3f" % (cid,
                                                                              self.dStats.getNpix(),
                                                                              self.dStats.getMean(),
                                                                              self.dStats.getRms()))

        
        diffim2 = afwImage.MaskedImageF(diffim, bbox)
        self.dStats.apply(diffim2)
        pexLog.Trace("lsst.ip.diffim.JackknifeResampleKernel", 1,
                     "Candidate %d : Residuals core (%d px): %.3f +/- %.3f" % (cid,
                                                                               self.dStats.getNpix(),
                                                                               self.dStats.getMean(),
                                                                               self.dStats.getRms()))
        
                             
                                             
        
    def assess(self, cand, kFn1, bgFn1, kFn2, bgFn2, frame0):
        tmi   = cand.getMiToConvolvePtr()
        smi   = cand.getMiToNotConvolvePtr()
        
        im1   = afwImage.ImageD(kFn1.getDimensions())
        kFn1.computeImage(im1, False,
                          afwImage.indexToPosition(int(cand.getXCenter())),
                          afwImage.indexToPosition(int(cand.getYCenter())))
        fk1   = afwMath.FixedKernel(im1)
        bg1   = bgFn1(afwImage.indexToPosition(int(cand.getXCenter())),
                      afwImage.indexToPosition(int(cand.getYCenter())))
        d1    = ipDiffim.convolveAndSubtract(tmi, smi, fk1, bg1)

        ####
        
        im2   = afwImage.ImageD(kFn2.getDimensions())
        kFn2.computeImage(im2, False,
                          afwImage.indexToPosition(int(cand.getXCenter())),
                          afwImage.indexToPosition(int(cand.getYCenter())))
        fk2   = afwMath.FixedKernel(im2)
        bg2   = bgFn2(afwImage.indexToPosition(int(cand.getXCenter())),
                      afwImage.indexToPosition(int(cand.getYCenter())))
        d2    = ipDiffim.convolveAndSubtract(tmi, smi, fk2, bg2)

        if display:
            ds9.mtv(tmi, frame=frame0+0)
            ds9.dot("Cand %d" % (cand.getId()), 0, 0, frame=frame0+0)
            
            ds9.mtv(smi, frame=frame0+1)
            ds9.mtv(im1, frame=frame0+2)
            ds9.mtv(d1,  frame=frame0+3)
            ds9.mtv(im2, frame=frame0+4)
            ds9.mtv(d2,  frame=frame0+5)

        pexLog.Trace("lsst.ip.diffim.JackknifeResampleKernel", 1,
                     "Full Spatial Model")
        self.stats(cand.getId(), d1)

        pexLog.Trace("lsst.ip.diffim.JackknifeResampleKernel", 1,
                     "N-1 Spatial Model")
        self.stats(cand.getId(), d2)
            
    def setStatus(self, cellSet, cid, value):
        # ideally
        # cellSet.getCandidateById(id).setStatus(value)
        for cell in cellSet.getCellList():
            for cand in cell.begin(False):
                cand = ipDiffim.cast_KernelCandidateF(cand)
                if (cand.getId() == cid):
                    cand.setStatus(value)
                    return cand

    def jackknifeResample(self, results):
        # do as little re-processing as possible
        self.policy.set("singleKernelClipping", False)
        self.policy.set("kernelSumClipping", False)
        self.policy.set("spatialKernelClipping", False)
        
        kernel, bg, cellSet = results
        basisList   = kernel.getKernelList()
        kFunctor    = ipDiffim.PsfMatchingFunctorF(basisList)
        
        goodList = []
        for cell in cellSet.getCellList():
            print
            for cand in cell.begin(False):
                cand = ipDiffim.cast_KernelCandidateF(cand)

                if cand.getStatus() == afwMath.SpatialCellCandidate.GOOD:
                    goodList.append(cand.getId())
                else:
                    # This is so that UNKNOWNs are not processed
                    cand.setStatus(afwMath.SpatialCellCandidate.BAD)

        for idx in range(len(goodList)):
            cid   = goodList[idx]

            print # clear the screen
            pexLog.Trace("lsst.ip.diffim.JackknifeResampleKernel", 1,
                         "Removing candidate %d" % (cid))
            
            cand = self.setStatus(cellSet, cid, afwMath.SpatialCellCandidate.BAD)

            jkResults = ipDiffim.fitSpatialKernelFromCandidates(kFunctor,
                                                                cellSet,
                                                                self.policy)
            jkKernel  = jkResults[0]
            jkBg      = jkResults[1]

            # lots of windows
            # self.assess(cand, kernel, bg, jkKernel, jkBg, 6*idx+1)

            # only 6 windows
            self.assess(cand, kernel, bg, jkKernel, jkBg, 1)

            self.setStatus(cellSet, cid, afwMath.SpatialCellCandidate.GOOD)
       

    def runTest(self, mode):
        pexLog.Trace("lsst.ip.diffim.JackknifeResampleKernel", 1,
                     "Mode %s" % (mode))
        if mode == "DF":
            self.policy.set("kernelBasisSet", "delta-function")
            self.policy.set("useRegularization", False)
            self.policy.set("usePcaForSpatialKernel", True)
        elif mode == "DFr":
            self.policy.set("kernelBasisSet", "delta-function")
            self.policy.set("useRegularization", True)
            self.policy.set("usePcaForSpatialKernel", True)
        elif mode == "AL":
            self.policy.set("kernelBasisSet", "alard-lupton")
            self.policy.set("useRegularization", False)
            self.policy.set("usePcaForSpatialKernel", False)
        elif mode == "ALp":
            self.policy.set("kernelBasisSet", "alard-lupton")
            self.policy.set("useRegularization", False)
            self.policy.set("usePcaForSpatialKernel", True)
        else:
            raise
        
        results = ipDiffim.psfMatchImageToImage(self.templateMaskedImage,
                                                self.scienceMaskedImage,
                                                self.policy)
        self.jackknifeResample(results)
        
    def test(self):
        if not defDataDir:
            print >> sys.stderr, "Warning: afwdata not set up; not running JackknifeResampleSpatialKernel.py"
            return
        
        self.runTest(mode="DFr")

#####

def suite():
    """Returns a suite containing all the test cases in this module."""
    tests.init()

    suites = []
    suites += unittest.makeSuite(DiffimTestCases)
    suites += unittest.makeSuite(tests.MemoryTestCase)
    return unittest.TestSuite(suites)

def run(doExit=False):
    """Run the tests"""
    tests.run(suite(), doExit)

if __name__ == "__main__":
    if len(sys.argv) > 3:
        defTemplatePath = sys.argv[1]
        defSciencePath  = sys.argv[2]

    if '-d' in sys.argv:
        display = True

    run(True)

# python tests/JackknifeResampleSpatialKernel.py -d
# python tests/JackknifeResampleSpatialKernel.py $AFWDATA_DIR/CFHT/D4/cal-53535-i-797722_1_tmpl
# ... $AFWDATA_DIR/CFHT/D4/cal-53535-i-797722_1 -d
