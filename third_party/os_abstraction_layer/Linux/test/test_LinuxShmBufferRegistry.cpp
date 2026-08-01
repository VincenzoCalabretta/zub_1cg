
#include "OS/Linux/LinuxShmBufferRegistry.hpp"

#include <cstdint>

#include "UnitTesting/UnitTestingCore.hpp"

namespace {

namespace test = Encore::UnitTesting;
using namespace Encore;
using namespace Encore::ErrorHandling;
using namespace Encore::Utils;
using namespace Encore::DataModel;
using namespace Encore::OS;


void basicAPI( test::UnitTestConfig const & pConfig ) {

    test::UnitTest<void> ut(pConfig);

    LinuxShmBufferRegistry<5> lbr(kTestInitOnly,kNotThreadSafe,true);

    ut.assertFalse( lbr.query(0).success(), "Query for const base class access" );
}

}

UNITTESTER_MAIN( test_LinuxShmBufferRegistry, int pArgc, char const** pArgv ) {

    test::UnitTester  tester( pArgc, pArgv );

    return tester.filterAndExec( { { test::TestTypeID::micro, "BasicAPI", &basicAPI }
                                 } );
}
