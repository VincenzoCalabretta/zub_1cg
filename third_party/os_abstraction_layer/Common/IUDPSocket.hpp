
///
/// @file
/// @brief Defines interface to a UDP socket
///
#ifndef ENCORE_OS_NETWORK_IUDPSocket_HPP
#define ENCORE_OS_NETWORK_IUDPSocket_HPP

#include <cstdint>

#include "encore/ErrorHandling/Result.hpp"
#include "encore/ErrorHandling/StdFswErrors.hpp"
#include "encore/Utils/Maybe.hpp"
#include "encore/Utils/Span.hpp"

#include "encore/OS/Common/Network.hpp"
#include "encore/OS/Common/IPv4Address.hpp"
#include "encore/OS/Common/IDgramSocket.hpp"

namespace Encore::OS {


/**
 * @brief Enumerates the usage modes for UDP sockets
 */
enum class UDPSocketMode : std::uint8_t {
    read_write = 1, ///<@brief Socket is bound to port to read data, can send to any destination
    write_only = 2, ///<@brief Socket not bound to port, can only send to destinations
};


/**
 * @brief Aggregate containing info from a IUDPSocket::recvFrom()
 */
struct RecvFromInfo {
    IPAndPort ipp;           ///<@brief The source IP/port
    std::size_t numBytes{0}; ///<@brief The number of bytes received
};


/**
 * @brief An IDgramSocket for IP/UDP
 */
class IUDPSocket : public IDgramSocket {
public:

    ///@brief Send bytes to a particular destination
    virtual ErrorHandling::Result<std::size_t> sendTo( Utils::BytesView const &, IPAndPort const & ) =0;

    ///@brief Try receiving bytes and notify of source/origin
    virtual ErrorHandling::Result<Utils::Maybe<RecvFromInfo>> recvFrom( Utils::BytesRange ) =0;
};


/**
 * @brief A no-op UDP Socket
 */
class NoOpUDPSocket final : public IUDPSocket {
public: /*IUDPSocket*/

    ///@brief Trivially opens or returns error based on construction
    ErrorHandling::Result<bool> open() noexcept final {
        if( mAllowOpen ) { mIsOpen = true; return true; }
        return EG_ERROR_CODE( ErrorHandling::StdFswErrors, logic_error );
    }

    ///@brief Returns if open[TRUE] or not[FALSE]
    bool isOpen() const noexcept final { return mIsOpen; }

    ///@brief Trivially returns size of input BytesView -- "every write is successful" @param pBytes Bytes to write
    ErrorHandling::Result<std::size_t> write( Utils::BytesView const & pBytes ) noexcept final { return pBytes.size(); }

    ///@brief Trivially returns size of input BytesView @param pBytes Bytes to send
    ErrorHandling::Result<std::size_t> sendTo( Utils::BytesView const & pBytes, IPAndPort const & ) noexcept final {
        return write( pBytes );
    }

    ///@brief Trivially returns 0 -- "reads nothing"
    ErrorHandling::Result<std::size_t> read( Utils::BytesRange ) noexcept final { return 0; }

    ///@brief Trivially returns an empty -- "reads nothing"
    ErrorHandling::Result<Utils::Maybe<RecvFromInfo>> recvFrom( Utils::BytesRange ) noexcept final {
        return Utils::Maybe<RecvFromInfo>();
    }

    ///@brief Trivially opens
    ErrorHandling::Result<bool> close() noexcept final { mIsOpen = false; return true; }

public:

    ///@brief Constructor @param pAllowOpen Whether to allow "quiet" no-op behavior or an error upon open
    NoOpUDPSocket( bool pAllowOpen = true ) : mAllowOpen{pAllowOpen} {}

private:

    bool mAllowOpen{true};
    bool mIsOpen{false};
};


}

#endif
