
///
/// @file
/// @brief Implements IUDPSocket using Eclipse NetX Duo
///
#ifndef ENCORE_OS_ThreadXUDPSocket_HPP
#define ENCORE_OS_ThreadXUDPSocket_HPP

#include <cstddef>

#include "nx_api.h"

#include "encore/ErrorHandling/StdFswErrors.hpp"
#include "encore/OS/Common/IUDPSocket.hpp"
#include "encore/OS/Common/IPv4Address.hpp"
#include "encore/OS/Common/UDP.hpp"
#include "encore/Utils/DeviceErrors.hpp"

namespace Encore::OS {

/**
 * @brief IUDPSocket implementation backed by Eclipse NetX Duo.
 *
 * Usage:
 * @code
 *   ThreadXUDPSocket sock;
 *   sock.setNetXContext(&ip, &pool, {.ipAndPort = {addr, 5000}, .mode = UDPSocketMode::read_write});
 *   sock.open();
 *   sock.sendTo(payload, destIPP);
 * @endcode
 *
 * For write_only mode the socket is not bound to a port; recvFrom() always
 * returns an empty Maybe.  For read_write mode the socket is bound to the
 * port specified in UDPSocketParams and can both send and receive.
 *
 * recvFrom() / read() are non-blocking (NX_NO_WAIT); call from a thread
 * that polls, or extend with NX_WAIT_FOREVER if blocking is acceptable.
 */
class ThreadXUDPSocket final : public IUDPSocket {
public:

    ThreadXUDPSocket() = default;

    ~ThreadXUDPSocket() { if (isOpen()) { (void)close(); } }

    ThreadXUDPSocket(const ThreadXUDPSocket&) = delete;
    ThreadXUDPSocket& operator=(const ThreadXUDPSocket&) = delete;

    /**
     * @brief Provide NetX context and socket parameters before open().
     *
     * @param pIp    Pointer to the initialised NX_IP instance.
     * @param pPool  Packet pool used to allocate send/receive packets.
     * @param pParams Local endpoint (port) and socket mode.
     */
    void setNetXContext(NX_IP* pIp, NX_PACKET_POOL* pPool, UDPSocketParams const& pParams)
    {
        mIpPtr    = pIp;
        mPoolPtr  = pPool;
        mMode     = pParams.mode;
        mLocalPort = static_cast<UINT>(pParams.ipAndPort.port);
    }

    /// @brief Set a default destination for write() (not required for sendTo()).
    void setDefaultDestination(IPAndPort const& pDest) noexcept { mDefaultDest = pDest; }

    // ── IUDPSocket ──────────────────────────────────────────────────────────

    ErrorHandling::Result<std::size_t> sendTo(Utils::BytesView const& pView,
                                               IPAndPort const&        pDest) final
    {
        if (!isOpen()) { return EG_ERROR_CODE(Utils::DeviceErrors, not_open); }

        NX_PACKET *pkt = NX_NULL;
        if (nx_packet_allocate(mPoolPtr, &pkt, NX_UDP_PACKET, NX_WAIT_FOREVER)
                != NX_SUCCESS) {
            return EG_ERROR_CODE(ErrorHandling::StdFswErrors, logic_error);
        }

        // const_cast: nx_packet_data_append only reads the source buffer
        if (nx_packet_data_append(pkt,
                                  const_cast<void*>(static_cast<const void*>(pView.data())),
                                  static_cast<ULONG>(pView.size()),
                                  mPoolPtr,
                                  NX_WAIT_FOREVER) != NX_SUCCESS) {
            nx_packet_release(pkt);
            return EG_ERROR_CODE(ErrorHandling::StdFswErrors, logic_error);
        }

        ULONG destIP   = static_cast<ULONG>(pDest.addr.asInteger());
        UINT  destPort = static_cast<UINT>(pDest.port);

        /* nx_udp_socket_send_to releases the packet on success */
        if (nx_udp_socket_send_to(&mSocket, pkt, destIP, destPort) != NX_SUCCESS) {
            nx_packet_release(pkt);
            return EG_ERROR_CODE(ErrorHandling::StdFswErrors, logic_error);
        }

        return pView.size();
    }

