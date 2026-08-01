
#include "OS/Linux/LinuxShmRegion.hpp"

#include "UnitTesting/UnitTestingCore.hpp"

using namespace Encore;
using namespace Encore::OS;


struct NotDefConst {

    NotDefConst(int pX) : x{pX} {}

    int x{0};
};


//no-op test for static assertion
UNITTESTER_MAIN( test_LinuxShmRegion, int, char const** ) {

#ifdef SHARED_MEM_DEFAULT_CONSTRUCTIBLE
    auto lsm = LinuxShmRegion<NotDefConst>( kTestInitOnly, kNotThreadSafe, "NotDefConst", true );
#endif

    return 0;
}
