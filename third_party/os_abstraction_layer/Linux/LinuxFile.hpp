
///
/// @file
/// @brief Defines a linux implementation of the resizable file interface
///
#ifndef ENCORE_OS_LinuxFile_HPP
#define ENCORE_OS_LinuxFile_HPP

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "encore/Encore.hpp"
#include "encore/ErrorHandling/ErrorGroup.hpp"
#include "encore/ErrorHandling/Result.hpp"
#include "encore/ErrorHandling/StdFswErrors.hpp"
#include "encore/ErrorHandling/SystemErrors.hpp"
#include "encore/Logging/ILogRoutable.hpp"
#include "encore/Logging/Logger.hpp"
#include "encore/Logging/StackTraceHelper.hpp"
#include "encore/Utils/IReaderWriter.hpp"
#include "encore/Utils/Span.hpp"
#include "encore/Utils/StringUtils.hpp"

#include "encore/OS/Common/FileErrors.hpp"
#include "encore/OS/Common/IFile.hpp"
#include "encore/OS/Linux/Linux.hpp"

namespace Encore::OS {

/**
 * @brief Class for interacting with files using POSIX API
 */
class LinuxFile final : public IResizableFile, public LinuxDescriptable, public Logging::LogRoutable {
public:

    static constexpr std::size_t kMaxFileModeSize{8}; ///< @brief maximum file mode length

    /// @brief Enum indicating what return code constitutes an error condition
    enum ErrorCondition {
        error_if_non_zero = 1, ///<@brief Indicates errors are anything non zero
        error_if_negative, ///<@brief Indicates errors are anything less than zero
    };

public: /*IResizableFile*/

    ErrorHandling::ReturnCode resize( std::size_t pSize ) final;

    ErrorHandling::Result<FileInfo> getFileInfo() final;

public: /*IFile*/

    ErrorHandling::ReturnCode setFileOpenParams( std::string_view pFileName, FileOpenMode pFileMode ) final;


    ErrorHandling::Result<std::size_t> getPos() final;

    ErrorHandling::ReturnCode setPos( std::size_t pPos ) final;

public: // ISocket

    ErrorHandling::Result<bool> open() noexcept final;

    ErrorHandling::Result<std::size_t> write( Utils::BytesView const & pView ) noexcept final;

    ErrorHandling::Result<std::size_t> read( Utils::BytesRange pBytes ) noexcept final;

    ErrorHandling::Result<bool> close() noexcept final;

    bool isOpen() const noexcept final;

public:

    LinuxFile( Utils::Span<char> pNameSpan );

    LinuxFile() = delete;
    LinuxFile( const LinuxFile& ) = delete;
    LinuxFile( LinuxFile&& ) = delete;
    LinuxFile& operator=( const LinuxFile& ) = delete;
    LinuxFile& operator=( LinuxFile&& ) = delete;

    ~LinuxFile() override;

    static ErrorHandling::Result<long> checkRetCode( long pRet, ErrorCondition pType );

    template <class T>
    static ErrorHandling::Result<T> closeIfError( ErrorHandling::Result<T> const & pResult, IFile & pFileIO );

    static ErrorHandling::Result<int> composeOpenFlags( FileOpenMode pOpenFlags );

    void setFilePermissions( mode_t pPermissions );

private:

    std::string_view mFileName;
    FileOpenMode mFileMode{FileOpenMode::invalid};
    mode_t mPermissions{0};
    int mOpenFlags{0};
    Utils::Span<char> mNameSpan{};
};


/**
 * @brief Resizes a file (if necessary) to the desired size
 *
 * @param pSize Size (in bytes) to resize the file to
 *
 * @return ReturnCode indicating if resize was successful
 */
inline ErrorHandling::ReturnCode LinuxFile::resize( std::size_t pSize ) {

    if( not isOpen() ) { return EG_ERROR_CODE( FileErrors, not_open ); }

    auto res = checkRetCode( ::ftruncate( getFd(), static_cast<off_t>( pSize ) ), error_if_non_zero );
    if( not res.success() ) { return TRACE_CODE( logger(), res.error() ); }

    return {};
}


/**
 * @brief Retrieves the file size of the provided file
 *
 * @return Size in bytes if successful, otherwise error code
 */
inline ErrorHandling::Result<FileInfo> LinuxFile::getFileInfo() {

    if( not isOpen() ) { return EG_ERROR_CODE( FileErrors, not_open ); }

    struct stat stats{};
    auto res = checkRetCode( ::stat(mFileName.data(), &stats), error_if_non_zero );
    if( not res.success() ) { return TRACE_CODE( logger(), res.error() ); }

    FileInfo info{};
    info.name = mFileName;
    info.mode = mFileMode;
    info.size = static_cast<std::size_t>(stats.st_size);

    return info;
}


/**
 * @brief Sets the next file name and open mode
 *
 * @param pFileName The file name to be opened
 * @param pFileMode The file mode to use when opening
 *
 * @details Not allowed while the file is opened
 */
inline ErrorHandling::ReturnCode LinuxFile::setFileOpenParams( std::string_view pFileName, FileOpenMode pFileMode ) {

    if( isOpen() ) { return EG_ERROR_CODE( FileErrors, not_allowed_while_open ); }

    mFileName = {};
    mFileMode = {};
    mOpenFlags = {};

    auto fileName = Utils::copyString( pFileName, mNameSpan );
    if( not fileName.success() ) { return TRACE_CODE( logger(), fileName.error() ); }

    auto openFlags = composeOpenFlags( pFileMode );
    if( not openFlags.success() ) { return TRACE_CODE( logger(), openFlags.error() ); }

    mFileName = fileName.value();
    mFileMode = pFileMode;
    mOpenFlags = openFlags.value();

    return {};
}


/**
 * @brief Retrieves the current file position
 */
inline ErrorHandling::Result<std::size_t> LinuxFile::getPos() {

    if( not isOpen() ) { return EG_ERROR_CODE( FileErrors, not_open ); }

    auto res = checkRetCode( ::lseek( getFd(), off_t{ 0 }, SEEK_CUR ), error_if_negative );
    if( not res.success() ) { return TRACE_CODE( logger(), res.error() ); }

    return static_cast<std::size_t>( res.value() );
}


/**
 * @brief Sets the current file position relative to the start of the file
 *
 * @param pPos The position in the file to move to (relative to start of file)
 */
inline ErrorHandling::ReturnCode LinuxFile::setPos( std::size_t pPos ) {

    if( not isOpen() ) { return EG_ERROR_CODE( FileErrors, not_open ); }

    auto res = checkRetCode( ::lseek( getFd(), static_cast<off_t>( pPos ), SEEK_SET ), error_if_negative );
    if( not res.success() ) { return TRACE_CODE( logger(), res.error() ); }

    return {};
}


/**
 * @brief Opens a file (if it is not already opened)
 *
 * @return boolean indicating of file was fully opened
 */
inline ErrorHandling::Result<bool> LinuxFile::open() noexcept {

    if( not isOpen() ) {

        auto fd = ::open( mFileName.data(), mOpenFlags, 0 );
        auto res = checkRetCode( fd, error_if_negative );
        if( not res.success() ) { return TRACE_CODE( logger(), res.error() ); }

        setFd( fd );

        if( mPermissions != 0 ) {
            auto rc = ::fchmod( getFd(), mPermissions ); // NOLINT(clang-analyzer-unix.StdCLibraryFunctions) - out of
                                                         // bounds value checked above
            res = closeIfError( checkRetCode( rc, error_if_non_zero ), *this );
            if( not res.success() ) { return TRACE_CODE( logger(), res.error() ); }
        }
    }

    return true;
}


/**
 * @brief Writes bytes to file
 *
 * @param pView A view of bytes to write to the file
 */
inline ErrorHandling::Result<std::size_t> LinuxFile::write( Utils::BytesView const & pView ) noexcept {

    if( not isOpen() ) { return EG_ERROR_CODE( FileErrors, not_open ); }

    auto written = checkRetCode( ::write( getFd(), pView.data(), pView.size() ), error_if_negative );
    if( not written.success() ) { return TRACE_CODE( logger(), written.error() ); }

    return static_cast<std::size_t>( written.value() );
}


/**
 * @brief Reads bytes from file
 *
 * @param pBytes A BytesRange to fill with data from the file
 */
inline ErrorHandling::Result<std::size_t> LinuxFile::read( Utils::BytesRange pBytes ) noexcept {

    if( not isOpen() ) { return EG_ERROR_CODE( FileErrors, not_open ); }

    auto numRead = checkRetCode( ::read( getFd(), pBytes.data(), pBytes.size() ), error_if_negative );
    if( not numRead.success() ) { return TRACE_CODE( logger(), numRead.error() ); }

    return static_cast<std::size_t>( numRead.value() );
}


/**
 * @brief Closes the file
 */
inline ErrorHandling::Result<bool> LinuxFile::close() noexcept {

    if( not isOpen() ) {
        return true;
    }

    auto res = checkRetCode( ::close( getFd() ), error_if_non_zero );
    if( not res.success() ) { return TRACE_CODE( logger(), res.error() ); }

    setFd( -1 );

    return true;
}


/**
 * @brief Returns if the file descriptor is active
 */
inline bool LinuxFile::isOpen() const noexcept {
    return getFd() >= 0;
}


/**
 * @brief Constructs from a name span
 *
 * @param pNameSpan a span to be used to store the file name
 */
inline LinuxFile::LinuxFile( Utils::Span<char> pNameSpan )
: mNameSpan{pNameSpan}
{}


/**
 * @brief Destructor that closes file if opened
 */
inline LinuxFile::~LinuxFile() {
    std::ignore = close();
}


/**
 * @brief Utility for checking a POSIX return code and returning the system error if it exists
 *
 * @param pRet POSIX return code
 * @param pType Type of error condition check
 *
 * @return Success if the return code does not indicate an error, else returns the system error code stored in errno
 */
inline ErrorHandling::Result<long> LinuxFile::checkRetCode( long pRet, ErrorCondition pType ) {

    bool isError = (pType == error_if_non_zero) ? (pRet != 0) : (pRet < 0);

    if( isError ) {
        return CURRENT_SYSTEM_ERROR_CODE();
    }

    return pRet;
}


/**
 * @brief Utility function that closes the file if the provided result is an error
 *
 * @tparam T Result type
 *
 * @param pResult Result instance
 * @param pFileIO IFileIO instance to execute close() on error
 *
 * @return Returns the provided \p pResult unmodified
 */
template <class T>
inline ErrorHandling::Result<T> LinuxFile::closeIfError( ErrorHandling::Result<T> const & pResult, IFile & pFileIO ) {
    if( not pResult.success() ) {
        // ignore the return code because there is nothing else we can do
        std::ignore = pFileIO.close();
    }
    return pResult;
}


/**
 * @brief Creates the posix open bitflag representation of the string open flags
 *
 * @param pOpenFlags The desired file open type
 *
 * @note "Binary" flag is ignored as it is default behavior within linux
 */
inline ErrorHandling::Result<int> LinuxFile::composeOpenFlags( FileOpenMode pOpenFlags ) {

    switch( pOpenFlags ) {
        case FileOpenMode::read_only:
            return O_RDONLY;
        case FileOpenMode::truncate_write_only:
            return O_WRONLY | O_TRUNC | O_CREAT;
        case FileOpenMode::exclusive_write_only:
            return O_WRONLY | O_EXCL  | O_CREAT;
        case FileOpenMode::append_write_only:
            return O_WRONLY | O_APPEND | O_CREAT;
        case FileOpenMode::read_write:
            return O_RDWR;
        case FileOpenMode::truncate_read_write:
            return O_RDWR | O_TRUNC | O_CREAT;
        case FileOpenMode::exclusive_read_write:
            return O_RDWR | O_EXCL  | O_CREAT;
        case FileOpenMode::append_read_write:
            return O_RDWR | O_APPEND | O_CREAT;
        case FileOpenMode::invalid: [[ fallthrough ]];
        default: {
            return EG_ERROR_CODE( FileErrors, invalid_open_mode );
        }
    }
}


/**
 * @brief stores the desired linux access permissions to apply to the file after opening
 *
 * @param pPermissions The file access permissions set (read/write/own) to store
 */
inline void LinuxFile::setFilePermissions( mode_t pPermissions ) {

    mPermissions = pPermissions;
}

}

#endif
