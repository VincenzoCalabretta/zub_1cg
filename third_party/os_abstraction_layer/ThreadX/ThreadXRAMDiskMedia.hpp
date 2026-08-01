
///
/// @file
/// @brief RAM-disk FX_MEDIA for FileX bring-up without flash or SD card
///
#ifndef ENCORE_OS_ThreadXRAMDiskMedia_HPP
#define ENCORE_OS_ThreadXRAMDiskMedia_HPP

#include <cstdint>

#include "fx_api.h"

#include "encore/ErrorHandling/StdFswErrors.hpp"
#include "encore/ErrorHandling/ReturnCode.hpp"

// _fx_ram_driver is FileX's built-in RAM-disk driver (declared in fx_api.h
// or its included port header).
extern "C" void _fx_ram_driver(FX_MEDIA *media_ptr);

namespace Encore::OS {

/**
 * @brief Self-contained RAM-disk backed FX_MEDIA, formatted on first init().
 *
 * @tparam tSectorCount  Number of 512-byte sectors.  Default 256 = 128 KB.
 * @tparam tSectorSize   Bytes per sector.  FileX requires power-of-2; 512 is standard.
 * @tparam tCacheSectors Sectors to keep in the media cache (I/O buffer passed to FileX).
 *
 * Usage:
 * @code
 *   ThreadXRAMDiskMedia<256> ram;
 *   ram.init();
 *   ThreadXFile f;
 *   f.setMedia(ram.media());
 * @endcode
 */
template <uint32_t tSectorCount  = 256U,
          uint32_t tSectorSize   = 512U,
          uint32_t tCacheSectors = 4U>
class ThreadXRAMDiskMedia {
public:

    ThreadXRAMDiskMedia() = default;

    /// @brief Format (first call only) and open the RAM disk.
    ErrorHandling::ReturnCode init()
    {
        if (!mFormatted) {
            // fx_media_format() writes the FAT boot sector and directory entries
            // into mDisk[].  Arguments mirror the standard FileX RAM-disk example.
            UINT rc = fx_media_format(
                &mMedia,
                _fx_ram_driver,
                mDisk,          /* driver info = pointer to the RAM buffer    */
                mCache,         /* I/O buffer (cache)                          */
                kCacheBytes,    /* cache size in bytes                         */
                "RAM_DISK",     /* volume label                                */
                1U,             /* number of FATs                              */
                32U,            /* directory entries in root                   */
                0U,             /* hidden sectors                              */
                tSectorCount,   /* total sectors                               */
                tSectorSize,    /* bytes per sector                            */
                0U,             /* sectors per cluster (0 = auto)              */
                1U,             /* heads per cylinder                          */
                1U);            /* sectors per track                           */

            if (rc != FX_SUCCESS) {
                return EG_ERROR_CODE(ErrorHandling::StdFswErrors, logic_error);
            }
            mFormatted = true;
        }

        if (fx_media_open(&mMedia, "RAM_DISK", _fx_ram_driver,
                          mDisk, mCache, kCacheBytes) != FX_SUCCESS) {
            return EG_ERROR_CODE(ErrorHandling::StdFswErrors, logic_error);
        }

        return {};
    }

    /// @brief Returns the FX_MEDIA pointer to pass to ThreadXFile::setMedia().
    FX_MEDIA* media() noexcept { return &mMedia; }

private:
    static constexpr uint32_t kCacheBytes = tSectorSize * tCacheSectors;

    UCHAR    mDisk[tSectorCount * tSectorSize]{};
    UCHAR    mCache[kCacheBytes]{};
    FX_MEDIA mMedia{};
    bool     mFormatted{false};
};

}  // namespace Encore::OS

#endif
