
// NOLINTBEGIN(cppcoreguidelines-macro-usage) -- overriding system calls
#define lseek mock_lseek
#define close mock_close
#define write mock_write
#define ftruncate mock_ftruncate
#define stat mock_stat
#define fchmod mock_fchmod
#define open mock_open
#define read mock_read
// NOLINTEND(cppcoreguidelines-macro-usage)

/**
 * NOTE: Due to the above system call overrides and a naming conflict between std::basic_filebuf::open and POSIX ::open
 * The inclusion of the UnitTestExtension and UnitTester header files is delayed until after the mocks are defined.
 * Additionally, open is undefined prior to the inclusion of UnitTester so that the override doesn't break the linkage.
 *
 * Finally, redundant-decls is disabled for this file since the compiler is having issues with the macro definitions.
 */

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#include "OS/Linux/LinuxFile.hpp"

#include "UnitTesting/UnitTestingPragma.hpp"


namespace Encore::UnitTesting {

struct MockLinuxFileIO {
    std::function<int(int, off_t)> ftruncateImpl{ [](int, off_t) noexcept -> off_t{ return 0; } };
    std::function<int(const char*,struct stat*)> statImpl{ [](const char*, struct stat*) noexcept -> int {
                                                               return 0;
                                                           }
                                                         };
    std::function<int(const char*,int)> openImpl{ [](const char*, int ) noexcept -> int { return 0; } };
    std::function<int(int)> closeImpl{ []( int ) noexcept -> int { return 0; } };
    std::function<off_t(int, off_t, int)> lseekImpl{ [](int, off_t, int) noexcept -> off_t { return 0; } };
    std::function<int(int, mode_t)> fchmodImpl{ [](int, mode_t) noexcept -> int { return 0; } };
    std::function<ssize_t(int, const void*, size_t)> writeImpl{ [](int, const void*, int) noexcept -> ssize_t {
                                                                    return 0;
                                                                }
                                                              };
    std::function<ssize_t(int, void*, size_t)> readImpl{ [](int, void*, int) noexcept -> ssize_t { return 0; } };
};

static inline MockLinuxFileIO gMockLinuxFileIO{};

}

DISABLE_COMPILER_WARNING_PUSH
DISABLE_COMPILER_WARNING(-Wredundant-decls)
extern "C" off_t mock_lseek(int pFD, off_t pOff, int pRef) noexcept {
    return Encore::UnitTesting::gMockLinuxFileIO.lseekImpl(pFD, pOff, pRef);
}

extern "C" int mock_ftruncate( int pFD, off_t pSize ) noexcept {
    return Encore::UnitTesting::gMockLinuxFileIO.ftruncateImpl( pFD, pSize );
}

extern "C" int mock_stat( const char* pName, struct stat* pStat) noexcept {
    return Encore::UnitTesting::gMockLinuxFileIO.statImpl(pName, pStat);
}

extern "C" int mock_open( const char* pName, int pFlags, ...) {
    return Encore::UnitTesting::gMockLinuxFileIO.openImpl( pName, pFlags );
}

extern "C" int mock_close( int pFD ) {
    return Encore::UnitTesting::gMockLinuxFileIO.closeImpl( pFD );
}

extern "C" int mock_fchmod( int pFD, mode_t pMode ) noexcept {
    return Encore::UnitTesting::gMockLinuxFileIO.fchmodImpl( pFD, pMode );
}

extern "C" ssize_t mock_write( int pFD, const void* pBuff, size_t pSize ) {
    return Encore::UnitTesting::gMockLinuxFileIO.writeImpl( pFD, pBuff, pSize );
}

extern "C" ssize_t mock_read( int pFD, void* pBuff, size_t pSize ) {
    return Encore::UnitTesting::gMockLinuxFileIO.readImpl( pFD, pBuff, pSize );
}
DISABLE_COMPILER_WARNING_POP

#include "encore/ErrorHandling/test/UnitTestExtensions.hpp"

