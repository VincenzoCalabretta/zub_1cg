
///
/// @file
/// @brief Defines a linux based serial interface.
///
#ifndef ENCORE_OS_LinuxSerialDevice_HPP
#define ENCORE_OS_LinuxSerialDevice_HPP

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <functional>
#include <string_view>
#include <utility>

#include "encore/Encore.hpp"
#include "encore/ErrorHandling/ErrorGroup.hpp"
#include "encore/ErrorHandling/Result.hpp"
#include "encore/ErrorHandling/SystemErrors.hpp"
#include "encore/Logging/ILogRoutable.hpp"
#include "encore/Logging/Logger.hpp"
#include "encore/Logging/StackTraceHelper.hpp"
#include "encore/Utils/IDevice.hpp"
#include "encore/Utils/DeviceErrors.hpp"
#include "encore/Utils/IReaderWriter.hpp"
#include "encore/Utils/Span.hpp"
#include "encore/Utils/StringUtils.hpp"

#include "encore/OS/Common/IStreamSocket.hpp"
#include "encore/OS/Linux/Linux.hpp"


namespace Encore::OS {

/// @brief Parameterization block for LinuxSerialDevice
struct LinuxSerialDeviceFlags {

    int openFlags{ O_RDWR | O_NOCTTY | O_NONBLOCK }; ///<@brief Flags used on initial device open
    tcflag_t controlModeFlags{ CREAD | CS8 | CLOCAL }; ///<@brief Control mode flags set after device is opened
    tcflag_t localModeFlags{ NOFLSH }; ///<@brief Local mode flags set after device is opened
};


/**
 * @brief Linux implementation of a serial device interface
 */
class LinuxSerialDevice final : public OS::IStreamSocket, public LinuxDescriptable, public Logging::LogRoutable {

public: /*IStreamSocket*/

    ErrorHandling::Result<bool> open() final;

    ErrorHandling::Result<bool> close() noexcept final;

    bool isOpen() const noexcept final;

public: /*IReaderWriter*/

    ErrorHandling::Result<std::size_t> read( Utils::BytesRange pBytes ) final;

    ErrorHandling::Result<std::size_t> write( Utils::BytesView const & pView ) final;

public:

    static constexpr LinuxSerialDeviceFlags kDefaultFlags{}; ///<@brief default flags for device configuration

    LinuxSerialDevice( Utils::Span<char> pDeviceNameSpan );

    LinuxSerialDevice() = delete;
    LinuxSerialDevice( const LinuxSerialDevice & ) = delete;
    LinuxSerialDevice( LinuxSerialDevice && ) = delete;
    LinuxSerialDevice & operator=( const LinuxSerialDevice & ) = delete;
    LinuxSerialDevice & operator=( LinuxSerialDevice && ) = delete;

    ~LinuxSerialDevice() final;

    ErrorHandling::ReturnCode
    setSerialOpenParams( std::string_view pDevName, speed_t pBaud, LinuxSerialDeviceFlags pFlags = kDefaultFlags );

    ErrorHandling::ReturnCode flush() noexcept;

private:

    using LinuxDescriptable::setFd;

    ErrorHandling::ReturnCode configureSerial( int pFd ) const noexcept;

