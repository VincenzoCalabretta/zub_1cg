
///
/// @file
/// @brief
///
#ifndef ENCORE_OS_ProcFileReader_HPP
#define ENCORE_OS_ProcFileReader_HPP

#include <string>

#include "encore/ErrorHandling/ErrorGroup.hpp"
#include "encore/ErrorHandling/Result.hpp"
#include "encore/ErrorHandling/StdFswErrors.hpp"
#include "encore/OS/Linux/Linux.hpp"
#include "encore/OS/Linux/LinuxFile.hpp"

namespace Encore::OS {

/**
 * @brief An implementation of an IReaderWriter designed to parse a /proc/ file
 *
 * @details Assumes space delimited data with alphanumeric fields (including . and -)
 */
class ProcFileReader : public Utils::IReaderWriter {

public: /* IReaderWriter */
    /**
     * @brief Not implemented
     */
    ErrorHandling::Result<std::size_t> write( Utils::BytesView const & ) final {
        return EG_ERROR_CODE( ErrorHandling::StdFswErrors, not_implemented );
    }

    /**
     * @brief Reads the next data field from the /proc file
     *
     * @param pOut A BytesRange to populate with the data field
     *
     * @details Only reads from the actual file when instructed to, data is stored in a buffer so that a consistent
     * snapshot of the proc file state can be processed
     */
    ErrorHandling::Result<std::size_t> read( Utils::BytesRange pOut ) final {

        static constexpr std::string_view kDelim = {" \n\t"};
        static constexpr std::string_view kAlphabet {"abcdefghijklmnopqrstuvwxyz"
                                                     "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-"};

        if( pOut.data() == nullptr ) {
            return EG_ERROR_CODE( ErrorHandling::StdFswErrors, null_ptr );
        }
        if( rebufferFile ) {
            auto rc = mProcFile.read( { mBuffer.begin(), mBuffer.size() } );
            if( not rc.success() ) { return rc; }
            mIdx = 0;
            rebufferFile = false;
        }

        std::string_view bufferSv{ reinterpret_cast<char*>(mBuffer.data()), mBuffer.size() };
        std::size_t startIdx = bufferSv.find_first_of( kAlphabet, mIdx );
        if( startIdx == std::string::npos ) {
            mIdx = mBuffer.size();
            return 0;
        }
        std::size_t endIdx = bufferSv.find_first_of( kDelim, startIdx );
        if( endIdx  == std::string::npos ) {
            mIdx = mBuffer.size();
            return 0;
        }

        if( pOut.size() >= endIdx-startIdx ) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index) - startIdx is bounds checked
            std::memcpy( pOut.data(), &mBuffer[startIdx], endIdx - startIdx );
        }
        else {
            mIdx = endIdx;
            return 0;
        }
        mIdx = endIdx;

        return endIdx - startIdx;
    }

public:
    /**
     * @brief Instructs the object to rebuffer the proc file contents
     */
    ErrorHandling::ReturnCode reset() {
        rebufferFile = true;
        return mProcFile.setPos( 0 );
    }

    static constexpr std::size_t kDefaultBufferSize{400}; ///<@brief Default Buffer Size

    /**
     * @brief Initializes the underlying buffer LinuxFile
     *
     * @param pInitOnly The InitOnly specifier
     * @param pFileName The proc file name to open
     * @param pBufSize The buffer size to use for the file data (default \p kDefaultBufferSize (must be > 0))
     */
    ErrorHandling::Result<bool> init( [[ maybe_unused ]] InitOnly const & pInitOnly
                                    , std::string & pFileName
                                    , std::size_t pBufSize = kDefaultBufferSize
                                    )
    {

        if( pBufSize <= 0 ) {
            return EG_ERROR_CODE( ErrorHandling::StdFswErrors, invalid_argument );
        }
        mBuffer.resize( pBufSize );

        auto rc = mProcFile.setFileOpenParams( pFileName, FileOpenMode::read_only );
        if( not rc.success() ) {
            return TRACE_CODE_DEFAULT( rc.error() );
        }
        auto openRc  = mProcFile.open();
        if( not openRc.success() ) {
            return TRACE_CODE_DEFAULT( openRc );
        }
        return true;
    }

    /**
     * @brief Constructor
     *
     * @param pInit InitOnly specifier
     */
    ProcFileReader( [[ maybe_unused ]] InitOnly const & pInit )
    : mBuffer( kDefaultBufferSize )
    , mProcFile( Utils::Span<char>( mFileName.data(), mFileName.size() ) )
    {
    }

private:

    std::array<char, 32> mFileName{};
    std::vector<EncoreByte> mBuffer{};
    LinuxFile mProcFile;
    std::size_t mIdx{0};
    bool rebufferFile{true};
};

}

#endif
