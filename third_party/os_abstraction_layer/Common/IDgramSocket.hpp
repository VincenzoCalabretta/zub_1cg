
///
/// @file
/// @brief Defines interface to a datagram socket
///
#ifndef ENCORE_OS_NETWORK_IDgramSocket_HPP
#define ENCORE_OS_NETWORK_IDgramSocket_HPP

#include "encore/Utils/IDevice.hpp"

#include "encore/OS/Common/ISocket.hpp"
#include "encore/OS/Common/Network.hpp"

namespace Encore::OS {

/**
 * @brief A distinct ISocket for type disambiguation
 *
 * @details Children of IDgramSocket can introduce family/protocol-specific methods
 */
class IDgramSocket : public ISocket {};


/// @brief Useful typedef for a no-op dgram socket
typedef Utils::NoOpDevice<IDgramSocket> NoOpDgramSocket;

}

#endif
