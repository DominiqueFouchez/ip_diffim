#include <lsst/mwi/utils/Trace.h>
#include <lsst/mwi/data/Citizen.h>
#include <lsst/fw/MaskedImage.h>
#include <lsst/detection/Footprint.h>

using namespace std;
using namespace lsst::fw;

typedef uint8 MaskT;
typedef float ImageT;
typedef double KernelT;
typedef double FuncT;

int main( int argc, char** argv )
{
    {
        lsst::mwi::utils::Trace::setDestination(cout);
        lsst::mwi::utils::Trace::setVerbosity(".", 4);

        string templateImage = argv[1];
        MaskedImage<ImageT,MaskT> templateMaskedImage;
        try {
            templateMaskedImage.readFits(templateImage);
        } catch (lsst::mwi::exceptions::Exception &e) {
            cerr << "Failed to open template image " << templateImage << ": " << e.what() << endl;
            return 1;
        }

        float threshold = atof(argv[2]);

        // Find detections
        lsst::detection::DetectionSet<ImageT,MaskT> 
            detectionSet(templateMaskedImage, lsst::detection::Threshold(threshold, lsst::detection::Threshold::VALUE));
        vector<lsst::detection::Footprint::PtrType> footprintVector = detectionSet.getFootprints();
        cout << " Detected " << footprintVector.size() << " footprints at value threshold " << threshold << endl;

    }
    
    if (Citizen::census(0) == 0) {
        cerr << "No leaks detected" << endl;
    } else {
        cerr << "Leaked memory blocks:" << endl;
        Citizen::census(cerr);
    } 
}
