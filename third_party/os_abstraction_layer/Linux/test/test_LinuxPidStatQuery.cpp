
///
/// @file
/// @brief
///

#include "../LinuxPidStatQuery.hpp"
#include "../LinuxPidStatQuerySerializer.hpp"

#include <vector>

#include "UnitTesting/UnitTester.hpp"
#include "UnitTesting/UnitTestConfig.hpp"
#include "UnitTesting/UnitTest.hpp"
#include "ErrorHandling/test/UnitTestExtensions.hpp"

#include "Utils/ASCIISerializer.hpp"
#include "Utils/test/ASCIIReaderWriter.hpp"

namespace Encore::OS {

inline bool operator==( OS::LinuxPidStatQuery const & lhs, OS::LinuxPidStatQuery const & rhs ) {
    return ( lhs.pid == rhs.pid && lhs.state == rhs.state && lhs.ppid == rhs.ppid && lhs.pgrp == rhs.pgrp &&
             lhs.session == rhs.session && lhs.tty_nr == rhs.tty_nr && lhs.tpgid == rhs.tpgid &&
             lhs.flags == rhs.flags && lhs.minflt == rhs.minflt && lhs.cminflt == rhs.cminflt &&
             lhs.majflt == rhs.majflt && lhs.cmajflt == rhs.cmajflt && lhs.utime == rhs.utime &&
             lhs.stime == rhs.stime && lhs.cutime == rhs.cutime && lhs.cstime == rhs.cstime &&
             lhs.priority == rhs.priority && lhs.nice == rhs.nice && lhs.num_threads == rhs.num_threads &&
             std::memcmp( &lhs.comm, &rhs.comm, sizeof(lhs.comm) ) == 0 );
}

}

namespace {

namespace test = Encore::UnitTesting;
using namespace Encore::ErrorHandling;
using namespace Encore::OS;
using namespace Encore;

void linuxPidStatQuerySerializer( test::UnitTestConfig const & pConfig ) {
    test::UnitTest<test::ErrorHandlingExtension> ut( pConfig );

    LinuxPidStatQuery q1{};
    q1.pid = 1337;
    q1.state = 'R';
    q1.ppid = 1336;
    q1.tpgid = 1000;
    q1.num_threads = 4;
    q1.comm = {'p', 'i', 'd', 's', 't', 'a', 't'};

    ut.areEqual( q1.qType(), LinuxProcQueryType::process, "query type is correct" );

    Utils::Serializer<LinuxPidStatQuery, Utils::SerializationPolicy::ascii_based> seri{};
    test::ASCIIReaderWriter arw;

    auto rc = seri.toBytes( q1, arw );
    ut.isSuccessful( rc, "serialization successful" );

    LinuxPidStatQuery q2{};
    [[ maybe_unused ]] auto rc2 = seri.toObj( arw, q2 );
    ut.isSuccessful( rc2, "deserialization successful" );
    ut.areEqual( q1, q2, "data matches" );

    LinuxPidStatQuery q3{};
    q3.pid = 1337;
    q3.state = 'R';
    q3.ppid = 1336;
    q3.tpgid = 1000;
    q3.num_threads = 4;
    q3.comm = {'p', 'i', 'd', 's', 't', 'a', 't', '8', '9', '0', '1', '2', '3', '4', '5', '6'};

    rc = seri.toBytes( q3, arw );
    ut.isSuccessful( rc, "re-serialization successful" );
}

}

UNITTESTER_MAIN( test_LinuxPidStatQuery, int pArgc, char const** pArgv ) {

    test::UnitTester  tester( pArgc, pArgv );

    std::vector<test::TestDescriptor> tests;

    tests.push_back( { test::TestTypeID::micro, "LinuxPidStatQuerySerializer", &linuxPidStatQuerySerializer } );

    return tester.filterAndExec( tests );

}
