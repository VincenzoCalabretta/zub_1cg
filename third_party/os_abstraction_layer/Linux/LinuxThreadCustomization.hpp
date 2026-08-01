
///
/// @file
/// @brief Implements IThreadCustomization for Linux
///
#ifndef ENCORE_OS_ThreadCustomization_HPP
#define ENCORE_OS_ThreadCustomization_HPP

#include <sched.h>
#include <pthread.h>
#include <algorithm>

#include <cerrno>
#include <cstdint>

#include <thread>

#include "encore/ErrorHandling/ErrorGroup.hpp"
#include "encore/ErrorHandling/SystemErrors.hpp"
#include "encore/OS/Common/IThreadCustomization.hpp"

namespace Encore::OS {

/**
 * @brief Implementation of IThreadCustomization in a Linux/POSIX hybrid environment
 */
class LinuxThreadCustomization final : public IThreadCustomization {
public:

    /**
     * @brief Sets the priority of the thread in the context of the underlying OS scheduler
     *
     * @param pThread The thread to modify. If no thread is specified, the calling thread is assumed.
     * @param pPriority The desired priority of the thread
     *
     * @details Input priority is clamped to range of scheduler-dependent values; see man page sched(7)
     */
    ErrorHandling::ReturnCode setPriority( ThreadAttributes::Priority pPriority
                                         , std::thread * pThread = nullptr
                                         ) final
    {
        using namespace ErrorHandling;

        int policy{};
        sched_param sp{};

        pthread_t nativeHandle{ pThread ? pThread->native_handle() : ::pthread_self() };

        if ( ::pthread_getschedparam( nativeHandle, &policy, &sp ) ) { return CURRENT_SYSTEM_ERROR_CODE(); }

        int prioMin{ ::sched_get_priority_min(policy) };
        int prioMax{ ::sched_get_priority_max(policy) };
        if (prioMin < 0 or prioMax < 0) { return CURRENT_SYSTEM_ERROR_CODE(); }

        int priority = std::clamp( prioMin, prioMax, pPriority );

        if ( ::pthread_setschedprio( nativeHandle, priority ) ) { return CURRENT_SYSTEM_ERROR_CODE(); }

        return {};
    }

    /**
     * @brief Adds cpu to set of available cpus able to run the thread
     *
     * @param pCPU Numeric id of cpu to use
     * @param pClear Whether[TRUE] or not[FALSE] to clear the affinity set before adjusting
     * @param pThread The thread to modify. If no thread is specified, the calling thread is assumed
     */
    ErrorHandling::ReturnCode setCPUAffinity( ThreadAttributes::CPUAffinity pCPU
                                            , bool pClear
                                            , std::thread * pThread = nullptr
                                            ) final
    {
        using namespace ErrorHandling;

        pthread_t nativeHandle{ pThread ? pThread->native_handle() : ::pthread_self() };

        cpu_set_t current{};

        if( ::pthread_getaffinity_np(nativeHandle, sizeof(current), &current) ) { return CURRENT_SYSTEM_ERROR_CODE(); }

        if( pClear ) { CPU_ZERO( &current ); }

        CPU_SET( pCPU, &current );

        if( ::pthread_setaffinity_np(nativeHandle, sizeof(current), &current) ) { return CURRENT_SYSTEM_ERROR_CODE(); }

        return {};
    }

    /**
     * @brief Retrieves the threads cpu affinity for a given CPU
     *
     * @param pCPU The CPU to check
     * @param pThread The thread to retrieve the cpu affinity for
     *
     * @details If no thread is specified, the calling thread is assumed
     *
     * @return True if the thread has an affinity for the provided CPU. False
     *         otherwise
     */
    ErrorHandling::Result<bool> getCPUAffinity( ThreadAttributes::CPUAffinity pCPU
                                              , std::thread * pThread = nullptr
                                              ) final
    {
        using namespace ErrorHandling;

        pthread_t nativeHandle{ pThread ? pThread->native_handle() : ::pthread_self() };

        cpu_set_t current{};

        if( ::pthread_getaffinity_np(nativeHandle, sizeof(current), &current) ) { return CURRENT_SYSTEM_ERROR_CODE(); }

        return CPU_ISSET(pCPU, &current);
    }
};

}

#endif
