
#include "../IProcessProfiler.hpp"

#include <cmath>

#include "encore/UnitTesting/UnitTestingCore.hpp"
#include "encore/ErrorHandling/test/UnitTestExtensions.hpp"


namespace Encore::OS {

inline bool operator==( ProcessInfo const & lhs, ProcessInfo const & rhs ) {
    return std::fabs(std::fabs(lhs.cpuUsage) - std::fabs(rhs.cpuUsage)) < std::numeric_limits<double>::epsilon()
        && std::fabs(std::fabs(lhs.systemUsage) - std::fabs(rhs.systemUsage)) < std::numeric_limits<double>::epsilon()
        && lhs.threads == rhs.threads;
}

}

namespace {

namespace test = Encore::UnitTesting;
using namespace Encore;
using namespace Encore::OS;


void noop( test::UnitTestConfig const & pConfig ) {

    test::UnitTest<test::ErrorHandlingExtension> ut{ pConfig };

    ut.areEqual( NoOpProcessProfiler().query().value(), ProcessInfo{}, "Default info returned" );
}

}

UNITTESTER_MAIN( test_IProcessProfiler, int pArgc, char const ** pArgv ) {

    test::UnitTester tester( pArgc, pArgv );

    return tester.filterAndExec( { { test::TestTypeID::use_case, "Noop", &noop }
                                 } );
}
