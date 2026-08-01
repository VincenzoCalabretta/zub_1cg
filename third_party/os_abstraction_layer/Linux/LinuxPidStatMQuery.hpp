
///
/// @file
/// @brief An entity capable of reading /proc/stat
///
#ifndef ENCORE_OS_LinuxPidStatMQuery_HPP
#define ENCORE_OS_LinuxPidStatMQuery_HPP

#include <string_view>

#include "encore/OS/Linux/Linux.hpp"


namespace Encore::OS {

/**
 * @brief Struct for containing /proc/pid/statm data
 */
struct LinuxPidStatMQuery {

    std::uint64_t size{}; ///< @brief process memory size
    std::uint64_t resident{}; ///< @brief process resident memory size
    std::uint64_t shared{}; ///< @brief process shared memory size

    /**
     * @brief Defines the specific proc file name to read
     */
    static constexpr std::string_view getFileName() { return "statm"; }

    /**
     * @brief Defines the proc file type
     */
    static constexpr LinuxProcQueryType qType() { return LinuxProcQueryType::process; }
};

}


#endif
