
#include "../LinuxTCPServer.hpp"

#include <unistd.h>

#include <string_view>
#include <vector>

#include "encore/UnitTesting/UnitTestingCore.hpp"
#include "encore/ErrorHandling/test/UnitTestExtensions.hpp"

namespace {

namespace test = Encore::UnitTesting;
using namespace Encore;

enum UseCaseParams {
    none = 0,
    no_delay = 1 << 0,
    listen_block = 1 << 1,
};

void useCase( UseCaseParams pParams, test::UnitTestConfig const & pConfig ) {

    test::UnitTest<test::ErrorHandlingExtension> ut{ pConfig };

    OS::FixedLinuxTCPServerParams params;
    params.queueDepth = 2;
    params.noDelay = (pParams & no_delay) != 0;
    params.listenBlock = (pParams & listen_block) != 0;
    OS::FixedLinuxTCPServer<2> server{ params };

    ut.isSuccessful( server.start(), "Server startup" );

    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons( server.getPort() );
    addr.sin_addr.s_addr = ::htonl( OS::kLoopBack.asInteger() );

    // Only want to try this with non-blocking accept calls
    if( (pParams & listen_block) == 0 ) {

        auto empty = server.accept();
        ut.isSuccessful( empty, "Empty accept call" );
        ut.areEqual( empty.value().socket, nullptr, "Empty socket" );
    }

    // NOLINTBEGIN(clang-analyzer-unix.StdCLibraryFunctions) -- While the linter is correct that client1 may be invalid
    // if ::socket fails, this will be caught when ::connect fails on a socket FD of -1.
    auto client1 = ::socket( AF_INET, SOCK_STREAM, 0 );
    ut.areEqual( ::connect( client1, reinterpret_cast<sockaddr const *>(&addr), sizeof( addr ) ), 0, "client 1 connects" );

    auto client2 = ::socket( AF_INET, SOCK_STREAM, 0 );
    ut.areEqual( ::connect( client2, reinterpret_cast<sockaddr const *>(&addr), sizeof( addr ) ), 0, "client 2 connects" );
    // NOLINTEND(clang-analyzer-unix.StdCLibraryFunctions)

    auto server1 = server.accept();
    ut.isSuccessful( server1, "Server socket 1 accepted" );
    ut.notEqual( server1.value().socket, nullptr, "Server socket 1 is not null" );

    auto server2 = server.accept();
    ut.isSuccessful( server2, "Server socket 2 accepted" );
    ut.notEqual( server2.value().socket, nullptr, "Server socket 2 is not null" );

    using namespace Encore::ErrorHandling;
    ut.failsWithCode( server.accept(), buildCode<StdFswErrors>(StdFswErrors::length_error), "Can't accept any more" );

    // Test send
    std::vector<EncoreByte> data;
    data.resize(64);
    {
        constexpr std::string_view kMessage{ "12n4(2jng34g" };
        ut.succeedsWithValue( server1.value().socket->write( { reinterpret_cast<EncoreByte const *>(kMessage.data()), kMessage.size() } ), kMessage.size(), "server sends" );

        ut.areEqual( ::read( client1, data.data(), data.size() ), static_cast<ssize_t>( kMessage.size() ), "Client reads" );

        std::string_view kReceived{ reinterpret_cast<char const *>(data.data()), kMessage.size() };

        ut.areEqual( kReceived, kMessage, "Received message correct" );
    }

    // Test receive
    {
        constexpr std::string_view kMessage{ "oowjwaw62" };

        ut.areEqual( ::write( client1, kMessage.data(), kMessage.size() ), static_cast<ssize_t>( kMessage.size() ), "Client sends" );

        ut.succeedsWithValue( server1.value().socket->read( { reinterpret_cast<EncoreByte *>(data.data()), data.size() } ), kMessage.size(), "server receives" );

        std::string_view kReceived{ reinterpret_cast<char const *>(data.data()), kMessage.size() };

        ut.areEqual( kReceived, kMessage, "Received message correct" );
    }

    server.release( *server1.value().socket );
    server.release( *server2.value().socket );
    OS::NoOpStreamSocket noop;
    server.release( noop ); // For coverage

    ut.isSuccessful( server.stop(), "Server shutdown" );

    ::close( client1 );
    ::close( client2 );

    ut.failsWithCode( server.stop(), buildCode<SystemErrors>( SystemErrors::bad_file_descriptor ), "Already stopped" );
}

void forceBindFail( test::UnitTestConfig const & pConfig ) {

    test::UnitTest<test::ErrorHandlingExtension> ut{ pConfig };

    OS::FixedLinuxTCPServer<3> server1;

    ut.isSuccessful(server1.start(), "Server 1 startup");

    OS::FixedLinuxTCPServerParams params2;
    params2.port = server1.getPort(); // Should induce bind failure
    OS::FixedLinuxTCPServer<3> server2{params2};

    using namespace Encore::ErrorHandling;
    ut.failsWithCode( server2.start(), buildCode<SystemErrors>(SystemErrors::address_in_use), "Can't bind to same port" );

    ut.isSuccessful(server1.stop(), "Server 1 shutdown");

}

void acceptFail( test::UnitTestConfig const & pConfig ) {

    test::UnitTest<test::ErrorHandlingExtension> ut{ pConfig };

    OS::FixedLinuxTCPServer<3> server;

    ut.failsWithCode( server.accept(), EG_ERROR_CODE( ErrorHandling::SystemErrors, bad_file_descriptor ), "bad fd" );
}

}

UNITTESTER_MAIN( test_LinuxTCPServer, int pArgc, char const ** pArgv ) {

    test::UnitTester tester( pArgc, pArgv );

    return tester.filterAndExec( {
        { test::TestTypeID::use_case, "UseCase", [](auto && pConfig) { useCase(none, pConfig); } },
        { test::TestTypeID::use_case, "NoDelay", [](auto && pConfig) { useCase(no_delay, pConfig); } },
        { test::TestTypeID::use_case, "ListenBlock", [](auto && pConfig) { useCase(listen_block, pConfig); } },
        { test::TestTypeID::use_case, "NoDelayListenBlock", [](auto && pConfig) { useCase(UseCaseParams(no_delay | listen_block), pConfig); } },
        { test::TestTypeID::misc, "ForceBindFail", &forceBindFail },
        { test::TestTypeID::misc, "AcceptFail", &acceptFail },
    } );
}
