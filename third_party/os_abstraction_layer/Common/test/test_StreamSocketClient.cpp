
#include "../StreamSocketClient.hpp"

#include <cstdint>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>

#include "encore/ErrorHandling/test/UnitTestExtensions.hpp"
#include "encore/UnitTesting/UnitTestingCore.hpp"
#include "encore/Utils/Serializer.hpp"

#include <parcore/Logging/OStreamLogger.hpp>
#include <mockcore/LoopbackDevice.hpp>

namespace {

using namespace Encore;
using namespace Encore::OS;
using namespace Encore::Mocks;
using namespace Encore::Messaging;
using namespace Encore::Utils;
using namespace Encore::ErrorHandling;
using namespace Encore::Logging;
namespace test = Encore::UnitTesting;

#define STR_INDIRECTION(pVar) #pVar
#define STR(pVar) STR_INDIRECTION(pVar)
#define FILE_AND_LINE __FILE__ ":" STR(__LINE__)

constexpr auto kSeriPolicy = SerializationPolicy::packed_big_endian;

/// @brief Simple protocol that sticks a 4 byte size field in front of the streamed payload
class SimpleProtocol final : public IStreamProtocol {
public: /*IStreamClientProtocol*/

    void onResetReadStream() final {
        ++numResets;
    }

    Result<StreamMessageInfo> readMsg( BytesView const & pStream, BytesRange pDestination ) final {

        StreamMessageInfo info{};
        mReadInProgress = false;

        std::string_view view{ reinterpret_cast<char const *>( pStream.data() ), pStream.size()};

        constexpr std::string_view kDelim{"\xFF\xFF"};
        auto idx = view.find(kDelim);
        if( idx == view.npos ) {
            return info;
        }

        if( idx + kHeaderSize > view.size() ) { // Not enough to read header
            mReadInProgress = true;
            return info;
        }

        std::uint16_t packetSize{0u};

        //NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic) -- Silence... safe pointer math is speaking
        BytesViewBuffer dev{ BytesView{ reinterpret_cast<EncoreByte const *>( view.data() ) + idx + kDelim.size()
                           , pStream.size() }
                           };

        (void)deserializeCluster<kSeriPolicy>(dev, packetSize); // can't fail

        if (idx + kHeaderSize + packetSize > view.size()) { // Not enough to read payload
            mReadInProgress = true;
            return info;
        }

        BytesBuffer buf{pDestination};
        auto written = buf.write( { &pStream[idx + kHeaderSize], packetSize } );
        if( not written.success() ) {
            return TRACE_CODE_DEFAULT( written.error() );
        }

        info.endPos = idx + kHeaderSize + packetSize;
        info.size = skipReads ? 0 : packetSize;
        info.isValid = true;

        return info;
    }

    Result<std::size_t> writeMsg( BytesView const & pPayload, BytesRange pDest ) final {

        auto & logger = LogGroup::getDefaultLogger();

        auto size = static_cast<std::uint16_t>(pPayload.size());

        BytesBuffer buf{pDest};

        auto written = serializeCluster<kSeriPolicy>(buf, std::uint16_t{0xFFFFu});
        if( not written.success()) { return TRACE_CODE( logger, written.error() ); }

        written = serializeCluster<kSeriPolicy>(buf, size);
        if( not written.success()) { return TRACE_CODE( logger, written.error() ); }

        written = buf.write( { pPayload.data(), pPayload.size() } );
        if( not written.success()) { return TRACE_CODE( logger, written.error() ); }

        return buf.tellp();
    }

public:

    static constexpr std::size_t kHeaderSize{2 * sizeof(std::uint16_t)};

    unsigned numResets{0};

    bool readInProgress() const { return mReadInProgress; }

    bool skipReads{false};

private:

    bool mReadInProgress{false};
};


class SlowStreamWriter final : public OS::IStreamSocket {
public:

    ErrorHandling::Result<bool> open() noexcept final {
        return mSocket.open();
    }

    ErrorHandling::Result<std::size_t> write( Utils::BytesView const & pView ) noexcept final {

        BytesView view;
        if( maxBytesToWrite.has_value() ) {
            view = { pView.data(), std::min( pView.size(), maxBytesToWrite.value() ) };
        }
        else {
            view = pView;
        }

        return mSocket.write(view);
    }

    ErrorHandling::Result<std::size_t> read( Utils::BytesRange pBytes ) noexcept final {
        return mSocket.read(pBytes);
    }

