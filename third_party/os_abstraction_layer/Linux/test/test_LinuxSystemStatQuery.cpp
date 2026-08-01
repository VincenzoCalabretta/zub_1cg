
///
/// @file
/// @brief
///

#include "../LinuxSystemStatQuery.hpp"
#include "../LinuxSystemStatQuerySerializer.hpp"

#include <vector>

#include "UnitTesting/UnitTester.hpp"
#include "UnitTesting/UnitTestConfig.hpp"
#include "UnitTesting/UnitTest.hpp"
#include "ErrorHandling/test/UnitTestExtensions.hpp"
#include "Utils/ASCIISerializer.hpp"
#include "Utils/test/ASCIIReaderWriter.hpp"

namespace Encore::OS {

inline bool operator==( LinuxSystemStatQuery const & lhs, LinuxSystemStatQuery const & rhs ) {
    return ( lhs.user == rhs.user && lhs.nice == rhs.nice && lhs.system == rhs.system && lhs.idle == rhs.idle &&
             lhs.iowait == rhs.iowait && lhs.irq == rhs.irq && lhs.softirq == rhs.softirq && lhs.steal == rhs.steal &&
             lhs.guest == rhs.guest && lhs.guest_nice == rhs.guest_nice &&
             std::memcmp( &lhs.cpu, &rhs.cpu, sizeof(lhs.cpu) ) == 0 );
}

}

namespace {

namespace test = Encore::UnitTesting;
using namespace Encore::ErrorHandling;
using namespace Encore::OS;
using namespace Encore;

void linuxSystemStatQuerySerializer( test::UnitTestConfig const & pConfig ) {
    test::UnitTest<test::ErrorHandlingExtension> ut( pConfig );


    LinuxSystemStatQuery q1{};
    q1.user = 1337;
    q1.nice = 99;
    q1.system = 1336;
    q1.idle = 1000;
    q1.iowait = 4;
    q1.irq = 4;
    q1.softirq = 4;
    q1.steal = 4;
    q1.guest = 4;
    q1.guest_nice = 4;
    q1.cpu = {'s', 'y', 's', 's', 't', 'a', 't'};

    ut.areEqual( q1.qType(), LinuxProcQueryType::system, "query type is correct" );

    Utils::Serializer<LinuxSystemStatQuery, Utils::SerializationPolicy::ascii_based> seri{};
    test::ASCIIReaderWriter arw;

    auto rc = seri.toBytes( q1, arw );
    ut.isSuccessful( rc, "serialization successful" );

    LinuxSystemStatQuery q2{};
    [[ maybe_unused ]] auto rc2 = seri.toObj( arw, q2 );
    ut.isSuccessful( rc2, "deserialization successful" );
    ut.areEqual( q1, q2, "data matches" );

    LinuxSystemStatQuery q3{};
    q3.user = 1337;
    q3.nice = 99;
    q3.system = 1336;
    q3.idle = 1000;
    q3.iowait = 4;
    q3.irq = 4;
    q3.softirq = 4;
    q3.steal = 4;
    q3.guest = 4;
    q3.guest_nice = 4;
    q3.cpu = {'s', 'y', 's', 's', 't', 'a', 't', '8', '9', '0', '1', '2', '3', '4', '5', '6'};

    rc = seri.toBytes( q3, arw );
    ut.isSuccessful( rc, "re-serialization successful" );
}

}

UNITTESTER_MAIN( test_LinuxSystemStatQuery, int pArgc, char const** pArgv ) {

    test::UnitTester  tester( pArgc, pArgv );

    std::vector<test::TestDescriptor> tests;

    tests.push_back( { test::TestTypeID::micro, "LinuxSystemStatQuerySerializer", &linuxSystemStatQuerySerializer } );

    return tester.filterAndExec( tests );

}
