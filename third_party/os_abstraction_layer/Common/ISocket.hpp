
///
/// @file
/// @brief Defines interface to a Socket
///
#ifndef ENCORE_OS_NETWORK_ISocket_HPP
#define ENCORE_OS_NETWORK_ISocket_HPP

#include "encore/Utils/IDevice.hpp"

#include "encore/OS/Common/Network.hpp"

namespace Encore::OS {

/**
 * @brief Defines interface to a socket -- the ISocket is just a Utils::IDevice
 */
typedef Utils::IDevice ISocket;


/**
 * @brief A no-op ISocket implementation -- just a Utils::NoOpDevice
 */
typedef Utils::NoOpDevice<ISocket> NoOpSocket;

}

#endif
