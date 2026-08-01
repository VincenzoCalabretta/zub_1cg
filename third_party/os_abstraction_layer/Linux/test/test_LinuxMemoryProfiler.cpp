
// NOLINTBEGIN(cppcoreguidelines-macro-usage) -- overriding system calls
#define open mock_open
#define read mock_read
#define lseek mock_lseek
// NOLINTEND(cppcoreguidelines-macro-usage)

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#include "../LinuxMemoryProfiler.hpp"

#include <cstdint>
#include <map>

#include "UnitTesting/UnitTestingPragma.hpp"
#include "encore/Encore.hpp"
#include "DataModel/SingleThreadedRegistry.hpp"
#include "DataModel/TypedDataHandles.hpp"

#include <mockcore/TestFSWInterface.hpp>

namespace Encore::UnitTesting {

struct MockLinuxFileIO {
    std::function<int(const char*,int)> openImpl{ [](const char*, int ) noexcept -> int { return 0; } };
    std::function<ssize_t(int, void*, size_t)> readImpl{ [](int, void*, int) noexcept -> ssize_t { return 0; } };
    std::function<off_t(int, off_t, int)> lseekImpl{ [](int, off_t, int) noexcept -> off_t { return 0; } };
};

static inline MockLinuxFileIO gMockLinuxFileIO{};

}

DISABLE_COMPILER_WARNING_PUSH
DISABLE_COMPILER_WARNING(-Wredundant-decls)
extern "C" int mock_open( const char* pName, int pFlags, ...) {
    return Encore::UnitTesting::gMockLinuxFileIO.openImpl( pName, pFlags );
}
extern "C" ssize_t mock_read( int pFD, void* pBuff, size_t pSize ) {
    return Encore::UnitTesting::gMockLinuxFileIO.readImpl( pFD, pBuff, pSize );
}

extern "C" off_t mock_lseek(int pFD, off_t pOff, int pRef) noexcept {
    return Encore::UnitTesting::gMockLinuxFileIO.lseekImpl(pFD, pOff, pRef);
}
DISABLE_COMPILER_WARNING_POP

#include "encore/ErrorHandling/test/UnitTestExtensions.hpp"

