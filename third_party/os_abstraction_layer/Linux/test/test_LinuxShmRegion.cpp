
// NOLINTBEGIN(cppcoreguidelines-macro-usage) -- overriding system calls
#define shm_open mock_shm_open
#define shm_unlink mock_shm_unlink
#define ftruncate mock_ftruncate
#define mmap mock_mmap
#define munmap mock_munmap
#define close mock_close
#define fstat mock_fstat
// NOLINTEND(cppcoreguidelines-macro-usage)

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#include "OS/Linux/LinuxShmRegion.hpp"

#include <wait.h>

#include "UnitTesting/UnitTester.hpp"
#include "UnitTesting/UnitTestConfig.hpp"
#include "UnitTesting/UnitTest.hpp"
#include "ErrorHandling/test/UnitTestExtensions.hpp"

#include <sys/mman.h>
#include <sys/types.h>
#include <functional>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#include "encore/Encore.hpp"

namespace Encore::UnitTesting {

struct MockLinuxSharedMem {
    std::function<int(int, mode_t)> shmopenImpl{ [](int, mode_t) noexcept -> int{ return 0; } };
    std::function<int(int, struct stat*)> fstatImpl{ [](int, struct stat* pStat) noexcept -> int{
                                                                                                    pStat->st_size = 0;
                                                                                                    return 0;
                                                                                                } };
    std::function<int(off_t)> ftruncateImpl{ [](off_t size) noexcept -> off_t{ return size; } };
    std::function<void*()> mmapImpl{ []() noexcept -> void*{ return nullptr; } };
    std::function<int()> munmapImpl{ []() noexcept -> int{ return 0; } };
    std::function<int()> closeImpl{ []() noexcept -> int{ return 0; } };
    std::function<int()> shmunlinkImpl{ []() noexcept -> int{ return 0; } };

    void reset() {
        shmopenImpl = [](int, mode_t) noexcept -> int{ return 0; };
        fstatImpl = [](int, struct stat* pStat) noexcept -> int{ pStat->st_size = 0; return 0; };
        ftruncateImpl = [](off_t size) noexcept -> off_t{ return size; };
        mmapImpl = []() noexcept -> void*{ return nullptr; };
        munmapImpl = []() noexcept -> int{ return 0; };
        closeImpl = []() noexcept -> int{ return 0; };
        shmunlinkImpl = []() noexcept -> int{ return 0; };
    }
};

static inline MockLinuxSharedMem gMockSharedMem{};

}

extern "C" int mock_shm_open( const char * /*pData*/, int pFlags, mode_t pMode)  {
    return Encore::UnitTesting::gMockSharedMem.shmopenImpl( pFlags, pMode );
}

extern "C" int mock_fstat( int pFd, struct stat* pStat ) noexcept {
    return Encore::UnitTesting::gMockSharedMem.fstatImpl( pFd, pStat );
}

extern "C" int mock_ftruncate( int, off_t size ) noexcept {
    return Encore::UnitTesting::gMockSharedMem.ftruncateImpl(size);
}

extern "C" void* mock_mmap( void*, size_t, int, int, int, off_t ) noexcept {
    return Encore::UnitTesting::gMockSharedMem.mmapImpl();
}

extern "C" int mock_munmap( void*, size_t ) noexcept {
    return Encore::UnitTesting::gMockSharedMem.munmapImpl();
}

extern "C" int mock_close( int ) {
    return Encore::UnitTesting::gMockSharedMem.closeImpl();
}

extern "C" int mock_shm_unlink( char const * ) {
    return Encore::UnitTesting::gMockSharedMem.shmunlinkImpl();
}



