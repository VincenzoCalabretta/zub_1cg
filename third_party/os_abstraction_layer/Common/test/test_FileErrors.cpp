
#include "../FileErrors.hpp"

#include <cstdint>
#include <cstdio>

#include <array>
#include <string_view>
#include <vector>

#include "encore/ErrorHandling/test/UnitTestExtensions.hpp"
#include "encore/Logging/Logger.hpp"
#include "encore/UnitTesting/UnitTestingCore.hpp"

namespace {

namespace test = Encore::UnitTesting;
using namespace Encore;
using namespace Encore::OS;
using namespace Encore::Logging;
using namespace Encore::Utils;


void errorGroupCoverage( test::UnitTestConfig const & pConfig ) {

    test::UnitTest<void> ut{ pConfig };

    using namespace std::string_view_literals;

    ut.areEqual( FileErrors::name( kThreadSafe ), "FileErrors"sv, "group name" );
}

void errorMessageCoverage( test::UnitTestConfig const & pConfig ) {

    test::UnitTest<void> ut{ pConfig };

    using namespace std::string_view_literals;

    ut.areEqual( FileErrors::message( kThreadSafe, FileErrors::failed_to_open )
               , "File failed to open"sv
               , "failed_to_open" );
    ut.areEqual( FileErrors::message( kThreadSafe, FileErrors::not_open )
               , "File is not open"sv
               , "not_open" );
    ut.areEqual( FileErrors::message( kThreadSafe, FileErrors::not_allowed_while_open )
               , "Operation not allowed while file is open"sv
               , "not_allowed_while_open" );
    ut.areEqual( FileErrors::message( kThreadSafe, FileErrors::invalid_open_mode )
               , "Specified open mode is invalid"sv
               , "invalid_open_mode" );
    ut.areEqual( FileErrors::message( kThreadSafe, 100 )
               , "Unknown FileErrors code"sv
               , "Unknown error" );
}

}

UNITTESTER_MAIN( test_StdFile, int pArgc, char const** pArgv ) {

    test::UnitTester tester( pArgc, pArgv );

    return tester.filterAndExec( {
        { test::TestTypeID::misc, "ErrorGroupCoverage", &errorGroupCoverage },
        { test::TestTypeID::misc, "ErrorMessageCoverage", &errorMessageCoverage },
    } );
}
