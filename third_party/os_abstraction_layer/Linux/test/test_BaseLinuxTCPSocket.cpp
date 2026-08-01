
#include "../BaseLinuxTCPSocket.hpp"

#include <sys/types.h>
#include <sys/socket.h>

#include <chrono>
#include <future>

#include "encore/UnitTesting/UnitTestingCore.hpp"
#include "encore/ErrorHandling/test/UnitTestExtensions.hpp"

#include <parcore/Logging/OStreamLogger.hpp>

#include "TestTCPServer.hpp"

namespace {

namespace test = Encore::UnitTesting;
using namespace Encore;

class SimpleSocket final : public OS::BaseLinuxTCPSocket {
public: /*ISocket*/

    ErrorHandling::Result<bool> open() final { return true; }

public:

    using OS::BaseLinuxTCPSocket::setFd;
    using OS::BaseLinuxTCPSocket::setConnected;
};

class TestFixture {
public:

    TestFixture()
    : server{ serverAddr }
    {}

    ~TestFixture() {

        server.stop();
        std::ignore = serverSocket.close();
        std::ignore = clientSocket.close();
    }

    void setup( test::UnitTest<void> & pUt ) {

        pUt.areEqual( server.start(), 0, "Server starts" );

        serverAddr.port = static_cast<OS::NetworkPort>( server.port() );

        auto serverFd = std::async( std::launch::async, [&]() -> int {
            return server.accept();
        } );

        auto clientFd = ::socket( AF_INET, SOCK_STREAM, 0 );
        pUt.notEqual( clientFd, -1, "Client FD is good" );

        {
            sockaddr_in addr;
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = ::htonl( serverAddr.addr.asInteger() );
            addr.sin_port = ::htons( serverAddr.port );

            pUt.notEqual( ::connect( clientFd, reinterpret_cast<sockaddr const *>(&addr), sizeof(addr) ), -1
                        , "Client connects" );
        }

        clientSocket.setFd( clientFd );
        clientSocket.setConnected( true );

        using namespace std::chrono_literals;
        auto res = serverFd.wait_for( 1000ms );
        if( res == std::future_status::timeout ) {
            std::cerr << "Failed to accept socket connection" << std::endl;
            std::terminate(); // Regular error checks will invoke the destructor which will cause the std::future object
                              // to block forever. Terminate aborts hard and fast so we don't break CI.
        }

        int fd = serverFd.get();
        pUt.notEqual( fd, -1, "Server FD valid" );

        serverSocket.setFd( fd );
        serverSocket.setConnected( true );
    }

