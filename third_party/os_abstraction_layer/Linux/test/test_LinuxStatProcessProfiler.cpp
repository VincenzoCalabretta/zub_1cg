
// NOLINTBEGIN(cppcoreguidelines-macro-usage) -- overriding system calls
#define open mock_open
#define read mock_read
#define lseek mock_lseek
// NOLINTEND(cppcoreguidelines-macro-usage)

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#include "../LinuxStatProcessProfiler.hpp"

#include <condition_variable>
#include <cstdint>
#include <thread>
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

    test::gMockLinuxFileIO.openImpl = [&](const char* pF, int ) -> int {
        if( pF == std::string("/proc/stat") ) {
            return 5;
        }
        else {
            return 6;
        }
    };

    std::map<int, std::string> fileContents { {5, "cpu 1 2 3 4 5 6 7 8 9 10  0  0  0  0  0" }
                                            , {6, "10 file R  0  0  0  0  0  0  0  0  0  0 22 23  0  0  0  0 1  0  0"
                                            } };
    test::gMockLinuxFileIO.readImpl = [&](int pFD, void* pBuf, size_t pSize) -> ssize_t {
        auto itr = fileContents.find(pFD);
        if( itr != fileContents.end() ) {
            auto copySize = std::min(itr->second.size(), pSize);
            std::memcpy( pBuf, itr->second.c_str(), copySize );
            return static_cast<ssize_t>(copySize);
        }
        return 0;
    };

    LinuxStatProcessProfiler profiler(kTestInitOnly);
    ut.isSuccessful( profiler.init(kTestInitOnly), "init succeeds" );
    fileContents[5] = std::string("cpu 4 5 6 7 8 9 10 11 12 13  0  0  0  0  0");
    fileContents[6] = std::string("10 file R  0  0  0  0  0  0  0  0  0  0 32 33  0  0  0  0 1  0  0");
    auto perf = profiler.query();
    ut.isSuccessful( perf, "query succeeds" );
    ut.areClose( perf.value().systemUsage, 66.66, 0.1, "percent matches on utilization" );

    fileContents[6] = std::string("10 file R  0  0  0  0  0  0  0  0  0  0 42 43  0  0  0  0 1  0  0");
    perf = profiler.query();
    ut.isSuccessful( perf, "query succeeds" );
    ut.areClose( perf.value().systemUsage, 0.0, 0.1, "zero system usage" );

    fileContents[5] = std::string("cpu 5 6 7 8 9 10 11 12 13 14  0  0  0  0  0");
    fileContents[6] = std::string("10 file R  0  0  0  0  0  0  0  0  0  0 52 53  0  0  0  0 1  0  0");
    perf = profiler.query();
    ut.isSuccessful( perf, "query succeeds" );
    ut.areClose( perf.value().systemUsage, 100.0, 0.1, "percent overflow" );
}

void handlePublication( test::UnitTestConfig const & pConfig ) {
    test::UnitTest<test::ErrorHandlingExtension> ut ( pConfig );

    auto fsw = std::make_unique<Mocks::TestFSWInterface>(kTestInitOnly);
    fsw->setRegistry<SingleThreadedRegistry>();

    auto & reg = fsw->getRegistrar();
    TypedReadOnlyDataHandle<ProcessInfo> procReader;
    ut.isSuccessful( reg.getReadHandle( kTestInitOnly, "procData", procReader ), "get the read handle" );

    test::gMockLinuxFileIO.openImpl = [&](const char* pF, int ) -> int {
        if( pF == std::string("/proc/stat") ) {
            return 5;
        }
        else {
            return 6;
        }
    };

    std::map<int, std::string> fileContents { {5, "cpu 1 2 3 4 5 6 7 8 9 10  0  0  0  0  0" }
                                            , {6, "10 file R  0  0  0  0  0  0  0  0  0  0 22 23  0  0  0  0 1  0  0"
                                            } };
    test::gMockLinuxFileIO.readImpl = [&](int pFD, void* pBuf, size_t pSize) -> ssize_t {
        auto itr = fileContents.find(pFD);
        if( itr != fileContents.end() ) {
            auto copySize = std::min(itr->second.size(), pSize);
            std::memcpy( pBuf, itr->second.c_str(), copySize );
            return static_cast<ssize_t>(copySize);
        }
        return 0;
    };

    LinuxStatProcessProfiler profiler(kTestInitOnly);
    ut.isSuccessful( profiler.registerHandle(kTestInitOnly, *fsw, "procData"), "register handle" );
    ut.isSuccessful( profiler.init(kTestInitOnly), "init succeeds" );

    fileContents[5] = std::string("cpu 4 5 6 7 8 9 10 11 12 13  0  0  0  0  0");
    fileContents[6] = std::string("10 file R  0  0  0  0  0  0  0  0  0  0 32 33  0  0  0  0 1  0  0");
    ut.isSuccessful( profiler.query(), "query succeeds" );
    ut.areClose( procReader.get().systemUsage, 66.67, 0.1, "system usage matches" );
    ut.areClose( procReader.get().cpuUsage
               , 66.6666667 * LinuxStatProcessProfiler::kHardwareConcurrency
               , 0.1
               , "cpu usage matches" );
    ut.areEqual( procReader.get().threads, 1u, "thread count matches" );
}

