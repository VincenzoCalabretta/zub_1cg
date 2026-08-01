
///
/// @file
/// @brief Implements IStreamSocket (TCP client) and ITCPClientSocketFactory using NetX Duo
///
#ifndef ENCORE_OS_ThreadXTCPClientSocket_HPP
#define ENCORE_OS_ThreadXTCPClientSocket_HPP

#include <cstddef>
#include <cstdint>

#include "nx_api.h"

#include "encore/ErrorHandling/StdFswErrors.hpp"
#include "encore/OS/Common/IStreamSocket.hpp"
#include "encore/OS/Common/TCP.hpp"
#include "encore/Utils/DeviceErrors.hpp"

namespace Encore::OS {

/**
 * @brief IStreamSocket TCP client implementation backed by Eclipse NetX Duo.
 *
 * Lifecycle:
 *   1. setNetXContext(ip_ptr, pool_ptr)
 *   2. setTCPClientParams(params)    — sets server IP/port and blocking flag
 *   3. open()                        — creates socket, binds ephemeral port, connects
 *   4. read() / write()
 *   5. close()
 *
 * read() and write() are non-blocking by default (NX_NO_WAIT).  Set
 * mWaitTicks to NX_WAIT_FOREVER or a timeout for blocking behaviour.
 */
class ThreadXTCPClientSocket final : public IStreamSocket {
public:

    ThreadXTCPClientSocket() = default;

    ~ThreadXTCPClientSocket() { if (isOpen()) { (void)close(); } }

    ThreadXTCPClientSocket(const ThreadXTCPClientSocket&) = delete;
    ThreadXTCPClientSocket& operator=(const ThreadXTCPClientSocket&) = delete;

    void setNetXContext(NX_IP* pIp, NX_PACKET_POOL* pPool) noexcept
    {
        mIpPtr   = pIp;
        mPoolPtr = pPool;
    }

    void setTCPClientParams(TCPClientParams const& pParams) noexcept
    {
        mParams = pParams;
    }

    /// @brief Ticks to wait for receive/send.  Default: non-blocking (NX_NO_WAIT = 0).
    void setWaitTicks(ULONG pTicks) noexcept { mWaitTicks = pTicks; }

    // ── IDevice ──────────────────────────────────────────────────────────────

    ErrorHandling::Result<bool> open() final
    {
        if (isOpen()) { return true; }
        if (!mIpPtr || !mPoolPtr) {
            return EG_ERROR_CODE(ErrorHandling::StdFswErrors, logic_error);
        }

        if (nx_tcp_socket_create(mIpPtr, &mSocket, "TCP",
                                 NX_IP_NORMAL, NX_FRAGMENT_OKAY,
                                 0x80U, 8192U,
                                 NX_NULL, NX_NULL) != NX_SUCCESS) {
            return EG_ERROR_CODE(ErrorHandling::StdFswErrors, logic_error);
        }

        if (nx_tcp_client_socket_bind(&mSocket, NX_ANY_PORT, NX_WAIT_FOREVER)
                != NX_SUCCESS) {
            nx_tcp_socket_delete(&mSocket);
            return EG_ERROR_CODE(ErrorHandling::StdFswErrors, logic_error);
        }

        ULONG serverIP   = static_cast<ULONG>(mParams.ipAndPort.addr.asInteger());
        UINT  serverPort = static_cast<UINT>(mParams.ipAndPort.port);
        ULONG connectWait = mParams.nonBlocking ? NX_NO_WAIT : NX_WAIT_FOREVER;

        if (nx_tcp_client_socket_connect(&mSocket, serverIP, serverPort, connectWait)
                != NX_SUCCESS) {
            nx_tcp_client_socket_unbind(&mSocket);
            nx_tcp_socket_delete(&mSocket);
            return EG_ERROR_CODE(ErrorHandling::StdFswErrors, logic_error);
        }

        mIsOpen = true;
        return true;
    }

    ErrorHandling::Result<bool> close() noexcept final
    {
        if (!mIsOpen) { return true; }
        nx_tcp_socket_disconnect(&mSocket, NX_WAIT_FOREVER);
        nx_tcp_client_socket_unbind(&mSocket);
        nx_tcp_socket_delete(&mSocket);
        mIsOpen = false;
        return true;
    }

    bool isOpen() const noexcept final { return mIsOpen; }

