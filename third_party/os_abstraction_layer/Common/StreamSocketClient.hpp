
///
/// @file
/// @brief Defines a generic, stream based IMessageClient implementation
///

#ifndef ENCORE_OS_StreamSocketClient_HPP
#define ENCORE_OS_StreamSocketClient_HPP

#include <cstdint>

#include <algorithm>
#include <type_traits>
#include <utility>

#include "encore/Encore.hpp"
#include "encore/Logging/ILogger.hpp"
#include "encore/Logging/ILogRoutable.hpp"
#include "encore/Logging/Logger.hpp"
#include "encore/Logging/LoggerTraits.hpp"
#include "encore/Logging/StackTraceHelper.hpp"
#include "encore/Messaging/IMessageClient.hpp"
#include "encore/Utils/IReaderWriter.hpp"
#include "encore/Utils/Span.hpp"

#include "encore/OS/Common/IStreamProtocol.hpp"
#include "encore/OS/Common/IStreamSocket.hpp"

namespace Encore::OS {

/**
 * @brief Generic stream-based socket client with configurable
 */
class StreamSocketClient final : public Messaging::IMessageClient, public Logging::LogRoutable {
public: /*IMessageClient*/

    ErrorHandling::ReturnCode init( InitOnly const & ) final;

    ErrorHandling::Result<std::size_t> sendMsg( Utils::BytesView const & pView ) final;

    ErrorHandling::Result<std::size_t> recvMsg( Utils::BytesRange pBytes ) final;

public:

    StreamSocketClient( IStreamProtocol & pProtocol
                      , IStreamSocket & pSocket
                      , Utils::BytesRange pSendRange
                      , Utils::BytesRange pRecvRange
                      , Utils::BytesRange pSocketReadRange
                      );

    /// @brief Returns the number of bytes available for reading
    std::size_t numBytesToRead() const noexcept { return mRecvBuf.tellp() - mRecvBuf.tellg(); }

    /// @brief Returns the total number of bytes sent to the underlying socket by this endpoint
    std::uint64_t getNumBytesSent() const noexcept { return mNumBytesSent; }

    /// @brief Returns the total number of bytes read from the underlying socket by this endpoint
    std::uint64_t getNumBytesReceived() const noexcept { return mNumBytesRecv; }

    /// @brief Convenience constant for indicating that you simply wish to flush the existing send buffer
    static constexpr Utils::BytesView kFlushSendBuffer{};

private:

    void resetRecvDev();