namespace {

namespace test = Encore::UnitTesting;
using namespace Encore::OS;
using namespace Encore;


struct TwoFields {
    int fromChild;
    int fromParent;
};

void nominalCreateUse( test::UnitTestConfig const & pConfig ) {

    test::UnitTest<test::ErrorHandlingExtension> ut( pConfig );

    test::gMockSharedMem.reset();

    ut.addRequiredCondition("create shm region");
    ut.addRequiredCondition("fstat called");
    ut.addRequiredCondition("ftruncate called");
    ut.addRequiredCondition("memory mapped");

    test::gMockSharedMem.shmopenImpl = [&](int pFlags, mode_t pMode) {
                                                                        if( pFlags == (O_RDWR | O_CREAT | O_EXCL) &&
                                                                            pMode == (S_IRUSR | S_IWUSR) )
                                                                            {
                                                                                ut.meetCondition( "create shm region" );
                                                                            }
                                                                        return 1;
                                                                     };
    test::gMockSharedMem.fstatImpl = [&](int, struct stat* pFdStat) {
                                                                        pFdStat->st_size = sizeof(TwoFields)-1;
                                                                        ut.meetCondition( "fstat called" );
                                                                        return 0;
                                                                    };
    test::gMockSharedMem.ftruncateImpl = [&](off_t pOffset) {
                                                                if( pOffset == sizeof(TwoFields) ) {
                                                                    ut.meetCondition( "ftruncate called" );
                                                                }
                                                                return 0;
                                                            };
    TwoFields tfBase;
    tfBase.fromChild = 33;
    test::gMockSharedMem.mmapImpl = [&]() {
                                              ut.meetCondition( "memory mapped" );
                                              return static_cast<void*>(&tfBase);
                                          };

    LinuxShmRegion<TwoFields> tf( kTestInitOnly, kNotThreadSafe, "TwoFields", true ); // create

    ut.areEqual( tf.data(), &tfBase, "Mapped memory address matches" );
    TwoFields & ref = *tf.data();
    ut.areEqual( ref.fromChild, 0, "Data was reinitialized" );
}

void nominalUse( test::UnitTestConfig const & pConfig ) {

    test::UnitTest<test::ErrorHandlingExtension> ut( pConfig );

    test::gMockSharedMem.reset();

    ut.addRequiredCondition("create shm region");
    ut.addRequiredCondition("fstat called");
    ut.addRequiredCondition("memory mapped");

    test::gMockSharedMem.shmopenImpl = [&](int pFlags, mode_t pMode){
                                                                        if( pFlags == O_RDWR &&
                                                                            pMode == (S_IRUSR | S_IWUSR) )
                                                                            {
                                                                                ut.meetCondition( "create shm region" );
                                                                            }
                                                                        return 0;
                                                                    };
    test::gMockSharedMem.fstatImpl = [&](int, struct stat* pFdStat) {
                                                                        pFdStat->st_size = sizeof(TwoFields);
                                                                        ut.meetCondition( "fstat called" );
                                                                        return 0;
                                                                    };
    TwoFields tfBase;
    tfBase.fromChild = 33;
    test::gMockSharedMem.mmapImpl = [&]() {
                                              ut.meetCondition( "memory mapped" );
                                              return static_cast<void*>(&tfBase);
                                          };

    LinuxShmRegion<TwoFields> tf( kTestInitOnly, kNotThreadSafe, "TwoFields", false ); // don't create

    ut.areEqual(tf.data(), &tfBase, "Mapped memory address matches" );
    TwoFields const * ref = tf.data();
    ut.areEqual(ref->fromChild, 33, "Data was reinitialized" );
}

void failedOpen( test::UnitTestConfig const & pConfig ) {

    test::UnitTest<test::ErrorHandlingExtension> ut( pConfig );

    test::gMockSharedMem.reset();

    test::gMockSharedMem.shmopenImpl = [&](int, mode_t) noexcept {
                                                                     return -1;
                                                                 };
    ut.catchException<std::runtime_error>( [&](){ auto shm = LinuxShmRegion<TwoFields>( kTestInitOnly
                                                                                      , kNotThreadSafe
                                                                                      , "TwoFields"
                                                                                      , false ); } );

}

void failedFstat( test::UnitTestConfig const & pConfig ) {

    test::UnitTest<test::ErrorHandlingExtension> ut( pConfig );

    test::gMockSharedMem.reset();

    test::gMockSharedMem.fstatImpl = [&](int, struct stat*) noexcept {
                                                                         return -1;
                                                                     };
    ut.catchException<std::runtime_error>( [&](){ auto shm = LinuxShmRegion<TwoFields>( kTestInitOnly
                                                                                      , kNotThreadSafe
                                                                                      , "TwoFields"
                                                                                      , false ); } );

}

void failedFtruncate( test::UnitTestConfig const & pConfig ) {

    test::UnitTest<test::ErrorHandlingExtension> ut( pConfig );

    test::gMockSharedMem.reset();

    test::gMockSharedMem.fstatImpl = [&](int, struct stat* pFdStat) noexcept {
                                                                                 pFdStat->st_size = sizeof(TwoFields)-1;
                                                                                 return 0;
                                                                             };
    test::gMockSharedMem.ftruncateImpl = [&](off_t) noexcept {
                                                                 return -1;
                                                             };
    ut.catchException<std::runtime_error>( [&](){ auto shm = LinuxShmRegion<TwoFields>( kTestInitOnly
                                                                                      , kNotThreadSafe
                                                                                      , "TwoFields"
                                                                                      , false ); } );

}

void failedUnmap( test::UnitTestConfig const & pConfig ) {

    test::UnitTest<test::ErrorHandlingExtension> ut( pConfig );

    test::gMockSharedMem.reset();

    test::gMockSharedMem.shmopenImpl = [&](int, mode_t ) noexcept {
                                                                     return 1;
                                                                  };
    test::gMockSharedMem.munmapImpl = [&]() noexcept {
                                                         return -1;
                                                     };
    LinuxShmRegion<TwoFields> tf( kTestInitOnly, kNotThreadSafe, "TwoFields", false );

}

void failedClose( test::UnitTestConfig const & pConfig ) {

    test::UnitTest<test::ErrorHandlingExtension> ut( pConfig );

    test::gMockSharedMem.reset();

    test::gMockSharedMem.shmopenImpl = [&](int, mode_t ) noexcept {
                                                                     return 1;
                                                                  };
    test::gMockSharedMem.closeImpl = [&]() noexcept {
                                                        return -1;
                                                    };
    LinuxShmRegion<TwoFields> tf( kTestInitOnly, kNotThreadSafe, "TwoFields", false );

}

void failedUnlink( test::UnitTestConfig const & pConfig ) {

    test::UnitTest<test::ErrorHandlingExtension> ut( pConfig );

    test::gMockSharedMem.reset();

    TwoFields tfBase;
    test::gMockSharedMem.mmapImpl = [&]() noexcept {
                                                       return static_cast<void*>(&tfBase);
                                                   };
    test::gMockSharedMem.shmunlinkImpl = [&]() noexcept {
                                                            return -1;
                                                        };
    LinuxShmRegion<TwoFields> tf( kTestInitOnly, kNotThreadSafe, "TwoFields", true );

}

class ShMemOwner {
public:
    ShMemOwner( InitOnly const & pInit, NotThreadSafe pNotTS )
        : lsm(pInit, pNotTS, "owner", true)
    {}

