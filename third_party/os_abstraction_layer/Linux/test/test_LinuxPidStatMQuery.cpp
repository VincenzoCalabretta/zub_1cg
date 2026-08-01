
///
/// @file
/// @brief
///

#include "../LinuxPidStatMQuery.hpp"
#include "../LinuxPidStatMQuerySerializer.hpp"

#include <vector>

#include "UnitTesting/UnitTester.hpp"
#include "UnitTesting/UnitTestConfig.hpp"
#include "UnitTesting/UnitTest.hpp"
#include "ErrorHandling/test/UnitTestExtensions.hpp"
#include "Utils/ASCIISerializer.hpp"
#include "Utils/test/ASCIIReaderWriter.hpp"

namespace Encore::OS{

inline bool operator==( LinuxPidStatMQuery const & lhs, LinuxPidStatMQuery const & rhs ) {
    return ( lhs.size == rhs.size && lhs.resident == rhs.resident && lhs.shared == rhs.shared );
}

}

namespace {

namespace test = Encore::UnitTesting;
using namespace Encore::ErrorHandling;
using namespace Encore::OS;
using namespace Encore;

void linuxPidStatMQuerySerializer( test::UnitTestConfig const & pConfig ) {
    test::UnitTest<test::ErrorHandlingExtension> ut( pConfig );

    LinuxPidStatMQuery q1{};
    q1.size = 1337;
    q1.resident = 42;
    q1.shared = 1336;

    ut.areEqual( q1.qType(), LinuxProcQueryType::process, "query type is correct" );

    Utils::Serializer<LinuxPidStatMQuery, Utils::SerializationPolicy::ascii_based> seri{};
    test::ASCIIReaderWriter arw;

    auto rc = seri.toBytes( q1, arw );
    ut.isSuccessful( rc, "serialization successful" );

    LinuxPidStatMQuery q2{};
    auto rc2 = seri.toObj( arw, q2 );
    ut.isSuccessful( rc2, "deserialization successful" );
    ut.areEqual( q1, q2, "data matches" );
}

}

UNITTESTER_MAIN( test_LinuxPidStatMQuery, int pArgc, char const** pArgv ) {

    test::UnitTester  tester( pArgc, pArgv );

    std::vector<test::TestDescriptor> tests;

    tests.push_back( { test::TestTypeID::micro, "LinuxPidStatMQuerySerializer", &linuxPidStatMQuerySerializer } );

    return tester.filterAndExec( tests );

}
