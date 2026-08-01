
///
/// @file
/// @brief Miscellaneous Linux specific implementation information
///
#ifndef ENCORE_OS_Linux_HPP
#define ENCORE_OS_Linux_HPP

#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <cstdint>

#include "encore/ErrorHandling/Result.hpp"
#include "encore/ErrorHandling/SystemErrors.hpp"

#include "encore/OS/OS.hpp"

namespace Encore::OS {

    /**
     * @brief A mix-in for Linux-specific IDevices -- only usable through inheritance
     */
    class LinuxDescriptable {
    public:

        ///@brief Returns file descriptor
        int getFd() const { return mFd; }

        ErrorHandling::Result<bool> poll( Nanoseconds, short = POLLOUT ) const;

    protected:

        ///@brief Destructor
        ~LinuxDescriptable() = default;

        ///@brief Mutator for file descriptor @param pFd The new descriptor value
        void setFd( int pFd ) { mFd = pFd; }

    private:

        int mFd{-1};
    };

    /**
     * @brief Enumerates types of /proc queries
     */
    enum class LinuxProcQueryType : std::uint8_t {
        unknown = 0, ///<@brief Unknown query
        system, ///<@brief System-wide query
        process ///<@brief Per-process query
    };

    ///@brief Trait to identify valid queries
    template <class T, class = void>
    struct IsLinuxProcFsQuery : std::false_type {};

    /**
     * @brief Partial specialization type is a Linux Proc FS Query
     *
     * @tparam T Type to check
     */
    template <class T>
    struct IsLinuxProcFsQuery< T, std::enable_if_t< std::is_same_v< decltype( std::declval<T>().qType() )
                                                                  , LinuxProcQueryType
                                                                  > > > : std::true_type {};

    /////////////////
    // Definitions //
    /////////////////

    /**
     * @brief Polls until the underlying file descriptor is ready or until a timeout occurs
     * @param pTimeout Max timeout duration
     * @param pEvents Event flags to check for when polling
     * @return true if the file descriptor is ready, false if timeout, ErrorCode if an error occurs
     */
    inline ErrorHandling::Result<bool> LinuxDescriptable::poll( Nanoseconds pTimeout, short pEvents ) const {

        pollfd pollInfo{};
        pollInfo.fd = getFd();
        pollInfo.events = pEvents;

        constexpr Nanoseconds kNanosInMilli{ 1'000'000 };

        auto numFds = ::poll( &pollInfo, 1, static_cast<int>( pTimeout / kNanosInMilli ) );
        //LCOV_EXCL_BR_START -- Defensive programming, not possible to induce failure due to our arguments
        if( numFds < 0 ) { return CURRENT_SYSTEM_ERROR_CODE(); }
        //LCOV_EXCL_BR_STOP

        if( numFds == 0 ) {
            return false;
        }

        int val{0};
        socklen_t len = static_cast<socklen_t>( sizeof(val) );
        auto ret = ::getsockopt( getFd(), SOL_SOCKET, SO_ERROR, &val, &len );
        if( ret < 0 ) { return CURRENT_SYSTEM_ERROR_CODE(); }

        //LCOV_EXCL_START -- Very difficult to induce poll failures without mocking out an OS layer
        if( val != 0 ) {
            auto & eg = ErrorHandling::getGroup<ErrorHandling::SystemErrors>();
            return ERROR_CODE( eg, ErrorHandling::SystemErrors::Enum{val} );
        }
        //LCOV_EXCL_STOP

        return true;
    }

    /**
     * @brief Meta-utility for discerning if type is Shared Memory compatible
     *
     * @tparam T the Type to check
     */
    template <class T>
    struct IsSharedMemCompatible : std::is_default_constructible<T> {};

}

#endif
