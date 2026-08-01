
///
/// @file
/// @brief Implements IStreamSocket for the Xilinx PS UART (XUartPs) under ThreadX
///
#ifndef ENCORE_OS_ThreadXSerialDevice_HPP
#define ENCORE_OS_ThreadXSerialDevice_HPP

#include <cstddef>

extern "C" {
#include "xparameters.h"
#include "xuartps.h"
}

#include "encore/ErrorHandling/StdFswErrors.hpp"
#include "encore/OS/Common/IStreamSocket.hpp"
#include "encore/Utils/DeviceErrors.hpp"

namespace Encore::OS {

/**
 * @brief IStreamSocket implementation wrapping the Xilinx XUartPs driver (polled mode)
 *
 * @details The toolchain is built with -DSDT, so XUartPs_LookupConfig takes a u32 base
 *          address rather than a u16 device ID. Call setSerialOpenParams() before open().
 *
 *          read() / write() are non-blocking polled. read() returns 0 bytes when the
 *          FIFO is empty, consistent with LinuxSerialDevice's EAGAIN handling.
 *
 *          XUartPs instances must not be relocated after init; copy and move are deleted.
 */
class ThreadXSerialDevice final : public IStreamSocket {
public:

    ThreadXSerialDevice() = default;

    ~ThreadXSerialDevice() { (void)close(); }

    ThreadXSerialDevice(const ThreadXSerialDevice&) = delete;
    ThreadXSerialDevice& operator=(const ThreadXSerialDevice&) = delete;

    /**
     * @brief Pre-open configuration. Must be called before open().
     *
     * @param pBaseAddress  UART base address, e.g. XPAR_XUARTPS_0_BASEADDR.
     * @param pBaudRate     Desired baud rate in bits/s, e.g. 115200.
     */
    ErrorHandling::ReturnCode setSerialOpenParams(u32 pBaseAddress, u32 pBaudRate)
    {
        if (isOpen()) { return EG_ERROR_CODE(Utils::DeviceErrors, must_be_closed); }
        mBaseAddress = pBaseAddress;
        mBaudRate    = pBaudRate;
        return {};
    }

    ErrorHandling::Result<bool> open() final
    {
        if (isOpen()) { return true; }

        XUartPs_Config* cfg = XUartPs_LookupConfig(mBaseAddress);
        if (!cfg) { return EG_ERROR_CODE(ErrorHandling::StdFswErrors, logic_error); }

        if (XUartPs_CfgInitialize(&mInstance, cfg, cfg->BaseAddress) != XST_SUCCESS) {
            return EG_ERROR_CODE(ErrorHandling::StdFswErrors, logic_error);
        }

        if (XUartPs_SetBaudRate(&mInstance, mBaudRate) != XST_SUCCESS) {
            return EG_ERROR_CODE(ErrorHandling::StdFswErrors, logic_error);
        }

        mIsOpen = true;
        return true;
    }

    ErrorHandling::Result<bool> close() noexcept final
    {
        mIsOpen = false;
        return true;
    }

    bool isOpen() const noexcept final { return mIsOpen; }

    ErrorHandling::Result<std::size_t> read(Utils::BytesRange pBytes) final
    {
        if (!isOpen()) { return EG_ERROR_CODE(Utils::DeviceErrors, not_open); }

        return static_cast<std::size_t>(
            XUartPs_Recv(&mInstance,
                         reinterpret_cast<u8*>(pBytes.data()),
                         static_cast<u32>(pBytes.size())));
    }

    ErrorHandling::Result<std::size_t> write(Utils::BytesView const& pView) final
    {
        if (!isOpen()) { return EG_ERROR_CODE(Utils::DeviceErrors, not_open); }

        // const_cast is safe: XUartPs_Send only reads from the buffer (C API does not take const)
        return static_cast<std::size_t>(
            XUartPs_Send(&mInstance,
                         const_cast<u8*>(reinterpret_cast<const u8*>(pView.data())),
                         static_cast<u32>(pView.size())));
    }

private:
    XUartPs  mInstance{};
    u32      mBaseAddress{XPAR_XUARTPS_0_BASEADDR};
    u32      mBaudRate{XUARTPS_DFT_BAUDRATE};
    bool     mIsOpen{false};
};

}  // namespace Encore::OS

#endif
