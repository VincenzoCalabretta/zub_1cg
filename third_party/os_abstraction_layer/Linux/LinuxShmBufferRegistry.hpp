
///
/// @file
/// @brief A BaseBufferRegistry in a Linux shared memory setting
///
#ifndef ENCORE_SHM_LinuxShmBufferRegistry_HPP
#define ENCORE_SHM_LinuxShmBufferRegistry_HPP

#include <cstdint>
#include <array>

#include "encore/ErrorHandling/Result.hpp"
#include "encore/Utils/OffsetPtr.hpp"
#include "encore/DataModel/BaseBufferRegistry.hpp"

#include "encore/OS/OS.hpp"
#include "encore/OS/Linux/LinuxShmRegion.hpp"


namespace Encore::OS {

/**
 * @brief Implementation of BaseBufferRegistry for linux shared memory usage
 *
 * @tparam tNumHandles The max number of handles for the HR
 * @tparam tBufferSize The byte size of the internal buffer on which DataField values will reside
 * @tparam tLookup The "quick hash" lookup policy for register/getRead quieries
 * @tparam tAlign The alignment policy of the HR
 *
 * @details Intended for use across multiple memory space (eg. multiple Linux processes)
 */
template < std::uint32_t tNumHandles
         , std::uint32_t tBufferSize = 8*tNumHandles
         , class tLookup = DataModel::TwoByteQuickLookup
         , class tAlign = DataModel::TypeSizeAlignmentPolicy
         >
class LinuxShmBufferRegistry final : public DataModel::BaseBufferRegistry<tNumHandles,tLookup,Utils::OffsetPtr,tAlign> {

    typedef typename DataModel::BaseBufferRegistry<tNumHandles,tLookup,Utils::OffsetPtr,tAlign>::Registry Registry;

    static_assert( tBufferSize >= 8*tNumHandles, "Buffer must support tNumHandles 64-bit fields" );

    struct Data {
        std::array<EncoreByte,tBufferSize> buffer{};
        DataModel::DataFieldBufferCoordinator<tNumHandles,tLookup,Utils::OffsetPtr,tAlign> hr;
    };

public:

    /**
     * @brief Constructor
     *
     * @param pInit INIT_ONLY specifier
     * @param pNotTS NOT_THREADSAFE specifier (constructs underlying shm region)
     * @param pCreateRegion A boolean [true] to create region, [false] to just map existing region
     */
    LinuxShmBufferRegistry(InitOnly const & pInit, NotThreadSafe pNotTS, bool pCreateRegion)
    : mShmRegion( pInit, pNotTS, "EncoreShmBufferRegistry", pCreateRegion )
    {
        auto code = registry().setExternalBuffer( pInit, regionData().buffer );
        //LCOV_EXCL_BR_START -- defensive programming, should not happen via static assertion and DFBC logic
        if( not code.success() ) { throw std::logic_error("Improper setExternalBuffer() in LinuxShmBufferRegistry"); }
        //LCOV_EXCL_BR_STOP
    }

private:

    Data       & regionData()       { return *mShmRegion.data(); }
    Data const & regionData() const { return *mShmRegion.data(); }

    Registry       & registry()       final { return regionData().hr; }
    Registry const & registry() const final { return regionData().hr; }

private:

    LinuxShmRegion<Data> mShmRegion;
};

}

#endif