namespace {

namespace test = Encore::UnitTesting;
using namespace Encore;
using namespace Encore::OS;
using namespace Encore::Utils;
using namespace Encore::ErrorHandling;


void fileNameTooLong( test::UnitTestConfig const & pConfig ) {

    test::MixinUnitTest<test::ErrorHandlingExtension> ut{pConfig};

    std::vector<char> name{};
    name.resize(1000);
    for (auto & ch : name) { ch = 'a'; }

    std::array<char, 4> nameBuf{};
    auto fileioPtr = std::make_unique<LinuxFile>(nameBuf);
    auto & fileio = *fileioPtr;
    std::string_view view{name.data(), name.size()};

    constexpr auto kMode{FileOpenMode::read_write};

    ut.failsWithCode( fileio.setFileOpenParams( view, kMode )
                    , EG_ERROR_CODE( ErrorHandling::StdFswErrors, length_error )
                    , PREPEND_FL("expected fail")
                    );
}

void checkReturnCode( test::UnitTestConfig const & pConfig ) {

    test::MixinUnitTest<test::ErrorHandlingExtension> ut{pConfig};

    ut.isSuccessful( LinuxFile::checkRetCode( 0, LinuxFile::error_if_negative ), PREPEND_FL("expect success") );
    ut.isSuccessful( LinuxFile::checkRetCode( 0, LinuxFile::error_if_non_zero ), PREPEND_FL("expect success") );
    ut.isSuccessful( LinuxFile::checkRetCode( 1, LinuxFile::error_if_negative ), PREPEND_FL("expect success") );
    ut.notSuccessful( LinuxFile::checkRetCode( 1, LinuxFile::error_if_non_zero ), PREPEND_FL("expect success") );
    ut.notSuccessful( LinuxFile::checkRetCode( -1, LinuxFile::error_if_negative ), PREPEND_FL("expect success") );
    ut.notSuccessful( LinuxFile::checkRetCode( -1, LinuxFile::error_if_non_zero ), PREPEND_FL("expect success") );
}

void composeOpenFlags( test::UnitTestConfig const & pConfig ) {

    test::UnitTest<test::ErrorHandlingExtension> ut{pConfig};

    ut.succeedsWithValue( LinuxFile::composeOpenFlags( FileOpenMode::read_only )
                        , O_RDONLY
                        , PREPEND_FL("flags correct")
                        );
    ut.succeedsWithValue( LinuxFile::composeOpenFlags( FileOpenMode::truncate_write_only )
                        , O_CREAT | O_TRUNC | O_WRONLY
                        , PREPEND_FL("flags correct")
                        );
    ut.succeedsWithValue( LinuxFile::composeOpenFlags( FileOpenMode::exclusive_write_only )
                        , O_CREAT | O_EXCL | O_WRONLY
                        , PREPEND_FL("flags correct")
                        );
    ut.succeedsWithValue( LinuxFile::composeOpenFlags( FileOpenMode::append_write_only )
                        , O_CREAT | O_APPEND | O_WRONLY
                        , PREPEND_FL("flags correct")
                        );
    ut.succeedsWithValue( LinuxFile::composeOpenFlags( FileOpenMode::read_write )
                        , O_RDWR
                        , PREPEND_FL("flags correct")
                        );
    ut.succeedsWithValue( LinuxFile::composeOpenFlags( FileOpenMode::truncate_read_write )
                        , O_CREAT | O_TRUNC | O_RDWR
                        , PREPEND_FL("flags correct")
                        );
    ut.succeedsWithValue( LinuxFile::composeOpenFlags( FileOpenMode::exclusive_read_write )
                        , O_CREAT | O_EXCL | O_RDWR
                        , PREPEND_FL("flags correct")
                        );
    ut.succeedsWithValue( LinuxFile::composeOpenFlags( FileOpenMode::append_read_write )
                        , O_CREAT | O_APPEND | O_RDWR
                        , PREPEND_FL("flags correct")
                        );

    ut.failsWithCode( LinuxFile::composeOpenFlags( FileOpenMode{100} )
                    , EG_ERROR_CODE( FileErrors, invalid_open_mode )
                    , PREPEND_FL("bad flags")
                    );
}

void invalidOpenMode( test::UnitTestConfig const & pConfig ) {

    test::UnitTest<test::ErrorHandlingExtension> ut{pConfig};

    auto nameBuf = std::array<char, 64>();
    LinuxFile lf( nameBuf );

    ut.failsWithCode( lf.setFileOpenParams( "foo.txt", FileOpenMode::invalid )
                    , EG_ERROR_CODE( FileErrors, invalid_open_mode )
                    , "invalid open mode" );
    ut.failsWithCode (lf.getPos(), EG_ERROR_CODE( FileErrors, not_open ), "file not open yet to tell" );
    ut.failsWithCode (lf.setPos( 8 ), EG_ERROR_CODE( FileErrors, not_open ), "file not open yet to seek" );

}

void syscallErrors( test::UnitTestConfig const & pConfig ) {

    test::UnitTest<test::ErrorHandlingExtension> ut{pConfig};

    std::string fileName = "fileName.txt";
    {
        test::gMockLinuxFileIO = test::MockLinuxFileIO();
        test::gMockLinuxFileIO.statImpl = [](const char*, struct stat*) noexcept -> int{ return -1; };

        std::array<char, 64> fname;
        LinuxFile lf( fname );
        ut.isSuccessful(lf.setFileOpenParams( fileName, FileOpenMode::read_only ));
        std::ignore = lf.open();
        errno = EFAULT;
        ut.failsWithCode( lf.getFileInfo(), EG_ERROR_CODE(SystemErrors, bad_address), "stat fails" );
    }
    {
        test::gMockLinuxFileIO = test::MockLinuxFileIO();
        test::gMockLinuxFileIO.ftruncateImpl = [](int, int) noexcept -> int{ return -1; };

        std::array<char, 64> fname;
        LinuxFile lf( fname );
        ut.isSuccessful(lf.setFileOpenParams( fileName, FileOpenMode::read_only ));
        std::ignore = lf.open();
        errno = EFAULT;
        ut.failsWithCode( lf.resize( 1 ), EG_ERROR_CODE(SystemErrors, bad_address), "ftruncate fails" );
    }
    {
        test::gMockLinuxFileIO = test::MockLinuxFileIO();
        test::gMockLinuxFileIO.openImpl = [](const char*, int) noexcept -> int{ return 3; };

        std::array<char, 64> fname;
        LinuxFile lf( fname );
        ut.isSuccessful(lf.setFileOpenParams( fileName, FileOpenMode::read_only ));
        ut.isSuccessful( lf.open(), "open passes" );
        ut.isSuccessful( lf.open(), "open passes again" );
    }
    {
        test::gMockLinuxFileIO = test::MockLinuxFileIO();
        test::gMockLinuxFileIO.closeImpl = [](int) noexcept -> int{ return -1; };

        std::array<char, 64> fname;
        LinuxFile lf( fname );
        ut.isSuccessful(lf.setFileOpenParams( fileName, FileOpenMode::read_only ));
        ut.isSuccessful( lf.open(), PREPEND_FL("open passes" ));
        errno = EIO;
        ut.failsWithCode( lf.close(), EG_ERROR_CODE(SystemErrors, io_error), "close fails" );
    }
    {
        test::gMockLinuxFileIO = test::MockLinuxFileIO();
        test::gMockLinuxFileIO.openImpl = [](const char*, int) noexcept -> int{ return -1; };

        std::array<char, 64> fname;
        LinuxFile lf( fname );
        ut.isSuccessful(lf.setFileOpenParams( fileName, FileOpenMode::read_only ));
        errno = ENAMETOOLONG;
        ut.failsWithCode( lf.open(), EG_ERROR_CODE(SystemErrors, filename_too_long), "open fails" );
    }
    {
        test::gMockLinuxFileIO = test::MockLinuxFileIO();
        test::gMockLinuxFileIO.openImpl = [](const char*, int) noexcept -> int{ return 3; };
        test::gMockLinuxFileIO.lseekImpl = [](int, off_t, int) noexcept -> off_t{ return -1; };

        std::array<char, 64> fname;
        LinuxFile lf( fname );
        ut.isSuccessful(lf.setFileOpenParams( fileName, FileOpenMode::read_only ));
        ut.isSuccessful( lf.open(), PREPEND_FL("open passes" ));
        errno = ESPIPE;
        ut.failsWithCode( lf.getPos(), EG_ERROR_CODE(SystemErrors, invalid_seek), "get fails" );
        ut.failsWithCode( lf.setPos( 200 ), EG_ERROR_CODE(SystemErrors, invalid_seek), "set fails" );
    }
    {
        test::gMockLinuxFileIO = test::MockLinuxFileIO();
        test::gMockLinuxFileIO.openImpl = [](const char*, int) noexcept -> int{ return 3; };
        test::gMockLinuxFileIO.fchmodImpl = [](int, int) noexcept -> int{ return -1; };

        std::array<char, 64> fname;
        LinuxFile lf( fname );
        ut.isSuccessful(lf.setFileOpenParams( fileName, FileOpenMode::read_only ));
        lf.setFilePermissions( 1 );
        errno = EINVAL;
        ut.failsWithCode( lf.open(), EG_ERROR_CODE(SystemErrors, invalid_argument), "open fails chmod" );
    }
    {
        test::gMockLinuxFileIO = test::MockLinuxFileIO();
        test::gMockLinuxFileIO.openImpl = [](const char*, int) noexcept -> int{ return 3; };
        test::gMockLinuxFileIO.writeImpl = [](int, const void*, size_t) noexcept -> ssize_t{ return -1; };

        std::array<char, 64> fname;
        LinuxFile lf( fname );
        ut.isSuccessful(lf.setFileOpenParams( fileName, FileOpenMode::read_only ));
        ut.isSuccessful( lf.open(), PREPEND_FL("open passes" ));
        errno = EIO;
        std::array<EncoreByte, 128> buf;
        ut.failsWithCode( lf.write(BytesView(buf)), EG_ERROR_CODE(SystemErrors, io_error), "write fails" );
    }
    {
        test::gMockLinuxFileIO = test::MockLinuxFileIO();
        test::gMockLinuxFileIO.openImpl = [](const char*, int) noexcept -> int{ return 3; };
        test::gMockLinuxFileIO.readImpl = [](int, void*, size_t) noexcept -> ssize_t{ return -1; };

        std::array<char, 64> fname;
        LinuxFile lf( fname );
        ut.isSuccessful(lf.setFileOpenParams( fileName, FileOpenMode::read_only ));
        ut.isSuccessful( lf.open(), PREPEND_FL("open passes" ));
        errno = EIO;
        std::array<EncoreByte, 128> buf;
        ut.failsWithCode( lf.read(BytesRange(buf)), EG_ERROR_CODE(SystemErrors, io_error), "read fails" );
    }

}

}

#undef open
#include "encore/UnitTesting/UnitTester.hpp"

UNITTESTER_MAIN( test_LinuxFile, int pArgc, char const** pArgv ) {

    test::UnitTester tester( pArgc, pArgv );

    std::vector<test::TestDescriptor> tests{};
    tests.push_back( { test::TestTypeID::misc, "FileNameTooLong", &fileNameTooLong } );
    tests.push_back( { test::TestTypeID::micro, "CheckReturnCode", &checkReturnCode } );
    tests.push_back( { test::TestTypeID::micro, "ComposeOpenFlags", &composeOpenFlags } );
    tests.push_back( { test::TestTypeID::micro, "InvalidOpenMode", &invalidOpenMode } );
    tests.push_back( { test::TestTypeID::micro, "SyscallErrors", &syscallErrors } );

    return tester.filterAndExec( tests );
}