namespace {

namespace test = Encore::UnitTesting;
using namespace Encore;
using namespace Encore::OS;
using namespace Encore::Utils;
using namespace Encore::DataModel;
using namespace Encore::ErrorHandling;

void goodQuery( test::UnitTestConfig const & pConfig ) {
    test::UnitTest<test::ErrorHandlingExtension> ut ( pConfig );

    test::gMockLinuxFileIO.openImpl = [&](const char*, int ) noexcept -> int {
        return 5;
    };

    std::string memory{"500 550 575 585 590 592 597"};
    test::gMockLinuxFileIO.readImpl = [&](int, void* pBuf, size_t pSize) noexcept -> ssize_t {
        auto copySize = std::min(memory.size(), pSize);
        std::memcpy( pBuf, memory.c_str(), copySize );
        return static_cast<ssize_t>(copySize);
    };

    LinuxMemoryProfiler profiler(kTestInitOnly);
    ut.isSuccessful( profiler.init(kTestInitOnly), "init succeeds" );
    auto mem = profiler.query();
    ut.isSuccessful( mem, "query succeeds" );
    ut.areEqual( mem.value().size, 500u, "size matches" );
    ut.areEqual( mem.value().resident, 550u, "resident matches" );
    ut.areEqual( mem.value().shared, 575u, "shared matches" );

    memory = "600 650 675 585 590 592 597";
    mem = profiler.query();
    ut.isSuccessful( mem, "query succeeds" );
    ut.areEqual( mem.value().size, 600u, "updated size matches" );
    ut.areEqual( mem.value().resident, 650u, "updated resident matches" );
    ut.areEqual( mem.value().shared, 675u, "updated shared matches" );
}

void handlePublication( test::UnitTestConfig const & pConfig ) {
    test::UnitTest<test::ErrorHandlingExtension> ut ( pConfig );

    auto fsw = std::make_unique<Mocks::TestFSWInterface>(kTestInitOnly);
    fsw->setRegistry<SingleThreadedRegistry>();

    auto & reg = fsw->getRegistrar();
    TypedReadOnlyDataHandle<MemInfo> memReader;
    ut.isSuccessful( reg.getReadHandle( kTestInitOnly, "memory", memReader ), "get the read handle" );

    test::gMockLinuxFileIO.openImpl = [&](const char*, int ) noexcept -> int {
        return 5;
    };

    std::string memory{"500 550 575 585 590 592 597"};
    test::gMockLinuxFileIO.readImpl = [&](int, void* pBuf, size_t pSize) noexcept -> ssize_t {
        auto copySize = std::min(memory.size(), pSize);
        std::memcpy( pBuf, memory.c_str(), copySize );
        return static_cast<ssize_t>(copySize);
    };

    LinuxMemoryProfiler profiler(kTestInitOnly);
    ut.isSuccessful( profiler.init(kTestInitOnly), "init succeeds" );
    ut.isSuccessful( profiler.registerHandle(kTestInitOnly, *fsw, "memory"), "register handle" );

    ut.isSuccessful( profiler.query(), "query succeeds" );
    ut.areEqual( memReader.get().size, 500u, "size matches" );
    ut.areEqual( memReader.get().resident, 550u, "resident matches" );
    ut.areEqual( memReader.get().shared, 575u, "shared matches" );
}

void badQuery( test::UnitTestConfig const & pConfig ) {
    test::UnitTest<test::ErrorHandlingExtension> ut ( pConfig );

    auto statmFD = 5;
    test::gMockLinuxFileIO.openImpl = [&](const char*, int ) noexcept -> int {
        return statmFD;
    };

    std::string memory{"500 550 575 585 590 592 597"};
    auto statmRead = -1;
    test::gMockLinuxFileIO.readImpl = [&](int, void* pBuf, size_t pSize) noexcept -> ssize_t {
        auto copySize = std::min(memory.size(), pSize);
        std::memcpy( pBuf, memory.c_str(), copySize );

        return statmRead;
    };

    LinuxMemoryProfiler profiler(kTestInitOnly);
    ut.isSuccessful( profiler.init(kTestInitOnly), "init fails on file open" );
    ut.notSuccessful( profiler.query(), "query fails on file read" );
}

class BadRegistry final : public Encore::DataModel::IRegistry {
    Result<DataFieldInfo> registerField( InitOnly const &, DataFieldDescriptor const & pDesc, bool ) noexcept final {
        return EG_ERROR_CODE_WITH_PAYLOAD(StdFswErrors, logic_error, pDesc.id );
    }

    ReturnCode prepExchanger( InitOnly const &, ExecId, IFieldExchanger & ) noexcept final { return {}; }

    ReturnCode init( InitOnly const & ) noexcept final { return {}; }

    ReturnCode setExecId( InitOnly const &, ExecId ) noexcept final { return {}; }

    ExecId getExecId( InitOnly const & ) const noexcept final {
        return 0;
    }

    Result<DataFieldDescriptor> query( DataFieldId ) const noexcept final {
        return EG_ERROR_CODE( StdFswErrors, not_implemented );
    }
};

void badInit( test::UnitTestConfig const & pConfig ) {

    test::UnitTest<test::ErrorHandlingExtension> ut ( pConfig );

    auto fsw = std::make_unique<Mocks::TestFSWInterface>(kTestInitOnly);
    fsw->setRegistry<BadRegistry>();

    auto statFD = -1;
    test::gMockLinuxFileIO.openImpl = [&](const char*, int ) noexcept -> int {
            return statFD;
    };

    LinuxMemoryProfiler profiler(kTestInitOnly, std::numeric_limits<pid_t>::max() );
    ut.notSuccessful( profiler.init(kTestInitOnly), "init fails on file open" );
    ut.notSuccessful( profiler.registerHandle(kTestInitOnly, *fsw, "wont reg"), "handle reg fails" );
    ut.notSuccessful( profiler.query(), "query fails on file read" );
}

}

#undef open
#undef mock_open
#include "encore/UnitTesting/UnitTester.hpp"

UNITTESTER_MAIN( test_StatProcessProfiler, int pArgc, char const** pArgv ) {

    test::UnitTester  tester( pArgc, pArgv );
    return tester.filterAndExec( { {test::TestTypeID::misc, "GoodQuery", &goodQuery}
                                 , {test::TestTypeID::misc, "HandlePublication", &handlePublication}
                                 , {test::TestTypeID::misc, "BadQuery", &badQuery}
                                 , {test::TestTypeID::misc, "BadInit", &badInit}
                                 } );
}
