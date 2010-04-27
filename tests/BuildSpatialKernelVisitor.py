#!/usr/bin/env python
import os, sys
import unittest
import lsst.utils.tests as tests

import eups
import lsst.afw.image as afwImage
import lsst.afw.math as afwMath
import lsst.ip.diffim as ipDiffim
import lsst.ip.diffim.diffimTools as diffimTools
import lsst.pex.logging as pexLog
import lsst.afw.display.ds9 as ds9

import numpy
import pylab

diffimDir    = eups.productDir('ip_diffim')
diffimPolicy = os.path.join(diffimDir, 'pipeline', 'ImageSubtractStageDictionary.paf')

pexLog.Trace_setVerbosity('lsst.ip.diffim', 5)
display = True
class DiffimTestCases(unittest.TestCase):
    
    def setUp(self):
        self.policy = ipDiffim.generateDefaultPolicy(diffimPolicy)
        self.kSize = 11
        self.policy.set("kernelRows", self.kSize)
        self.policy.set("kernelCols", self.kSize)
        self.sizeCell = 64
        self.policy.set("sizeCellX", self.sizeCell)
        self.policy.set("sizeCellY", self.sizeCell)

        nGauss = 1
        wGauss = [2.5,]
        self.policy.set('alardNGauss', nGauss)
        self.policy.set('alardSigGauss', wGauss[0])
        self.policy.set('alardDegGauss', 0)
        for i in range(1, len(wGauss)):
            self.policy.add('alardSigGauss', wGauss[i])
            self.policy.add('alardDegGauss', 0)

    def tearDown(self):
        del self.policy

    def xtestSolve(self):
        pass

    def testDeltaFunction(self):
        self.policy.set('kernelBasisSet', 'delta-function')
        self.policy.set('useRegularization', False)
        self.runGaussianField(0, title = 'DF')

    def testAlardLupton(self):
        self.policy.set('kernelBasisSet', 'alard-lupton')
        self.policy.set('useRegularization', False)
        self.runGaussianField(0, title = 'AL')

    def testRegularization(self):
        self.policy.set('kernelBasisSet', 'delta-function')
        self.policy.set('useRegularization', True)
        self.runGaussianField(0, title = 'DFr')

    def testShow(self):
        pylab.show()  
    
    def runGaussianField(self, order, title):
        self.policy.set('spatialKernelOrder', order)
        
        # set up basis list
        nGauss = self.policy.get('alardNGauss')
        wGauss = self.policy.getDoubleArray('alardSigGauss')
        basisList = afwMath.KernelList()
        for i in range(nGauss):
            gaussFunction = afwMath.GaussianFunction2D(wGauss[i], wGauss[i])
            gaussKernel   = afwMath.AnalyticKernel(self.kSize, self.kSize, gaussFunction)
            basisList.append(gaussKernel)

        # first kernel has no spatial variation; the others have no kernel sum
        basisList = afwMath.KernelList(ipDiffim.renormalizeKernelList(basisList))

        tMi, sMi, sKernel, kernelCellSet = diffimTools.makeFakeKernelSet(self.policy,
                                                                         basisList,
                                                                         nCell = 5,
                                                                         addNoise=True,
                                                                         bgValue = 1.e2)

        # single kernel visitor
        basisListToFit = ipDiffim.makeKernelBasisList(self.policy)
        
        if self.policy.get("useRegularization"):
            hMat = ipDiffim.makeRegularizationMatrix(self.policy)
            bskv = ipDiffim.BuildSingleKernelVisitorF(basisListToFit, self.policy, hMat)
        else:
            bskv = ipDiffim.BuildSingleKernelVisitorF(basisListToFit, self.policy)

        # visit candidates by hand
        frame = 7
        for cell in kernelCellSet.getCellList():
            for cand in cell.begin(False): # False = include bad candidates
                cand = ipDiffim.cast_KernelCandidateF(cand)
                bskv.processCandidate(cand)

        s1o, s1i = self.accumulateDiffimStats(kernelCellSet, title)
        s2o, s2i = self.compareNeighbors(kernelCellSet, title)
        print '# CAW', title, s1i.mean(), s1i.std(), s1o.mean(), s1o.std(), \
              s2i.mean(), s2i.std(), s2o.mean(), s2o.std()


    def compareNeighbors(self, kernelCellSet, title):
        # lotsa permutations here...
        allStats1 = []
        allStats2 = []
        for cell1 in kernelCellSet.getCellList():
            for cand1 in cell1.begin(False):
                cand1   = ipDiffim.cast_KernelCandidateF(cand1)
                kernel1 = cand1.getKernel(ipDiffim.KernelCandidateF.ORIG)

                for cell2 in kernelCellSet.getCellList():
                    for cand2 in cell2.begin(False):
                        cand2  = ipDiffim.cast_KernelCandidateF(cand2)
                        if cand1.getId() == cand2.getId():
                            continue

                        diffim = cand2.getDifferenceImage(kernel1, 0.0)
                        itcv   = cand2.getMiToConvolvePtr().getVariance()
                        itncv  = cand2.getMiToNotConvolvePtr().getVariance()

                        p0, p1 = diffimTools.getConvolvedImageLimits(kernel1, diffim)
                        bbox   = afwImage.BBox(p0, p1)
                        diffim = afwImage.MaskedImageF(diffim, bbox)
                        itcv   = afwImage.ImageF(itcv, bbox)
                        itncv  = afwImage.ImageF(itncv, bbox)
                
                        pval   = diffimTools.vectorFromImage(diffim.getImage())
                        vval   = diffimTools.vectorFromImage(diffim.getVariance())
                        itcv   = diffimTools.vectorFromImage(itcv)
                        itncv  = diffimTools.vectorFromImage(itncv)
                        
                        allStats1.append( pval / numpy.sqrt(vval) )
                        allStats2.append( pval / numpy.sqrt(itcv + itncv) )
                        


        allStats1 = numpy.ravel(allStats1) 
        pylab.figure()
        n, b, p = pylab.hist(allStats1, bins=100, normed=True)
        pylab.title('%s : For neighbors (output variance)' % (title))
        pylab.xlabel('N Sigma')
        pylab.ylabel('N Pix')
        
        allStats2 = numpy.ravel(allStats2) 
        pylab.figure()
        n, b, p = pylab.hist(allStats2, bins=100, normed=True)
        pylab.title('%s : For neighbors (input variance)' % (title))
        pylab.xlabel('N Sigma')
        pylab.ylabel('N Pix')
        return allStats1, allStats2

    def accumulateDiffimStats(self, kernelCellSet, title):
        allStats1 = []
        allStats2 = []
        for cell in kernelCellSet.getCellList():
            for cand in cell.begin(False): # False = include bad candidates
                cand = ipDiffim.cast_KernelCandidateF(cand)
                kernel = cand.getKernel(ipDiffim.KernelCandidateF.ORIG)
                diffim = cand.getDifferenceImage(ipDiffim.KernelCandidateF.ORIG)
                itcv   = cand.getMiToConvolvePtr().getVariance()
                itncv  = cand.getMiToNotConvolvePtr().getVariance()
                
                
                p0, p1 = diffimTools.getConvolvedImageLimits(kernel, diffim)
                bbox   = afwImage.BBox(p0, p1)
                diffim = afwImage.MaskedImageF(diffim, bbox)
                itcv   = afwImage.ImageF(itcv, bbox)
                itncv  = afwImage.ImageF(itncv, bbox)
                
                pval   = diffimTools.vectorFromImage(diffim.getImage())
                vval   = diffimTools.vectorFromImage(diffim.getVariance())
                itcv   = diffimTools.vectorFromImage(itcv)
                itncv  = diffimTools.vectorFromImage(itncv)

                allStats1.append( pval / numpy.sqrt(vval) )
                allStats2.append( pval / numpy.sqrt(itcv + itncv) )

        allStats1 = numpy.ravel(allStats1) 
        pylab.figure()
        n, b, p = pylab.hist(allStats1, bins=100, normed=True)
        pylab.title('%s : For itself (output variance)' % (title))
        pylab.xlabel('N Sigma')
        pylab.ylabel('N Pix')
        
        allStats2 = numpy.ravel(allStats2) 
        pylab.figure()
        n, b, p = pylab.hist(allStats2, bins=100, normed=True)
        pylab.title('%s : For itself (input variance)' % (title))
        pylab.xlabel('N Sigma')
        pylab.ylabel('N Pix')
        return allStats1, allStats2
        

 
    def xtestDeltaFunction(self):
        pass
    
    def xtestVisit(self):
        pass

        
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
    run(True)
