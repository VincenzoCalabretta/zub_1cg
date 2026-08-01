
// NOLINTBEGIN(cppcoreguidelines-macro-usage) -- overriding system calls
#define close mock_close
#define open mock_open
#define write mock_write
#define read mock_read
#define tcflush mock_tcflush
#define cfsetospeed mock_cfsetospeed
#define tcsetattr mock_tcsetattr
// NOLINTEND(cppcoreguidelines-macro-usage)

/**
 * NOTE: Due to the above system call overrides and a naming conflict between std::basic_filebuf::open and POSIX ::open
 * The inclusion of the UnitTestExtension and UnitTester header files is delayed until after the mocks are defined.
 * Additionally, open is undefined prior to the inclusion of UnitTester so that the override doesn't break the linkage.
 *
 * Finally, redundant-decls is disabled for this file since the compiler is having issues with the macro definitions.
 */

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#include "OS/Linux/LinuxSerialDevice.hpp"

#include "UnitTesting/UnitTestingPragma.hpp"


namespace Encore::UnitTesting {

struct MockLinuxFileIO {
    std::function<int(const char*,int)> openImpl{ [](const char*, int ) noexcept->int { return 0; } };
    std::function<int(int)> closeImpl{ []( int ) noexcept->int { return 0; } };
    std::function<ssize_t(int, const void*, size_t)> writeImpl{ [](int, const void*, int) noexcept->ssize_t {
                                                                    return 0;
                                                                }
                                                              };
    std::function<ssize_t(int, void*, size_t)> readImpl{ [](int, void*, int) noexcept->ssize_t { return 0; } };
    std::function<int(int, int)> tcflushImpl{ [](int, int) noexcept -> int { return 0; } };
    std::function<int(struct termios*, speed_t)> cfsetospeedImpl{ [](struct termios*, speed_t) noexcept->int
                                                                    {
                                                                        return 0;
                                                                    } };
    std::function<int(int, int, const struct termios*)> tcsetattrImpl{ [](int, int, const struct termios*) noexcept->int
                                                                         {
                                                                             return 0;
                                                                         } };
};

static inline MockLinuxFileIO gMockLinuxSerialDevice{};

}

DISABLE_COMPILER_WARNING_PUSH
DISABLE_COMPILER_WARNING(-Wredundant-decls)
extern "C" int mock_open( const char* pName, int pFlags, ...) {
    return Encore::UnitTesting::gMockLinuxSerialDevice.openImpl( pName, pFlags );
}

extern "C" int mock_close( int pFD ) {
    return Encore::UnitTesting::gMockLinuxSerialDevice.closeImpl( pFD );
}

extern "C" ssize_t mock_write( int pFD, const void* pBuff, size_t pSize ) {
    return Encore::UnitTesting::gMockLinuxSerialDevice.writeImpl( pFD, pBuff, pSize );
}

extern "C" ssize_t mock_read( int pFD, void* pBuff, size_t pSize ) {
    return Encore::UnitTesting::gMockLinuxSerialDevice.readImpl( pFD, pBuff, pSize );
}

extern "C" int mock_tcflush( int pFD, int pQueue ) noexcept {
    return Encore::UnitTesting::gMockLinuxSerialDevice.tcflushImpl( pFD, pQueue );
}

extern "C" int mock_cfsetospeed( struct termios* pTermios, speed_t pSpeed ) noexcept {
    return Encore::UnitTesting::gMockLinuxSerialDevice.cfsetospeedImpl( pTermios, pSpeed );
}

extern "C" int mock_tcsetattr( int pFD, int pActions, const struct termios* pTermios ) noexcept {
    return Encore::UnitTesting::gMockLinuxSerialDevice.tcsetattrImpl( pFD, pActions, pTermios );
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
    name.assign(1000, 'a');

    std::array<char, 4> nameBuf{};
    auto devicePtr = std::make_unique<LinuxSerialDevice>(nameBuf);
    auto & device = *devicePtr;
    std::string_view view{name.data(), name.size()};

    ut.failsWithCode( device.setSerialOpenParams( view, B9600 )
                    , EG_ERROR_CODE( ErrorHandling::StdFswErrors, length_error )
                    , PREPEND_FL("name is too long")
                    );
}