void wrapTime( test::UnitTestConfig const & pConfig ) {
    test::UnitTest<test::ErrorHandlingExtension> ut ( pConfig );

    test::gMockLinuxFileIO.openImpl = [&](const char* pF, int ) -> int {
        if( pF == std::string("/proc/stat") ) {
            return 5;
        }
        else {
            return 6;
        }
    };

    std::map<int, std::string> fileContents { {5, "cpu 1 2 3 4 5 6 7 8 9 9223372036854775612  0  0  0  0  0" }
                                            , {6, "10 file R 0 0 0 0 0 0 0 0 0 0 9223372036854775707 50 0 0 0 0 1 0 0"
                                            } };
    test::gMockLinuxFileIO.readImpl = [&](int pFD, void* pBuf, size_t pSize) -> ssize_t {
        auto itr = fileContents.find(pFD);
        if( itr != fileContents.end() ) {
            auto copySize = std::min(itr->second.size(), pSize);
            std::memcpy( pBuf, itr->second.c_str(), copySize );
            return static_cast<ssize_t>(copySize);
        }
        return 0;
    };

    LinuxStatProcessProfiler profiler(kTestInitOnly);
    ut.isSuccessful( profiler.init(kTestInitOnly), "init succeeds" );

    fileContents[5] = std::string("cpu 0 0 0 0 0 0 50 0 0 0 0 0  0  0  0  0");
    fileContents[6] = std::string("10 file R  0  0  0  0  0  0  0  0  0  0 25 25  0  0  0  0 1  0  0");
    auto perf = profiler.query();
    ut.areClose( perf.value().systemUsage, 50.0, 0.1, "percent overflow" );
}


void badQuery( test::UnitTestConfig const & pConfig ) {
    test::UnitTest<test::ErrorHandlingExtension> ut ( pConfig );

    auto statFD = 5;
    auto pidFD = 6;
    test::gMockLinuxFileIO.openImpl = [&](const char* pF, int ) -> int {
        if( pF == std::string("/proc/stat") ) {
            return statFD;
        }
        else {
            return pidFD;
        }
    };

    std::map<int, std::string> fileContents { {5, "cpu 1 2 3 4 5 6 7 8 9 10  0  0  0  0  0" }
                                            , {6, "10 file R  0  0  0  0  0  0  0  0  0  0 22 23  0  0  0  0 1  0  0"
                                            } };
    auto statRead = -1;
    auto pidRead = -1;
    test::gMockLinuxFileIO.readImpl = [&](int pFD, void* pBuf, size_t pSize) noexcept -> ssize_t {
        auto itr = fileContents.find(pFD);
        if( itr != fileContents.end() ) {
            auto copySize = std::min(itr->second.size(), pSize);
            std::memcpy( pBuf, itr->second.c_str(), copySize );

            if( pFD == statFD ) {
                return statRead;
            }
            if( pFD == pidFD ) {
                return pidRead;
            }
        }
        return 0;
    };

    {
        LinuxStatProcessProfiler profiler(kTestInitOnly);
        ut.notSuccessful( profiler.init(kTestInitOnly), "init fails on PID file read" );
    }

    pidRead = 1;
    {
        LinuxStatProcessProfiler profiler(kTestInitOnly);
        ut.notSuccessful( profiler.init(kTestInitOnly), "init fails on Sys file read" );
    }
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
    auto pidFD = -1;
    test::gMockLinuxFileIO.openImpl = [&](const char* pF, int ) -> int {
        if( pF == std::string("/proc/stat") ) {
            return statFD;
        }
        else {
            return pidFD;
        }
    };

    std::map<int, std::string> fileContents { {5, "cpu 1 2 3 4 5 6 7 8 9 10  0  0  0  0  0" }
                                            , {6, "10 file R  0  0  0  0  0  0  0  0  0  0 22 23  0  0  0  0 1  0  0"
                                            } };
    auto statRead = -1;
    auto pidRead = -1;
    test::gMockLinuxFileIO.readImpl = [&](int pFD, void* pBuf, size_t pSize) noexcept -> ssize_t {
        auto itr = fileContents.find(pFD);
        if( itr != fileContents.end() ) {
            auto copySize = std::min(itr->second.size(), pSize);
            std::memcpy( pBuf, itr->second.c_str(), copySize );

            if( pFD == statFD ) {
                return statRead;
            }
            if( pFD == pidFD ) {
                return pidRead;
            }
        }
        return 0;
    };


    LinuxStatProcessProfiler profiler(kTestInitOnly, std::numeric_limits<pid_t>::max() );
    ut.notSuccessful( profiler.init(kTestInitOnly), "init fails on PID file open" );

    pidFD = 6;
    ut.notSuccessful( profiler.init(kTestInitOnly), "init fails on Sys file open" );

    ut.notSuccessful( profiler.registerHandle(kTestInitOnly, *fsw, "wont reg"), "handle reg fails" );
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
                                 , {test::TestTypeID::misc, "WrapTime", &wrapTime}
                                 , {test::TestTypeID::misc, "BadInit", &badInit}
                                 } );
}
