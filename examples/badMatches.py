import lsst.afw.image as afwImage
import lsst.afw.geom as afwGeom
import lsst.afw.math as afwMath
import lsst.ip.diffim as ipDiffim
import numpy as num
import lsst.pex.logging as pexLogging
import lsst.afw.display.ds9 as ds9
verbosity = 5
pexLogging.Trace_setVerbosity("lsst.ip.diffim", verbosity)

imSize         = 51
rdm            = afwMath.Random(afwMath.Random.MT19937, 10101)

def makeTest1(doAddNoise):
    gaussian1 = afwMath.GaussianFunction2D(1., 1., 0.)
    kernel1   = afwMath.AnalyticKernel(imSize, imSize, gaussian1)
    image1    = afwImage.ImageD(kernel1.getDimensions())
    kernel1.computeImage(image1, False)
    image1   *= 10000
    image1    = image1.convertF()
    mask1     = afwImage.MaskU(kernel1.getDimensions())
    var1      = afwImage.ImageF(image1, True)
    mi1       = afwImage.MaskedImageF(image1, mask1, var1)
    if doAddNoise: addNoise(mi1)
    
    gaussian2 = afwMath.GaussianFunction2D(2., 1.5, 0.5 * num.pi)
    kernel2   = afwMath.AnalyticKernel(imSize, imSize, gaussian2)
    image2    = afwImage.ImageD(kernel2.getDimensions())
    kernel2.computeImage(image2, False)
    image2   *= 10000
    image2    = image2.convertF()
    mask2     = afwImage.MaskU(kernel2.getDimensions())
    var2      = afwImage.ImageF(image2, True)
    mi2       = afwImage.MaskedImageF(image2, mask2, var2)
    if doAddNoise: addNoise(mi2)
    return mi1, mi2

def makeTest2(doAddNoise):
    gaussian1 = afwMath.GaussianFunction2D(1., 1., 0.)
    kernel1   = afwMath.AnalyticKernel(imSize, imSize, gaussian1)
    image1    = afwImage.ImageD(kernel1.getDimensions())
    kernel1.computeImage(image1, False)
    image1   *= 10000
    image1    = image1.convertF()
    ####
    boxA     = afwGeom.Box2I(afwGeom.PointI(imSize//2, imSize//2),
                             afwGeom.ExtentI(imSize//2, imSize//2))
    boxB     = afwGeom.Box2I(afwGeom.PointI(imSize//2 - 5, imSize//2 - 3),
                             afwGeom.ExtentI(imSize//2, imSize//2))
    subregA   = afwImage.ImageF(image1, boxA, afwImage.PARENT)
    subregB   = afwImage.ImageF(image1, boxB, afwImage.PARENT, True)
    subregA  += subregB
    ###
    mask1     = afwImage.MaskU(kernel1.getDimensions())
    var1      = afwImage.ImageF(image1, True)
    mi1       = afwImage.MaskedImageF(image1, mask1, var1)
    if doAddNoise: addNoise(mi1)
    
    gaussian2 = afwMath.GaussianFunction2D(2., 1.5, 0.5 * num.pi)
    kernel2   = afwMath.AnalyticKernel(imSize, imSize, gaussian2)
    image2    = afwImage.ImageD(kernel2.getDimensions())
    kernel2.computeImage(image2, False)
    image2   *= 10000
    image2    = image2.convertF()
    mask2     = afwImage.MaskU(kernel2.getDimensions())
    var2      = afwImage.ImageF(image2, True)
    mi2       = afwImage.MaskedImageF(image2, mask2, var2)
    if doAddNoise: addNoise(mi2)
    
    return mi1, mi2

def fft(im1, im2, fftSize):
    arr1 = im1.getArray()
    arr2 = im2.getArray()

    fft1 = num.fft.rfft2(arr1)
    fft2 = num.fft.rfft2(arr2)
    rat  = fft2 / fft1

    kfft = num.fft.irfft2(rat, s=fftSize)
    kim  = afwImage.ImageF(fftSize)
    kim.getArray()[:] = kfft
    
    ds9.mtv(kim, frame=5)
       

# If we don't add noise, the edges of the Gaussian images go to zero,
# and that boundary causes artificial artefacts in the kernels
def addNoise(mi):
    sfac      = 1.0
    img       = mi.getImage()
    rdmImage  = img.Factory(img.getDimensions())
    afwMath.randomGaussianImage(rdmImage, rdm)
    rdmImage *= sfac
    img      += rdmImage

    # and don't forget to add to the variance
    var       = mi.getVariance()
    var      += sfac



if __name__ == '__main__':

    doAddNoise = True
    
    policy = ipDiffim.makeDefaultPolicy()
    policy.set("fitForBackground", False)
    policy.set("checkConditionNumber", False)
    policy.set("constantVarianceWeighting", True)
    fnum = 1
    
    for switch in ['A', 'B', 'C']:

        if switch == 'A':
            # Default Alard Lupton
            pass
        elif switch == 'B':
            # AL with ~320 bases
            policy.set("alardDegGauss", 15)
            policy.add("alardDegGauss", 10)
            policy.add("alardDegGauss", 5)
        elif switch == 'C':
            # Delta function
            policy.set("kernelBasisSet", "delta-function")
    
        kList = ipDiffim.makeKernelBasisList(policy)
        bskv  = ipDiffim.BuildSingleKernelVisitorF(kList, policy)
    
        # TEST 1
        tmi, smi = makeTest1(doAddNoise)
        kc       = ipDiffim.makeKernelCandidate(0.0, 0.0, tmi, smi, policy)
        bskv.processCandidate(kc)
    
        kernel   = kc.getKernel(ipDiffim.KernelCandidateF.ORIG)
        kimage   = afwImage.ImageD(kernel.getDimensions())
        kernel.computeImage(kimage, False)
        diffim   = kc.getDifferenceImage(ipDiffim.KernelCandidateF.ORIG)
    
        ds9.mtv(tmi,    frame = fnum) ; fnum += 1
        ds9.mtv(smi,    frame = fnum) ; fnum += 1
        ds9.mtv(kimage, frame = fnum) ; fnum += 1
        ds9.mtv(diffim, frame = fnum) ; fnum += 1
    
        # TEST 2
        tmi, smi = makeTest2(doAddNoise)
        kc       = ipDiffim.makeKernelCandidate(0.0, 0.0, tmi, smi, policy)
        bskv.processCandidate(kc)
    
        kernel   = kc.getKernel(ipDiffim.KernelCandidateF.ORIG)
        kimage   = afwImage.ImageD(kernel.getDimensions())
        kernel.computeImage(kimage, False)
        diffim   = kc.getDifferenceImage(ipDiffim.KernelCandidateF.ORIG)
    
        ds9.mtv(tmi,    frame = fnum) ; fnum += 1
        ds9.mtv(smi,    frame = fnum) ; fnum += 1
        ds9.mtv(kimage, frame = fnum) ; fnum += 1
        ds9.mtv(diffim, frame = fnum) ; fnum += 1
        
    