void syscallErrors( test::UnitTestConfig const & pConfig ) {

    test::UnitTest<test::ErrorHandlingExtension> ut{pConfig};

    std::string devName = "device1";
    {
        test::gMockLinuxSerialDevice = test::MockLinuxFileIO();
        int openRet = -1;
        test::gMockLinuxSerialDevice.openImpl = [&](const char*, int) noexcept -> int { return openRet; };
        test::gMockLinuxSerialDevice.closeImpl = [&](int) noexcept -> int { return -1; };

        std::array<char, 64> fname;
        LinuxSerialDevice lsd( fname );
        ut.isSuccessful( lsd.setSerialOpenParams( devName, B115200 ), "configure successful" );
        errno = ENODEV;
        ut.failsWithCode( lsd.open(), EG_ERROR_CODE( SystemErrors, no_such_device ), "open fails" );

        openRet = 4;
        ut.isSuccessful( lsd.open(), "open succeeds" );
        errno = ENOTCONN;
        ut.failsWithCode( lsd.close(), EG_ERROR_CODE( SystemErrors, not_connected ), "close fails" );

    }
    {
        test::gMockLinuxSerialDevice = test::MockLinuxFileIO();
        test::gMockLinuxSerialDevice.cfsetospeedImpl = [&](struct termios*, speed_t) noexcept -> int { return -1; };

        std::array<char, 64> fname;
        LinuxSerialDevice lsd( fname );
        ut.isSuccessful( lsd.setSerialOpenParams( devName, B115200 ), "configure successful" );
        errno = EINVAL;
        ut.failsWithCode( lsd.open(), EG_ERROR_CODE( SystemErrors, invalid_argument ), "open with bad set speed" );
    }
    {
        test::gMockLinuxSerialDevice = test::MockLinuxFileIO();
        test::gMockLinuxSerialDevice.tcsetattrImpl = [&](int,int,const struct termios*) noexcept -> int { return -1; };

        std::array<char, 64> fname;
        LinuxSerialDevice lsd( fname );
        ut.isSuccessful( lsd.setSerialOpenParams( devName, B115200 ), "configure successful" );
        errno = EINVAL;
        ut.failsWithCode( lsd.open(), EG_ERROR_CODE( SystemErrors, invalid_argument ), "open fails with bad set attr" );
    }
    {
        test::gMockLinuxSerialDevice = test::MockLinuxFileIO();
        test::gMockLinuxSerialDevice.tcflushImpl = [&](int, int) noexcept -> int { return -1; };

        std::array<char, 64> fname;
        LinuxSerialDevice lsd( fname );
        ut.isSuccessful( lsd.setSerialOpenParams( devName, B115200 ), "configure successful" );
        ut.isSuccessful( lsd.open(), "open succeeds" );
        errno = EIO;
        ut.failsWithCode( lsd.flush(), EG_ERROR_CODE( SystemErrors, io_error ), "flush fails" );
    }
    {
        test::gMockLinuxSerialDevice = test::MockLinuxFileIO();
        test::gMockLinuxSerialDevice.tcflushImpl = [&](int, int) noexcept -> int { return -1; };

        std::array<char, 64> fname;
        LinuxSerialDevice lsd( fname );
        ut.isSuccessful( lsd.setSerialOpenParams( devName, B115200 ), "configure successful" );
        ut.isSuccessful( lsd.open(), "open succeeds" );
        errno = EIO;
        ut.failsWithCode( lsd.flush(), EG_ERROR_CODE( SystemErrors, io_error ), "flush fails" );
    }
    {
        test::gMockLinuxSerialDevice = test::MockLinuxFileIO();
        test::gMockLinuxSerialDevice.readImpl = [&](int, void*, size_t) noexcept -> int { return -1; };

        std::array<char, 64> fname;
        LinuxSerialDevice lsd( fname );
        ut.isSuccessful( lsd.setSerialOpenParams( devName, B115200 ), "configure successful" );
        ut.isSuccessful( lsd.open(), "open succeeds" );
        errno = EAGAIN;
        std::array<EncoreByte, 128> buf;
        auto rc = lsd.read(buf);
        ut.isSuccessful( rc, "read succeeds" );
        ut.areEqual( rc.value(), 0u, "zero bytes read" );
        errno = EMSGSIZE;
        ut.failsWithCode( lsd.read(buf), EG_ERROR_CODE( SystemErrors, message_size ), "read fails" );
    }
    {
        test::gMockLinuxSerialDevice = test::MockLinuxFileIO();
        test::gMockLinuxSerialDevice.writeImpl = [&](int, const void*, size_t) noexcept -> int { return -1; };

        std::array<char, 64> fname;
        LinuxSerialDevice lsd( fname );
        ut.isSuccessful( lsd.setSerialOpenParams( devName, B115200 ), "configure successful" );
        ut.isSuccessful( lsd.open(), "open succeeds" );
        errno = EAGAIN;
        std::array<EncoreByte, 128> buf;
        auto rc = lsd.write(buf);
        ut.isSuccessful( rc, "write succeeds" );
        ut.areEqual( rc.value(), 0u, "zero bytes written" );
        errno = EMSGSIZE;
        ut.failsWithCode( lsd.write(buf), EG_ERROR_CODE( SystemErrors, message_size ), "write fails" );
    }
}

}

#undef open
#include "encore/UnitTesting/UnitTester.hpp"

UNITTESTER_MAIN( test_LinuxSerialDevice, int pArgc, char const** pArgv ) {

    test::UnitTester tester( pArgc, pArgv );

    std::vector<test::TestDescriptor> tests{};
    tests.push_back( { test::TestTypeID::misc, "FileNameTooLong", &fileNameTooLong } );
    tests.push_back( { test::TestTypeID::micro, "SyscallErrors", &syscallErrors } );

    return tester.filterAndExec( tests );
}
