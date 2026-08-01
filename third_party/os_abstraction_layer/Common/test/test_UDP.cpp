
#include "OS/Common/UDP.hpp"

#include <cstdint>

#include "UnitTesting/UnitTestingCore.hpp"
#include "ErrorHandling/test/UnitTestExtensions.hpp"

namespace {

namespace test = Encore::UnitTesting;
using namespace Encore;
using namespace Encore::ErrorHandling;
using namespace Encore::Utils;
using namespace Encore::OS;


void noOpUDPFactory( test::UnitTestConfig const & pConfig ) {

    test::UnitTest<test::ErrorHandlingExtension> ut( pConfig );

    NoOpUDPSocketFactory f;

    auto sock = f.create( kTestInitOnly, UDPSocketParams{} );

    ut.assertFalse( sock->isOpen(), "Socket not open" );
    ut.succeedsWithValue( sock->open(), true, "Open socket" );
    ut.assertTrue( sock->isOpen(), "Socket now open" );

    ut.succeedsWithValue( sock->sendTo( BytesView{}, IPAndPort{} ), std::size_t{0}, "Trivial write" );
    ut.succeedsWithValue( sock->read( BytesRange{} ), std::size_t{0}, "Trivial no read" );

    auto ri = sock->recvFrom( BytesRange{} );
    ut.isSuccessful( ri, "Successful but..." );
    ut.assertFalse( ri.value().hasValue(), "...Nothing read" );

    ut.succeedsWithValue( sock->close(), true, "Close socket" );
}

}

UNITTESTER_MAIN( test_UDP, int pArgc, char const ** pArgv ) {

    test::UnitTester  tester( pArgc, pArgv );

    return tester.filterAndExec( { { test::TestTypeID::misc, "NoOpUDPFactory", &noOpUDPFactory }
                                 } );
}
