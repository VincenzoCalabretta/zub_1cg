
///
/// @file
/// @brief Defines a UDP socket for Linux
///
#ifndef ENCORE_OS_NETWORK_LinuxUDPSocket_HPP
#define ENCORE_OS_NETWORK_LinuxUDPSocket_HPP

#include <arpa/inet.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstdint>
#include <utility>

#include "encore/ErrorHandling/ErrorGroup.hpp"
#include "encore/ErrorHandling/Result.hpp"
#include "encore/ErrorHandling/StdFswErrors.hpp"
#include "encore/ErrorHandling/SystemErrors.hpp"
#include "encore/Utils/Utils.hpp"
#include "encore/Utils/Span.hpp"

#include "encore/OS/Common/Network.hpp"
#include "encore/OS/Common/IPv4Address.hpp"
#include "encore/OS/Common/IUDPSocket.hpp"
#include "encore/OS/Common/UDP.hpp"
#include "encore/OS/Linux/Linux.hpp"


namespace Encore::OS {


/**
 * @brief A UDP Socket for Linux/POSIX
 */
class LinuxUDPSocket final : public OS::LinuxDescriptable, public IUDPSocket {
public: /*IUDPSocket*/

    ErrorHandling::Result<bool> open() noexcept final;

    ErrorHandling::Result<std::size_t> write( Utils::BytesView const & ) noexcept final;

    ErrorHandling::Result<std::size_t> sendTo( Utils::BytesView const &, IPAndPort const & ) noexcept final;

    ErrorHandling::Result<std::size_t> read( Utils::BytesRange ) noexcept final;

    ErrorHandling::Result<Utils::Maybe<RecvFromInfo>> recvFrom( Utils::BytesRange ) noexcept final;

    ErrorHandling::Result<bool> close() noexcept final;

    ///@brief Returns if socket is open
    bool isOpen() const noexcept final { return mIsOpen; }

public:

    LinuxUDPSocket( IPv4Address, NetworkPort, UDPSocketMode = UDPSocketMode::read_write ) noexcept;

    LinuxUDPSocket( IPConnection const & ) noexcept;

    ///@brief Destructor
    ~LinuxUDPSocket() noexcept final {
        std::ignore = close();
    }
    LinuxUDPSocket( LinuxUDPSocket const & ) = delete;
    LinuxUDPSocket( LinuxUDPSocket && ) noexcept = default; ///<@brief Default move constructor
    LinuxUDPSocket & operator=( LinuxUDPSocket const & ) = delete;
    LinuxUDPSocket & operator=( LinuxUDPSocket && ) = delete;

    ///@brief Returns the current destination IP addr and port for sending messages
    IPAndPort getSendAddr() const { return IPAndPort{mSendIP,mSendPort}; }

    ///@brief Sets destination IP addr and port for sent messages @param pSendIP The IP address @param pPort The port
    void setSendAddr( IPv4Address pSendIP, NetworkPort pPort ) {
        mSendIP = pSendIP;
        mSendPort = pPort;
    }

private:

    static sockaddr const * castConstInetAddr( sockaddr_in const * );
    static sockaddr * castInetAddr( sockaddr_in * );
    ErrorHandling::Result<std::size_t> recvFromImpl( Utils::BytesRange, sockaddr *, socklen_t * ) noexcept;

    sockaddr_in         mSockRecvAddrIn{};
    IPv4Address         mSendIP;
    NetworkPort         mSendPort{0};
    bool                mIsOpen{false};
    UDPSocketMode       mMode{UDPSocketMode::read_write};
};


/**
 * @brief An IUDPSocketFactory that can create LinuxUDPSocket
 */
class LinuxUDPSocketFactory final : public IUDPSocketFactory {
public: /*IUDPSocketFactory*/