    ErrorHandling::Result<Utils::Maybe<RecvFromInfo>> recvFrom(Utils::BytesRange pBytes) final
    {
        if (!isOpen()) { return EG_ERROR_CODE(Utils::DeviceErrors, not_open); }
        if (mMode == UDPSocketMode::write_only) { return Utils::Maybe<RecvFromInfo>{}; }

        NX_PACKET *pkt = NX_NULL;
        UINT rc = nx_udp_socket_receive(&mSocket, &pkt, NX_NO_WAIT);
        if (rc == NX_NO_PACKET) { return Utils::Maybe<RecvFromInfo>{}; }
        if (rc != NX_SUCCESS)   { return EG_ERROR_CODE(ErrorHandling::StdFswErrors, logic_error); }

        ULONG srcIP   = 0U;
        UINT  srcPort = 0U;
        nx_udp_source_extract(pkt, &srcIP, &srcPort);

        ULONG copied = 0U;
        nx_packet_data_retrieve(pkt, pBytes.data(), &copied);
        nx_packet_release(pkt);

        RecvFromInfo info;
        info.ipp      = { IPv4Address{ static_cast<uint32_t>(srcIP) },
                          static_cast<NetworkPort>(srcPort) };
        info.numBytes = static_cast<std::size_t>(copied);

        return Utils::Maybe<RecvFromInfo>{ info };
    }

    // ── IDevice ──────────────────────────────────────────────────────────────

    ErrorHandling::Result<bool> open() final
    {
        if (isOpen()) { return true; }
        if (!mIpPtr || !mPoolPtr) {
            return EG_ERROR_CODE(ErrorHandling::StdFswErrors, logic_error);
        }

        if (nx_udp_socket_create(mIpPtr, &mSocket, "UDP",
                                 NX_IP_NORMAL, NX_FRAGMENT_OKAY,
                                 0x80U, 5U) != NX_SUCCESS) {
            return EG_ERROR_CODE(ErrorHandling::StdFswErrors, logic_error);
        }

        if (mMode == UDPSocketMode::read_write) {
            if (nx_udp_socket_bind(&mSocket, mLocalPort, NX_WAIT_FOREVER) != NX_SUCCESS) {
                nx_udp_socket_delete(&mSocket);
                return EG_ERROR_CODE(ErrorHandling::StdFswErrors, logic_error);
            }
        }

        mIsOpen = true;
        return true;
    }

    ErrorHandling::Result<bool> close() noexcept final
    {
        if (!mIsOpen) { return true; }
        if (mMode == UDPSocketMode::read_write) { nx_udp_socket_unbind(&mSocket); }
        nx_udp_socket_delete(&mSocket);
        mIsOpen = false;
        return true;
    }

    bool isOpen() const noexcept final { return mIsOpen; }

    /// @brief Sends to the default destination set via setDefaultDestination().
    ErrorHandling::Result<std::size_t> write(Utils::BytesView const& pView) final
    {
        return sendTo(pView, mDefaultDest);
    }

    /// @brief Equivalent to recvFrom(); ignores source address.
    ErrorHandling::Result<std::size_t> read(Utils::BytesRange pBytes) final
    {
        auto result = recvFrom(pBytes);
        if (!result.success()) { return result.errorCode(); }
        if (!result.value().hasValue()) { return std::size_t{0U}; }
        return result.value().value().numBytes;
    }

private:
    NX_IP*          mIpPtr{nullptr};
    NX_PACKET_POOL* mPoolPtr{nullptr};
    NX_UDP_SOCKET   mSocket{};
    UDPSocketMode   mMode{UDPSocketMode::write_only};
    UINT            mLocalPort{0U};
    IPAndPort       mDefaultDest{};
    bool            mIsOpen{false};
};


/**
 * @brief IUDPSocketFactory that creates ThreadXUDPSocket instances.
 *
 * Call setNetXContext() once after construction; all created sockets inherit
 * the same NX_IP and NX_PACKET_POOL.
 */
class ThreadXUDPSocketFactory final : public IUDPSocketFactory {
public:

    void setNetXContext(NX_IP* pIp, NX_PACKET_POOL* pPool) noexcept
    {
        mIpPtr   = pIp;
        mPoolPtr = pPool;
    }

    std::unique_ptr<IUDPSocket> create(InitOnly const &,
                                       UDPSocketParams const& pParams) final
    {
        auto sock = std::make_unique<ThreadXUDPSocket>();
        sock->setNetXContext(mIpPtr, mPoolPtr, pParams);
        return sock;
    }

private:
    NX_IP*          mIpPtr{nullptr};
    NX_PACKET_POOL* mPoolPtr{nullptr};
};

}  // namespace Encore::OS

#endif
