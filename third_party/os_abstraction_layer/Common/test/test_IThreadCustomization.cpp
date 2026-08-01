
#include "OS/Common/IThreadCustomization.hpp"

#include <thread>

#include "encore/ErrorHandling/test/UnitTestExtensions.hpp"
#include "UnitTesting/UnitTestingCore.hpp"

namespace {

namespace test = Encore::UnitTesting;
using namespace Encore::OS;

void noOpThreadCustomization( test::UnitTestConfig const & pConfig ) {

    test::UnitTest<test::ErrorHandlingExtension> ut ( pConfig );

    std::thread t( []()noexcept{ using namespace std::chrono_literals; std::this_thread::sleep_for(100ms); } );

    NoOpThreadCustomization tc;

    ut.isSuccessful( tc.setPriority( 10, &t ) );
    ut.isSuccessful( tc.setCPUAffinity( 0, true, &t ) );
    ut.isSuccessful( tc.getCPUAffinity( 0, &t ) );

    t.join();

    ut.assertTrue( true, "No op coverage" );
}

}

UNITTESTER_MAIN( test_IThreadCustomization, int pArgc, char const** pArgv ) {

    test::UnitTester  tester( pArgc, pArgv );

    return tester.filterAndExec( { { test::TestTypeID::misc, "NoOpThreadCustomization", &noOpThreadCustomization }
                                 } );


}