    OS::IPAndPort serverAddr{};
    test::TestTCPServer server;
    SimpleSocket clientSocket;
    SimpleSocket serverSocket;
};

void isOpen( test::UnitTestConfig const & pConfig ) {

    test::UnitTest<void> ut{ pConfig };

    SimpleSocket socket;
    socket.setConnected( false );
    socket.setFd( -1 );

    ut.assertFalse( socket.isOpen(), "Is closed when not connected with bad fd" );

    socket.setConnected( true );
    ut.assertFalse( socket.isOpen(), "Is closed when connected with bad fd" );

    socket.setConnected( false );
    socket.setFd( 0 );
    ut.assertFalse( socket.isOpen(), "Is closed when not connected with good fd" );

    socket.setConnected( true );
    ut.assertTrue( socket.isOpen(), "Is open when connected with good fd" );
}


void notOpenErrors( test::UnitTestConfig const & pConfig ) {

    test::UnitTest<test::ErrorHandlingExtension> ut{ pConfig };

    SimpleSocket socket;
    socket.setConnected( false );
    socket.setFd( -1 );

    using namespace Encore::ErrorHandling;
    ut.failsWithCode( socket.write( {} ), buildCode<Utils::DeviceErrors>( Utils::DeviceErrors::not_open )
                    , "Write fails" );

    Utils::BytesRange range{nullptr, 0};
    ut.failsWithCode( socket.read( range ), buildCode<Utils::DeviceErrors>( Utils::DeviceErrors::not_open )
                    , "Read fails" );
}


void endOfStream( test::UnitTestConfig const & pConfig ) {

    test::UnitTest<test::ErrorHandlingExtension> ut{ pConfig };

    auto tok = Logging::LogGroup::requestDefaultLogAPIToken( kTestInitOnly );
    ut.isSuccessful( tok, "Token success" );
    ut.isSuccessful( Logging::LogGroup::getDefaultLogger().setBackend<PAR::OStreamLogger>( kTestInitOnly
                                                                                         , tok.value()
                                                                                         , std::cout )
                   , "set logger");

    TestFixture fixture;

    fixture.setup( ut );
    fixture.clientSocket.routeLoggingTo( Logging::LogGroup::getDefaultLogger() );
    fixture.serverSocket.routeLoggingTo( Logging::LogGroup::getDefaultLogger() );

    EncoreByte sent{7};
    ut.isSuccessful( fixture.serverSocket.write( { &sent, sizeof(sent) } ), "Write successfully" );

    EncoreByte received{};
    Utils::BytesRange range{ &received, sizeof(received) };

    ut.succeedsWithValue( fixture.clientSocket.read( range ), std::size_t{1u}, "Read data successfully" );
    ut.areEqual( received, 7, "Received correct data" );

    ut.succeedsWithValue( fixture.clientSocket.read( range ), std::size_t{0u}, "Empty read success" );
    ut.assertTrue( fixture.clientSocket.isOpen(), "Client socket still open after empty read" );

    Utils::BytesRange emptyRange{ nullptr, 0 };
    ut.succeedsWithValue( fixture.clientSocket.read( emptyRange ), std::size_t{0u}, "Read empty range" );
    ut.assertTrue( fixture.clientSocket.isOpen(), "Client socket still open after empty range provided" );

    ut.succeedsWithValue( fixture.serverSocket.close(), true, "Server socket closed" );

    ut.succeedsWithValue( fixture.clientSocket.read( range ), std::size_t{0u}, "End of stream read success" );
    ut.assertFalse( fixture.clientSocket.isOpen(), "Client socket closed after end of stream" );
}


void writeError( test::UnitTestConfig const & pConfig ) {

    test::UnitTest<test::ErrorHandlingExtension> ut{ pConfig };

    TestFixture fixture;

    fixture.setup( ut );

    ut.isSuccessful( fixture.clientSocket.close(), "Client closes connection" );

    using namespace Encore::ErrorHandling;
#ifndef ENCORE_DYNAMIC_ANALYSIS_TYPE_VALGRIND // Valgrind catches this intentional nullptr and raises an error.
    ut.failsWithCode( fixture.serverSocket.write( { nullptr, 1 } )
                    , buildCode<SystemErrors>( SystemErrors::bad_address ) , "Write fails with bad address" );
#endif
}


void readError( test::UnitTestConfig const & pConfig ) {

    test::UnitTest<test::ErrorHandlingExtension> ut{ pConfig };

    TestFixture fixture;

    fixture.setup( ut );

    EncoreByte sent{7};
    ut.isSuccessful( fixture.clientSocket.write( { &sent, sizeof(sent) } ), "Write successfully" );

    using namespace Encore::ErrorHandling;
#ifndef ENCORE_DYNAMIC_ANALYSIS_TYPE_VALGRIND // Valgrind catches this intentional nullptr and raises an error.
    ut.failsWithCode( fixture.serverSocket.read( { nullptr, 1 } )
                    , buildCode<SystemErrors>( SystemErrors::bad_address ) , "Read fails with bad address" );
#endif
}


void zeroLengthRead( test::UnitTestConfig const & pConfig ) {

    test::UnitTest<test::ErrorHandlingExtension> ut{ pConfig };

    TestFixture fixture;

    fixture.setup( ut );

    EncoreByte sent{7};
    ut.isSuccessful( fixture.clientSocket.write( { &sent, sizeof(sent) } ), "Write successfully" );

    EncoreByte received{};
    Utils::BytesRange range{ &received, sizeof(received) };

    ut.succeedsWithValue( fixture.serverSocket.read( { &received, 0 } ), std::size_t{0u}, "Empty read" );
}


}

UNITTESTER_MAIN( test_BaseLinuxTCPSocket, int pArgc, char const ** pArgv ) {

    test::UnitTester tester( pArgc, pArgv );

    return tester.filterAndExec( { { test::TestTypeID::micro, "IsOpen", &isOpen }
                                 , { test::TestTypeID::misc, "NotOpenErrors", &notOpenErrors }
                                 , { test::TestTypeID::misc, "EndOfStream", &endOfStream }
                                 , { test::TestTypeID::misc, "WriteError", &writeError }
                                 , { test::TestTypeID::misc, "ReadError", &readError }
                                 , { test::TestTypeID::misc, "ZeroLengthRead", &zeroLengthRead }
                                 } );
}
