///
/// @file
/// @brief Defines a message client based on datagram sockets.
///
#ifndef ENCORE_OS_DgramSocketClient_HPP
#define ENCORE_OS_DgramSocketClient_HPP

#include <cstdint>

#include "encore/Encore.hpp"
#include "encore/ErrorHandling/Result.hpp"
#include "encore/Messaging/IMessageClient.hpp"
#include "encore/Utils/Span.hpp"

#include "encore/OS/Common/IDgramSocket.hpp"

namespace Encore::OS {

/**
 * @brief   Messaging client implementation for datagram sockets.
 * @details Messages are directly written to and read from the datagram socket.
 */
class DgramSocketClient : public Messaging::IMessageClient {
public: /*IMessageClient*/

    /// @brief No-op init
    ErrorHandling::ReturnCode init( InitOnly const & ) final { return {}; }

    ErrorHandling::Result<std::size_t> sendMsg( Utils::BytesView const & pView ) final;

    ErrorHandling::Result<std::size_t> recvMsg( Utils::BytesRange pBytes ) final;

public:

    DgramSocketClient( IDgramSocket & pSocket );

private:

    IDgramSocket & mSocket;
};

/////////////////
// Definitions //
/////////////////

/**
 * @brief   Sends bytes over the datagram socket
 * @param   pView   Bytes to send via the socket
 * @return  Number of bytes sent
 */
inline ErrorHandling::Result<std::size_t> DgramSocketClient::sendMsg( Utils::BytesView const & pView ) {
    return mSocket.write( pView );
}


/**
 * @brief   Tries to receive a message from the datagram socket.
 * @param   pBytes  Bytes to write received messages to
 * @return  Number of bytes received, 0 if nothing was received
 */
inline ErrorHandling::Result<std::size_t> DgramSocketClient::recvMsg( Utils::BytesRange pBytes ) {

    if( pBytes.size() == 0 ) {
        return EG_ERROR_CODE( Messaging::MessageClientErrors, destination_range_too_small );
    }

    return mSocket.read( pBytes );
}


/**
 * @brief   Creates a DgramSocketClient from an input datagram socket
 * @param   pSocket Reference to a datagram socket
 */
inline DgramSocketClient::DgramSocketClient( IDgramSocket & pSocket )
: mSocket{pSocket}
{}

}

#endif

