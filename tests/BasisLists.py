#!/usr/bin/env python
import os
import numpy as num

import unittest
import lsst.utils.tests as tests

import eups
import lsst.afw.image as afwImage
import lsst.afw.math as afwMath
import lsst.ip.diffim as ipDiffim
import lsst.pex.logging as logging

verbosity = 4
logging.Trace_setVerbosity("lsst.ip.diffim", verbosity)

class DiffimTestCases(unittest.TestCase):
    
    def setUp(self):
        self.policy = ipDiffim.makeDefaultPolicy()
        self.kSize  = self.policy.getInt("kernelSize")

    def tearDown(self):
        del self.policy

    #
    ### Delta function
    #
    
    def deltaFunctionTest(self, ks):
        # right shape
        nk  = 0
        for rowi in range(self.kSize):
            for colj in range(self.kSize):
                kernel = ks[nk]
                kimage = afwImage.ImageD(kernel.getDimensions())
                ksum   = kernel.computeImage(kimage, False)
                self.assertEqual(ksum, 1.)

                for rowk in range(self.kSize):
                    for coll in range(self.kSize):
                        if (rowi == rowk) and (colj == coll):
                            self.assertEqual(kimage.get(coll, rowk), 1.)
                        else:
                            self.assertEqual(kimage.get(coll, rowk), 0.)
                nk += 1
        

    def testMakeDeltaFunction(self):
        ks = ipDiffim.makeDeltaFunctionBasisList(self.kSize, self.kSize)

        # right size
        self.assertEqual(len(ks), self.kSize * self.kSize)

        # right shape
        self.deltaFunctionTest(ks)


    def testDeltaFunction(self):
        self.policy.set("kernelBasisSet", "delta-function")
        ks = ipDiffim.makeKernelBasisList(self.policy)

        # right size
        self.assertEqual(len(ks), self.kSize * self.kSize)

        # right shape
        self.deltaFunctionTest(ks)

    #
    ### Alard Lupton
    #

    def alardLuptonTest(self, ks):
        kim    = afwImage.ImageD(ks[0].getDimensions())
        nBasis = len(ks)

        # the first one sums to 1; the rest sum to 0
        ks[0].computeImage(kim, False)
        self.assertAlmostEqual(num.sum(num.ravel(kim.getArray())), 1.0)
        
        for k in range(1, nBasis):
            ks[k].computeImage(kim, False)
            self.assertAlmostEqual(num.sum(num.ravel(kim.getArray())), 0.0)

        # the images dotted with themselves is 1, except for the first
        for k in range(1, nBasis):
            ks[k].computeImage(kim, False)
            arr = kim.getArray()
            self.assertAlmostEqual(num.sum(arr*arr), 1.0)


    def testAlardLupton(self):
        self.policy.set("kernelBasisSet", "alard-lupton")
        ks = ipDiffim.makeKernelBasisList(self.policy)

        # right size
        nTot = 0
        for deg in self.policy.getIntArray("alardDegGauss"):
            nTot += (deg + 1) * (deg + 2) / 2
        self.assertEqual(len(ks), nTot)

        # right orthogonality
        self.alardLuptonTest(ks)

    def testMakeAlardLupton(self):
        nGauss   = self.policy.get("alardNGauss")
        sigGauss = self.policy.getDoubleArray("alardSigGauss")
        degGauss = self.policy.getIntArray("alardDegGauss")
        self.assertEqual(len(sigGauss), nGauss)
        self.assertEqual(len(degGauss), nGauss)
        self.assertEqual(self.kSize % 2, 1)       # odd sized
        kHalfWidth = self.kSize // 2

        ks = ipDiffim.makeAlardLuptonBasisList(kHalfWidth, nGauss, sigGauss, degGauss)

        # right size
        nTot = 0
        for deg in degGauss:
            nTot += (deg + 1) * (deg + 2) / 2
        self.assertEqual(len(ks), nTot)

        # right orthogonality
        self.alardLuptonTest(ks)
        
    #
    ### Renormalize
    #

    def testRenormalize(self):
        # inputs
        gauss1 = afwMath.GaussianFunction2D(2, 2)
        gauss2 = afwMath.GaussianFunction2D(3, 4)
        gauss3 = afwMath.GaussianFunction2D(0.2, 0.25)
        gaussKernel1 = afwMath.AnalyticKernel(self.kSize, self.kSize, gauss1)
        gaussKernel2 = afwMath.AnalyticKernel(self.kSize, self.kSize, gauss2)
        gaussKernel3 = afwMath.AnalyticKernel(self.kSize, self.kSize, gauss3)
        kimage1 = afwImage.ImageD(gaussKernel1.getDimensions())
        ksum1   = gaussKernel1.computeImage(kimage1, False)
        kimage2 = afwImage.ImageD(gaussKernel2.getDimensions())
        ksum2   = gaussKernel2.computeImage(kimage2, False)
        kimage3 = afwImage.ImageD(gaussKernel3.getDimensions())
        ksum3   = gaussKernel3.computeImage(kimage3, False)
        self.assertTrue(ksum1 != 1.)
        self.assertTrue(ksum2 != 1.)
        self.assertTrue(ksum3 != 1.)
        # no constraints on first kernels norm
        self.assertTrue(num.sum(num.ravel(kimage2.getArray())**2 ) != 1.)
        self.assertTrue(num.sum(num.ravel(kimage3.getArray())**2 ) != 1.)
        basisListIn = afwMath.KernelList()
        basisListIn.push_back(gaussKernel1)
        basisListIn.push_back(gaussKernel2)
        basisListIn.push_back(gaussKernel3)
        
        # outputs
        basisListOut = ipDiffim.renormalizeKernelList(basisListIn)
        gaussKernel1 = basisListOut[0]
        gaussKernel2 = basisListOut[1]
        gaussKernel3 = basisListOut[2]
        ksum1 = gaussKernel1.computeImage(kimage1, False)
        ksum2 = gaussKernel2.computeImage(kimage2, False)
        ksum3 = gaussKernel3.computeImage(kimage3, False)
        self.assertAlmostEqual(ksum1, 1.)
        self.assertAlmostEqual(ksum2, 0.)
        self.assertAlmostEqual(ksum3, 0.)
        # no constraints on first kernels norm
        self.assertAlmostEqual(num.sum(num.ravel(kimage2.getArray())**2 ), 1.)
        self.assertAlmostEqual(num.sum(num.ravel(kimage3.getArray())**2 ), 1.)

    #
    ### Regularize
    #

    def testCentralRegularization(self):
        # 
        self.policy.set("regularizationType", "centralDifference")
        try:
            self.policy.set("centralRegularizationStencil", 1)
            h = ipDiffim.makeRegularizationMatrix(self.policy)
        except Exception, e: # stencil of 1 not allowed
            pass
        else:
            self.fail()
            
        self.policy.set("centralRegularizationStencil", 5)
        try:
            h = ipDiffim.makeRegularizationMatrix(self.policy)
        except Exception, e: # stencil of 5 allowed
            print e
            self.fail()
        else:
            pass

        self.policy.set("centralRegularizationStencil", 9)
        try:
            h = ipDiffim.makeRegularizationMatrix(self.policy)
        except Exception, e: # stencil of 9 allowed
            print e
            self.fail()
        else:
            pass

        self.policy.set("regularizationBorderPenalty", -1.0)
        try:
            h = ipDiffim.makeRegularizationMatrix(self.policy)
        except Exception, e: # border penalty > 0
            pass
        else:
            self.fail()

        self.policy.set("regularizationBorderPenalty", 0.0)
        try:
            h = ipDiffim.makeRegularizationMatrix(self.policy)
        except Exception, e: # border penalty > 0
            print e
            self.fail()
        else:
            pass

    def testForwardRegularization(self):
        self.policy.set("regularizationType", "forwardDifference")
        self.policy.set("forwardRegularizationOrders", 0)
        try:
            h = ipDiffim.makeRegularizationMatrix(self.policy)
        except Exception, e: # order 1..3 allowed
            pass
        else:
            self.fail()

        self.policy.set("forwardRegularizationOrders", 1)
        try:
            h = ipDiffim.makeRegularizationMatrix(self.policy)
        except Exception, e: # order 1..3 allowed
            print e
            self.fail()
        else:
            pass

        self.policy.set("forwardRegularizationOrders", 4)
        try:
            h = ipDiffim.makeRegularizationMatrix(self.policy)
        except Exception, e: # order 1..3 allowed
            pass
        else:
            self.fail()

        self.policy.set("forwardRegularizationOrders", 1)
        self.policy.add("forwardRegularizationOrders", 2)
        try:
            h = ipDiffim.makeRegularizationMatrix(self.policy)
        except Exception, e: # order 1..3 allowed
            print e
            self.fail()
        else:
            pass

    def testBadRegularization(self):
        try:
            self.policy.set("regularizationType", "foo")
            h = ipDiffim.makeRegularizationMatrix(self.policy)
        except Exception, e: # invalid option
            pass
        else:
            self.fail()

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