    Utils::Span<char> mNameSpan;
    std::string_view mDevName{};
    speed_t mBaud{};
    LinuxSerialDeviceFlags mFlags{};
};


/**
 *  @brief Opens the serial device connection
 */
inline ErrorHandling::Result<bool> LinuxSerialDevice::open() {

    // If the fd is already open, no-op.
    if( isOpen() ) {
        return true;
    }

    auto fd = ::open( mDevName.data(), mFlags.openFlags ); // NOLINT(cppcoreguidelines-pro-type-vararg) - POSIX call
    if( fd < 0 ) { return CURRENT_SYSTEM_ERROR_CODE(); }

    auto configured = configureSerial( fd );
    if( not configured.success() ){ ::close(fd); return TRACE_CODE( logger(), configured.error() ); }

    setFd( fd );

    return true;
}


/**
 * @brief Closes the underlying serial file descriptor
 */
inline ErrorHandling::Result<bool> LinuxSerialDevice::close() noexcept {

    if( not isOpen() ) {
        return true;
    }

    int rc = ::close( getFd() );
    if( rc < 0 ) { return CURRENT_SYSTEM_ERROR_CODE(); }

    setFd( -1 );

    return true;
}


/**
 * @brief Returns if the serial device connection is active
 */
inline bool LinuxSerialDevice::isOpen() const noexcept {
    return getFd() >= 0;
}


/**
 * @brief Reads bytes from the serial port
 *
 * @param pBytes Bytes range to populate
 *
 * @return The number of bytes read, or error code
 */
inline ErrorHandling::Result<std::size_t> LinuxSerialDevice::read( Utils::BytesRange pBytes ) {

    if( not isOpen() ) { return EG_ERROR_CODE( Utils::DeviceErrors, not_open ); }

    auto bytesRead = ::read( getFd(), pBytes.data(), pBytes.size() );

    if( bytesRead < 0 ) {

        if( errno == EAGAIN ) {

            logger().log("EAGAIN on read\n", Logging::Severity::debug);

            return 0;
        }

        return CURRENT_SYSTEM_ERROR_CODE();
    }

    return static_cast<std::size_t>( bytesRead );
}


/**
 * @brief Writes bytes to the serial port
 *
 * @param pView View of data to write
 *
 * @return The number of bytes written, or error code
 */
inline ErrorHandling::Result<std::size_t> LinuxSerialDevice::write( Utils::BytesView const & pView ) {

    if( not isOpen() ) { return EG_ERROR_CODE( Utils::DeviceErrors, not_open ); }

    auto bytesWritten = ::write( getFd(), pView.data(), pView.size() );

    if( bytesWritten < 0 ) {

        if( errno == EAGAIN ) {

            logger().log( "EAGAIN on write\n", Logging::Severity::debug );

            return 0;
        }

        return CURRENT_SYSTEM_ERROR_CODE();
    }

    return static_cast<std::size_t>( bytesWritten );
}


/**
 * @brief Constructs device from a name buffer
 *
 * @param pDeviceNameSpan A span to copy the device name to
 */
inline LinuxSerialDevice::LinuxSerialDevice( Utils::Span<char> pDeviceNameSpan )
: mNameSpan{ pDeviceNameSpan }
{}


/**
 * @brief Destructor, closes the device
 */
inline LinuxSerialDevice::~LinuxSerialDevice() {
    std::ignore = this->close();
}


/**
 * @brief Sets the serial device name, baud rate, and flags
 *
 * @param pDevName The serial device name
 * @param pBaud The serial baud rate to communicate at
 * @param pFlags The set of flags to configure the device with
 */
inline ErrorHandling::ReturnCode
LinuxSerialDevice::setSerialOpenParams( std::string_view pDevName, speed_t pBaud, LinuxSerialDeviceFlags pFlags ) {

    if( isOpen() ) { return EG_ERROR_CODE( Utils::DeviceErrors, must_be_closed ); }

    mDevName = {};

    auto name = Utils::copyString( pDevName, mNameSpan );
    if( not name.success() ) { return TRACE_CODE( logger(), name.error() ); }

    mDevName = name.value();
    mBaud = pBaud;
    mFlags = pFlags;

    return {};
}


/**
 * @brief Flushes the contents of serial buffer and discards any unread or unsent bytes.
 */
inline ErrorHandling::ReturnCode LinuxSerialDevice::flush() noexcept {

    if( not isOpen() ) { return EG_ERROR_CODE( Utils::DeviceErrors, not_open ); }

    auto rc = ::tcflush( getFd(), TCIOFLUSH );
    if( rc < 0 ) { return CURRENT_SYSTEM_ERROR_CODE(); }

    return {};
}


inline ErrorHandling::ReturnCode LinuxSerialDevice::configureSerial( int pFd ) const noexcept {

    struct termios options = {};
    options.c_cflag = mFlags.controlModeFlags;
    options.c_lflag = mFlags.localModeFlags;

    int rc = ::cfsetospeed( &options, mBaud );
    if( rc < 0 ) { return CURRENT_SYSTEM_ERROR_CODE(); }

    options.c_cc[VMIN] = 0;
    options.c_cc[VTIME] = 0;

    rc = ::tcsetattr( pFd, TCSANOW, &options );
    if( rc < 0 ) { return CURRENT_SYSTEM_ERROR_CODE(); }

    return {};
}

}

#endif
