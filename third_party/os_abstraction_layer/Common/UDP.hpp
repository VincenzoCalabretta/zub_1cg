
///
/// @file
/// @brief Defines interfaces to UDP/IP encore utils
///
#ifndef ENCORE_OS_UDP_HPP
#define ENCORE_OS_UDP_HPP

#include <memory>

#include "encore/ErrorHandling/ResultTraits.hpp"

#include "encore/OS/Common/Network.hpp"
#include "encore/OS/Common/IUDPSocket.hpp"
#include "encore/OS/Common/IPv4Address.hpp"


namespace Encore::OS {

/**
 * @brief Parameters used for UDP socket creation
 */
struct UDPSocketParams {
    IPAndPort     ipAndPort{};                     ///<@brief IP/Port that the socket should bind to
    UDPSocketMode mode{UDPSocketMode::write_only}; ///<@brief Mode of socket
};


/**
 * @brief Interface for factory that can create UDP sockets
 */
class IUDPSocketFactory {
public:

    ///@brief Default destructor
    virtual ~IUDPSocketFactory() = default;

    ///@brief Creates a UDP socket with the provided params
    virtual std::unique_ptr<IUDPSocket> create( InitOnly const &, UDPSocketParams const & ) =0;
};


/**
 * @brief A factory that can create no op UDPSockets
 */
class NoOpUDPSocketFactory final : public IUDPSocketFactory {
public: /*IUDPSocketFactory*/

    ///@brief Creates NoOpUDPSockets
    std::unique_ptr<IUDPSocket> create( InitOnly const &, UDPSocketParams const & ) final {
        return std::make_unique<NoOpUDPSocket>(mQuietNoOpSockets);
    }

public:

    ///@brief Constructor @param pQuiet Whether[TRUE] or not[FALSE] to allow the no op sockets to be opened
    NoOpUDPSocketFactory( bool pQuiet = true ) : mQuietNoOpSockets{pQuiet} {}

private:

    bool mQuietNoOpSockets{true};
};

}


namespace Encore::ErrorHandling::ResultTraits {

///@brief Specialization of trait for IUDPSocketFactory
template <>
struct AbstractSelector<OS::IUDPSocketFactory> {
    using type = OS::NoOpUDPSocketFactory; ///<@brief Required alias
};

}

#endif
