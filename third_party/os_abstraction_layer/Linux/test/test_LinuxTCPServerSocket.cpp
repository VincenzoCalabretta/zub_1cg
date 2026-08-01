
#include "encore/OS/Linux/LinuxTCPServerSocket.hpp"

#include <chrono>
#include <future>

#include "encore/UnitTesting/UnitTestingCore.hpp"
#include "encore/ErrorHandling/test/UnitTestExtensions.hpp"

#include "encore/OS/Common/IPv4Address.hpp"
#include "encore/OS/Linux/test/TestTCPServer.hpp"

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
    auto accepted = std::async(std::launch::async, [&]() {
        serverFd = server.accept();
    } );

    int clientFd = ::socket( AF_INET, SOCK_STREAM, 0 );
    ut.assertTrue( clientFd >= 0, "Client socket created" );

    {
        sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = ::htonl( serverAddr.addr.asInteger() );
        addr.sin_port = ::htons( serverAddr.port );

        ut.areEqual( ::connect( clientFd, reinterpret_cast<sockaddr const *>( &addr ), sizeof( addr ) ), 0
                   , "Client socket connect successful" );
    }

    using namespace std::chrono_literals;
    accepted.wait_for(1000ms);

    ut.assertTrue( serverFd >= 0, "Server accepted connection successfully" );

    OS::LinuxTCPServerSocket socket;
    socket.reset( serverFd );

    ut.isSuccessful( socket.open(), "Successfully \"opened\" server socket" );

    {
        constexpr std::string_view kMessage{ "Greetings" };
        Utils::BytesView view{ reinterpret_cast<EncoreByte const *>(kMessage.data()), kMessage.size() };
        ut.isSuccessful( socket.write( view ), "Sent via server socket" );
    
        auto numRead = ::read( clientFd, data.data(), data.size() );
        ut.areEqual( static_cast<std::size_t>( numRead ), kMessage.size(), "Client socket received sent data" );

        std::string_view received{ reinterpret_cast<char const *>( data.data() ), static_cast<std::size_t>( numRead ) };
        
        ut.areEqual( received, kMessage, "Client received the correct data" );
    }

    {
        constexpr std::string_view kMessage{ "Bonjour" };
        auto numSent = ::send( clientFd, kMessage.data(), kMessage.size(), 0 );
        ut.areEqual( numSent, static_cast<ssize_t>(kMessage.size()), "Client sent correct number of bytes" );

        Utils::BytesRange range{ data.data(), data.size() };
        auto recvd = socket.read( range );
        ut.isSuccessful( recvd, "Server socket successfully received client data" );

        std::string_view received{ reinterpret_cast<char const *>( data.data() ), recvd.value() };

        ut.areEqual( received, kMessage, "Server socket received correct data");
    }

    ut.isSuccessful( socket.close(), "Server socket closed" );
    ut.assertFalse( socket.isOpen(), "Socket is no-longer open" );
    ut.areEqual( socket.getFd(), -1, "Socket FD is -1 after close" );

    ::close(clientFd);

    server.stop();
}

}

UNITTESTER_MAIN( test_LinuxTCPServerSocket, int pArgc, char const ** pArgv ) {

    test::UnitTester tester( pArgc, pArgv );

    return tester.filterAndExec( { { test::TestTypeID::use_case, "UseCase", &useCase }, } );
}
