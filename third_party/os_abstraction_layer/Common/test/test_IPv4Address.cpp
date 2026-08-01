
#include "OS/Common/IPv4Address.hpp"

#include <cstdint>

#include "UnitTesting/UnitTestingCore.hpp"

namespace {

namespace test = Encore::UnitTesting;
using namespace Encore;
using namespace Encore::OS;


void basicAPI( test::UnitTestConfig const & pConfig ) {

    test::UnitTest<void> ut( pConfig );

    constexpr IPv4Address addr1;

    ut.areEqual( addr1.asArray(), {127,0,0,1}, "Default address as array" );
    ut.areEqual( addr1.asInteger(), 2130706433u, "Default address as integer" );

    static_assert( kLoopBack == addr1 );
    ut.areEqual( kLoopBack, addr1, "Loopback is default IPv4 address" );

    constexpr IPv4Address addr2( {0,0,10,10} );

    ut.areEqual( addr2.asArray(), {0,0,10,10}, "Custom address as array" );
    ut.areEqual( addr2.asInteger(), 2570u, "Custom address as integer" );

    IPv4Address addr3( kTestInitOnly, "0.0.10.10" );

    ut.areEqual( addr3.asArray(), {0,0,10,10}, "Custom address as array 2" );
    ut.areEqual( addr3.asInteger(), 2570u, "Custom address as integer 2" );

    constexpr IPv4Address addr4( 2130706433u );

    static_assert( addr4.asArray()[0] == 127 );
    static_assert( addr4.asArray()[1] == 0 );
    static_assert( addr4.asArray()[2] == 0 );
    static_assert( addr4.asArray()[3] == 1 );

    ut.areEqual( addr4.asArray(), {127,0,0,1}, "Custom address as array 3" );
    ut.areEqual( addr4.asInteger(), 2130706433u, "Custom address as integer 3" );

    IPAndPort ipAndPort1 = { IPv4Address(kTestInitOnly,"0.0.0.1"), NetworkPort{13} };

    ut.areEqual( ipAndPort1.addr.asInteger(), 1u, "IP ok" );
    ut.areEqual( ipAndPort1.port, NetworkPort{13}, "NetworkPort ok" );

    auto key = makeIPKey( ipAndPort1 );
    ut.areEqual( key, IPKey(4294967309ul), "Key ok" );

    auto ipAndPortFromKey = makeIPAndPort( key );
    ut.areEqual( ipAndPortFromKey.addr, ipAndPort1.addr, "Key to IPAndPort1" );
    ut.areEqual( ipAndPortFromKey.port, ipAndPort1.port, "Key to IPAndPort2" );

    constexpr IPAndPort ipAndPort2 = { kLoopBack, NetworkPort{65'000} };
    constexpr IPAndPort ipAndPort3 = { addr1, NetworkPort{65'000} };

    static_assert( ipAndPort2 == ipAndPort3 );
    ut.areEqual( ipAndPort2, ipAndPort3, "IPAndPort == operator" );

    IPv4Address addr5;
    addr5 = IPv4Address( {0,0,10,10} );

    ut.areEqual( addr2, addr5, "Move assignability by default" );

    IPv4Address addr6{};

    ut.areEqual( addr6.asArray(), {127,0,0,1}, "Default address as array" );
    ut.areEqual( addr6.asInteger(), 2130706433u, "Default address as integer" );

}


void constructorErrors( test::UnitTestConfig const & pConfig ) {

    test::UnitTest<void> ut( pConfig );

    ut.catchException<std::invalid_argument>( [](){ IPv4Address addr(kTestInitOnly, "0,10,10,0");   }
                                            , "Bad IP string (no dots)"   );
    ut.catchException<std::invalid_argument>( [](){ IPv4Address addr(kTestInitOnly, "0.1000.10.0"); }
                                            , "Bad IP string (big octet)" );
    ut.catchException<std::invalid_argument>( [](){ IPv4Address addr(kTestInitOnly, "1.-2.10.0"); }
                                            , "Bad IP string (negative octet)" );
}


void ipAndPortEquality( test::UnitTestConfig const & pConfig ) {

    test::UnitTest<void> ut( pConfig );

    IPAndPort ipAndPort1 = { IPv4Address(), NetworkPort{65'000} };
    IPAndPort ipAndPort2 = { kLoopBack, NetworkPort{65'000} };
    IPAndPort ipAndPort3 = { IPv4Address( std::array<std::uint8_t,4>{127,0,0,1} ), NetworkPort{16'000} };
    IPAndPort ipAndPort4 = { IPv4Address(114), NetworkPort{16'000} };

    ut.areEqual( ipAndPort1, ipAndPort2, "Equality" );
    ut.notEqual( ipAndPort1, ipAndPort3, "Ports don't match" );
    ut.notEqual( ipAndPort3, ipAndPort4, "IPs don't match" );
}

}

UNITTESTER_MAIN( test_IPv4Address, int pArgc, char const** pArgv ) {

    test::UnitTester  tester( pArgc, pArgv );

    return tester.filterAndExec( { { test::TestTypeID::micro, "BasicAPI", &basicAPI }
                                 , { test::TestTypeID::misc , "ConstructorErrors", &constructorErrors }
                                 , { test::TestTypeID::misc , "IPAndPortEquality", &ipAndPortEquality }
                                 } );
}
