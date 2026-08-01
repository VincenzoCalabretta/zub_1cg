
#include "encore/OS/Common/TCP.hpp"

#include <cstdint>

#include "encore/UnitTesting/UnitTestingCore.hpp"
#include "encore/ErrorHandling/test/UnitTestExtensions.hpp"

namespace {

namespace test = Encore::UnitTesting;
using namespace Encore;
using namespace Encore::ErrorHandling;
using namespace Encore::OS;


void runMiscTests( test::UnitTestConfig const & pConfig ) {

    test::UnitTest<test::ErrorHandlingExtension> ut( pConfig );

    NoOpStreamSocket sock;

    NoOpTCPClientSocketFactory sf;
    auto sock1 = sf.create({});
    ut.isSuccessful( sock1, "Factory returns a socket" );
    ut.assertTrue( dynamic_cast<NoOpStreamSocket *>( sock1.value() ) != nullptr, "Factory returned a NoOpSocket" );
    sf.destroy( sock );

    NoOpTCPServer serv;

    ut.failsWithCode( serv.start(), EG_ERROR_CODE( StdFswErrors, logic_error ), "Cannot start noop server by default" );
    auto accepted = serv.accept();
    ut.isSuccessful( accepted , "Accept successful" );
    ut.notEqual( dynamic_cast<NoOpStreamSocket *>( accepted.value().socket ), nullptr, "Returns a no-op socket" );

    NoOpTCPServer serv2(true, false);

    ut.isSuccessful( serv2.start(), "But we can start one if we need to" );

    ut.failsWithCode( serv2.accept(), EG_ERROR_CODE( StdFswErrors, logic_error ), "Accept fails when disallowed" );

    serv.release( sock ); // Coverage

    ut.isSuccessful( serv2.stop(), "Server stops" );

    NoOpStreamSocket noop1;
    NoOpStreamSocket noop2;

    TCPSocketEndpoint ep1{};
    TCPSocketEndpoint ep2{};
    ut.assertTrue( ep1 == ep2, "EP equal" );

    ep1.endPoint.port = 5;
    ut.assertFalse( ep1 == ep2, "EP not equal" );

    ep2.endPoint.port = 5l;
    ut.assertTrue( ep1 == ep2, "EP equal again" );

    ep1.socket = &noop1;
    ep2.socket = &noop2;
    ut.assertFalse( ep1 == ep2, "EP not equal again" );
}

}

UNITTESTER_MAIN( test_TCP, int pArgc, char const ** pArgv ) {

    test::UnitTester  tester( pArgc, pArgv );

    return tester.filterAndExec( { { test::TestTypeID::misc, "Misc", &runMiscTests }
                                 } );
}
