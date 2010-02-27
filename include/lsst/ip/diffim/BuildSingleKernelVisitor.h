// -*- lsst-c++ -*-
/**
 * @file BuildSingleKernelVisitor.h
 *
 * @brief Declaration of BuildSingleKernelVisitor 
 *
 * @author Andrew Becker, University of Washington
 *
 * @ingroup ip_diffim
 */

#ifndef LSST_IP_DIFFIM_BUILDSINGLEKERNELVISITOR_H
#define LSST_IP_DIFFIM_BUILDSINGLEKERNELVISITOR_H

#include "lsst/afw/image.h"
#include "lsst/afw/math.h"

#include "lsst/pex/policy/Policy.h"

#include "lsst/ip/diffim/ImageSubtract.h"
#include "lsst/ip/diffim/PsfMatchingFunctor.h"

namespace lsst { 
namespace ip { 
namespace diffim { 
namespace detail {

    template<typename PixelT>
    class BuildSingleKernelVisitor : public lsst::afw::math::CandidateVisitor {
        typedef lsst::afw::image::MaskedImage<PixelT> MaskedImageT;
    public:
        BuildSingleKernelVisitor(
            PsfMatchingFunctor<PixelT> &kFunctor,    ///< Functor that builds the kernels
            lsst::pex::policy::Policy const& policy  ///< Policy file directing behavior
            );
        virtual ~BuildSingleKernelVisitor() {};
        
        /* 
         * This functionality allows the user to not set the "kernel" and thus
         * "image" values of the KernelCandidate.  When running a PCA fit on the
         * kernels, we want to keep the delta-function representation of the raw
         * kernel which are used to derive the eigenBases, while still being
         * able to modify the _M and _B matrices with linear fits to the
         * eigenBases themselves.
         */
        void setCandidateKernel(bool set) {_setCandidateKernel = set;}
        
        /* 
           Don't reprocess candidate if its already been build.  The use
           case for this functionality is : when iterating over all Cells
           and rejecting bad Kernels, we need to re-visit *all* Cells to
           build the next candidate in the list.  Without this flag we would
           unncessarily re-build all the good Kernels.
        */
        void setSkipBuilt(bool skip)      {_skipBuilt = skip;}
        
        int  getNRejected()   {return _nRejected;}
        void reset()          {_nRejected = 0;}
        
        void processCandidate(lsst::afw::math::SpatialCellCandidate *candidate);

    private:
        PsfMatchingFunctor<PixelT> _kFunctor; ///< Psf matching functor
        lsst::pex::policy::Policy _policy;    ///< Policy controlling behavior
        ImageStatistics<PixelT> _imstats;     ///< To calculate statistics of difference image
        bool _setCandidateKernel;             ///< Do you set the KernelCandidate kernel, or just matrices
        bool _skipBuilt;                      ///< Skip over built candidates during processCandidate()
        int _nRejected;                       ///< Number of candidates rejected during processCandidate()
    };
    

}}}} // end of namespace lsst::ip::diffim::detail

#endif
