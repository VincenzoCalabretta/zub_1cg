
///
/// @file
/// @brief Defines a system profiler utility to retrieve CPU usage, etc.
///
#ifndef ENCORE_OS_IProcessProfiler_HPP
#define ENCORE_OS_IProcessProfiler_HPP

#include "encore/ErrorHandling/ErrorHandling.hpp"
#include "encore/ErrorHandling/Result.hpp"
#include "encore/OS/Common/AppProfiling.hpp"

namespace Encore::OS {

/**
 * @brief Interface for a profiling class that can provice insights into currently running process such as cpu usage,
 *        number of active threads, etc.
 */
class IProcessProfiler {
public:
    virtual ~IProcessProfiler() = default;
    /**
     * @brief Query the current state.
     * @see OS::ProcessInfo for what information is returned.
     */
    virtual ErrorHandling::Result<ProcessInfo> query() =0;
};


/**
 * @brief A trivial no-op process profiler that returns a default OS::ProcessInfo object.
*/
class NoOpProcessProfiler final : public IProcessProfiler {
public:

    ErrorHandling::Result<ProcessInfo> query() final { return ProcessInfo{}; }
};

}
#endif
