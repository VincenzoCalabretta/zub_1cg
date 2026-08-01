
///
/// @file
/// @brief ThreadX-specific base utilities for the encore OS abstraction layer
///
#ifndef ENCORE_OS_ThreadX_HPP
#define ENCORE_OS_ThreadX_HPP

#include <thread>

#include "tx_api.h"

#include "encore/OS/OS.hpp"

namespace Encore::OS {

/// @brief Returns the TX_THREAD* for the current thread.
///
/// On bare-metal EL3, std::thread is not functional — ThreadX threads are TX_THREAD structs.
/// The IThreadCustomization interface passes std::thread* which is always nullptr here;
/// we always resolve to the currently-running ThreadX thread via tx_thread_identify().
inline TX_THREAD* getThreadXHandle(std::thread* /*pThread*/)
{
    return tx_thread_identify();
}

}  // namespace Encore::OS

#endif
