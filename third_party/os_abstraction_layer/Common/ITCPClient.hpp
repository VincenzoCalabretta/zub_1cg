
///
/// @file
/// @brief Defines a tcp client interface
///
#ifndef ENCORE_OS_ITCPClient_HPP
#define ENCORE_OS_ITCPClient_HPP

#include "encore/Encore.hpp"
#include "encore/ErrorHandling/Result.hpp"

#include "encore/OS/Common/IPv4Address.hpp"
#include "encore/OS/Common/TCP.hpp"

namespace Encore::OS {

/**
 * @brief Defines interface for objects that can be configured with TCPClientParams 
 */
class ITCPClient {
public:

    /// @brief Default destructor
    virtual ~ITCPClient() = default;

    /// @brief Applies the provided TCPClientParams 
    virtual ErrorHandling::ReturnCode setTCPClientParams( TCPClientParams const & ) =0;
};

}

#endif