    ///@brief Creates LinuxUDPSocket @param pParams Params to create socket
    std::unique_ptr<IUDPSocket> create( InitOnly const &, UDPSocketParams const & pParams ) final {
        return std::make_unique<LinuxUDPSocket>( pParams.ipAndPort.addr, pParams.ipAndPort.port, pParams.mode );
    }
};


/**
 * @brief Opens the underlying UDP socket
 *
 * @details Socket is created via socket() and, if configured to be a read/write socket, bound via bind().
 *          If socket is already open(), call is a no-op
 */
inline ErrorHandling::Result<bool> LinuxUDPSocket::open() noexcept {

    if( isOpen() ) { return true; }

    int fd = ::socket( AF_INET, SOCK_DGRAM, 0 );
    // LCOV_EXCL_BR_START -- platform-specific branch coverage needed for file descriptor exhaustion
    if( fd < 0 ) { return CURRENT_SYSTEM_ERROR_CODE(); }
    // LCOV_EXCL_BR_STOP

    setFd(fd);

    if( mMode == UDPSocketMode::read_write ) {

        if( ::bind( getFd(), castConstInetAddr(&mSockRecvAddrIn) , sizeof(mSockRecvAddrIn) ) < 0 ) {

            auto code = CURRENT_SYSTEM_ERROR_CODE();

            std::ignore = close();

            return code;
        }
    }

    mIsOpen = true;

    return true;
}


/**
 * @brief Writes data to the UDP socket to the current specified destination (see setSendAddr()/getSendAddr())
 *
 * @param pBytes The bytes that will be copied to the socket
 */
inline ErrorHandling::Result<std::size_t> LinuxUDPSocket::write( Utils::BytesView const & pBytes ) noexcept {
    return sendTo( pBytes, {mSendIP,mSendPort} );
}


/**
 * @brief Sends data to specified destination
 *
 * @param pBytes The bytes to send
 * @param pDest  The destination IP/NetworkPort to receive the bytes
 */
inline ErrorHandling::Result<std::size_t> LinuxUDPSocket::sendTo( Utils::BytesView const & pBytes
                                                                , IPAndPort const & pDest
                                                                ) noexcept
{
    sockaddr_in dest{};

    dest.sin_family      = AF_INET;
    dest.sin_port        = htons( pDest.port );
    dest.sin_addr.s_addr = htonl( pDest.addr.asInteger() );

    // NOLINTBEGIN(clang-analyzer-unix.StdCLibraryFunctions) -- will capture error code for bad file descriptor
    auto bytesSent = ::sendto( getFd(), pBytes.data(), pBytes.size(), 0, castConstInetAddr(&dest), sizeof(dest) );
    // NOLINTEND(clang-analyzer-unix.StdCLibraryFunctions)
    if( bytesSent < 0 ) { return CURRENT_SYSTEM_ERROR_CODE(); }

    return static_cast<std::size_t>(bytesSent);
}


/**
 * @brief Reads data from the UDP socket
 *
 * @param pBytes The bytes range to receive data from the socket
 *
 * @details Source/origin of bytes not conveyed -- see recvFrom if interested in that behavior. In the common event
 *          of EAGAIN, a Result of 0 is returned (i.e. no bytes read but not an error condition)
 */
inline ErrorHandling::Result<std::size_t> LinuxUDPSocket::read( Utils::BytesRange pBytes ) noexcept {
    return recvFromImpl( pBytes, nullptr, nullptr );
}


/**
 * @brief Reads data from the UDP socket
 *
 * @param pBytes The bytes range to receive data from the socket
 *
 * @details Source/origin of bytes returned in RecvFromInfo -- if no bytes are read, the Maybe::hasValue() will evaluate
 *          to FALSE. This includes when the read operation would return EAGAIN
 */
inline ErrorHandling::Result<Utils::Maybe<RecvFromInfo>> LinuxUDPSocket::recvFrom( Utils::BytesRange pBytes ) noexcept {

    sockaddr_in src{};
    socklen_t len{0};

    auto bytesRead = recvFromImpl( pBytes, castInetAddr(&src), &len );
    if( not bytesRead.success() ) { return bytesRead.error(); }

    if( bytesRead.value() == 0 ) { return Utils::Maybe<RecvFromInfo>(); }

    RecvFromInfo out;
    out.numBytes = bytesRead.value();
    out.ipp.addr = IPv4Address( ntohl( src.sin_addr.s_addr ) );
    out.ipp.port = NetworkPort{ ntohs( src.sin_port ) };

    return Utils::Maybe{out};
}


/**
 * @brief Closes the UDP socket - read/write will not work after this call
 */
inline ErrorHandling::Result<bool> LinuxUDPSocket::close() noexcept {

    if( ::close( getFd() ) < 0 ) { return CURRENT_SYSTEM_ERROR_CODE(); }

    mIsOpen = false;

    setFd(-1);

    return true;
}


/**
 * @brief Constructor
 *
 * @param pAddr The IPv4Address that this socket will sendto/recvfrom
 * @param pPort The port number at the address this socket will sendto/recvfrom
 * @param pMode UDPSocket usage mode
 *
 * @details Not RAII... one must call open() before socket can be used for read.
 *          If the socket mode is set to read_write, then both the send address and receive address will be set to
 *          the provided address. Otherwise, the socket is setup to be a write-only socket and only the send
 *          address is set.
 */
inline LinuxUDPSocket::LinuxUDPSocket( IPv4Address pAddr, NetworkPort pPort, UDPSocketMode pMode ) noexcept
: mSendIP{ pAddr }
, mSendPort{ pPort }
, mMode{ pMode }
{
    if( pMode == UDPSocketMode::read_write ) {
        mSockRecvAddrIn.sin_family      = AF_INET;
        mSockRecvAddrIn.sin_port        = htons( pPort );
        mSockRecvAddrIn.sin_addr.s_addr = htonl( pAddr.asInteger() );
    }
}


/**
 * @brief Constructor for representing a couple of IPAndPort, represents a 1-1 relationship between two IPAndPorts
 *
 * @param pInfo The IPConnection (two IPAddresses) representing this socket (recv=data coming FROM, send=data going TO)
 *
 * @details Not RAII... one must call open() before socket can be used for read
 */
inline LinuxUDPSocket::LinuxUDPSocket( IPConnection const & pInfo ) noexcept
: mSendIP{ pInfo.send.addr }
, mSendPort{ pInfo.send.port }
{
    mSockRecvAddrIn.sin_family      = AF_INET;
    mSockRecvAddrIn.sin_port        = htons( pInfo.recv.port );
    mSockRecvAddrIn.sin_addr.s_addr = htonl( pInfo.recv.addr.asInteger() );
}


inline sockaddr const * LinuxUDPSocket::castConstInetAddr( sockaddr_in const * pPtr ) {

    // Standard C POSIX networking patterns here
    return reinterpret_cast<sockaddr const *>(pPtr);
}


inline sockaddr * LinuxUDPSocket::castInetAddr( sockaddr_in * pPtr ) {

    // Standard C POSIX networking patterns here
    return reinterpret_cast<sockaddr *>(pPtr);
}


inline ErrorHandling::Result<std::size_t> LinuxUDPSocket::recvFromImpl( Utils::BytesRange pBytes
                                                                      , sockaddr * pAddr
                                                                      , socklen_t * pLen
                                                                      ) noexcept
{
    // NOLINTBEGIN(clang-analyzer-unix.StdCLibraryFunctions) -- will capture error code for bad file descriptor
    auto bytesRead = ::recvfrom( getFd(), pBytes.data(), pBytes.size(), MSG_DONTWAIT, pAddr, pLen );
    // NOLINTEND(clang-analyzer-unix.StdCLibraryFunctions)
    if( bytesRead < 0 ) {

        if( mMode == UDPSocketMode::write_only ) {
            return EG_ERROR_CODE( ErrorHandling::StdFswErrors, not_implemented );
        }

        if( errno == EAGAIN ) { return 0; }

        return CURRENT_SYSTEM_ERROR_CODE();
    }

    return static_cast<std::size_t>(bytesRead);
}

}

#endif