    ErrorHandling::Result<bool> close() noexcept final {

        return mSocket.close();
    }

    bool isOpen() const noexcept final { return mSocket.isOpen(); }

public:

    std::optional<std::size_t> maxBytesToWrite{};

private:

    Mocks::LoopbackStreamSocket mSocket;
};


template <class tByte>
constexpr bool isByteType() {
    return std::is_same_v<tByte, char> or std::is_same_v<tByte, std::int8_t> or std::is_same_v<tByte, std::uint8_t>;
}


template <class tByte, std::size_t N>
BytesView getView(std::array<tByte, N> const & pData, std::size_t pStart = 0, std::size_t pSize = N) {
    static_assert( isByteType<tByte>() );
    //NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic) -- Silence... safe pointer math is speaking
    return BytesView{ reinterpret_cast<EncoreByte const *>( pData.data() ) + pStart, pSize };
}


class BytesViewExtension {
public:

    BytesViewExtension( test::UnitTest<void> & pTest )
    : mTest{ pTest }
    {}

    void viewsEqual(BytesView const & pView, BytesView const & pExp, std::string const & pMsg = {}) {

        mTest.areEqual(pView.size(), pExp.size(), "sizes match -- " + pMsg);

        for (std::uint32_t ii{0}; ii < pView.size(); ++ii) {
            mTest.areEqual(pView.data()[ii] & 0xFFu
                          , pExp.data()[ii] & 0xFFu
                          , "byte " + std::to_string(ii) + " matches -- " + pMsg
                          );
        }
    }

    template <class tByte, std::size_t N>
    void viewsEqual(BytesView const & pView, std::array<tByte, N> const & pExp, std::string const & pMsg = {}) {

        static_assert( isByteType<tByte>() );

        BytesView view {
            reinterpret_cast<EncoreByte const *>(pExp.data()),
            pExp.size()
        };

        viewsEqual(pView, view, pMsg);
    }

private:

