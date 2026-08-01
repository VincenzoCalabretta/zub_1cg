
///
/// @file
/// @brief Generates ProcessInfo from data in /proc
///
#ifndef ENCORE_OS_LinuxStatProcessProfiler_HPP
#define ENCORE_OS_LinuxStatProcessProfiler_HPP

#include <cstdint>
#include <thread>
#include <limits>

#include "encore/DataModel/TypedDataHandles.hpp"
#include "encore/DataModel/DataHandleTraits.hpp"
#include "encore/DataModel/Registrar.hpp"
#include "encore/IFSW.hpp"
#include "encore/ErrorHandling/Result.hpp"
#include "encore/Logging/FileLineHelper.hpp"
#include "encore/Logging/ILogRoutable.hpp"
#include "encore/Logging/Logger.hpp"
#include "encore/OS/Common/IProcessProfiler.hpp"
#include "encore/Utils/ASCIISerializer.hpp"
#include "encore/Utils/Utils.hpp"

#include "encore/OS/Linux/Linux.hpp"
#include "encore/OS/Linux/LinuxPidStatQuery.hpp"
#include "encore/OS/Linux/LinuxPidStatQuerySerializer.hpp"
#include "encore/OS/Linux/LinuxProcQuerier.hpp"
#include "encore/OS/Linux/LinuxSystemStatQuery.hpp"
#include "encore/OS/Linux/LinuxSystemStatQuerySerializer.hpp"


namespace Encore::OS {

/**
 * @brief Implementation of IProcessProfiler meant for stat data in /proc
 */
class LinuxStatProcessProfiler : public IProcessProfiler, public Logging::LogRoutable {

public: /*IProcessProfiler*/

    /**
     * @brief Executes a PID Stat and System Stat queries and computes the percent utilization for both the CPU
     * and the system overall
     */
    ErrorHandling::Result<ProcessInfo> query() override {

        auto q1 = mPidStatQuerier.query();
        if( not q1.success() ) { return TRACE_CODE( logger(), q1.error() ); }

        auto processCur = static_cast<std::uint64_t>(q1.value().utime + q1.value().stime);
        auto threadCnt = q1.value().num_threads;

        auto q2 = mSysStatQuerier.query();
        if( not q2.success() ) { return TRACE_CODE( logger(), q2.error() ); }

        auto sysStat = q2.value();

        auto sysCur{static_cast<std::uint64_t> ( sysStat.user
                                               + sysStat.nice
                                               + sysStat.system
                                               + sysStat.idle
                                               + sysStat.iowait
                                               + sysStat.irq
                                               + sysStat.softirq
                                               + sysStat.steal
                                               + sysStat.guest
                                               + sysStat.guest_nice
                                               )};

        auto const processDelta = (mProcessPrev <= processCur) ? processCur-mProcessPrev
                                                               : std::numeric_limits<long>::max() - mProcessPrev
                                                                                                  + processCur;
        auto const sysDelta = (mSysPrev <= sysCur) ? sysCur-mSysPrev : std::numeric_limits<long>::max() - mSysPrev
                                                                                                        + sysCur;
        auto const usage = std::min( 100.0,
                                     sysDelta ? static_cast<double>(processDelta)/static_cast<double>(sysDelta)*100.0
                                              : 0.
                                   );

        mProcessPrev = processCur;
        mSysPrev = sysCur;

        ProcessInfo p;
        p.cpuUsage = usage * kHardwareConcurrency;
        p.systemUsage = usage;
        p.threads = static_cast<std::uint32_t>(threadCnt);

        if( mHandle.isRegistered() ) {
            mHandle.set( p );
        }

        return p;
    }

public:

    /**
     * @brief Constructor
     *
     * @param pInit INIT_ONLY behavior (requires allocation)
     * @param pPid  Process id of process to query
     */
    LinuxStatProcessProfiler( InitOnly const & pInit, pid_t const pPid = 0 )
    : mPidStatQuerier(pInit)
    , mSysStatQuerier(pInit)
    , mPid(pPid)
    {
    }

    /**
     * @brief Initializes the proc queries and establishes a base measurement for process metrics
     *
     * @param pInit InitOnly specifier
     */
    ErrorHandling::ReturnCode init( InitOnly const & pInit ) {

        auto rc = mPidStatQuerier.init( pInit, mPid );
        if( not rc.success() ) { return TRACE_CODE( logger(), rc.error() ); }

        rc = mSysStatQuerier.init( pInit );
        if( not rc.success() ) { return TRACE_CODE( logger(), rc.error() ); }

        // Run a query to establish a cpu ticks baseline
        auto q = query();
        if( not q.success() ) { return TRACE_CODE( logger(), q.error() ); }

        return {};
    }

    /**
     * @brief Registers an PODLike handle of ProcessInfo for use with Handle telemetry
     *
     * @tparam tId The Handle ID Type
     *
     * @param pInit INIT_ONLY behavior
     * @param pFSW  The FSW interface
     * @param pId   The id of the handle containing Info for this query
     */
    template <class tId>
    std::enable_if_t<Utils::IsEncoreId<tId>::value, ErrorHandling::ReturnCode>
    registerHandle( InitOnly const & pInit, IFSW & pFSW, tId && pId ) {

        auto & reg = pFSW.getRegistrar();

        auto code = reg.registerHandle( pInit, std::forward<tId>(pId), mHandle );
        if( not code.success() ) { return TRACE_CODE( logger(), code ); }

        return {};
    }

    /**
     * @brief Hint about the number of concurrent threads supported by the OS
     *
     * @details Only a hint. May represent the number of cores or threads allowed depending on architecture
     */
    static inline unsigned int const kHardwareConcurrency{std::thread::hardware_concurrency()};

private:

    ProcQuerier<LinuxPidStatQuery> mPidStatQuerier;
    ProcQuerier<LinuxSystemStatQuery> mSysStatQuerier;
    DataModel::TypedDataHandle<ProcessInfo> mHandle;
    std::uint64_t mProcessPrev{};
    std::uint64_t mSysPrev{};
    pid_t mPid{};
};

}

#endif
