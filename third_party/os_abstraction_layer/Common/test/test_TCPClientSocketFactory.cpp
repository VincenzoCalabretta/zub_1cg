
#include "../TCPClientSocketFactory.hpp"

#include <optional>

#include "encore/ErrorHandling/StdFswErrors.hpp"
#include "encore/ErrorHandling/test/UnitTestExtensions.hpp"
#include "encore/Logging/Logger.hpp"
#include "encore/UnitTesting/UnitTestingCore.hpp"

#include <parcore/Logging/OStreamLogger.hpp>

namespace {

namespace test = Encore::UnitTesting;
using namespace Encore;

static constexpr OS::TCPClientParams kFailOpen{ OS::IPAndPort{ {}, 12345 }, false };
static constexpr OS::TCPClientParams kFailParams{ OS::IPAndPort{ {}, 54321 }, false };

class SillySocket : public OS::IStreamSocket, public OS::ITCPClient {
public: /*IReaderWriter*/

    ErrorHandling::Result<std::size_t> write( Utils::BytesView const & ) final { return 0; }

    ErrorHandling::Result<std::size_t> read( Utils::BytesRange ) final { return 0; }

    ErrorHandling::Result<bool> open() final {

        if( params.ipAndPort.port == kFailOpen.ipAndPort.port ) {
            return EG_ERROR_CODE( ErrorHandling::StdFswErrors, logic_error );
        }

        mOpen = true;
        return true;
    }

public: /*IDevice*/

    bool isOpen() const final { return mOpen; }

    ErrorHandling::Result<bool> close() final {
        mOpen = false;
        return true;
    }

public: /*ITCPClient*/

    ErrorHandling::ReturnCode setTCPClientParams( OS::TCPClientParams const & pParams ) final {

        if( pParams.ipAndPort.port == kFailParams.ipAndPort.port ) {
            return EG_ERROR_CODE( ErrorHandling::StdFswErrors, logic_error );
        }

        params = pParams;

        return {};
    }

public:

    OS::TCPClientParams params{};

private:

    bool mOpen{false};
};



void useCase( test::UnitTestConfig const & pConfig ) {

    test::UnitTest<test::ErrorHandlingExtension> ut{ pConfig };

    auto tok = Logging::LogGroup::requestDefaultLogAPIToken( kTestInitOnly );
    ut.isSuccessful( tok, "Token success" );
    ut.isSuccessful( Logging::LogGroup::getDefaultLogger().setBackend<PAR::OStreamLogger>( kTestInitOnly
                                                                                                      , tok.value()
                                                                                                      , std::cout )
                   , "set logger");

    OS::FixedTCPClientSocketFactory<SillySocket, 2> factory;
    factory.routeLoggingTo( Logging::LogGroup::getDefaultLogger() );

    ut.areEqual( &factory.logger(), &Logging::LogGroup::getDefaultLogger(), "Logger is correct" );

    SillySocket * sock1;
    {
        OS::TCPClientParams tcpParams;
        tcpParams.ipAndPort.addr = OS::IPv4Address{ kTestInitOnly, "192.168.101.1" };
        tcpParams.ipAndPort.port = 1234;
        tcpParams.nonBlocking = true;
        auto res = factory.create( tcpParams );
        ut.isSuccessful( res, "Created socket1" );
        sock1 = dynamic_cast<SillySocket *>( res.value() );
    }

    ut.assertTrue( sock1->isOpen(), "Socket 1 opened successfully" );
    ut.areEqual( sock1->params.nonBlocking, true, "Socket 1 non-blocking correct" );
    ut.areEqual( sock1->params.ipAndPort.addr.asArray()
               , std::array<std::uint8_t, 4>{192,168,101,1}
               , "Socket 1 addr correct" );
    ut.areEqual( sock1->params.ipAndPort.port, 1234, "Socket 1 port correct" );

    ut.failsWithCode( factory.create( kFailOpen )
                    , EG_ERROR_CODE( ErrorHandling::StdFswErrors, logic_error )
                    , "Failed open" );
    ut.failsWithCode( factory.create( kFailParams )
                    , EG_ERROR_CODE( ErrorHandling::StdFswErrors, logic_error )
                    , "Failed params" );

    SillySocket * sock2;
    {
        OS::TCPClientParams tcpParams;
        tcpParams.ipAndPort.addr = OS::IPv4Address{ kTestInitOnly, "127.0.0.1" };
        tcpParams.ipAndPort.port = 5;
        tcpParams.nonBlocking = false;
        auto res = factory.create( tcpParams );
        ut.isSuccessful( res, "Created socket2" );
        sock2 = dynamic_cast<SillySocket *>( res.value() );
    }

    ut.assertTrue( sock2->isOpen(), "Socket 2 opened successfully" );
    ut.areEqual( sock2->params.nonBlocking, false, "Socket 2 non-blocking correct" );
    ut.areEqual( sock2->params.ipAndPort.addr.asArray()
               , std::array<std::uint8_t, 4>{127,0,0,1}
               , "Socket 2 addr correct" );
    ut.areEqual( sock2->params.ipAndPort.port, 5, "Socket 2 port correct" );

    ut.failsWithCode( factory.create( {} ), EG_ERROR_CODE( ErrorHandling::StdFswErrors, length_error )
                    , "Failed to create more sockets than allowed" );

    factory.destroy( *sock1 );

    OS::NoOpStreamSocket noOpSock;
    factory.destroy( noOpSock ); // Coverage

    SillySocket * sock3;
    {
        OS::TCPClientParams tcpParams;
        tcpParams.ipAndPort.addr = OS::IPv4Address{ kTestInitOnly, "0.0.0.0" };
        tcpParams.ipAndPort.port = 1138;
        tcpParams.nonBlocking = false;
        auto res = factory.create( tcpParams );
        ut.isSuccessful( res, "Created socket3" );
        sock3 = dynamic_cast<SillySocket *>( res.value() );
    }

    ut.assertTrue( sock3->isOpen(), "Socket 3 opened successfully" );
    ut.areEqual( sock3->params.nonBlocking, false, "Socket 3 non-blocking correct" );
    ut.areEqual( sock3->params.ipAndPort.addr.asArray()
               , std::array<std::uint8_t, 4>{0,0,0,0}
               , "Socket 3 addr correct" );
    ut.areEqual( sock3->params.ipAndPort.port, 1138, "Socket 3 port correct" );
}

}

UNITTESTER_MAIN( test_TCPClientSocketFactory, int pArgc, char const ** pArgv ) {

    test::UnitTester tester( pArgc, pArgv );

    return tester.filterAndExec( { { test::TestTypeID::use_case, "UseCase", &useCase }, } );
}
