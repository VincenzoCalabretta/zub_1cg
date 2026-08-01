
#include "encore/OS/Linux/LinuxTCPClientSocket.hpp"

#include <chrono>
#include <future>
#include <vector>

#include "encore/UnitTesting/UnitTestingCore.hpp"
#include "encore/ErrorHandling/test/UnitTestExtensions.hpp"

#include "encore/OS/Linux/test/TestTCPServer.hpp"

#include "encore/Utils/IClock.hpp"

namespace {

namespace test = Encore::UnitTesting;
using namespace Encore;

void useCase( test::UnitTestConfig const & pConfig ) {

    test::UnitTest<test::ErrorHandlingExtension> ut{ pConfig };

    OS::IPAndPort serverAddr;
    test::TestTCPServer server{ serverAddr };

    std::vector<EncoreByte> data{};
    data.resize( 64 );

    auto rc = server.start();
    ut.areEqual( rc, 0, "Server started" );

    serverAddr.port = static_cast<OS::NetworkPort>( server.port() );

    int serverFd{-1};
    auto accepted = std::async( std::launch::async, [&]() {
        serverFd = server.accept();
    } );

    OS::TCPClientParams params;
    params.ipAndPort = serverAddr;
    params.nonBlocking = false;

    OS::LinuxTCPClientSocket socket;
    ut.isSuccessful( socket.setTCPClientParams( params ), "Set socket open params" );
    ut.areEqual( socket.isOpen(), false, "Socket is not open before... open" );

    ut.succeedsWithValue( socket.open(), true, "Client socket opened" );
    ut.areEqual( socket.isOpen(), true, "Socket is open" );
    ut.assertTrue( socket.getFd() >= 0, "Socket FD is valid" );

    using namespace std::chrono_literals;
    accepted.wait_for(1000ms);

    ut.assertTrue( serverFd >= 0, "Server accepted connection successfully" );

    {
        constexpr std::string_view kMessage{ "Greetings" };
        Utils::BytesView view{ reinterpret_cast<EncoreByte const *>(kMessage.data()), kMessage.size() };
        ut.isSuccessful( socket.write( view ), "Sent via client socket" );
    
        auto numRead = ::read( serverFd, data.data(), data.size() );
        ut.areEqual( static_cast<std::size_t>( numRead ), kMessage.size(), "Server side socket received sent data" );

        std::string_view received{ reinterpret_cast<char const *>( data.data() ), static_cast<std::size_t>( numRead ) };
        
        ut.areEqual( received, kMessage, "Server received the correct data" );
    }

    {
        constexpr std::string_view kMessage{ "Bonjour" };
        auto numSent = ::send( serverFd, kMessage.data(), kMessage.size(), 0 );
        ut.areEqual( numSent, static_cast<ssize_t>(kMessage.size()), "Server sent correct number of bytes" );

        Utils::BytesRange range{ data.data(), data.size() };
        auto recvd = socket.read( range );
        ut.isSuccessful( recvd, "Client socket successfully received server data" );

        std::string_view received{ reinterpret_cast<char const *>( data.data() ), recvd.value() };

        ut.areEqual( received, kMessage, "Client socket received correct data");
    }

    ut.succeedsWithValue( socket.close(), true, "Client socket closes" );
    ut.areEqual( socket.getFd(), -1, "Client socket FD is -1 after close" );

    ::close(serverFd);

    server.stop();
}


void nonBlocking( test::UnitTestConfig const & pConfig ) {

    test::UnitTest<test::ErrorHandlingExtension> ut{ pConfig };

    OS::LinuxTCPClientSocket socket;
    ut.isSuccessful( socket.setTCPClientParams( {} ), "Setting params is successful" );

    ut.failsWithCode( socket.open(), ErrorHandling::buildCode<ErrorHandling::SystemErrors>( ECONNREFUSED ), "Socket open refused" );
    ut.notEqual( socket.getFd(), -1, "Socket FD is valid but..." );
    ut.assertFalse( socket.isOpen(), "... socket is not open" );

    ut.isSuccessful( socket.close(), "Close socket after failed open" );

    OS::IPAndPort serverAddr;
    test::TestTCPServer server{ serverAddr };

    std::vector<EncoreByte> data{};
    data.resize( 64 );

    auto rc = server.start();
    ut.areEqual( rc, 0, "Server started" );

    serverAddr.port = static_cast<OS::NetworkPort>( server.port() );

    OS::TCPClientParams params;
    params.ipAndPort = serverAddr;
    params.nonBlocking = true;

    ut.isSuccessful( socket.setTCPClientParams( params ), "Set socket open params" );
    ut.succeedsWithValue( socket.open(), false, "Socket open successful but not complete" );
    ut.failsWithCode( socket.setTCPClientParams( params ), ErrorHandling::buildCode<Utils::DeviceErrors>( Utils::DeviceErrors::must_be_closed ), "Set socket open params" );
    auto fd = socket.getFd();
    ut.assertTrue( fd >= 0, "We have a valid file descriptor" );
    ut.assertFalse( socket.isOpen(), "Socket still not open yet" );

    int serverFd;
    auto accepted = std::async( std::launch::async, [&]() {
        serverFd = server.accept();
    } );

    ut.succeedsWithValue( socket.poll( 1'000'000'000 ), true, "Polling successful" );
    
    ut.succeedsWithValue( socket.open(), true, "Client socket finished opening" );
    ut.areEqual( socket.isOpen(), true, "Socket is open" );
    ut.areEqual( socket.getFd(), fd, "Socket file descriptor is valid and hasn't changed" );

    using namespace std::chrono_literals;
    accepted.wait_for(1'000ms);

    ut.succeedsWithValue( socket.close(), true, "Client socket closes" );

    ::close(serverFd);

    server.stop();
}

}

UNITTESTER_MAIN( test_LinuxTCPClientSocket, int pArgc, char const ** pArgv ) {

    test::UnitTester tester( pArgc, pArgv );

    return tester.filterAndExec( { { test::TestTypeID::use_case, "UseCase", &useCase }
                                 , { test::TestTypeID::use_case, "NonBlocking", &nonBlocking }
                                 } );
}
