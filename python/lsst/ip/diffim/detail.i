// -*- lsst-c++ -*-

%{
//#include "lsst/ip/diffim/AssessSpatialKernelVisitor.h"
//#include "lsst/ip/diffim/BuildSingleKernelVisitor.h"
//#include "lsst/ip/diffim/BuildSpatialKernelVisitor.h"
//#include "lsst/ip/diffim/KernelPcaVisitor.h"
#include "lsst/ip/diffim/KernelSumVisitor.h"
%}

//SWIG_SHARED_PTR(AssessSpatialKernelVisitor, lsst::ip::diffim::detail::AssessSpatialKernelVisitor);
//SWIG_SHARED_PTR(BuildSingleKernelVisitor, lsst::ip::diffim::detail::BuildSingleKernelVisitor);
//SWIG_SHARED_PTR(BuildSpatialKernelVisitor, lsst::ip::diffim::detail::BuildSpatialKernelVisitor);
//SWIG_SHARED_PTR(KernelPcaVisitor, lsst::ip::diffim::detail::KernelPcaVisitor);

//%include "lsst/ip/diffim/AssessSpatialKernelVisitor.h"
//%include "lsst/ip/diffim/BuildSingleKernelVisitor.h"
//%include "lsst/ip/diffim/BuildSpatialKernelVisitor.h"
//%include "lsst/ip/diffim/KernelPcaVisitor.h"

/******************************************************************************/

%{
#include "lsst/ip/diffim/KernelSumVisitor.h"
%}

%define %KernelSumVisitorPtr(NAME, TYPE)
SWIG_SHARED_PTR_DERIVED(KernelSumVisitor##NAME, 
                        lsst::afw::math::CandidateVisitor, 
                        lsst::ip::diffim::detail::KernelSumVisitor<TYPE>);
%enddef

%define %KernelSumVisitor(NAME, TYPE)
%template(KernelSumVisitor##NAME) lsst::ip::diffim::detail::KernelSumVisitor<TYPE>;
%template(makeKernelSumVisitor) lsst::ip::diffim::detail::makeKernelSumVisitor<TYPE>;
%enddef

%KernelSumVisitorPtr(F, float)

%include "lsst/ip/diffim/KernelSumVisitor.h"

%KernelSumVisitor(F, float)

/******************************************************************************/

%{
#include "lsst/ip/diffim/KernelPcaVisitor.h"
%}

%define %KernelPcaVisitorPtr(NAME, TYPE)
SWIG_SHARED_PTR_DERIVED(KernelPcaVisitor##NAME, 
                        lsst::afw::math::CandidateVisitor, 
                        lsst::ip::diffim::detail::KernelPcaVisitor<TYPE>);
%enddef

%define %KernelPcaVisitor(NAME, TYPE)
%template(KernelPcaVisitor##NAME) lsst::ip::diffim::detail::KernelPcaVisitor<TYPE>;
%template(makeKernelPcaVisitor) lsst::ip::diffim::detail::makeKernelPcaVisitor<TYPE>;
%enddef

%KernelPcaVisitorPtr(F, float)

%include "lsst/ip/diffim/KernelPcaVisitor.h"

%KernelPcaVisitor(F, float)

/******************************************************************************/
