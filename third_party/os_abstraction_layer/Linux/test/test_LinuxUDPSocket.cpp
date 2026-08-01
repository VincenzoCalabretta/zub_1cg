
#include "OS/Linux/LinuxUDPSocket.hpp"

#include <cstdint>

#include "UnitTesting/UnitTestingCore.hpp"
#include "ErrorHandling/ErrorGroup.hpp"
#include "ErrorHandling/StdFswErrors.hpp"
#include "ErrorHandling/SystemErrors.hpp"
#include "ErrorHandling/test/UnitTestExtensions.hpp"
#include "Utils/Span.hpp"
#include "OS/Common/IPv4Address.hpp"

namespace {

namespace test = Encore::UnitTesting;
using namespace Encore;
using namespace Encore::ErrorHandling;
using namespace Encore::Utils;
using namespace Encore::OS;


void basicAPI( test::UnitTestConfig const & pConfig ) {

    test::UnitTest<test::ErrorHandlingExtension> ut( pConfig );

    LinuxUDPSocket sock( {{0,0,0,0}}, NetworkPort{65000} );

    ut.areEqual( sock.getFd(), -1, "File descriptor set to -1 before open" );
    ut.isSuccessful( sock.open(), "Opened a socket" );
    ut.notEqual( sock.getFd(), -1, "File descriptor different from -1 after open" );

    std::array<EncoreByte,16> writeBuf{};
    writeBuf.fill(1);
    ut.isSuccessful( sock.write( BytesView{writeBuf} ), "Write to a socket" );

    std::array<EncoreByte,16> readBuf{};
    ut.isSuccessful( sock.read( BytesRange{readBuf} ), "Read from a socket" );

    IPAndPort ipp{};
    ipp.addr = IPv4Address( {0,0,0,0} );
    ipp.port = NetworkPort{65'000};

    ut.isSuccessful( sock.sendTo( BytesView{writeBuf}, ipp ), "Same behavior with sendTo" );

    auto info = sock.recvFrom( BytesRange{readBuf} );
    ut.isSuccessful( info, "Good recvFrom" );
    ut.assertTrue( info.value().hasValue(), "Actual read done..." );
    ut.areEqual( info.value().value().ipp.addr, ipp.addr, "Source IP matches" );
    ut.areEqual( info.value().value().numBytes, 16u, "Number of bytes read good" );

    info = sock.recvFrom( BytesRange{readBuf} );
    ut.isSuccessful( info, "Another good recvFrom" );
    ut.assertFalse( info.value().hasValue(), "No data this time" );

    ut.isSuccessful( sock.close(), "Closed a socket" );
    ut.areEqual( sock.getFd(), -1, "File descriptor back to -1 after close" );
}


void testGetSetSendAddr( test::UnitTestConfig const & pConfig ) {

    test::UnitTest<test::ErrorHandlingExtension> ut( pConfig );

    LinuxUDPSocket sock( {{127,0,0,1}}, NetworkPort{65250} );

    sock.setSendAddr( {{127,0,0,1}}, NetworkPort{65251} );
    auto sp = sock.getSendAddr();
    ut.areEqual( sp.addr, IPv4Address({127,0,0,1}), "Send IP ok" );
    ut.areEqual( sp.port, NetworkPort{65251}, "Send port ok" );

    ut.isSuccessful( sock.open(), "Opened a socket" );

    std::array<EncoreByte,16> writeBuf{};
    writeBuf.fill('1');

    auto written = sock.write( BytesView{writeBuf} );
    ut.isSuccessful( written, "Write to a socket" );
    ut.areEqual( written.value(), 16u, "Number of bytes written ok" );
}


void ipConnectConstructor( test::UnitTestConfig const & pConfig ) {

    test::UnitTest<test::ErrorHandlingExtension> ut( pConfig );

    IPAndPort e1{IPv4Address({127,0,0,1}),NetworkPort{65252}};
    IPAndPort e2{IPv4Address({127,0,0,1}),NetworkPort{65253}};

    LinuxUDPSocket sock( IPConnection{e1,e2} );

    ut.isSuccessful( sock.open(), "Opened a socket" );

    std::array<EncoreByte,16> writeBuf{};
    writeBuf.fill('1');

    auto written = sock.write( BytesView{writeBuf} );
    ut.isSuccessful( written, "Write to a socket" );
    ut.areEqual( written.value(), 16u, "Number of bytes written ok" );
}


void branchCoverage( test::UnitTestConfig const & pConfig ) {

    {
        test::UnitTest<void> ut( "LinuxUDPSocket::Misc::BindAndCloseFail", pConfig );

        LinuxUDPSocket good( {{127,0,0,1}}, NetworkPort{65251} );
        ut.assertTrue( good.open().success(), "Created and bound to socket" );

        LinuxUDPSocket sock( {{127,0,0,1}}, NetworkPort{65251} );

        auto res = sock.open();

        ut.assertFalse( res.success(), "Created a socket but failed to bind" );
        ut.assertTrue( res.error().message().size() > 0, "Error message for bind" );
        ut.assertTrue( res.error().payload() > 0, "Payload (errno) is non-zero" );

        res = sock.close();

        ut.assertFalse( res.success(), "... but you can't close it again" );
        ut.assertTrue( res.error().message().size() > 0, "Error message for close" );
        ut.assertTrue( res.error().payload() > 0, "Payload (errno) is non-zero" );
    }
    {
        test::UnitTest<test::ErrorHandlingExtension> ut( "LinuxUDPSocket::Misc::RecvFromFail", pConfig );

        LinuxUDPSocket sock( {{0,0,0,0}}, NetworkPort{0} );

        ut.failsWithCode( sock.recvFrom( BytesRange{nullptr,0} )
                        , buildCode<SystemErrors>( SystemErrors::bad_file_descriptor )
                        , "Bad file descriptor (cannot recvFrom w/out opening socket" );
    }
    {
        test::UnitTest<void> ut( "LinuxUDPSocket::Misc::ReadAndWriteFail", pConfig );

        LinuxUDPSocket sock( {{0,0,0,0}}, NetworkPort{0} );

        ut.assertTrue( sock.open().success(), "Open and bound to a socket but..." );

        std::array<EncoreByte,16> writeBuf{};
        writeBuf.fill(1);

        auto res = sock.write( BytesView{writeBuf} );
        ut.assertFalse( res.success(), "... fail to write" );
        ut.assertTrue( res.error().message().size() > 0, "Error message for write" );
        ut.assertTrue( res.error().payload() > 0, "Payload (errno) is non-zero" );

        std::array<EncoreByte,16> readBuf{};
        res = sock.read( BytesRange{readBuf} );

        ut.assertTrue( res.success(), "EAGAIN read success" );
        ut.areEqual( res.value(), 0u, "EAGAIN read size" );

        // Close socket so that read will fail
        (void)sock.close();

        res = sock.read( BytesRange{readBuf} );

        ut.assertFalse( res.success(), "Also fail to read" );
        ut.assertTrue( res.error().message().size() > 0, "Error message for read" );
        ut.assertTrue( res.error().payload() > 0, "Payload (errno) is non-zero" );
    }
    {
        test::UnitTest<test::ErrorHandlingExtension> ut( "LinuxUDPSocket::Misc::OpenFail", pConfig );


        std::size_t const maxAttempts{10'000};

        std::vector<LinuxUDPSocket> sockets;
        sockets.reserve(maxAttempts);

        Result<bool> res{true};

        for( std::size_t i{0}; i < maxAttempts; ++i ) {

            sockets.push_back(LinuxUDPSocket( {{0,0,0,0}}, NetworkPort{65000} ));
            LinuxUDPSocket & sock = sockets.back();

            res = sock.open();

            if( not res.success() ) {
                break;
            }
        }

        for(auto & socket : sockets) {
            (void)socket.close();
        }

        ut.notSuccessful( res, "Contrived socket open() failure" );
    }
    {
        test::UnitTest<test::ErrorHandlingExtension> ut( "LinuxUDPSocket::Misc::ReadOnWriteOnly", pConfig );

        LinuxUDPSocket sock( {{0,0,0,0}}, NetworkPort{65000}, UDPSocketMode::write_only );

        ut.isSuccessful(sock.open(), "Socket opens");

        ut.failsWithCode(sock.read(BytesRange{}), buildCode<StdFswErrors>(StdFswErrors::not_implemented), "fail read");
    }
    {
        test::UnitTest<test::ErrorHandlingExtension> ut( "LinuxUDPSocket::Misc::DoubleOpen", pConfig );

        LinuxUDPSocket sock( {{0,0,0,0}}, NetworkPort{65000}, UDPSocketMode::write_only );

        ut.isSuccessful(sock.open(), "Open socket");

        auto fd1 = sock.getFd();

        ut.isSuccessful(sock.open(), "Open again?");

        auto fd2 = sock.getFd();

        ut.areEqual(fd1, fd2, "File descriptors are same -- repeat open() calls are no-op");
    }
}


void factory( test::UnitTestConfig const & pConfig ) {

    test::UnitTest<void> ut( pConfig );

    auto fac = std::make_unique<LinuxUDPSocketFactory>();

    auto socket = fac->create( kTestInitOnly, UDPSocketParams{} );

    ut.assertTrue( socket.get() != nullptr, "Factory made something" );
}

}

UNITTESTER_MAIN( test_LinuxUDPSocket, int pArgc, char const** pArgv ) {

    test::UnitTester  tester( pArgc, pArgv );

    return tester.filterAndExec( { { test::TestTypeID::micro, "Micro"  , &basicAPI   }
                                 , { test::TestTypeID::micro, "SendIP" , &testGetSetSendAddr }
                                 , { test::TestTypeID::micro, "IPConnectConstructor", &ipConnectConstructor }
                                 , { test::TestTypeID::misc , "BranchCoverage"   , &branchCoverage    }
                                 , { test::TestTypeID::micro, "Factory", &factory }
                                 } );
}
