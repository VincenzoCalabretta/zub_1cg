
///
/// @file
/// @brief Defines s Linux TCP/IP server class
///
#ifndef ENCORE_OS_LinuxTCPServer_HPP
#define ENCORE_OS_LinuxTCPServer_HPP

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include <cstdint>

#include <algorithm>
#include <array>
#include <utility>

#include "encore/ErrorHandling/Result.hpp"
#include "encore/ErrorHandling/StdFswErrors.hpp"
#include "encore/ErrorHandling/SystemErrors.hpp"
#include "encore/Utils/Span.hpp"

#include "encore/OS/Common/Network.hpp"
#include "encore/OS/Common/TCP.hpp"
#include "encore/OS/Linux/Linux.hpp"
#include "encore/OS/Linux/LinuxTCPServerSocket.hpp"

namespace Encore::OS {

/**
 * @brief Parameterization block for linux/posix TCP server
 */
struct LinuxTCPServerParams {
    NetworkPort port{0}; ///<@brief The desired port on which to monitor connection requests
    int queueDepth{10}; ///<@brief The max number of clients to simultaneously handle on the listening socket
    bool noDelay{false}; ///<@brief  Whether[TRUE] or not[FALSE] to disable Nagle algorithm (send frames ASAP)
    bool listenBlock{false}; ///<@brief Whether[TRUE] or not[FALSE] to make listening socket blocking
};


/**
 * @brief Implementation of ITCPServer for Linux OS that uses spans
 */
class LinuxTCPServer final : public ITCPServer {
public: /*ITCPServer*/

    ErrorHandling::ReturnCode start() noexcept final;

    ErrorHandling::ReturnCode stop() noexcept final;

    ErrorHandling::Result<TCPSocketEndpoint> accept() final;

    void release( IStreamSocket & ) final;

public:

    LinuxTCPServer( Utils::Span<std::pair<LinuxTCPServerSocket, bool>>
                  , LinuxTCPServerParams const & pParams = {}
                  ) noexcept;

    NetworkPort getPort() const noexcept;

private:

    Utils::Span<std::pair<LinuxTCPServerSocket, bool>> mSockets;
    sockaddr_in mServAddrIn{};
    int mListeningFd{-1};
    int mQueueDepth{0};
    NetworkPort mPort{0};
    bool mNoDelay{false};
    bool mListenBlock{false};
};


/// @brief Params struct for FixedLinuxTCPServer
typedef LinuxTCPServerParams FixedLinuxTCPServerParams;


/**
 * @brief Wrapper around LinuxTCPServer that utilizes std::array
 * @tparam tMaxServerSockets Maximum number of server sockets that can be managed at once
 */
template <std::size_t tMaxServerSockets>
class FixedLinuxTCPServer final : public ITCPServer {
public: /*ITCPServer*/

    ErrorHandling::ReturnCode start() noexcept final;

    ErrorHandling::ReturnCode stop() noexcept final;

    ErrorHandling::Result<TCPSocketEndpoint> accept() final;

    void release( IStreamSocket & pSocket ) final;

public:

    FixedLinuxTCPServer( FixedLinuxTCPServerParams const & pParams = {} ) noexcept;

    NetworkPort getPort() const noexcept;

private:

