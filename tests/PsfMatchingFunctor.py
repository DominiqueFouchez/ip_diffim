#!/usr/bin/env python
import os, pdb

import unittest
import lsst.utils.tests as tests

import eups
import lsst.afw.detection as afwDetection
import lsst.afw.image as afwImage
import lsst.afw.math as afwMath
import lsst.pex.policy as pexPolicy
import lsst.ip.diffim as ipDiffim
import lsst.pex.logging as logging

import lsst.afw.display.ds9 as ds9

Verbosity = 4
logging.Trace_setVerbosity('lsst.ip.diffim', Verbosity)

diffimDir    = eups.productDir('ip_diffim')
diffimPolicy = os.path.join(diffimDir, 'pipeline', 'ImageSubtractStageDictionary.paf')

class DiffimTestCases(unittest.TestCase):
    
    # D = I - (K.x.T + bg)
        
    def setUp(self):
        self.policy    = pexPolicy.Policy.createPolicy(diffimPolicy)
        self.kCols     = self.policy.getInt('kernelCols')
        self.kRows     = self.policy.getInt('kernelRows')
        self.basisList = ipDiffim.generateDeltaFunctionKernelSet(self.kCols, self.kRows)

        # gaussian reference kernel
        self.gSize         = self.kCols
        self.gaussFunction = afwMath.GaussianFunction2D(2, 3)
        self.gaussKernel   = afwMath.AnalyticKernel(self.gSize, self.gSize, self.gaussFunction)
        self.kImageIn      = afwImage.ImageD(self.gSize, self.gSize)
        self.gaussKernel.computeImage(self.kImageIn, False)

        # difference imaging functor
        self.kFunctor      = ipDiffim.PsfMatchingFunctorF(self.basisList)

    def tearDown(self):
        del self.policy

    def doDeltaFunction(self, dX, dY, scaling, background, imsize=50):
        # template image with a single hot pixel in the exact center
        tmi = afwImage.MaskedImageF(imsize, imsize)
        tmi.set(0, 0x0, 1e-4)
        # central pixel
        cpix = int(imsize/2)
        # where is the hot pixel?
        tmi.set(cpix, cpix, (1, 0x0, 1))

        # science image with a potentially offset and scaled hot pixel
        smi = afwImage.MaskedImageF(imsize, imsize)
        smi.set(0, 0x0, 1e-4)
        xpix = cpix + dX
        ypix = cpix + dY
        # where is the hot pixel?
        smi.set(xpix, ypix, (scaling, 0x0, scaling))
        # any additional background
        smi += background

        # estimate of the variance
        var  = afwImage.MaskedImageF(smi, True)
        var -= tmi

        # accepts : imageToConvolve, imageToNotConvolve
        self.kFunctor.apply(tmi.getImage(), smi.getImage(), var.getVariance(), self.policy)

        kernel   = self.kFunctor.getKernel()
        kImage   = afwImage.ImageD(self.kCols, self.kRows)
        kSum     = kernel.computeImage(kImage, False)

        # make sure the correct background and scaling have been determined
        self.assertAlmostEqual(self.kFunctor.getBackground(), background, 5)
        self.assertAlmostEqual(kSum, scaling, 5)
        #print background, self.kFunctor.getBackground(), scaling, kSum

        # make sure the delta function is in the right place
        xpix = kernel.getCtrX() - dX
        ypix = kernel.getCtrY() - dY
        for j in range(kImage.getHeight()):
            for i in range(kImage.getWidth()):

                if i == xpix and j == ypix:
                    self.assertAlmostEqual(kImage.get(i, j), scaling, 5)
                else:
                    self.assertAlmostEqual(kImage.get(i, j), 0., 5)
        
        
        
    def doGaussian(self, scaling, background, imsize=50):
        # NOTE : the size of these images have to be bigger
        #        size you lose pixels due to the convolution with the gaussian
        #        so adjust the size a bit to compensate 
        imsize += self.gSize
        
        # template image with a single hot pixel in the exact center
        tmi = afwImage.MaskedImageF(imsize, imsize)
        tmi.set(0, 0x0, 1e-4)
        # central pixel
        cpix = int(imsize/2)
        # where is the hot pixel?
        tmi.set(cpix, cpix, (1, 0x0, 1))
        
        # science image with a potentially offset and scaled hot pixel
        smi = afwImage.MaskedImageF(imsize, imsize)
        smi.set(0, 0x0, 1e-4)
        xpix = cpix
        ypix = cpix
        # where is the hot pixel?
        smi.set(xpix, ypix, (scaling, 0x0, scaling))
        # convolve with gaussian
        cmi = afwImage.MaskedImageF(imsize, imsize)
        afwMath.convolve(cmi, smi, self.gaussKernel, False)
        # this will adjust the kernel sum a bit
        # lose some at the outskirts of the kernel
        fc = ipDiffim.FindCountsF()
        fc.apply(cmi)
        cscaling = fc.getCounts()
        # any additional background
        cmi += background

        # grab only the non-masked subregion
        bbox     = afwImage.BBox(afwImage.PointI(self.gaussKernel.getCtrX(),
                                                 self.gaussKernel.getCtrY()) ,
                                 afwImage.PointI(imsize - (self.gaussKernel.getWidth()  - self.gaussKernel.getCtrX()),
                                                 imsize - (self.gaussKernel.getHeight() - self.gaussKernel.getCtrY())))
                                 
        tmi2     = afwImage.MaskedImageF(tmi, bbox)
        cmi2     = afwImage.MaskedImageF(cmi, bbox)

        #ds9.mtv(tmi,  frame=1)
        #ds9.mtv(cmi,  frame=2)
        #ds9.mtv(tmi2, frame=3)
        #ds9.mtv(cmi2, frame=4)
        #pdb.set_trace()
        
        # make sure its a valid subregion!
        mask     = cmi2.getMask()
        for j in range(mask.getHeight()):
            for i in range(mask.getWidth()):
                self.assertEqual(mask.get(i, j), 0)
                
        # estimate of the variance
        var  = afwImage.MaskedImageF(cmi2, True)
        var -= tmi2

        # accepts : imageToConvolve, imageToNotConvolve
        self.kFunctor.apply(tmi2.getImage(), cmi2.getImage(), var.getVariance(), self.policy)

        kernel    = self.kFunctor.getKernel()
        kImageOut = afwImage.ImageD(self.kCols, self.kRows)
        kSum      = kernel.computeImage(kImageOut, False)

        # make sure the correct background and scaling have been determined
        self.assertAlmostEqual(self.kFunctor.getBackground(), background, 4)
        self.assertAlmostEqual(kSum, cscaling, 4)
        #print background, self.kFunctor.getBackground(), scaling, cscaling, kSum

        #self.kImageIn.writeFits('k1.fits')
        #kImageOut.writeFits('k2.fits')
        #tmi2.getImage().writeFits('t.fits')
        #cmi2.getImage().writeFits('c.fits')

        # make sure the derived kernel looks like the input kernel
        for j in range(kImageOut.getHeight()):
            for i in range(kImageOut.getWidth()):

                # once we start to add in a background, the outer
                # portions of the kernel start to get a bit noisy.
                #
                # print i, j, self.kImageIn.get(i,j), kImageOut.get(i, j), kImageOut.get(i, j)/self.kImageIn.get(i,j)
                #
                # however, where the power is, the results are the
                # same

                if self.kImageIn.get(i,j) > 0.01:
                    self.assertAlmostEqual(kImageOut.get(i, j)/self.kImageIn.get(i,j), scaling, 4)
                elif self.kImageIn.get(i,j) > 0.001:
                    self.assertAlmostEqual(kImageOut.get(i, j)/self.kImageIn.get(i,j), scaling, 3)


            
    def testDeltaFunction(self):
        # hot central pixel 
        self.doDeltaFunction(0, 0, 1, 0)
        self.doDeltaFunction(0, 0, 7, 0)
        self.doDeltaFunction(0, 0, 0.375, 0)

        # hot central pixel with background
        self.doDeltaFunction(0, 0, 1, 100)
        self.doDeltaFunction(0, 0, 1, 0.391)
        self.doDeltaFunction(0, 0, 1, -17.9)

        # mixture of hot central pixel, scaling, and background
        self.doDeltaFunction(0, 0, 0.735, 14.5)
        self.doDeltaFunction(0, 0, 12.20, 14.6)
        self.doDeltaFunction(0, 0, 0.735, -12.1)
        self.doDeltaFunction(0, 0, 12.20, -12.1)

        # offset delta function
        self.doDeltaFunction(-3, 2, 1, 0)
        self.doDeltaFunction(1, -2, 1, 0)

        # offset, scaling, and background
        self.doDeltaFunction(-3, 2, 10,  0)
        self.doDeltaFunction(-3, 2, 0.1, 0)
        self.doDeltaFunction(-4, 1, 1, +10)
        self.doDeltaFunction(-4, 1, 1, -10)
        self.doDeltaFunction(-4, 1, 0.1, +10)
        self.doDeltaFunction(-4, 1, 10,  -10)
        
        
    def testGaussian(self):
        # different scalings
        self.doGaussian(1, 0)
        self.doGaussian(7, 0)
        self.doGaussian(0.375, 0)

        # different backgrounds
        self.doGaussian(1, 3.3)
        self.doGaussian(1, 0.18)
        self.doGaussian(1, -12.1)

        # scaling and background
        self.doGaussian(0.735, 14.5)
        self.doGaussian(12.20, 14.6)
        self.doGaussian(0.735, -12.1)
        self.doGaussian(12.20, -12.1)

        
#####
        
def suite():
    """Returns a suite containing all the test cases in this module."""
    tests.init()

    suites = []
    suites += unittest.makeSuite(DiffimTestCases)
    suites += unittest.makeSuite(tests.MemoryTestCase)
    return unittest.TestSuite(suites)

def run(exit=False):
    """Run the tests"""
    tests.run(suite(), exit)

if __name__ == "__main__":
    run(True)
