
///
/// @file
/// @brief Defines abstractions for determining how to read from and write to a byte stream
///

#ifndef ENCORE_OS_IStreamProtocol_HPP
#define ENCORE_OS_IStreamProtocol_HPP

#include <cstdint>

#include "encore/Encore.hpp"
#include "encore/ErrorHandling/Result.hpp"
#include "encore/Utils/Span.hpp"

namespace Encore::OS {

/// @brief Struct used to indicate where in a read stream a message exists
struct StreamMessageInfo {
    std::size_t endPos{0};      ///< @brief Location within the stream where the message ends
    std::size_t size{0};        ///< @brief Size of the message
    bool        isValid{false}; ///< @brief Indicates if the data contained within this struct is valid
};

/**
 * @brief Interface responsible for extracting messages from read streams and sending payloads over write streams.
 */
class IStreamProtocol {
public:

    /// @brief Default destructor
    virtual ~IStreamProtocol() = default;

    /// @brief Method invoked whenever the read stream has been reset. Any internal state associated with the read
    ///        stream should be discarded when this is called.
    virtual void onResetReadStream() =0;

    /// @brief Reads a message from the read stream into the provided destination range. Returns size of 0 if no
    ///        completed message has been found in the stream.
    virtual ErrorHandling::Result<StreamMessageInfo> readMsg( Utils::BytesView const &, Utils::BytesRange ) =0;

    /// @brief Writes a message to the provided destination range
    virtual ErrorHandling::Result<size_t> writeMsg( Utils::BytesView const &, Utils::BytesRange ) =0;
};


/**
 * @brief No-op implementation of IStreamProtocol that does nothing
 */
class NoOpStreamProtocol final : public IStreamProtocol {
public:

    /// @brief No-op reset
    void onResetReadStream() final {}

    /// @brief No-op read
    ErrorHandling::Result<StreamMessageInfo> readMsg( Utils::BytesView const &, Utils::BytesRange ) final {
        return StreamMessageInfo{};
    }

    /// @brief No-op write
    ErrorHandling::Result<size_t> writeMsg( Utils::BytesView const &, Utils::BytesRange ) final {
        return 0;
    }
};

}

#endif