    TwoFields const * getPtr() const { return lsm.data(); }


    LinuxShmRegion<TwoFields> lsm;
};

void constAccessor( test::UnitTestConfig const & pConfig ) {

    test::UnitTest<test::ErrorHandlingExtension> ut( pConfig );

    test::gMockSharedMem.reset();
    TwoFields tfBase;
    test::gMockSharedMem.mmapImpl = [&]() noexcept {
                                                       return static_cast<void*>(&tfBase);
                                                   };

    ShMemOwner smo( kTestInitOnly, kNotThreadSafe );
    ut.notEqual( smo.getPtr(), nullptr, "not null" );

    LinuxShmRegion<TwoFields> newOwner( kTestInitOnly, kNotThreadSafe, "TwoFields", false ); //start out non-owning

    smo.lsm.release();

    newOwner.inherit();
}

}

UNITTESTER_MAIN( test_LinuxShmRegion, int pArgc, char const** pArgv ) {

    test::UnitTester  tester( pArgc, pArgv );

    std::vector<test::TestDescriptor> tests;

    tests.push_back( { test::TestTypeID::micro, "NominalCreateUse", &nominalCreateUse } );
    tests.push_back( { test::TestTypeID::micro, "NominalUse"      , &nominalUse       } );
    tests.push_back( { test::TestTypeID::micro, "FailedOpen"      , &failedOpen       } );
    tests.push_back( { test::TestTypeID::micro, "FailedFstat"     , &failedFstat      } );
    tests.push_back( { test::TestTypeID::micro, "FailedFtruncate" , &failedFtruncate  } );
    tests.push_back( { test::TestTypeID::micro, "FailedUnmap"     , &failedUnmap      } );
    tests.push_back( { test::TestTypeID::micro, "FailedClose"     , &failedClose      } );
    tests.push_back( { test::TestTypeID::micro, "FailedUnlink"    , &failedUnlink     } );
    tests.push_back( { test::TestTypeID::micro, "ConstAccessor"   , &constAccessor   } );

    return tester.filterAndExec( tests );


}
