
///
/// @file
/// @brief An entity capable of reading /proc/PID/stat
///
#ifndef ENCORE_OS_LinuxPidStatQuery_HPP
#define ENCORE_OS_LinuxPidStatQuery_HPP

#include <array>
#include <string_view>
#include <cstring>

#include "encore/OS/Linux/Linux.hpp"

namespace Encore::OS {

/**
 * @brief Structure capable of holding info from /proc/PID/stat queries
 *
 * @details see man 5 proc for more details
 */
struct LinuxPidStatQuery {

    static constexpr std::size_t kCommArrayLen = 16; ///<@brief TASK_COMM_LEN (includes null termination byte)
    struct CommArray : public std::array<EncoreByte,kCommArrayLen> {};

    std::int32_t pid; ///<@brief process id
    CommArray comm;  ///<@brief proc name
    char state; ///<@brief process state
    std::int32_t ppid; ///<@brief parent process id
    std::int32_t pgrp; ///<@brief parent group id
    std::int32_t session; ///<@brief session id
    std::int32_t tty_nr; ///<@brief the terminal controlling the process
    std::int32_t tpgid; ///<@brief foreground process group id of the controlling terminal
    std::uint32_t flags; ///<@brief kernel flags, see include/linux/sched.h
    std::uint64_t minflt; ///<@brief number of minor faults
    std::uint64_t cminflt; ///<@brief number of process's children minor faults
    std::uint64_t majflt; ///<@brief number of major faults
    std::uint64_t cmajflt; ///<@brief number of process' children major faults
    std::uint64_t utime; ///<@brief Amount of time that the process has been scheduled in user mode, in clock ticks
    std::uint64_t stime; ///<@brief Amount of time the process has been scheduled in kernel mode
    std::int64_t cutime; ///<@brief amount of time the process' children have been scheduled for in user mode
    std::int64_t cstime; ///<@brief amount of time the process' children have been scheduled for in kernel mode
    std::int64_t priority; ///<@brief Real time scheduling priority (if real time scheduling policy) or nice value
    std::int64_t nice; ///<@brief the nice value for the priority
    std::int64_t num_threads; ///<@brief number of threads in the process
    // There are more attributes we don't care about (for now)

    /**
     * @brief Retrieves the query file name (within the proc fs)
     */
    static constexpr std::string_view getFileName() { return "stat"; }

    /**
     * @brief The proc file type
     */
    static constexpr LinuxProcQueryType qType() { return LinuxProcQueryType::process; }
};

}


#endif