    test::UnitTest<void> & mTest;
};


Logger & setupLogger() {

    LogGroup::resetAll( kTestInitOnly );

    constexpr LoggerId kAuxId{69};

    auto & logger = LogGroup::getAuxLogger( kTestInitOnly, kAuxId );
    auto tok = LogGroup::requestAuxLogAPIToken( kTestInitOnly, kAuxId );
    std::ignore = logger.setBackend( kTestInitOnly
                                   , tok.value()
                                   , std::make_unique<PAR::OStreamLogger>( std::cout )
                                   );
    std::ignore = logger.setSeverityModel( kTestInitOnly
                                         , tok.value()
                                         , std::make_unique<SingularAtomicLogSeverityModel>( Severity::debug )
                                         );

    return logger;
}


void receiveStream( test::UnitTestConfig const & pConfig ) {

    test::MixinUnitTest<test::ErrorHandlingExtension, BytesViewExtension> ut{pConfig};

    auto & logger = setupLogger();

    auto protocol = std::make_unique<SimpleProtocol>();
    auto socket = std::make_unique<Mocks::LoopbackStreamSocket>();
    std::array<EncoreByte, 32> sendBuf{};
    std::array<EncoreByte, 32> recvBuf{};
    std::array<EncoreByte, 32> socketReadBuf{};
    auto client = std::make_unique<StreamSocketClient>( *protocol, *socket, sendBuf, recvBuf, socketReadBuf );
    client->routeLoggingTo(logger);
    ut.isSuccessful(client->init(kTestInitOnly), "Client initializes -- " FILE_AND_LINE);

    ut.isSuccessful(socket->open(), "socket opens -- " FILE_AND_LINE);

    //// Empty Read ////
    {
        std::array<EncoreByte, 32> buf{};
        BytesRange bytes{buf};
        ut.succeedsWithValue(client->recvMsg(bytes), std::size_t{0}, "empty read is successful -- " FILE_AND_LINE);
    }
    ut.areEqual(protocol->numResets, 0u, "no reset -- " FILE_AND_LINE);
    ut.areEqual(client->getNumBytesSent(), 0u, "bytes sent -- " FILE_AND_LINE);
    ut.areEqual(client->getNumBytesReceived(), 0u, "bytes recvd -- " FILE_AND_LINE);

    //// Segmented Read ////

    std::array buf {
        '\xFF', '\xFF', '\x00', '\x04', // hdr 1
        '\xAB', '\xAD', '\xBA', '\xBE', // payload 2

        '\xFF', '\xFF', '\x00', '\x02', // hdr 2
        '\xFE', '\xED',                 // payload 2

        '\xFF', '\xFF', '\x00', '\x03', // hdr 3
        '\xBA', '\xDD', '\xAD',         // payload 3
    };

    std::ignore = socket->write(getView(buf, 0, 1));
    std::ignore = socket->write(getView(buf, 1, 3));
    std::ignore = socket->write(getView(buf, 4, 1));
    std::ignore = socket->write(getView(buf, 5, 5)); // Partial read of msg 2 hdr
    std::ignore = socket->write(getView(buf, 10, 4));

    {
        std::array<EncoreByte, 32> readBuf{};
        BytesRange bytes{readBuf};
        ut.succeedsWithValue(client->recvMsg(bytes), std::size_t{0}, "read is successful -- " FILE_AND_LINE);
    }
    ut.areEqual(protocol->numResets, 0u, "no reset -- " FILE_AND_LINE);
    ut.areEqual(client->getNumBytesSent(), 0u, "bytes sent -- " FILE_AND_LINE);
    ut.areEqual(client->getNumBytesReceived(), 1u, "bytes recvd -- " FILE_AND_LINE);

    {
        std::array<EncoreByte, 32> readBuf{};
        BytesRange bytes{readBuf};
        ut.succeedsWithValue(client->recvMsg(bytes), std::size_t{0}, "read is successful -- " FILE_AND_LINE);
    }
    ut.areEqual(protocol->numResets, 0u, "no reset -- " FILE_AND_LINE);
    ut.areEqual(client->getNumBytesSent(), 0u, "bytes sent -- " FILE_AND_LINE);
    ut.areEqual(client->getNumBytesReceived(), 4u, "bytes recvd -- " FILE_AND_LINE);

    {
        std::array<EncoreByte, 32> readBuf{};
        BytesRange bytes{readBuf};
        ut.succeedsWithValue(client->recvMsg(bytes), std::size_t{0}, "read is successful -- " FILE_AND_LINE);
    }
    ut.areEqual(protocol->numResets, 0u, "no reset -- " FILE_AND_LINE);
    ut.areEqual(client->getNumBytesSent(), 0u, "bytes sent -- " FILE_AND_LINE);
    ut.areEqual(client->getNumBytesReceived(), 5u, "bytes recvd -- " FILE_AND_LINE);

    {
        std::array<EncoreByte, 32> readBuf{};
        BytesRange bytes{readBuf};
        auto res = client->recvMsg(bytes);
        ut.succeedsWithValue(res, std::size_t{4}, "read is successful -- " FILE_AND_LINE);

        std::array exp{ '\xAB', '\xAD', '\xBA', '\xBE' };
        ut.viewsEqual({readBuf.data(), res.value()}, exp, "read correct bytes -- " FILE_AND_LINE);
    }
    ut.areEqual(protocol->numResets, 0u, "no reset -- " FILE_AND_LINE);
    ut.areEqual(client->getNumBytesSent(), 0u, "bytes sent -- " FILE_AND_LINE);
    ut.areEqual(client->getNumBytesReceived(), 10u, "bytes recvd -- " FILE_AND_LINE);

    //// Finish Partial Read ////

    {
        std::array<EncoreByte, 32> readBuf{};
        BytesRange bytes{readBuf};

        auto res = client->recvMsg(bytes);
        ut.succeedsWithValue(res, std::size_t{2}, "reading rest of segmented message is successful -- " FILE_AND_LINE);

        std::array exp{ '\xFE', '\xED' };
        ut.viewsEqual({readBuf.data(), res.value()}, exp, "read correct bytes -- " FILE_AND_LINE);
    }
    ut.areEqual(protocol->numResets, 0u, "no reset -- " FILE_AND_LINE);
    ut.areEqual(client->getNumBytesSent(), 0u, "bytes sent -- " FILE_AND_LINE);
    ut.areEqual(client->getNumBytesReceived(), 14u, "bytes recvd -- " FILE_AND_LINE);

    //// Full Read ////

    std::ignore = socket->write(getView(buf, 14, 7));
    {
        std::array<EncoreByte, 32> readBuf{};
        BytesRange bytes{readBuf};

        auto res = client->recvMsg(bytes);
        ut.succeedsWithValue(res, std::size_t{3}, "reading entire message in one go is successful -- " FILE_AND_LINE);

        std::array exp{ '\xBA', '\xDD', '\xAD' };
        ut.viewsEqual({readBuf.data(), res.value()}, exp, "read correct bytes -- " FILE_AND_LINE);
    }
    ut.areEqual(protocol->numResets, 0u, "no reset -- " FILE_AND_LINE);
    ut.areEqual(client->getNumBytesSent(), 0u, "bytes sent -- " FILE_AND_LINE);
    ut.areEqual(client->getNumBytesReceived(), 21u, "bytes recvd -- " FILE_AND_LINE);
}


void skipReads( test::UnitTestConfig const & pConfig ) {

    test::MixinUnitTest<test::ErrorHandlingExtension, BytesViewExtension> ut{pConfig};

    auto & logger = setupLogger();

    auto protocol = std::make_unique<SimpleProtocol>();
    auto socket = std::make_unique<Mocks::LoopbackStreamSocket>();
    std::array<EncoreByte, 32> sendBuf{};
    std::array<EncoreByte, 32> recvBuf{};
    std::array<EncoreByte, 32> socketReadBuf{};
    auto client = std::make_unique<StreamSocketClient>( *protocol, *socket, sendBuf, recvBuf, socketReadBuf );
    client->routeLoggingTo(logger);
    ut.isSuccessful(client->init(kTestInitOnly), "Client initializes -- " FILE_AND_LINE);

    ut.isSuccessful(socket->open(), "socket opens -- " FILE_AND_LINE);

    //// Empty Read ////
    {
        std::array<EncoreByte, 32> buf{};
        BytesRange bytes{buf};
        ut.succeedsWithValue(client->recvMsg(bytes), std::size_t{0}, "empty read is successful -- " FILE_AND_LINE);
    }
    ut.areEqual(protocol->numResets, 0u, "no reset -- " FILE_AND_LINE);
    ut.areEqual(client->getNumBytesSent(), 0u, "bytes sent -- " FILE_AND_LINE);
    ut.areEqual(client->getNumBytesReceived(), 0u, "bytes recvd -- " FILE_AND_LINE);

    //// Segmented Read ////

    std::array buf {
        '\xFF', '\xFF', '\x00', '\x04', // hdr 1
        '\xAB', '\xAD', '\xBA', '\xBE', // payload 2

        '\xFF', '\xFF', '\x00', '\x02', // hdr 2
        '\xFE', '\xED',                 // payload 2
    };

    std::ignore = socket->write(getView(buf, 0, 8));
    std::ignore = socket->write(getView(buf, 8, 6));

    // Here we will ignore the content of the first message, but we signal to the internal buffer to move past the
    // message we received. This emulates the behavior we'd want for something like a failed checksum.
    protocol->skipReads = true;
    {
        std::array<EncoreByte, 32> readBuf{};
        BytesRange bytes{readBuf};
        ut.succeedsWithValue(client->recvMsg(bytes), std::size_t{0}, "read is successful -- " FILE_AND_LINE);
    }
    ut.areEqual(protocol->numResets, 0u, "no reset -- " FILE_AND_LINE);
    ut.areEqual(client->getNumBytesSent(), 0u, "bytes sent -- " FILE_AND_LINE);
    ut.areEqual(client->getNumBytesReceived(), 8u, "bytes recvd -- " FILE_AND_LINE);

    protocol->skipReads = false;
    {
        std::array<EncoreByte, 32> readBuf{};
        BytesRange bytes{readBuf};

        auto res = client->recvMsg(bytes);
        ut.succeedsWithValue(res, std::size_t{2}, "Read second message -- " FILE_AND_LINE);

        std::array exp{ '\xFE', '\xED' };
        ut.viewsEqual({readBuf.data(), res.value()}, exp, "read correct bytes -- " FILE_AND_LINE);
    }
    ut.areEqual(protocol->numResets, 0u, "no reset -- " FILE_AND_LINE);
    ut.areEqual(client->getNumBytesSent(), 0u, "bytes sent -- " FILE_AND_LINE);
    ut.areEqual(client->getNumBytesReceived(), 14u, "bytes recvd -- " FILE_AND_LINE);
}


void fillReadBuffer( test::UnitTestConfig const & pConfig ) {

    test::MixinUnitTest<test::ErrorHandlingExtension, BytesViewExtension> ut{pConfig};

    auto & logger = setupLogger();

    SimpleProtocol protocol;
    Mocks::LoopbackStreamSocket socket;
    std::array<EncoreByte, 8> sendBuf{};
    std::array<EncoreByte, 8> recvBuf{};
    std::array<EncoreByte, 8> socketReadBuf{};
    StreamSocketClient client{protocol, socket, sendBuf, recvBuf, socketReadBuf};
    client.routeLoggingTo(logger);
    ut.isSuccessful(client.init(kTestInitOnly), "Client initializes -- " FILE_AND_LINE);

    ut.isSuccessful(socket.open(), "socket opens -- " FILE_AND_LINE);

    std::array<EncoreByte, 8> readBuf{};

    //// Fill the recv buffer with a large message ////
    {
        std::array data{'\xFF', '\xFF', '\x00', '\x05', 'a', 'b', 'c', 'd' };
        std::ignore = socket.write(getView(data));
        auto res = client.recvMsg( { readBuf } );
        ut.succeedsWithValue(res, {0}, "read is successful -- " FILE_AND_LINE);
        ut.areEqual(protocol.numResets, 0u, "no reset -- " FILE_AND_LINE);
        ut.areEqual(client.numBytesToRead(), 8u, "full read buffer -- " FILE_AND_LINE);
    }

    //// Try to read another message - Portion of the full buffer will be discarded ////
    {
        std::array data{'\xFF', '\xFF', '\x00', '\x02', 'f', 'g' };
        std::ignore = socket.write(getView(data));
        auto res = client.recvMsg( { readBuf } );
        ut.succeedsWithValue(res, {2}, "read is successful -- " FILE_AND_LINE);
        ut.areEqual(protocol.numResets, 1u, "reset to discard full buffer -- " FILE_AND_LINE);
        ut.viewsEqual(getView(readBuf, 0, 2), getView(data, 4, 2), "read new message -- " FILE_AND_LINE);
        ut.areEqual(protocol.readInProgress(), false, "read not in progress -- " FILE_AND_LINE);
        ut.areEqual(client.numBytesToRead(), 0u, "Done reading -- " FILE_AND_LINE);
    }
}


void partialFillReadBuffer( test::UnitTestConfig const & pConfig ) {

    test::MixinUnitTest<test::ErrorHandlingExtension, BytesViewExtension> ut{pConfig};

    auto & logger = setupLogger();

    SimpleProtocol protocol;
    Mocks::LoopbackStreamSocket socket;
    std::array<EncoreByte, 8> sendBuf{};
    std::array<EncoreByte, 8> recvBuf{};
    std::array<EncoreByte, 8> socketReadBuf{};
    StreamSocketClient client{protocol, socket, sendBuf, recvBuf, socketReadBuf};
    client.routeLoggingTo(logger);
    ut.isSuccessful(client.init(kTestInitOnly), "Client initializes -- " FILE_AND_LINE);

    ut.isSuccessful(socket.open(), "socket opens -- " FILE_AND_LINE);

    std::array<EncoreByte, 8> readBuf{};
    //// Fill part of the recv buffer ////
    {
        std::array data{'\xFF', '\xFF', '\x00', '\x02', 'a', 'b', };
        std::ignore = socket.write(getView(data));
        auto res = client.recvMsg( { readBuf } );
        ut.succeedsWithValue(res, {2}, "read is successful -- " FILE_AND_LINE);
        ut.areEqual(protocol.numResets, 0u, "no reset -- " FILE_AND_LINE);
        ut.viewsEqual(getView(readBuf, 0, 2), getView(data, 4, 2), "read new message -- " FILE_AND_LINE);
        ut.areEqual(protocol.readInProgress(), false, "no read in progress -- " FILE_AND_LINE);
        ut.areEqual(client.numBytesToRead(), 0u, "full read buffer -- " FILE_AND_LINE);
    }

    //// Try to read another message - Will need to reset to make space ////
    {
        std::array data{'\xFF', '\xFF', '\x00', '\x02', 'f', 'g' };
        std::ignore = socket.write(getView(data));
        auto res = client.recvMsg( { readBuf } );
        ut.succeedsWithValue(res, {2}, "read is successful -- " FILE_AND_LINE);
        ut.areEqual(protocol.numResets, 1u, "reset to make space -- " FILE_AND_LINE);
        ut.viewsEqual(getView(readBuf, 0, 2), getView(data, 4, 2), "read new message -- " FILE_AND_LINE);
        ut.areEqual(protocol.readInProgress(), false, "read not in progress -- " FILE_AND_LINE);
        ut.areEqual(client.numBytesToRead(), 0u, "Done reading -- " FILE_AND_LINE);
    }

    //// Try to read another message, will need to reset to make space ////
    {
        std::array data{'\xFF', '\xFF', '\x00', '\x03', 'h', 'i', 'j' };
        std::ignore = socket.write(getView(data));
        auto res = client.recvMsg( { readBuf } );
        ut.succeedsWithValue(res, {3}, "read is successful -- " FILE_AND_LINE);
        ut.areEqual(protocol.numResets, 2u, "reset to make space -- " FILE_AND_LINE);
        ut.viewsEqual(getView(readBuf, 0, 3), getView(data, 4, 3), "read new message -- " FILE_AND_LINE);
        ut.areEqual(protocol.readInProgress(), false, "read not in progress -- " FILE_AND_LINE);
        ut.areEqual(client.numBytesToRead(), 0u, "Done reading -- " FILE_AND_LINE);
    }
}


void sendStream( test::UnitTestConfig const & pConfig ) {

    test::MixinUnitTest<test::ErrorHandlingExtension, BytesViewExtension> ut{pConfig};

    auto protocol = std::make_unique<SimpleProtocol>();
    auto socket = std::make_unique<SlowStreamWriter>();
    std::array<EncoreByte, 32> sendBuf{};
    std::array<EncoreByte, 32> recvBuf{};
    std::array<EncoreByte, 32> socketReadBuf{};
    auto client = std::make_unique<StreamSocketClient>( *protocol, *socket, sendBuf, recvBuf, socketReadBuf );
    ut.isSuccessful(client->init(kTestInitOnly), "Client initializes -- " FILE_AND_LINE);

    ut.isSuccessful(socket->open(), "socket opens -- " FILE_AND_LINE);

    std::array<EncoreByte, 32> testReadBuf{};

    ut.succeedsWithValue( client->sendMsg({nullptr, 0}), std::size_t{0}, "empty send -- " FILE_AND_LINE );

    socket->maxBytesToWrite = 2; // Only half of header
    {
        std::array data{ '\xAB', '\xCD', '\xEF' };

        auto res = client->sendMsg(getView(data));
        ut.succeedsWithValue(res, {2}, "send is successful -- " FILE_AND_LINE);

        ut.succeedsWithValue(socket->read(testReadBuf), {2}, "socket read is successful -- " FILE_AND_LINE);

        std::array exp{ '\xFF', '\xFF' };
        ut.viewsEqual(
            getView(testReadBuf, 0, 2),
            getView(exp),
            "sent bytes are expected -- " FILE_AND_LINE
        );
        ut.areEqual(client->getNumBytesSent(), 2u, "bytes sent -- " FILE_AND_LINE);
        ut.areEqual(client->getNumBytesReceived(), 0u, "bytes recvd -- " FILE_AND_LINE);
    }

    // other half of header
    {
        auto res = client->sendMsg(StreamSocketClient::kFlushSendBuffer);
        ut.succeedsWithValue(res, {2}, "send is successful -- " FILE_AND_LINE);

        ut.succeedsWithValue(socket->read(testReadBuf), {2}, "socket read is successful -- " FILE_AND_LINE);

        std::array exp{ '\x00', '\x03' };
        ut.viewsEqual(
            getView(testReadBuf, 0, 2),
            getView(exp, 0, exp.size()),
            "sent bytes are expected -- " FILE_AND_LINE
        );
        ut.areEqual(client->getNumBytesSent(), 4u, "bytes sent -- " FILE_AND_LINE);
        ut.areEqual(client->getNumBytesReceived(), 0u, "bytes recvd -- " FILE_AND_LINE);
    }

    // Send another message before buffer is emptied
    socket->maxBytesToWrite = 2;
    {
        std::array data{ '\xFA', '\xCE' };

        auto res = client->sendMsg(getView(data));
        ut.succeedsWithValue(res, {2}, "send is successful -- " FILE_AND_LINE);

        ut.succeedsWithValue(socket->read(testReadBuf), {2}, "socket read is successful -- " FILE_AND_LINE);

        std::array exp{ '\xAB', '\xCD' };
        ut.viewsEqual(
            getView(testReadBuf, 0, 2),
            getView(exp, 0, exp.size()),
            "sent bytes are expected -- " FILE_AND_LINE
        );
        ut.areEqual(client->getNumBytesSent(), 6u, "bytes sent -- " FILE_AND_LINE);
        ut.areEqual(client->getNumBytesReceived(), 0u, "bytes recvd -- " FILE_AND_LINE);
    }

    {
        auto res = client->sendMsg(StreamSocketClient::kFlushSendBuffer);
        ut.succeedsWithValue(res, {2}, "send is successful -- " FILE_AND_LINE);

        ut.succeedsWithValue(socket->read(testReadBuf), {2}, "socket read is successful -- " FILE_AND_LINE);

        std::array exp{ '\xEF', '\xFF' };
        ut.viewsEqual(
            getView(testReadBuf, 0, 2),
            getView(exp, 0, exp.size()),
            "sent bytes are expected -- " FILE_AND_LINE
        );
        ut.areEqual(client->getNumBytesSent(), 8u, "bytes sent -- " FILE_AND_LINE);
        ut.areEqual(client->getNumBytesReceived(), 0u, "bytes recvd -- " FILE_AND_LINE);
    }

    socket->maxBytesToWrite = 100; // Finish our stream
    {
        auto res = client->sendMsg(StreamSocketClient::kFlushSendBuffer);
        ut.succeedsWithValue(res, {5}, "send is successful -- " FILE_AND_LINE);

        ut.succeedsWithValue(socket->read(testReadBuf), {5}, "socket read is successful -- " FILE_AND_LINE);

        std::array exp{ '\xFF', '\x00', '\x02', '\xFA', '\xCE' };
        ut.viewsEqual(
            getView(testReadBuf, 0, 5),
            getView(exp, 0, exp.size()),
            "sent bytes are expected -- " FILE_AND_LINE
        );
        ut.areEqual(client->getNumBytesSent(), 13u, "bytes sent -- " FILE_AND_LINE);
        ut.areEqual(client->getNumBytesReceived(), 0u, "bytes recvd -- " FILE_AND_LINE);
    }
}


struct FailCaseErrors {

