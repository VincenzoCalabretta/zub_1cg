
///
/// @file
/// @brief Base linux implementation of TCP sockets
///
#ifndef ENCORE_OS_BaseLinuxTCPSocket_HPP
#define ENCORE_OS_BaseLinuxTCPSocket_HPP

#include <arpa/inet.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstdint>

#include <utility>

#include "encore/Encore.hpp"
#include "encore/ErrorHandling/ErrorCode.hpp"
#include "encore/ErrorHandling/SystemErrors.hpp"
#include "encore/Logging/FileLineHelper.hpp"
#include "encore/Logging/ILogRoutable.hpp"
#include "encore/Utils/DeviceErrors.hpp"
#include "encore/Utils/Span.hpp"

#include "encore/OS/Common/IStreamSocket.hpp"
#include "encore/OS/Linux/Linux.hpp"

namespace Encore::OS {

/**
 * @brief A TCP implementation of ISocket
 *
 * @details An abstract class. Children will implement open(). The setFd() method is their muse.
 */
class BaseLinuxTCPSocket : public IStreamSocket, public LinuxDescriptable, public Logging::LogRoutable {
public: /*ISocket*/

    ~BaseLinuxTCPSocket() override;

    ErrorHandling::Result<std::size_t> write( Utils::BytesView const & ) noexcept final;

    ErrorHandling::Result<std::size_t> read( Utils::BytesRange ) final;

    ErrorHandling::Result<bool> close() noexcept final;

    bool isOpen() const noexcept final;

protected:

    /// @cond Defaults
    BaseLinuxTCPSocket() = default;
    BaseLinuxTCPSocket(BaseLinuxTCPSocket const &) = default;
    BaseLinuxTCPSocket(BaseLinuxTCPSocket &&) = default;
    BaseLinuxTCPSocket & operator =(BaseLinuxTCPSocket const &) = default;
    BaseLinuxTCPSocket & operator =(BaseLinuxTCPSocket &&) = default;
    /// @endcond

    void setConnected( bool );

    bool isFdValid() const noexcept;

    bool isConnected() const noexcept;

private:

    bool mConnected{false};
};

/////////////////
// Definitions //
/////////////////

/**
 * @brief Writes data to the socket
 * @param pSrc The source of the data
 * @return Number of bytes written
 */
inline ErrorHandling::Result<std::size_t> BaseLinuxTCPSocket::write( Utils::BytesView const & pSrc ) noexcept {

    if( not isOpen() ) {
        return EG_ERROR_CODE( Utils::DeviceErrors, not_open );
    }

    auto numWritten = ::send( getFd(), pSrc.data(), pSrc.size(), MSG_DONTWAIT | MSG_NOSIGNAL );
    if( numWritten < 0 ) {
        return CURRENT_SYSTEM_ERROR_CODE();
    }

    return static_cast<std::size_t>(numWritten);
}


/**
 * @brief Reads data from the socket
 * @param pDest The destination of the data
 * @return Number of bytes read
 */
inline ErrorHandling::Result<std::size_t> BaseLinuxTCPSocket::read( Utils::BytesRange pDest ) {

    if( not isOpen() ) {
        return EG_ERROR_CODE( Utils::DeviceErrors, not_open );
    }

    auto numRead = ::recv( getFd(), pDest.data(), pDest.size(), MSG_DONTWAIT );
    if( numRead < 0 ) {

        if( errno == EAGAIN ) { return 0; }

        return CURRENT_SYSTEM_ERROR_CODE();
    }

    if( numRead == 0 and pDest.size() != 0 ) {

        logger().log( "End of stream received, closing socket...\n", Logging::Severity::debug );

        auto res = close();

        // LCOV_EXCL_BR_START -- Defensive programming, a close failure in this context is nearly impossible
        if( not res.success() ) { return TRACE_CODE( logger(), res.error() ); }
        // LCOV_EXCL_BR_STOP
    }

    return static_cast<std::size_t>(numRead);
}


/**
 * @brief Closes the socket and releases file descriptor
 * @return If socket is fully closed
 */
inline ErrorHandling::Result<bool> BaseLinuxTCPSocket::close() noexcept {

    if( ::close( getFd() ) < 0 ) { return CURRENT_SYSTEM_ERROR_CODE(); }

    setConnected( false );
    setFd( -1 );

    return true;
}


///@brief Returns if file descriptor is valid and socket is connected
inline bool BaseLinuxTCPSocket::isOpen() const noexcept {
    return isConnected() and isFdValid();
}


/// @brief Destructor -- Invokes close on the socket
inline BaseLinuxTCPSocket::~BaseLinuxTCPSocket() {
    Logging::ScopedSeverityGuard sev{ logger(), Logging::Severity::silence };
    std::ignore = close();
}


/**
 * @brief Sets the "connected" state to the provided value
 * @param pVal Connected value to set
 */
inline void BaseLinuxTCPSocket::setConnected( bool pVal ) { mConnected = pVal; }


/// @brief Returns if the file descriptor is valid
inline bool BaseLinuxTCPSocket::isFdValid() const noexcept { return getFd() >= 0; }


/// @brief Returns if the socket is connected
inline bool BaseLinuxTCPSocket::isConnected() const noexcept { return mConnected; }

}

#endif
