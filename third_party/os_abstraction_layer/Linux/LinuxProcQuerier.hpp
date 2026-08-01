
///
/// @file
/// @brief An entity capable of reading a file from procfs
///
#ifndef ENCORE_OS_ProcQuerier_HPP
#define ENCORE_OS_ProcQuerier_HPP


#include <string>

#include "encore/ErrorHandling/ErrorGroup.hpp"
#include "encore/ErrorHandling/Result.hpp"
#include "encore/ErrorHandling/StdFswErrors.hpp"
#include "encore/Utils/Serializer.hpp"

#include "encore/OS/Linux/Linux.hpp"
#include "encore/OS/Linux/LinuxFile.hpp"
#include "encore/OS/Linux/ProcFileReader.hpp"
#include "encore/Utils/Serializer.hpp"


namespace Encore::OS {

/**
 * @brief A class capable of reading a file from /proc
 *
 * @tparam tQuery The type of query (must satisfy IsProcFsQuery<> trait)
 */
template<class tQuery>
class ProcQuerier {

    static_assert( IsLinuxProcFsQuery<tQuery>::value, "Must be procfs query type" );

public:

    /**
     * @brief Constructor
     *
     * @param pInit INIT_ONLY behavior (requires allocation)
     */
    ProcQuerier( InitOnly const & pInit )
    : mProcFileReader( pInit )
    {
    }

    /**
     * @brief Initializes the ProcQuerier by constructing the proc file name and initializing the reader
     *
     * @param pInit InitOnly specifier
     * @param pPid The process id to read (defaults to 0 aka self)
     * @param pBufferSize The size of the file buffer to use
     */
    ErrorHandling::ReturnCode init( InitOnly const & pInit
                                  , pid_t pPid = 0
                                  , std::size_t pBufferSize = ProcFileReader::kDefaultBufferSize
                                  )
    {
        std::string procName = "/proc/";
        if constexpr ( tQuery::qType() == LinuxProcQueryType::process ) {
            // LCOV_EXCL_BR_START -- Coverage cannot be achieved on to_string, however, all other paths are covered
            procName += pPid == 0 ? "self/" : std::to_string(pPid) + "/";
            // LCOV_EXCL_BR_STOP
        }
        procName += std::string(tQuery::getFileName());

        auto rc = mProcFileReader.init( pInit, procName, pBufferSize );
        if( not rc.success() ) { return TRACE_CODE_DEFAULT( rc.error() ); }
        return {};
    }

    /**
     * @brief Retrieves the current snapshot of the tQuery data from the proc file system
     */
    ErrorHandling::Result<tQuery> query() {

        tQuery query{};

        auto reset = mProcFileReader.reset();
        if( not reset.success() ) {
            return TRACE_CODE_DEFAULT( reset.error() );
        }

        Utils::Serializer<tQuery, Utils::SerializationPolicy::ascii_based> seri{};
        auto rc = seri.toObj( mProcFileReader, query );
        if( not rc.success() ) {
            return TRACE_CODE_DEFAULT( rc.error() );
        }

        return query;
    }

private:

    ProcFileReader mProcFileReader;
    std::string mProcName;
};

}

#endif