    std::uint64_t mNumBytesSent{0};
    std::uint64_t mNumBytesRecv{0};
    Utils::BytesRange mSendRange;
    Utils::BytesRange mRecvRange;
    Utils::BytesRange mSocketReadRange;
    Utils::BytesBuffer mSendBuf;
    Utils::BytesBuffer mRecvBuf;
    IStreamSocket & mSocket;
    IStreamProtocol & mProtocol;
};


/////////////////
// Definitions //
/////////////////


/**
 * @brief Initializes client by validating range sizes
 */
inline ErrorHandling::ReturnCode StreamSocketClient::init( InitOnly const & ) {

    if( mSocketReadRange.size() > mRecvBuf.size() ) {

        logger().log( Logging::Severity::error
                    , "Socket read range can not be larger than the receive range:"
                      "\n\tSocket read range size -- ", mSocketReadRange.size()
                    , "\n\t       Recv range size -- ", mRecvBuf.size()
                    , "\n"
                    );

        return EG_ERROR_CODE( ErrorHandling::StdFswErrors, logic_error );
    }

    if( mSocketReadRange.size() == 0 or mSendBuf.size() == 0 ) {

        logger().log( Logging::Severity::error
                    , "Neither the socket read nor the send range can have size of 0:"
                      "\n\tSocket read range size -- ", mSocketReadRange.size()
                    , "\n\t       Send range size -- ", mSendBuf.size()
                    , "\n"
                    );

        return EG_ERROR_CODE( Messaging::MessageClientErrors, destination_range_too_small );
    }

    return {};
}


/**
 * @brief   Sends a payload using the backing streaming socket
 * @details This method enqueues an outgoing payload via the selected IStreamClientProtocol and attempts to send
 *          as many bytes as it can. If the send buffer is not emptied by this call, [FALSE] will be returned.
 *          In this event, if you desire to send the rest of the bytes in the internal send buffer, call this
 *          function with the \c kFlushSendBuffer constant until this function returns [TRUE].
 * @todo    Remove send buffering behavior to simplify
 * @param   pView Payload to send
 * @return  Returns number of bytes sent
 * @note    The returned size may be larger than that of the user's requested \c pView since the protocol may add
 *          additional content to the message.
 */
inline ErrorHandling::Result<std::size_t> StreamSocketClient::sendMsg( Utils::BytesView const & pView ) {

    if( mSendBuf.tellp() == mSendBuf.tellg() ) {
        mSendBuf.reset();
    }

    if( pView.size() > 0 ) {

        Utils::BytesRange range{ &mSendRange[mSendBuf.tellp()], mSendBuf.size() - mSendBuf.tellp() };

        auto written = mProtocol.writeMsg( pView, range );
        if( not written.success() ) { return TRACE_CODE( logger(), written.error() ); }

        mSendBuf.seekp( mSendBuf.tellp() + written.value() );
    }

    std::size_t bytesRemaining = mSendBuf.tellp() - mSendBuf.tellg();

    // Don't bother touching the socket if we have nothing left to write
    if( bytesRemaining == 0 ) {
        return 0;
    }

    Utils::BytesView toSend{ &mSendRange[mSendBuf.tellg()], bytesRemaining };
    auto sent = mSocket.write( toSend );
    if( not sent.success() ) { return TRACE_CODE(logger(), sent.error()); }

    mNumBytesSent += sent.value();

    mSendBuf.seekg( mSendBuf.tellg() + sent.value() );

    return sent.value();
}


/**
 * @brief   Attempts to receive a complete payload from the backing streaming socket
 * @details This uses the IStreamClientProtocol to determine if/when it is able to read a complete payload. This
 *          client will aggregate received bytes until the protocol indicates that a full payload can be read.
 * @param   pBytes  Bytes range to populate with received messages
 * @return  Number of butes received. If a full payload it not received, this will return zero.
 */
inline ErrorHandling::Result<std::size_t> StreamSocketClient::recvMsg( Utils::BytesRange pBytes ) {

    auto bytesRead = mSocket.read( mSocketReadRange );
    if( not bytesRead.success() ) { return TRACE_CODE( logger(), bytesRead.error() ); }

    mNumBytesRecv += bytesRead.value();

    LOG_NUMERIC_MSG( logger(), "Bytes read from socket -- ", bytesRead.value(), Logging::Severity::debug );

    // NOTE: The below logic _requires_ the socket read buffer to be <= the size of the recv buffer.
    //       The following *must* hold true:
    //         end >= start
    //         => mRecvBuf.tellp() >= mRecvBuf.tellp() + bytesRead.value() - mRecvBuf.size()
    //         => 0 >= bytesRead.value() - mRecvBuf.size()
    //         => mRecvBuf.size >= bytesRead.value()
    //
    //       Our init checks assert that this holds true, so the below math will not overflow.
    auto newSize = mRecvBuf.tellp() + bytesRead.value();
    if( newSize > mRecvBuf.size() ) {

        // We want to discard at _least_ as many bytes as required to fit the contents of our new message. If more
        // bytes than the minimum have already been processed, we can discard those in addition.
        std::size_t spaceRequired = newSize - mRecvBuf.size();
        std::size_t numBytesToClear = std::max(spaceRequired, mRecvBuf.tellg());
        std::size_t start = numBytesToClear;
        std::size_t end = mRecvBuf.tellp();
        std::size_t remainingSize = end - start;

        logger().log( Logging::Severity::debug
                    , "Clearing space in recv buffer for staged bytes:"
                    , "\n\tSpace required --- ", spaceRequired
                    , "\n\tRecv device put -- ", mRecvBuf.tellp()
                    , "\n\tRecv device get -- ", mRecvBuf.tellg()
                    , "\n\tTo clear --------- ", numBytesToClear
                    , "\n\tRemaining -------- ", remainingSize
                    , "\n"
                    );

        using DiffType = decltype(mRecvRange)::difference_type;
        std::move( mRecvRange.begin() + static_cast<DiffType>(start)
                    , mRecvRange.begin() + static_cast<DiffType>(end)
                    , mRecvRange.begin()
                    );

        resetRecvDev();

        mRecvBuf.seekp(remainingSize);
    }

    // Can't fail due to the above checks
    std::ignore = mRecvBuf.write( { mSocketReadRange.data(), bytesRead.value() } );

    logger().log( Logging::Severity::debug
                , "Recv device state:"
                , "\n\tPut -------- ", mRecvBuf.tellp()
                , "\n\tGet -------- ", mRecvBuf.tellg()
                , "\n\tAvailable -- ", numBytesToRead()
                , "\n"
                );

    Utils::BytesView currentStream{ mRecvRange.data() + mRecvBuf.tellg(), numBytesToRead() };

    auto info = mProtocol.readMsg( currentStream, pBytes );
    if( not info.success() ) {
        return TRACE_CODE( logger(), info.error() );
    }

    if( info.value().isValid ) {

        auto & val = info.value();

        logger().log( Logging::Severity::debug
                    , "Read message from stream:"
                    , "\n\tSize ---- ", val.size
                    , "\n"
                    );

        mRecvBuf.seekg( mRecvBuf.tellg() + val.endPos );
        return val.size;
    }

    return 0u;
}


/**
 * @brief   Constructs a StreamSocketClient from a protocol and a IStreamSocket implementation
 * @details This construct requires 3 distinct spans:
 *          - One range for staging messages to send
 *          - One range for staging messages for receiving
 *          - One range for reading directly from the stream socket
 *          @par
 *          A distinct range is used for receive staging and socket reading so that if the staging range is filled,
 *          bytes can be read into the socket read range and space can be made in the receive range for this new
 *          data.
 * @note    \p pSocketReadRange should not be larger than \p pRecvRange, and if it is, an error will be raised at
 *          init. This requirement is imposed so that the staging buffer can always be moved into the receive ]
 *          buffer.
 * @param   pProtocol       Reference to the stream protocol implementation
 * @param   pSocket         Reference to the stream socket implementation
 * @param   pSendRange      Byte range to use for send buffering
 * @param   pRecvRange      Byte range to use for receive buffering
 * @param   pSocketReadRange   Byte range to use for staging fresh reads from socket
 */
inline StreamSocketClient::StreamSocketClient( IStreamProtocol & pProtocol
                                             , IStreamSocket & pSocket
                                             , Utils::BytesRange pSendRange
                                             , Utils::BytesRange pRecvRange
                                             , Utils::BytesRange pSocketReadRange
                                             )
: mSendRange{ pSendRange }
, mRecvRange{ pRecvRange }
, mSocketReadRange{ pSocketReadRange }
, mSendBuf{ mSendRange }
, mRecvBuf{ mRecvRange }
, mSocket{ pSocket }
, mProtocol{ pProtocol }
{}


inline void StreamSocketClient::resetRecvDev() {

    mRecvBuf.reset();

    mProtocol.onResetReadStream();

    logger().log(Logging::Severity::debug, "Recv device reset\n");
}

}

#endif
