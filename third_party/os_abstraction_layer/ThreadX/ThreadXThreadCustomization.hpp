
///
/// @file
/// @brief Implements IThreadCustomization for bare-metal ThreadX on Cortex-A53 EL3
///
#ifndef ENCORE_OS_ThreadXThreadCustomization_HPP
#define ENCORE_OS_ThreadXThreadCustomization_HPP

#include <algorithm>
#include <cstdint>
#include <thread>

#include "tx_api.h"

#include "encore/ErrorHandling/StdFswErrors.hpp"
#include "encore/OS/Common/IThreadCustomization.hpp"
#include "encore/OS/ThreadX/ThreadX.hpp"

namespace Encore::OS {

/**
 * @brief Implementation of IThreadCustomization for ThreadX on bare-metal Cortex-A53 EL3
 *
 * @details Priority mapping: the encore int32_t priority is clamped directly to
 *          [0, TX_MAX_PRIORITIES-1]. ThreadX priority 0 is highest; TX_MAX_PRIORITIES-1
 *          is lowest. The encore default of 50 clamps to 31 (lowest) for TX_MAX_PRIORITIES=32.
 *
 *          CPU affinity is a no-op: this target is single-core bare-metal.
 *
 *          The std::thread* parameter is ignored in all methods — on bare-metal, threads are
 *          TX_THREAD structs and tx_thread_identify() always provides the current handle.
 */
class ThreadXThreadCustomization final : public IThreadCustomization {
public:

    /**
     * @brief Sets priority of the current ThreadX thread via tx_thread_priority_change.
     *
     * @param pPriority Desired priority, clamped to [0, TX_MAX_PRIORITIES-1].
     * @param pThread   Ignored. Always operates on the calling ThreadX thread.
     */
    ErrorHandling::ReturnCode setPriority( ThreadAttributes::Priority pPriority
                                         , std::thread* pThread
                                         ) final
    {
        TX_THREAD* handle = getThreadXHandle(pThread);
        if (!handle) { return {}; }  // pre-scheduler or ISR context: no-op

        constexpr UINT kMin = 0U;
        constexpr UINT kMax = TX_MAX_PRIORITIES - 1U;

        UINT txPrio = static_cast<UINT>(
            std::clamp(pPriority,
                       static_cast<ThreadAttributes::Priority>(kMin),
                       static_cast<ThreadAttributes::Priority>(kMax)));

        UINT oldPrio = 0U;
        if (tx_thread_priority_change(handle, txPrio, &oldPrio) != TX_SUCCESS) {
            return EG_ERROR_CODE(ErrorHandling::StdFswErrors, logic_error);
        }

        return {};
    }

    /**
     * @brief No-op: single-core bare-metal target has no CPU affinity concept.
     */
    ErrorHandling::ReturnCode setCPUAffinity( ThreadAttributes::CPUAffinity /*pCPU*/
                                            , bool /*pClear*/
                                            , std::thread* /*pThread*/
                                            ) final
    {
        return {};
    }

    /**
     * @brief Returns true only for CPU 0 (the only CPU on this target).
     */
    ErrorHandling::Result<bool> getCPUAffinity( ThreadAttributes::CPUAffinity pCPU
                                              , std::thread* /*pThread*/
                                              ) final
    {
        return (pCPU == 0U);
    }
};

}  // namespace Encore::OS

#endif
