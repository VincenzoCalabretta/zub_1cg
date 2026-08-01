
///
/// @file
/// @brief Defines interfaces to TCP/IP encore utils
///
#ifndef ENCORE_OS_TCP_HPP
#define ENCORE_OS_TCP_HPP

#include <cstdint>

#include <type_traits>

#include "encore/ErrorHandling/Result.hpp"
#include "encore/ErrorHandling/ErrorGroup.hpp"
#include "encore/ErrorHandling/StdFswErrors.hpp"
#include "encore/OS/Common/Network.hpp"
#include "encore/OS/Common/ISocket.hpp"
#include "encore/OS/Common/IStreamSocket.hpp"
#include "encore/OS/Common/IPv4Address.hpp"


namespace Encore::OS {

/**
 * @brief Parameters used for TCP client connections
 */
struct TCPClientParams {
    IPAndPort ipAndPort{}; ///<@brief IP/Port that the client should connect to
    bool nonBlocking{ false }; ///<@brief If the connection should be non-blocking
};


/**
 * @brief Interface for factory that can create managed TCP sockets
 */
class ITCPClientSocketFactory {
public:

    virtual ~ITCPClientSocketFactory() = default;

    /// @brief Creates a TCP socket with the provided params
    virtual ErrorHandling::Result<IStreamSocket *> create( TCPClientParams const & ) =0;

    /// @brief Destroys the provided TCP socket if is being managed by the factory
    virtual void destroy( IStreamSocket & ) =0;
};


/**
 * @brief TCP client socket factory that returns no-op sockets
 */
class NoOpTCPClientSocketFactory final : public ITCPClientSocketFactory {
public:

    /// @brief Returns a no-op socket
    ErrorHandling::Result<IStreamSocket *> create( TCPClientParams const & ) final {
        return &mSocket;
    }

    /// @brief No-op
    void destroy( IStreamSocket & ) final {}

private:

    NoOpStreamSocket mSocket;
};


/**
 * @brief A structure for use with TCPServers
 */
struct TCPSocketEndpoint {
    IPAndPort  endPoint{}; ///<@brief The IPv4 address/port of the socket
    IStreamSocket *  socket{nullptr}; ///<@brief Pointer to actual TCP socket
};


/**
 * @brief Equality comparison operator for TCPSocketEndpoint
 *
 * @param pX First object
 * @param pY Second object
 */
inline constexpr bool operator==( TCPSocketEndpoint const & pX, TCPSocketEndpoint const & pY ) {
    return pX.endPoint == pY.endPoint and pX.socket == pY.socket;
}

///@brief Useful TCPEndpoint
inline constexpr TCPSocketEndpoint kNullTCPSocketEndpoint = { kNullIP, nullptr };


/**
 * @brief Defines interface to a TCP server
 */
class ITCPServer {
public:

    ///@brief Destructor
    virtual ~ITCPServer() = default;

    ///@brief Starts the server
    virtual ErrorHandling::ReturnCode start() =0;

    /// @brief Stops the server
    virtual ErrorHandling::ReturnCode stop() =0;

    ///@brief Accepts a connection
    virtual ErrorHandling::Result<TCPSocketEndpoint> accept() =0;

    ///@brief Try to close/remove a client socket that is no longer needed
    virtual void release( IStreamSocket & ) =0;
};


/**
 * @brief A safe server that hands out NoOpSockets
 */
class NoOpTCPServer final : public ITCPServer {
public:

    ErrorHandling::ReturnCode start() final;

    ErrorHandling::ReturnCode stop() final { return {}; }

    ErrorHandling::Result<TCPSocketEndpoint> accept() noexcept final;

    void release( IStreamSocket & ) noexcept final {}

public:

    NoOpTCPServer( bool pAllowStart = false, bool pAllowConnect = true );

private:

    NoOpStreamSocket mNoOpSocket;
    bool mAllowStart{false};
    bool mAllowConnect{true};
};

//////////////////
// Definitions //
////////////////

///@brief Starts server - depends on constructor if allowed
inline ErrorHandling::ReturnCode NoOpTCPServer::start() {

    if( not mAllowStart ) {

        auto & eg = ErrorHandling::ErrorGroup<ErrorHandling::StdFswErrors>::getInstance(kThreadSafe);

        return ERROR_CODE( eg, ErrorHandling::StdFswErrors::logic_error );
    }

    return {};
}


/**
 * @brief Accepts a connection
 * @return NoOpSocket with default IP address
 */
inline ErrorHandling::Result<TCPSocketEndpoint> NoOpTCPServer::accept() noexcept {

    if( not mAllowConnect ) {

        auto & eg = ErrorHandling::ErrorGroup<ErrorHandling::StdFswErrors>::getInstance(kThreadSafe);

        return ERROR_CODE( eg, ErrorHandling::StdFswErrors::logic_error );
    }

    return TCPSocketEndpoint{ IPAndPort{}, &mNoOpSocket };
}


/**
 * @brief Constructor
 * @param pAllowStart If start() is allowed [TRUE] or if it should error [FALSE] (Default FALSE)
 * @param pAllowConnect If accept() is allowed [TRUE] or if it should error [FALSE] (Default TRUE)
 */
inline NoOpTCPServer::NoOpTCPServer( bool pAllowStart, bool pAllowConnect )
: mAllowStart{ pAllowStart }
, mAllowConnect{ pAllowConnect }
{}


}

#endif
