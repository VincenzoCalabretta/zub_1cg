
///
/// @file
/// @brief Defines an interface and base implementation for file input output ops
///
#ifndef ENCORE_OS_IFile_HPP
#define ENCORE_OS_IFile_HPP

#include <cstdint>

#include <array>
#include <string_view>

#include "encore/Encore.hpp"
#include "encore/ErrorHandling/Result.hpp"
#include "encore/Utils/IDevice.hpp"

namespace Encore::OS {

/**
 * @brief Enum for file open flags
 */
enum class FileOpenMode : std::uint8_t{
    invalid = 0,            ///<@brief Invalid default mode
    read_only,              ///<@brief Opens file in read only mode from the start of the file ("rb")
    truncate_write_only,    ///<@brief Opens file in write only mode and truncates the file ("wb")
    exclusive_write_only,   ///<@brief Opens file in write only mode if it does not exist yet ("wxb")
    append_write_only,      ///<@brief Opens file in write only mode and appends to the end ("ab")
    read_write,             ///<@brief Opens file in read/write mode from the start of the file ("r+b")
    truncate_read_write,    ///<@brief Opens file in read/write mode and truncates ("w+b")
    exclusive_read_write,   ///<@brief Opens file in read/write mode only if it does not exist yet ("wx+b")
    append_read_write,      ///<@brief Opens file in read/write mode and appends to the end ("a+b")
};


/**
 * @brief Struct containing data pertaining to an open file
 */
struct FileInfo {
    std::string_view name; ///<@brief File name
    FileOpenMode mode{FileOpenMode::invalid}; ///<@brief File mode
    std::size_t size{0}; ///<@brief File size
};


/**
 * @brief Interface defining a generic file interface around an IDevice for reading, writing, and seeking within a file
 */
class IFile : public Utils::IDevice {
public:

    ~IFile() override = default;

    /**
     * @brief Sets the file name and file mode for the next open() call
     *
     * @param pFileName File name
     * @param pFileMode Mode to open the file in
     */
    virtual ErrorHandling::ReturnCode setFileOpenParams(std::string_view pFileName, FileOpenMode pFileMode) =0;

    /**
     * @brief Retrieves info (name, mode, size) related to the currently open file
     */
    virtual ErrorHandling::Result<FileInfo> getFileInfo() =0;

    /**
     * @brief Gets the current read/write position
     */
    virtual ErrorHandling::Result<std::size_t> getPos() =0;

    /**
     * @brief Sets the current read/write position
     *
     * @param pPos The position to seek to
     */
    virtual ErrorHandling::ReturnCode setPos(std::size_t pPos) =0;
};


/**
 * @brief File interface that allows for on-demand resizing
 */
class IResizableFile : public IFile {
public:

    /**
     * @brief Resizes file to the specified size
     *
     * @param pSize Desired size
     */
    virtual ErrorHandling::ReturnCode resize(std::size_t pSize) =0;

};

}

#endif
