
#include "../IStreamProtocol.hpp"

#include "encore/UnitTesting/UnitTestingCore.hpp"
#include "encore/ErrorHandling/test/UnitTestExtensions.hpp"

namespace {

namespace test = Encore::UnitTesting;
using namespace Encore;
using namespace Encore::OS;
using namespace Encore::Utils;

void noOpCoverage( test::UnitTestConfig const & pConfig ) {

    test::UnitTest<test::ErrorHandlingExtension> ut{ pConfig };

    NoOpStreamProtocol proto{};

    proto.onResetReadStream();

    EncoreByte by{};
    BytesView view{ &by, 1 };
    BytesRange range{ &by, 1 };

    auto read = proto.readMsg( view, range );

    ut.isSuccessful( read, "read" );
    ut.assertFalse( read.value().isValid, "Read is not valid" );
    ut.succeedsWithValue( proto.writeMsg( view, range ), std::size_t{0}, "write" );
}

}

UNITTESTER_MAIN( test_IStreamProtocol, int pArgc, char const ** pArgv ) {

    test::UnitTester tester( pArgc, pArgv );

    return tester.filterAndExec( { { test::TestTypeID::micro, "noOpCoverage", &noOpCoverage }, } );
}

