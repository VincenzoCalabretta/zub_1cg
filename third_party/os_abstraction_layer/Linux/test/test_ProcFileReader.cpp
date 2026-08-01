
///
/// @file
/// @brief
///

#include "../ProcFileReader.hpp"

#include "UnitTesting/UnitTester.hpp"
#include "UnitTesting/UnitTestConfig.hpp"
#include "UnitTesting/UnitTest.hpp"
#include "ErrorHandling/test/UnitTestExtensions.hpp"

#include "Utils/Span.hpp"

namespace {

namespace test = Encore::UnitTesting;
using namespace Encore::ErrorHandling;
using namespace Encore::Utils;
using namespace Encore::OS;
using namespace Encore;

void procFileBasicUse( test::UnitTestConfig const & pConfig ) {
    test::UnitTest<test::ErrorHandlingExtension> ut( pConfig );

    ProcFileReader pfr( kTestInitOnly );
    std::string fileName = "/proc/stat";
    ut.isSuccessful(pfr.init( kTestInitOnly, fileName ), "inits successfully" );
    std::array<EncoreByte, 16> buf{};
    BytesRange br( buf );
    auto rc = pfr.read( br );
    ut.isSuccessful( rc, "read something" );
    ut.areEqual( rc.value(), 3u, "some bytes" );
    ut.areEqual( buf, {'c', 'p', 'u'}, "read the right data" );

    // redo it
    std::ignore = pfr.reset();
    rc = pfr.read( br );
    ut.isSuccessful( rc, "read something again" );
    ut.areEqual( rc.value(), 3u, "some bytes again" );
    ut.areEqual( buf, {'c', 'p', 'u'}, "read the right data again" );
}

void procFileBadBufferSize( test::UnitTestConfig const & pConfig ) {
    test::UnitTest<test::ErrorHandlingExtension> ut( pConfig );

    ProcFileReader pfr( kTestInitOnly );

    std::string noFile = "/proc/self/nonexistant";
    ut.failsWithCode( pfr.init( kTestInitOnly, noFile, 0 )
                    , EG_ERROR_CODE( StdFswErrors, invalid_argument )
                    , "Bad buffer size" );
}

void procFileReadFails( test::UnitTestConfig const & pConfig ) {
    test::UnitTest<test::ErrorHandlingExtension> ut( pConfig );

    {
        ProcFileReader pfr( kTestInitOnly );
        std::string fileName = "/proc/stat";
        // Use a buffer size small enough that we have whitespace to process
        ut.isSuccessful(pfr.init( kTestInitOnly, fileName, 5 ), "inits successfully with 5 byte buffer" );
        std::array<EncoreByte, 16> buf{};
        BytesRange br( buf );
        // read the "cpu"
        auto rc = pfr.read( br );
        rc = pfr.read( br );
        ut.isSuccessful( rc, "read passes" );
        ut.areEqual( rc.value(), 0u, "but nothing was read" );
    }
    {
        ProcFileReader pfr( kTestInitOnly );
        std::string fileName = "/proc/stat";
        // Use a buffer small enough that there is nothing else to read
        ut.isSuccessful(pfr.init( kTestInitOnly, fileName, 3 ), "inits successfully with 3 byte buffer" );
        std::array<EncoreByte, 16> buf{};
        BytesRange br( buf );
        // read the "cpu"
        auto rc = pfr.read( br );
        rc = pfr.read( br );
        ut.isSuccessful( rc, "read passes" );
        ut.areEqual( rc.value(), 0u, "but nothing was read" );
    }
    {
        ProcFileReader pfr( kTestInitOnly );
        std::string fileName = "/proc/stat";
        // Use a buffer small enough that there is nothing else to read
        ut.isSuccessful(pfr.init( kTestInitOnly, fileName ), "inits successfully with 3 byte buffer" );
        std::array<EncoreByte, 1> buf{};
        BytesRange br( buf );
        // read the "cpu"
        auto rc = pfr.read( br );
        rc = pfr.read( br );
        ut.isSuccessful( rc, "read passes" );
        ut.areEqual( rc.value(), 0u, "but nothing was read" );
    }
    {
        ProcFileReader pfr( kTestInitOnly );
        std::string fileName = "/proc/stat";
        // Use a buffer small enough that there is nothing else to read
        ut.isSuccessful(pfr.init( kTestInitOnly, fileName ), "inits successfully with 3 byte buffer" );
        BytesRange br( nullptr, 0 );
        // read the "cpu"
        auto rc = pfr.read( br );
        rc = pfr.read( br );
        ut.failsWithCode( rc, EG_ERROR_CODE( StdFswErrors, null_ptr ), "read passes" );
        ut.areEqual( rc.value(), 0u, "but nothing was read" );
    }
}

void procFileReadForbidden( test::UnitTestConfig const & pConfig ) {
    test::UnitTest<test::ErrorHandlingExtension> ut( pConfig );

    ProcFileReader pfr( kTestInitOnly );

    std::array<EncoreByte, 10> buf;
    BytesRange br( buf );
    ut.failsWithCode( pfr.read( br ), EG_ERROR_CODE( FileErrors, not_open ), "cannot read" );
}

void procFileWriteForbidden( test::UnitTestConfig const & pConfig ) {
    test::UnitTest<test::ErrorHandlingExtension> ut( pConfig );

    ProcFileReader pfr( kTestInitOnly );

    std::array<EncoreByte, 10> buf;
    BytesView bv( buf );
    ut.failsWithCode( pfr.write( bv ), EG_ERROR_CODE( StdFswErrors, not_implemented ), "cannot write" );
}

void procFileLongName( test::UnitTestConfig const & pConfig ) {
    test::UnitTest<test::ErrorHandlingExtension> ut( pConfig );

    ProcFileReader pfr( kTestInitOnly );

    std::string longFile = "/proc/self/thisisafilenamethatiswaytoolongtobehandled";
    ut.failsWithCode( pfr.init( kTestInitOnly, longFile )
                    , EG_ERROR_CODE( StdFswErrors, length_error )
                    , "file name too long" );
    std::string nonexistantFile = "/proc/self/nonexistant";
    ut.failsWithCode( pfr.init( kTestInitOnly, nonexistantFile )
                    , EG_ERROR_CODE( SystemErrors, no_such_file_or_directory )
                    , "file does not exist" );
}

}


UNITTESTER_MAIN( test_ProcFileReader, int pArgc, char const** pArgv ) {

    test::UnitTester  tester( pArgc, pArgv );

    std::vector<test::TestDescriptor> tests;

    tests.push_back( { test::TestTypeID::micro, "ProcFileBasicUse", &procFileBasicUse } );
    tests.push_back( { test::TestTypeID::micro, "ProcFileBadBufferSize", &procFileBadBufferSize } );
    tests.push_back( { test::TestTypeID::micro, "ProcFileReadFails", &procFileReadFails } );
    tests.push_back( { test::TestTypeID::micro, "ProcFileReadForbidden", &procFileReadForbidden } );
    tests.push_back( { test::TestTypeID::micro, "ProcFileWriteForbidden", &procFileWriteForbidden } );
    tests.push_back( { test::TestTypeID::micro, "ProcFileLongName", &procFileLongName } );

    return tester.filterAndExec( tests );

}
