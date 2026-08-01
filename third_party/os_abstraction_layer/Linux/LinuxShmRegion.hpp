
///
/// @file
/// @brief RAII construct for a "typed" shm segment in linux environment
///
#ifndef ENCORE_SHM_LinuxShmRegion_HPP
#define ENCORE_SHM_LinuxShmRegion_HPP

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <system_error>

#include <cstdint>

#include <stdexcept>
#include <string>
#include <type_traits>

#include "encore/OS/OS.hpp"
#include "encore/OS/Linux/Linux.hpp"

#include "encore/Logging/ILogRoutable.hpp"


namespace Encore::OS {

/**
 * @brief Constructs a shm region containing a specific typed data structure
 *
 * @tparam T The type of data to placement-new construct in the shared memory region
 *
 * @details Class is neither copy nor move constructible/assignable
 */
template <class T>
class LinuxShmRegion final : public Logging::LogRoutable {

    static_assert( IsSharedMemCompatible<T>::value, "ShmRegion type must be default constructible" );

public:

    /**
     * @brief Constructor
     *
     * @param pInit INIT_ONLY behavior
     * @param pNotTS NOT_THREADSAFE specifier
     * @param pRegionName The name of shared data region to create/map to
     * @param pCreateRegion A boolean indicating whether to create a new region
     */
    LinuxShmRegion( [[maybe_unused]] InitOnly const & pInit
                  , [[maybe_unused]] NotThreadSafe pNotTS
                  , std::string const & pRegionName
                  , bool pCreateRegion
                  )
    : mRegionName{ pRegionName }
    , mShmDataSize{ sizeof(T) }
    , mOwnsShm{pCreateRegion}
    {

        std::int32_t flags = mOwnsShm ? O_RDWR | O_CREAT | O_EXCL : O_RDWR;
        mode_t mode = S_IRUSR | S_IWUSR;

        mShmFd = ::shm_open( mRegionName.data(), flags, mode );

        if( mShmFd < 0 ) {
            std::error_code ec(errno, std::generic_category());
            throw std::runtime_error( ec.message() );
        }

        struct stat memStat{};
        std::int32_t ret = ::fstat( mShmFd, &memStat );

        if( ret < 0 ) {
            std::error_code ec(errno, std::generic_category());
            throw std::runtime_error( ec.message() );
        }

        if( static_cast<std::uint64_t>(memStat.st_size) < mShmDataSize ) {

            ret = ::ftruncate( mShmFd, static_cast<off_t>(mShmDataSize) );

            if( ret < 0 ) {
                std::error_code ec(errno, std::generic_category());
                throw std::runtime_error( ec.message() );
            }
        }
        else {

            mShmDataSize = static_cast<std::uint64_t>( memStat.st_size );
        }

        mShmPtr = static_cast<char*>( ::mmap( nullptr, mShmDataSize, PROT_READ | PROT_WRITE, MAP_SHARED, mShmFd, 0 ) );

        if( mOwnsShm ) {

            new (mShmPtr) T();
        }
    }

    /**
     * @brief Destructor
     */
    ~LinuxShmRegion() override {

        if( mShmFd > 0 ) {

            std::int32_t ret = ::munmap( mShmPtr, mShmDataSize );

            if( ret < 0 ) {
                logger().log( "Failure to unmap shm region...", Logging::Severity::debug );
            }

            ret = ::close( mShmFd );

            if( ret < 0 ) {
                logger().log( "Failure to close shm region file descriptor...", Logging::Severity::debug );
            }
        }

        if( mOwnsShm ) {

            std::int32_t ret = ::shm_unlink( mRegionName.data() );

            if( ret < 0 ) {
                logger().log( "Failure to unlink shm region...", Logging::Severity::debug );
            }
        }
    }

    LinuxShmRegion( LinuxShmRegion const & ) = delete;
    LinuxShmRegion( LinuxShmRegion && )      = delete;

    LinuxShmRegion & operator=( LinuxShmRegion const & ) = delete;
    LinuxShmRegion & operator=( LinuxShmRegion && )      = delete;

    ///@brief Retrieves the pointer to the shared memory region
    T * data() { return static_cast<T *>( static_cast<void *>( mShmPtr ) ); }

    ///@brief Retrieves a const pointer to the shared memory region
    T const * data() const { return static_cast<T const *>( static_cast<void const *>( mShmPtr ) ); }

    ///@brief Releases a region from ownership (destruction duties)
    void release() noexcept { mOwnsShm = false; }

    ///@brief Endows a region with ownership (destruction duties)
    void inherit() noexcept { mOwnsShm = true; }

private:

    std::string  mRegionName;
    std::size_t  mShmDataSize;
    char *       mShmPtr;
    std::int32_t mShmFd{-1};
    bool         mOwnsShm;
};

}


#endif
