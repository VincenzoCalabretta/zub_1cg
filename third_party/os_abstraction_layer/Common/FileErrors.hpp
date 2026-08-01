
///
/// @file
/// @brief Defines common file errors
///
#ifndef ENCORE_OS_FileErrors_HPP
#define ENCORE_OS_FileErrors_HPP

#include <string_view>

#include "encore/Encore.hpp"
#include "encore/ErrorHandling/ErrorHandling.hpp"

namespace Encore::OS {

/**
 * @brief Collection of errors associated with file operations
 */
class FileErrors {
public:

    enum {
        failed_to_open = 1,
        not_open,
        not_allowed_while_open,
        invalid_open_mode,
    };

    /// @brief Returns the name of the error group
    static constexpr std::string_view name( ThreadSafe ) noexcept {

        using namespace std::string_view_literals;

        return "FileErrors"sv;
    }

    /// @brief Returns the error message associated with an error code
    /// @param pNum Error number
    static constexpr std::string_view message( ThreadSafe, ErrorHandling::ErrorNumber pNum ) noexcept {

        using namespace std::string_view_literals;

        switch (pNum) {
            case failed_to_open:
                return "File failed to open"sv;
            case not_open:
                return "File is not open"sv;
            case not_allowed_while_open:
                return "Operation not allowed while file is open"sv;
            case invalid_open_mode:
                return "Specified open mode is invalid"sv;
            default:
                return "Unknown FileErrors code"sv;
        }
    }
};

}

#endif
