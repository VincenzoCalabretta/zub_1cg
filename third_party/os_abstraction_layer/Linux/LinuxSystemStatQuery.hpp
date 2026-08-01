
///
/// @file LinuxSystemStatQuery.hpp
/// @brief An entity capable of reading /proc/stat
///
#ifndef ENCORE_OS_LinuxSystemStatQuery_HPP
#define ENCORE_OS_LinuxSystemStatQuery_HPP

#include <array>
#include <cstring>
#include <string_view>

#include "encore/OS/Linux/Linux.hpp"


namespace Encore::OS {

/**
 * @brief Structure capable of holding info from /proc/stat queries
 *
 * @details see man 5 proc for more details
 */
struct LinuxSystemStatQuery {

    static constexpr std::size_t kCpuArrayLen = 16; ///<@brief Max CPU name length
    struct CpuName : std::array<EncoreByte,kCpuArrayLen> {};

    CpuName cpu; ///<@brief CPU name
    std::uint64_t user; ///<@brief Time spent in user mode
    std::uint64_t nice; ///<@brief Time spent in user mode with low priority
    std::uint64_t system; ///<@brief Time spent in system mode
    std::uint64_t idle; ///<@brief Type spent in idle
    std::uint64_t iowait; ///<@brief Time waiting for I/O to complete
    std::uint64_t irq; ///<@brief Time servicing interrupts
    std::uint64_t softirq; ///<@brief Time servicing soft interrupts
    std::uint64_t steal; ///<@brief Stolen time
    std::uint64_t guest; ///<@brief Time spent running a virtual CPU for guest OS
    std::uint64_t guest_nice; ///<@brief Time spent running a niced guest OS
    // There are more attributes we don't care about (for now)

    /**
     * @brief retrieves the specific proc file name for the query
     */
    static constexpr std::string_view getFileName() { return "stat"; }

    /**
     * @brief defines the query as a sys query
     */
    static constexpr LinuxProcQueryType qType() { return LinuxProcQueryType::system; }
};

}


#endif
