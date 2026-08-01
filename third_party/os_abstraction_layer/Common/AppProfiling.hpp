
///
/// @file
/// @brief Defines items for Application Profiling within an OS context
///
#ifndef ENCORE_OS_AppProfiling_HPP
#define ENCORE_OS_AppProfiling_HPP

#include <cstdint>

#include <string_view>

#include "encore/Encore.hpp"
#include "encore/Utils/Serializer.hpp"


namespace Encore::OS {

/**
 * @brief Handle compatible storage for the total, resident, and shared memory sizes for an application
 */
struct MemInfo {

    /**
     * @brief Type ID for handle compatibility
     */
    static constexpr std::string_view getType() { return "encore_OS_MemInfo"; }

    std::uint64_t size{0}; ///<@brief total memory used by application
    std::uint64_t resident{0}; ///<@brief resident memory (in RAM) used by application
    std::uint64_t shared{0}; ///<@brief shared memory used by application
};

/**
 * @brief Handle compatible storage for process utilization and thread count
 */
struct ProcessInfo {

    /**
     * @brief Type name for handle compatibility
     */
    static constexpr std::string_view getType() { return "encore_OS_ProcessInfo"; }

    double        cpuUsage{0.}; ///<@brief Percentage relative to a single core
    double        systemUsage{0.}; ///<@brief Percentage relative to SoC
    std::uint32_t threads{1U}; ///<@brief Number of threads in process
};

}

#endif