    ErrorHandling::Result<std::size_t> read(Utils::BytesRange pBytes) final
    {
        if (!isOpen()) { return EG_ERROR_CODE(Utils::DeviceErrors, not_open); }

        NX_PACKET *pkt = NX_NULL;
        UINT rc = nx_tcp_socket_receive(&mSocket, &pkt, mWaitTicks);
        if (rc == NX_NO_PACKET)         { return std::size_t{0U}; }
        if (rc == NX_NOT_CONNECTED)     { return std::size_t{0U}; }  /* remote closed */
        if (rc != NX_SUCCESS)           { return EG_ERROR_CODE(ErrorHandling::StdFswErrors, logic_error); }

        ULONG copied = 0U;
        nx_packet_data_retrieve(pkt, pBytes.data(), &copied);
        nx_packet_release(pkt);

        return static_cast<std::size_t>(copied);
    }

    ErrorHandling::Result<std::size_t> write(Utils::BytesView const& pView) final
    {
        if (!isOpen()) { return EG_ERROR_CODE(Utils::DeviceErrors, not_open); }

        NX_PACKET *pkt = NX_NULL;
        if (nx_packet_allocate(mPoolPtr, &pkt, NX_TCP_PACKET, NX_WAIT_FOREVER)
                != NX_SUCCESS) {
            return EG_ERROR_CODE(ErrorHandling::StdFswErrors, logic_error);
        }

        // const_cast: nx_packet_data_append only reads from the source buffer
        if (nx_packet_data_append(pkt,
                                  const_cast<void*>(static_cast<const void*>(pView.data())),
                                  static_cast<ULONG>(pView.size()),
                                  mPoolPtr,
                                  NX_WAIT_FOREVER) != NX_SUCCESS) {
            nx_packet_release(pkt);
            return EG_ERROR_CODE(ErrorHandling::StdFswErrors, logic_error);
        }

        /* nx_tcp_socket_send releases the packet on success */
        if (nx_tcp_socket_send(&mSocket, pkt, NX_WAIT_FOREVER) != NX_SUCCESS) {
            nx_packet_release(pkt);
            return EG_ERROR_CODE(ErrorHandling::StdFswErrors, logic_error);
        }

        return pView.size();
    }

private:
    NX_IP*          mIpPtr{nullptr};
    NX_PACKET_POOL* mPoolPtr{nullptr};
    NX_TCP_SOCKET   mSocket{};
    TCPClientParams mParams{};
    ULONG           mWaitTicks{NX_NO_WAIT};
    bool            mIsOpen{false};
};


/**
 * @brief ITCPClientSocketFactory that creates ThreadXTCPClientSocket instances.
 *
 * Manages a fixed pool of kMaxSockets pre-allocated socket objects.
 * create() returns the first free slot; destroy() marks it free.
 */
class ThreadXTCPClientSocketFactory final : public ITCPClientSocketFactory {
public:

    static constexpr std::size_t kMaxSockets = 4U;

    void setNetXContext(NX_IP* pIp, NX_PACKET_POOL* pPool) noexcept
    {
        mIpPtr   = pIp;
        mPoolPtr = pPool;
    }

    ErrorHandling::Result<IStreamSocket*> create(TCPClientParams const& pParams) final
    {
        for (std::size_t i = 0U; i < kMaxSockets; ++i) {
            if (!mInUse[i]) {
                mSockets[i].setNetXContext(mIpPtr, mPoolPtr);
                mSockets[i].setTCPClientParams(pParams);
                mInUse[i] = true;
                return &mSockets[i];
            }
        }
        return EG_ERROR_CODE(ErrorHandling::StdFswErrors, logic_error);
    }

    void destroy(IStreamSocket& pSocket) final
    {
        for (std::size_t i = 0U; i < kMaxSockets; ++i) {
            if (&mSockets[i] == &pSocket) {
                if (mSockets[i].isOpen()) { (void)mSockets[i].close(); }
                mInUse[i] = false;
                return;
            }
        }
    }

private:
    NX_IP*                    mIpPtr{nullptr};
    NX_PACKET_POOL*           mPoolPtr{nullptr};
    ThreadXTCPClientSocket    mSockets[kMaxSockets];
    bool                      mInUse[kMaxSockets]{};
};

}  // namespace Encore::OS

#endif
