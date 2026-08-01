
#include "OS/Linux/LinuxShmBufferRegistry.hpp"

#include "UnitTesting/UnitTestingCore.hpp"

namespace {

using namespace Encore;
using namespace Encore::OS;

}

//no-op test for static assertion
UNITTESTER_MAIN( test_LinuxShmBufferRegistryStatic, int, char const** ) {

#ifdef UNDERSIZED_BUFFER
    LinuxShmBufferRegistry<4,16> lbr(kTestInitOnly,kNotThreadSafe,true);
#endif

    return 0;
}
