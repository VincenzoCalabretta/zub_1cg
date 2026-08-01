
///
/// @file
/// @brief Linux implementation of TCP client sockets
///
#ifndef ENCORE_OS_LinuxTCPClientSocket_HPP
#define ENCORE_OS_LinuxTCPClientSocket_HPP

#include <arpa/inet.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstdint>

#include <utility>

#include "encore/Encore.hpp"
#include "encore/ErrorHandling/ErrorCode.hpp"
#include "encore/ErrorHandling/ErrorGroup.hpp"
#include "encore/ErrorHandling/SystemErrors.hpp"
#include "encore/Logging/StackTraceHelper.hpp"
#include "encore/Utils/DeviceErrors.hpp"
#include "encore/Utils/Span.hpp"

#include "encore/OS/Common/IPv4Address.hpp"
#include "encore/OS/Common/ITCPClient.hpp"
#include "encore/OS/Common/TCP.hpp"
#include "encore/OS/Linux/BaseLinuxTCPSocket.hpp"
#include "encore/OS/Linux/Linux.hpp"

namespace Encore::OS {

/**
 * @brief A TCP Socket for Linux
 * @details The "client end" of a client-server TCP connection - it connects to the server
 */
class LinuxTCPClientSocket final : public BaseLinuxTCPSocket, public ITCPClient {
public: /*ISocket*/

    ErrorHandling::Result<bool> open() noexcept final;

public: /*ITCPClient*/

    ErrorHandling::ReturnCode setTCPClientParams( TCPClientParams const & ) final;

public:

    ///@brief Default constructor
    LinuxTCPClientSocket() = default;

private:

    bool mNonBlocking{ false };
    sockaddr_in  mSockAddrIn{};

};

/////////////////
// Definitions //
/////////////////

/**  
 * @brief Opens the socket
 * @details If the socket does not connect because it is non-blocking, the file descriptor will be valid so you can
 *          use it in poll() calls, but the socket will not be "opened" until open() returns true
 * @return If socket has fully opened
 */

inline ErrorHandling::Result<bool> LinuxTCPClientSocket::open() noexcept {

    if( not isFdValid() ) {

        int type = SOCK_STREAM | ( static_cast<int>( mNonBlocking ) * SOCK_NONBLOCK );

        int fd = ::socket( AF_INET, type, 0 );
        // LCOV_EXCL_BR_START -- Defensive programming, OS specific info required to reliably induce failures due to FD 
        //                       exhaustion.
        if( fd < 0 ) { return CURRENT_SYSTEM_ERROR_CODE(); }
        // LCOV_EXCL_BR_STOP

        setFd( fd );
    }

    if( ::connect( getFd(), reinterpret_cast<sockaddr const *>(&mSockAddrIn), sizeof(mSockAddrIn) ) < 0 ) {
        
        if( errno == EINPROGRESS ) {
            return false;
        }

        return CURRENT_SYSTEM_ERROR_CODE();
    }

    setConnected( true );

    return true;
}


/**
 * @brief Sets the TCP client params -- Only allowed if the socket file descriptor has not been set yet
 * @param pParams TCP client connection params
 */
inline ErrorHandling::ReturnCode LinuxTCPClientSocket::setTCPClientParams( TCPClientParams const & pParams ) {
    
    if( isFdValid() ) {
        return EG_ERROR_CODE( Utils::DeviceErrors, must_be_closed ); 
    }

    mSockAddrIn.sin_family      = AF_INET;
    mSockAddrIn.sin_port        = ::htons( pParams.ipAndPort.port );
    mSockAddrIn.sin_addr.s_addr = ::htonl( pParams.ipAndPort.addr.asInteger() );
    
    mNonBlocking = pParams.nonBlocking;

    return {};
}

}

#endif
