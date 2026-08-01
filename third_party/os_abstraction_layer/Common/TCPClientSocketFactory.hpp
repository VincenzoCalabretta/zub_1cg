
///
/// @file
/// @brief Defines span and array based TCP client socket factory implementations
///
#ifndef ENCORE_OS_TCPClientSocketFactory_HPP
#define ENCORE_OS_TCPClientSocketFactory_HPP

#include <algorithm>
#include <array>
#include <type_traits>
#include <utility>

#include "encore/Encore.hpp"
#include "encore/ErrorHandling/ErrorGroup.hpp"
#include "encore/ErrorHandling/Result.hpp"
#include "encore/ErrorHandling/StdFswErrors.hpp"
#include "encore/Utils/Span.hpp"
#include "encore/Logging/ILogRoutable.hpp"
#include "encore/Logging/FileLineHelper.hpp"

#include "encore/OS/Common/IPv4Address.hpp"
#include "encore/OS/Common/ISocket.hpp"
#include "encore/OS/Common/ITCPClient.hpp"
#include "encore/OS/Common/IStreamSocket.hpp"
#include "encore/OS/Common/TCP.hpp"

namespace Encore::OS {

/**
 * @brief TCP client socket factory that uses spans for storage/management
 * @tparam tSocket TCP socket type
 */
template <class tSocket>
class TCPClientSocketFactory final : public ITCPClientSocketFactory, public Logging::LogRoutable {

    static_assert( std::is_base_of_v<ITCPClient, tSocket>
               and std::is_base_of_v<IStreamSocket, tSocket>
               and std::is_nothrow_default_constructible_v<tSocket>
                 , "Socket must be an IStreamSocket, an ITCPClient, and must be default constructible" );

public: /*ITCPClientSocketFactory*/

    ErrorHandling::Result<IStreamSocket *> create( TCPClientParams const & ) final;

    void destroy( IStreamSocket & ) final;

public:

    TCPClientSocketFactory( Utils::Span<std::pair<tSocket, bool>> );

private:

    Utils::Span<std::pair<tSocket, bool>> mSockets;
};


/**
 * @brief Wrapper around TCPClientSocketFactory that uses a std::array for storage/management
 * @tparam tSocket TCP socket type
 * @tparam tMaxNumClients Maximum number of clients that can be managed by this class
 */
template <class tSocket, std::size_t tMaxNumClients>
class FixedTCPClientSocketFactory final : public ITCPClientSocketFactory, public Logging::ILogRoutable {
public: /*ITCPClientSocketFactory*/

    ErrorHandling::Result<IStreamSocket *> create( TCPClientParams const & ) final;

    void destroy( IStreamSocket & ) final;

public:

    FixedTCPClientSocketFactory();

    Logging::Logger & logger() const noexcept final;

    void routeLoggingTo( Logging::Logger & ) noexcept final;

private:

    std::array<std::pair<tSocket, bool>, tMaxNumClients> mSockets{};
    TCPClientSocketFactory<tSocket> mFactory;
};

//////////////////
// Definitions //
////////////////

/**
 * @brief Constructor from a span of socket-boolean pairs
 * @param pSockets Span of socket-bool pairs -- The bools indicate if the associated socket is in use
 */
template <class tSocket>
inline TCPClientSocketFactory<tSocket>::TCPClientSocketFactory( Utils::Span<std::pair<tSocket, bool>> pSockets )
: mSockets{ pSockets }
{}


/**
 * @brief Creates a new TCP socket from the provided params
 * @param pParams Params used for TCP socket construction
 * @return Pointer to the constructed socket
 */
template <class tSocket>
inline ErrorHandling::Result<IStreamSocket *> TCPClientSocketFactory<tSocket>::create( TCPClientParams const & pParams ) {

    auto it = std::find_if( mSockets.begin(), mSockets.end(), [](auto && pElem) -> bool {
        return not pElem.second;
    } );

    if( it == mSockets.end() ) {
        return EG_ERROR_CODE( ErrorHandling::StdFswErrors, length_error );
    }

    auto & [socket, inUse] = *it;

    socket = {};

    auto params = socket.setTCPClientParams( pParams );
    if( not params.success() ) { return TRACE_CODE( logger(), params.error() ); }

    auto opened = socket.open();
    if( not opened.success() ) { return TRACE_CODE( logger(), opened.error() ); }

    inUse = true;

    return &socket;
}


/**
 * @brief Destroys the provided socket if it exists
 * @param pSocket Socket to destroy
 */
template <class tSocket>
inline void TCPClientSocketFactory<tSocket>::destroy( IStreamSocket & pSocket ) {

    auto it = std::find_if( mSockets.begin(), mSockets.end(), [&](auto && pElem) -> bool {
        return &pSocket == &pElem.first;
    } );

    if( it != mSockets.end() ) {
        auto & [socket, inUse] = *it;
        std::ignore = socket.close();
        inUse = false;
    }
}


/// @brief Constructor
template <class tSocket, std::size_t tMaxNumClients>
inline FixedTCPClientSocketFactory<tSocket, tMaxNumClients>::FixedTCPClientSocketFactory()
: mFactory{ Utils::Span{ mSockets } }
{}


/// @copydoc TCPClientSocketFactory::create()
template <class tSocket, std::size_t tMaxNumClients>
inline ErrorHandling::Result<IStreamSocket *>
FixedTCPClientSocketFactory<tSocket, tMaxNumClients>::create( TCPClientParams const & pParams ) {
    return mFactory.create( pParams );
}


/// @copydoc TCPClientSocketFactory::destroy()
template <class tSocket, std::size_t tMaxNumClients>
inline void FixedTCPClientSocketFactory<tSocket, tMaxNumClients>::destroy( IStreamSocket & pSocket ) {
    mFactory.destroy( pSocket );
}


/// @copydoc TCPClientSocketFactory::logger()
template <class tSocket, std::size_t tMaxNumClients>
inline Logging::Logger & FixedTCPClientSocketFactory<tSocket, tMaxNumClients>::logger() const noexcept {
    return mFactory.logger();
}


/// @copydoc TCPClientSocketFactory::routeLoggingTo()
template <class tSocket, std::size_t tMaxNumClients>
inline void FixedTCPClientSocketFactory<tSocket, tMaxNumClients>::routeLoggingTo( Logging::Logger & pLogger ) noexcept {
    mFactory.routeLoggingTo( pLogger );
}

}

#endif
