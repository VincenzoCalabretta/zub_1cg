
///
/// @file
/// @brief Implements IResizableFile using Eclipse FileX
///
#ifndef ENCORE_OS_ThreadXFile_HPP
#define ENCORE_OS_ThreadXFile_HPP

#include <cstddef>
#include <cstring>
#include <string_view>

#include "fx_api.h"

#include "encore/ErrorHandling/StdFswErrors.hpp"
#include "encore/OS/Common/IFile.hpp"
#include "encore/Utils/DeviceErrors.hpp"

namespace Encore::OS {

/**
 * @brief IResizableFile implementation backed by Eclipse FileX.
 *
 * Call setMedia() with a pointer to an open FX_MEDIA before calling open().
 * The media lifetime must exceed the file lifetime.
 *
 * FileOpenMode semantics on FileX:
 *   read_only           → FX_OPEN_FOR_READ
 *   truncate_write_only → FX_OPEN_FOR_WRITE + fx_file_truncate_release to 0
 *   exclusive_write_only→ fx_file_create (fails if exists) + FX_OPEN_FOR_WRITE
 *   append_write_only   → FX_OPEN_FOR_WRITE + seek to end
 *   read_write          → FX_OPEN_FOR_WRITE  (FileX allows reads too)
 *   truncate_read_write → FX_OPEN_FOR_WRITE + fx_file_truncate_release to 0
 *   exclusive_read_write→ fx_file_create + FX_OPEN_FOR_WRITE
 *   append_read_write   → FX_OPEN_FOR_WRITE + seek to end
 *
 * FX_OPEN_FOR_WRITE in FileX permits both read and write access.  Read-only
 * versus write-only restrictions are enforced by mCanRead / mCanWrite.
 */
class ThreadXFile final : public IResizableFile {
public:

    ThreadXFile() = default;

    ~ThreadXFile() { if (isOpen()) { (void)close(); } }

    ThreadXFile(const ThreadXFile&) = delete;
    ThreadXFile& operator=(const ThreadXFile&) = delete;

    /// @brief Provide the open FX_MEDIA this file lives on.  Call before open().
    void setMedia(FX_MEDIA* pMedia) noexcept { mMedia = pMedia; }

    // ── IFile ──────────────────────────────────────────────────────────────

    ErrorHandling::ReturnCode setFileOpenParams(std::string_view pFileName,
                                                FileOpenMode     pFileMode) final
    {
        if (isOpen()) { return EG_ERROR_CODE(Utils::DeviceErrors, must_be_closed); }
        if (pFileName.size() >= FX_MAX_LONG_NAME_LEN) {
            return EG_ERROR_CODE(ErrorHandling::StdFswErrors, invalid_argument);
        }
        std::memcpy(mName, pFileName.data(), pFileName.size());
        mName[pFileName.size()] = '\0';
        mMode = pFileMode;
        return {};
    }

    ErrorHandling::Result<FileInfo> getFileInfo() final
    {
        if (!isOpen()) { return EG_ERROR_CODE(Utils::DeviceErrors, not_open); }
        ULONG size = 0U;
        fx_file_size_get(&mFile, &size);  // returns FX_SUCCESS or benign error
        return FileInfo{ mName, mMode, static_cast<std::size_t>(size) };
    }

    ErrorHandling::Result<std::size_t> getPos() final
    {
        if (!isOpen()) { return EG_ERROR_CODE(Utils::DeviceErrors, not_open); }
        return static_cast<std::size_t>(mFile.fx_file_current_byte_offset);
    }

    ErrorHandling::ReturnCode setPos(std::size_t pPos) final
    {
        if (!isOpen()) { return EG_ERROR_CODE(Utils::DeviceErrors, not_open); }
        if (fx_file_seek(&mFile, static_cast<ULONG>(pPos)) != FX_SUCCESS) {
            return EG_ERROR_CODE(ErrorHandling::StdFswErrors, logic_error);
        }
        return {};
    }

    // ── IResizableFile ──────────────────────────────────────────────────────

    ErrorHandling::ReturnCode resize(std::size_t pSize) final
    {
        if (!isOpen()) { return EG_ERROR_CODE(Utils::DeviceErrors, not_open); }
        if (!mCanWrite) { return EG_ERROR_CODE(Utils::DeviceErrors, not_open); }
        if (fx_file_truncate_release(&mFile, static_cast<ULONG>(pSize)) != FX_SUCCESS) {
            return EG_ERROR_CODE(ErrorHandling::StdFswErrors, logic_error);
        }
        return {};
    }

