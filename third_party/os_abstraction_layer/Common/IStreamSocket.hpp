
///
/// @file
/// @brief Defines interface to a stream socket
///
#ifndef ENCORE_OS_NETWORK_IStreamSocket_HPP
#define ENCORE_OS_NETWORK_IStreamSocket_HPP

#include "encore/ErrorHandling/Result.hpp"
#include "encore/ErrorHandling/ResultTraits.hpp"
#include "encore/Utils/IDevice.hpp"

#include "encore/OS/Common/ISocket.hpp"
#include "encore/OS/Common/Network.hpp"

namespace Encore::OS {

/**
 * @brief A distinct ISocket for type disambiguation
 *
 * @details Children of IStreamSocket can introduce family/protocol-specific methods
 */
class IStreamSocket : public ISocket {};


/// @brief Useful typedef for a no-op stream socket
typedef Utils::NoOpDevice<IStreamSocket> NoOpStreamSocket;

}


namespace Encore::ErrorHandling::ResultTraits {

///@brief Specialization of trait for IStreamSocket
template <>
struct AbstractSelector<OS::IStreamSocket> {
    using type = Utils::NoOpDevice<OS::IStreamSocket>; ///<@brief Required alias
};

}

#endif