    enum {
        fail_socket_write,
        fail_socket_read,
        fail_proto_read,
        fail_proto_write,
    };

    static constexpr std::string_view name( ThreadSafe ) { return "FailCaseErrors"; }

    static constexpr std::string_view message( ThreadSafe, ErrorNumber ) {
        return "not important";
    }
};


class BadSocket final : public IStreamSocket {
public: /*IStreamSocket*/

    Result<bool> open() noexcept final { return true; }

    Result<std::size_t> write(BytesView const &) noexcept final {
        return EG_ERROR_CODE( FailCaseErrors, fail_socket_write );
    }

    Result<std::size_t> read(BytesRange) noexcept final {
        return EG_ERROR_CODE( FailCaseErrors, fail_socket_read );
    }

    Result<bool> close() noexcept final { return true; }

    bool isOpen() const noexcept final { return true; }
};


class BadProtocol final : public IStreamProtocol {
public:

    void onResetReadStream() final {}

    Result<StreamMessageInfo> readMsg( BytesView const &, BytesRange ) final {
        return EG_ERROR_CODE( FailCaseErrors, fail_proto_read );
    }

    Result<std::size_t> writeMsg( BytesView const &, BytesRange ) final {
        return EG_ERROR_CODE( FailCaseErrors, fail_proto_write );
    }
};


void offNominal( test::UnitTestConfig const & pConfig ) {

    auto & logger = setupLogger();

    test::MixinUnitTest<test::ErrorHandlingExtension> ut{pConfig};

    EncoreByte ch;
    BytesView view{&ch, 1};
    {
        SimpleProtocol protocol;
        BadSocket socket{};
        std::array<EncoreByte, 32> sendBuf{};
        std::array<EncoreByte, 32> recvBuf{};
        std::array<EncoreByte, 32> socketReadBuf{};
        StreamSocketClient client{protocol, socket, sendBuf, recvBuf, socketReadBuf};
        client.routeLoggingTo(logger);

        ut.failsWithCode(client.recvMsg( {} )
                        , EG_ERROR_CODE( FailCaseErrors, fail_socket_read )
                        , "Fail socket read -- " FILE_AND_LINE
                        );
        ut.failsWithCode(client.sendMsg( view )
                        , EG_ERROR_CODE( FailCaseErrors, fail_socket_write )
                        , "Fail socket write -- " FILE_AND_LINE
                        );
    }
    {
        BadProtocol protocol;
        NoOpStreamSocket socket{};
        std::array<EncoreByte, 32> sendBuf{};
        std::array<EncoreByte, 32> recvBuf{};
        std::array<EncoreByte, 32> socketReadBuf{};
        StreamSocketClient client{protocol, socket, sendBuf, recvBuf, socketReadBuf};
        client.routeLoggingTo(logger);

        ut.failsWithCode(client.sendMsg(view)
                        , EG_ERROR_CODE( FailCaseErrors, fail_proto_write )
                        , "Fail proto write -- " FILE_AND_LINE
                        );

        BytesRange range{&ch, 1};
        ut.failsWithCode(client.recvMsg(range)
                        , EG_ERROR_CODE( FailCaseErrors, fail_proto_read )
                        , "Fail proto read -- " FILE_AND_LINE
                        );
    }
    {
        NoOpStreamProtocol protocol;
        NoOpStreamSocket socket;

        std::array<EncoreByte, 8> sendBuf{};
        std::array<EncoreByte, 8> recvBuf{};
        std::array<EncoreByte, 9> socketReadBuf{};
        StreamSocketClient client{protocol, socket, sendBuf, recvBuf, socketReadBuf};
        client.routeLoggingTo(logger);

        ut.failsWithCode(
            client.init(kTestInitOnly),
            EG_ERROR_CODE( StdFswErrors, logic_error ),
            "Client fails to initialize because read buf is too large -- " FILE_AND_LINE
        );
    }
    {
        NoOpStreamProtocol protocol;
        NoOpStreamSocket socket;

        std::array<EncoreByte, 8> sendBuf{};
        std::array<EncoreByte, 8> socketReadBuf{};
        StreamSocketClient client{protocol, socket, sendBuf, {nullptr, 0}, socketReadBuf};
        client.routeLoggingTo(logger);

        ut.failsWithCode(
            client.init(kTestInitOnly),
            EG_ERROR_CODE( StdFswErrors, logic_error ),
            "Client fails to initialize because read buf is too large -- " FILE_AND_LINE
        );
    }
    {
        NoOpStreamProtocol protocol;
        NoOpStreamSocket socket;

        std::array<EncoreByte, 8> recvBuf{};
        std::array<EncoreByte, 8> socketReadBuf{};
        StreamSocketClient client{protocol, socket, {nullptr, 0}, recvBuf, socketReadBuf};
        client.routeLoggingTo(logger);

        ut.failsWithCode(
            client.init(kTestInitOnly),
            EG_ERROR_CODE( MessageClientErrors, destination_range_too_small ),
            "Client fails to initialize because send buf is empty -- " FILE_AND_LINE
        );
    }
    {
        NoOpStreamProtocol protocol;
        NoOpStreamSocket socket;

        std::array<EncoreByte, 8> sendBuf{};
        StreamSocketClient client{protocol, socket, sendBuf, {nullptr, 0}, {nullptr, 0}};
        client.routeLoggingTo(logger);

        ut.failsWithCode(
            client.init(kTestInitOnly),
            EG_ERROR_CODE( MessageClientErrors, destination_range_too_small ),
            "Client fails to initialize because recv buf is empty -- " FILE_AND_LINE
        );
    }
    {
        NoOpStreamProtocol protocol;
        NoOpStreamSocket socket;

        std::array<EncoreByte, 8> sendBuf{};
        std::array<EncoreByte, 8> recvBuf{};
        StreamSocketClient client{protocol, socket, sendBuf, recvBuf, {nullptr, 0}};
        client.routeLoggingTo(logger);

        ut.failsWithCode(
            client.init(kTestInitOnly),
            EG_ERROR_CODE( MessageClientErrors, destination_range_too_small ),
            "Client fails to initialize because socket read buf is empty -- " FILE_AND_LINE
        );
    }
}

}

UNITTESTER_MAIN( test_StreamSocketClient, int pArgc, char const** pArgv ) {

    test::UnitTester tester( pArgc, pArgv );

    return tester.filterAndExec( {
        { test::TestTypeID::use_case, "recyBytes", &receiveStream },
        { test::TestTypeID::use_case, "skipReads", &skipReads },
        { test::TestTypeID::misc, "fillReadBuffer", &fillReadBuffer },
        { test::TestTypeID::misc, "partialFillReadBuffer", &partialFillReadBuffer },
        { test::TestTypeID::use_case, "sendBytes", &sendStream },
        { test::TestTypeID::misc, "offNominal", &offNominal },
    } );
}
