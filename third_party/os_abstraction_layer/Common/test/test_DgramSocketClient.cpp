
#include "OS/Common/DgramSocketClient.hpp"

#include "UnitTesting/UnitTestingCore.hpp"
#include "ErrorHandling/test/UnitTestExtensions.hpp"
#include "OS/Common/test/MockDgramSocket.hpp"

namespace {

namespace test = Encore::UnitTesting;
using namespace Encore;
using namespace Encore::Messaging;
using namespace Encore::OS;
using namespace Encore::Utils;

void useCase( test::UnitTestConfig const & pConfig ) {

    test::UnitTest<test::ErrorHandlingExtension> ut{ pConfig };

    MockDgramSocket socket{};

    DgramSocketClient client{socket};

    ut.areEqual( socket.numWrites, 0, "No writes initially" );
    ut.areEqual( socket.numReads, 0, "No reads initially" );

    ut.isSuccessful( client.init( kTestInitOnly ), "Init success" );

    ut.succeedsWithValue( client.sendMsg( BytesView{nullptr, 3} ), std::size_t{3}, "Send success" );
    ut.areEqual( socket.numWrites, 1, "Wrote once" );
    ut.areEqual( socket.numReads, 0, "Still no reads" );

    ut.succeedsWithValue( client.recvMsg( BytesRange{nullptr, 7} ), std::size_t{7}, "Recv success" );
    ut.areEqual( socket.numWrites, 1, "Wrote once still" );
    ut.areEqual( socket.numReads, 1, "Read once" );
}

void misc( test::UnitTestConfig const & pConfig ) {

    test::UnitTest<test::ErrorHandlingExtension> ut{ pConfig };

    MockDgramSocket socket{};

    DgramSocketClient client{socket};

    ut.isSuccessful( client.init( kTestInitOnly ), "Init success" );

    ut.failsWithCode( client.recvMsg( BytesRange{nullptr, 0} )
                    , EG_ERROR_CODE( MessageClientErrors, destination_range_too_small )
                    , "Empty recv fails"
                    );
}

}

UNITTESTER_MAIN( test_DgramSocketClient, int pArgc, char const ** pArgv ) {

    test::UnitTester tester( pArgc, pArgv );

    return tester.filterAndExec( { { test::TestTypeID::use_case, "UseCase", &useCase }
                                 , { test::TestTypeID::misc, "Misc", &misc }
                                 } );
}

