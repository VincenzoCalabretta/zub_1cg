
///
/// @file
/// @brief Defines linux TCP server socket implementation
///
#ifndef ENCORE_OS_LinuxTCPServerSocket_HPP
#define ENCORE_OS_LinuxTCPServerSocket_HPP

#include <utility>

#include "encore/OS/Linux/BaseLinuxTCPSocket.hpp"

namespace Encore::OS {

/**
 * @brief A TCP server socket for use with the LinuxTCPServer
 * @details The "server end" of a client-server TCP connection -- a TCP server listening socket creates this
 *          via the accept() call
 */
class LinuxTCPServerSocket final : public BaseLinuxTCPSocket {
public: /*ISocket*/

    ///@brief Trivial open - socket was constructed as open
    ErrorHandling::Result<bool> open() noexcept final { return true; }

    void reset( int );

public:

    using LinuxDescriptable::setFd;

    LinuxTCPServerSocket() = default;
};

/////////////////
// Definitions //
/////////////////

/**
 * @brief Resets the file descriptor and closes existing FD if necessary
 * @param pFd File descriptor
 */
inline void LinuxTCPServerSocket::reset( int pFd ) {

    {
        Logging::ScopedSeverityGuard sev{ logger(), Logging::Severity::silence };
        std::ignore = close();
    }

    setConnected( true );

    setFd( pFd );
}

}

#endif