    // ── IDevice ────────────────────────────────────────────────────────────

    ErrorHandling::Result<bool> open() final
    {
        if (isOpen()) { return true; }
        if (!mMedia) { return EG_ERROR_CODE(ErrorHandling::StdFswErrors, logic_error); }

        return openForMode();
    }

    ErrorHandling::Result<bool> close() noexcept final
    {
        if (!mIsOpen) { return true; }
        fx_file_close(&mFile);
        mIsOpen = false;
        return true;
    }

    bool isOpen() const noexcept final { return mIsOpen; }

    ErrorHandling::Result<std::size_t> read(Utils::BytesRange pBytes) final
    {
        if (!isOpen())   { return EG_ERROR_CODE(Utils::DeviceErrors, not_open); }
        if (!mCanRead)   { return EG_ERROR_CODE(Utils::DeviceErrors, not_open); }

        ULONG actual = 0U;
        UINT rc = fx_file_read(&mFile,
                               pBytes.data(),
                               static_cast<ULONG>(pBytes.size()),
                               &actual);
        if (rc != FX_SUCCESS && rc != FX_END_OF_FILE) {
            return EG_ERROR_CODE(ErrorHandling::StdFswErrors, logic_error);
        }
        return static_cast<std::size_t>(actual);
    }

    ErrorHandling::Result<std::size_t> write(Utils::BytesView const& pView) final
    {
        if (!isOpen())   { return EG_ERROR_CODE(Utils::DeviceErrors, not_open); }
        if (!mCanWrite)  { return EG_ERROR_CODE(Utils::DeviceErrors, not_open); }

        // const_cast: fx_file_write takes VOID* but only reads the buffer
        if (fx_file_write(&mFile,
                          const_cast<void*>(static_cast<const void*>(pView.data())),
                          static_cast<ULONG>(pView.size())) != FX_SUCCESS) {
            return EG_ERROR_CODE(ErrorHandling::StdFswErrors, logic_error);
        }
        return pView.size();
    }

private:

    ErrorHandling::Result<bool> openForMode()
    {
        UINT fxOpenFlag = FX_OPEN_FOR_WRITE;
        mCanRead  = true;
        mCanWrite = true;

        switch (mMode) {
            case FileOpenMode::read_only:
                fxOpenFlag = FX_OPEN_FOR_READ;
                mCanWrite  = false;
                break;

            case FileOpenMode::truncate_write_only:
            case FileOpenMode::truncate_read_write:
                mCanRead = (mMode == FileOpenMode::truncate_read_write);
                break;

            case FileOpenMode::exclusive_write_only:
            case FileOpenMode::exclusive_read_write:
                /* Create fails if file already exists → exclusive semantics */
                if (fx_file_create(mMedia, mName) != FX_SUCCESS) {
                    return EG_ERROR_CODE(ErrorHandling::StdFswErrors, logic_error);
                }
                mCanRead = (mMode == FileOpenMode::exclusive_read_write);
                break;

            case FileOpenMode::append_write_only:
            case FileOpenMode::append_read_write:
                mCanRead = (mMode == FileOpenMode::append_read_write);
                break;

            default:
                return EG_ERROR_CODE(ErrorHandling::StdFswErrors, invalid_argument);
        }

        if (fx_file_open(mMedia, &mFile, mName, fxOpenFlag) != FX_SUCCESS) {
            return EG_ERROR_CODE(ErrorHandling::StdFswErrors, logic_error);
        }

        /* Truncate modes: zero the file after opening */
        if (mMode == FileOpenMode::truncate_write_only ||
            mMode == FileOpenMode::truncate_read_write) {
            (void)fx_file_truncate_release(&mFile, 0U);
        }

        /* Append modes: seek to end */
        if (mMode == FileOpenMode::append_write_only ||
            mMode == FileOpenMode::append_read_write) {
            ULONG size = 0U;
            (void)fx_file_size_get(&mFile, &size);
            (void)fx_file_seek(&mFile, size);
        }

        mIsOpen = true;
        return true;
    }

    FX_MEDIA*    mMedia{nullptr};
    FX_FILE      mFile{};
    char         mName[FX_MAX_LONG_NAME_LEN]{};
    FileOpenMode mMode{FileOpenMode::invalid};
    bool         mIsOpen{false};
    bool         mCanRead{true};
    bool         mCanWrite{false};
};

}  // namespace Encore::OS

#endif