    std::array<std::pair<LinuxTCPServerSocket, bool>, tMaxServerSockets> mSockets{};
    LinuxTCPServer mImpl;
};

/////////////////
// Definitions //
/////////////////

/// @brief Starts the server and binds on the configured port
inline ErrorHandling::ReturnCode LinuxTCPServer::start() noexcept {

    auto socketType = mListenBlock ? SOCK_STREAM : SOCK_STREAM | SOCK_NONBLOCK;

    mListeningFd = ::socket( AF_INET, socketType, 0 );
    //LCOV_EXCL_BR_START -- Defensive programming, only way to fail is via file descriptor exhaustion
    if( mListeningFd < 0 ) { return CURRENT_SYSTEM_ERROR_CODE(); }
    //LCOV_EXCL_BR_STOP

    int const opt{1};

    //LCOV_EXCL_START -- Defensive programming, not possible to induce failure due to our arguments
    if( ::setsockopt( mListeningFd, SOL_SOCKET, SO_REUSEADDR , &opt, sizeof(opt) ) < 0 ) {
        return CURRENT_SYSTEM_ERROR_CODE();
    }
    //LCOV_EXCL_STOP

    if( mNoDelay ) {
        //LCOV_EXCL_START -- Defensive programming, not possible to induce failure due to our arguments
        if( ::setsockopt( mListeningFd, IPPROTO_IP, TCP_NODELAY, &opt, sizeof(opt) ) ) {
            return CURRENT_SYSTEM_ERROR_CODE();
        }
        //LCOV_EXCL_STOP
    }

    if( ::bind( mListeningFd, reinterpret_cast<sockaddr const *>(&mServAddrIn)
              , sizeof(mServAddrIn) ) < 0 ) { return CURRENT_SYSTEM_ERROR_CODE(); }

    socklen_t len = static_cast<socklen_t>( sizeof(mServAddrIn) );

    //LCOV_EXCL_BR_START -- Only way to fail is via OS memory exhaustion given our parameters
    if( ::getsockname( mListeningFd, reinterpret_cast<sockaddr *>(&mServAddrIn)
                     , &len ) < 0 ) { return CURRENT_SYSTEM_ERROR_CODE(); }
    //LCOV_EXCL_BR_STOP

    //LCOV_EXCL_BR_START -- Defensive programming, not possible to hit based on previous calls in this method
    if( ::listen( mListeningFd, mQueueDepth ) < 0 ) { return CURRENT_SYSTEM_ERROR_CODE(); }
    //LCOV_EXCL_BR_STOP

    mPort = NetworkPort{ ::ntohs( mServAddrIn.sin_port ) };

    return {};
}


/// @brief Stops the server and closes the listening socket
inline ErrorHandling::ReturnCode LinuxTCPServer::stop() noexcept {

    auto res = ::close( mListeningFd );
    if( res != 0 ) { return CURRENT_SYSTEM_ERROR_CODE(); }

    return {};
}


/**
 * @brief Accepts a connection request
 * @return TCPSocketEndpoint with a pointer to the opened socket and the IP/Port of the client
 */
inline ErrorHandling::Result<TCPSocketEndpoint> LinuxTCPServer::accept() {

    auto it = std::find_if( mSockets.begin(), mSockets.end(), [](auto && pElem) -> bool {
        return not pElem.second;
    } );

    if( it == mSockets.end() ) {
        return EG_ERROR_CODE( ErrorHandling::StdFswErrors, length_error );
    }

    auto & [socket, inUse] = *it;

    sockaddr_in clientAddr{};
    socklen_t len = static_cast<socklen_t>( sizeof(clientAddr) );

    int fd = ::accept4( mListeningFd, reinterpret_cast<sockaddr *>(&clientAddr), &len, SOCK_NONBLOCK );
    if( fd < 0 ) {

        //LCOV_EXCL_BR_START -- Not possible to reliably induce non-EAGAIN accept4 failures in this context without
        //                      mocking linux library calls
        if( errno == EAGAIN ) { return kNullTCPSocketEndpoint; }
        //LCOV_EXCL_BR_STOP

        return CURRENT_SYSTEM_ERROR_CODE();
    }

    inUse = true;
    socket.reset( fd );

    IPAndPort ep;
    ep.addr = IPv4Address( ::ntohl( clientAddr.sin_addr.s_addr ) );
    ep.port = NetworkPort{ ::ntohs( clientAddr.sin_port ) };

    return TCPSocketEndpoint{ ep, &socket };
}

/**
 * @brief Closes the provided socket and makes it available for recycling
 * @details If the provided socket is not managed by this class, the behavior is no-op
 * @param pSocket Socket to release
 */
inline void LinuxTCPServer::release( IStreamSocket & pSocket ) {

    auto it = std::find_if( mSockets.begin(), mSockets.end()
                          , [&pSocket](auto && pEntry){ return &pEntry.first == &pSocket; }
                          );

    if( it != mSockets.end() ) {
        auto & [socket, inUse] = *it;
        std::ignore = socket.close();
        inUse = false;
    }
}


/**
 * @brief Constructor from the params struct and a span of LinuxTCPServerSocket-bool pairs
 * @param pSockets Span of LinuxTCPServer-bool pairs -- The bools indicate if the associated socket is in use
 * @param pParams Parameters used for construction
 */
inline LinuxTCPServer::LinuxTCPServer( Utils::Span<std::pair<LinuxTCPServerSocket, bool>> pSockets
                                     , LinuxTCPServerParams const & pParams
                                     ) noexcept
: mSockets{ pSockets }
, mQueueDepth{ pParams.queueDepth }
, mPort{ pParams.port }
, mNoDelay{ pParams.noDelay }
, mListenBlock{ pParams.listenBlock }
{
    mServAddrIn.sin_family      = AF_INET;
    mServAddrIn.sin_port        = ::htons(pParams.port);
    mServAddrIn.sin_addr.s_addr = ::htonl( INADDR_ANY );
}


///@brief Returns port number
inline NetworkPort LinuxTCPServer::getPort() const noexcept { return mPort; }

/**
 * @brief Constructor
 * @param pParams Parameters struct
 */
template <std::size_t tMaxServerSockets>
inline FixedLinuxTCPServer<tMaxServerSockets>::FixedLinuxTCPServer( FixedLinuxTCPServerParams const & pParams ) noexcept
: mImpl{ mSockets, pParams }
{}


/// @copydoc LinuxTCPServer::start()
template <std::size_t tMaxServerSockets>
inline ErrorHandling::ReturnCode FixedLinuxTCPServer<tMaxServerSockets>::start() noexcept {
    return mImpl.start();
}


/// @copydoc LinuxTCPServer::stop()
template <std::size_t tMaxServerSockets>
inline ErrorHandling::ReturnCode FixedLinuxTCPServer<tMaxServerSockets>::stop() noexcept {
    return mImpl.stop();
}


/// @copydoc LinuxTCPServer::accept()
template <std::size_t tMaxServerSockets>
inline ErrorHandling::Result<TCPSocketEndpoint> FixedLinuxTCPServer<tMaxServerSockets>::accept() {
    return mImpl.accept();
}


/// @copydoc LinuxTCPServer::release()
template <std::size_t tMaxServerSockets>
inline void FixedLinuxTCPServer<tMaxServerSockets>::release( IStreamSocket & pSocket ) {
    return mImpl.release( pSocket );
}


/// @copydoc LinuxTCPServer::getPort()
template <std::size_t tMaxServerSockets>
inline NetworkPort FixedLinuxTCPServer<tMaxServerSockets>::getPort() const noexcept {
    return mImpl.getPort();
}

}

#endif
