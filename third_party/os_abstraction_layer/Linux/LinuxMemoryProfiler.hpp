
///
/// @file
/// @brief An entity capable of reading /proc/pid/statm and reporting items as
///        handle telemetry
///
#ifndef ENCORE_OS_LinuxMemoryProfiler_HPP
#define ENCORE_OS_LinuxMemoryProfiler_HPP

#include <cstdint>

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "encore/DataModel/TypedDataHandles.hpp"
#include "encore/DataModel/DataHandleTraits.hpp"
#include "encore/DataModel/Registrar.hpp"
#include "encore/IFSW.hpp"
#include "encore/OS/Common/AppProfiling.hpp"
#include "encore/OS/Linux/LinuxProcQuerier.hpp"
#include "encore/OS/Linux/LinuxPidStatMQuery.hpp"
#include "encore/OS/Linux/LinuxPidStatMQuerySerializer.hpp"
#include "encore/Logging/ILogRoutable.hpp"
#include "encore/Utils/Utils.hpp"


namespace Encore::OS {

/**
 * @brief Class capable of populating /proc statm queries in the Registrar
 */
class LinuxMemoryProfiler : public Logging::LogRoutable {
public:

    /**
     * @brief Constructor
     *
     * @param pInit INIT_ONLY behavior (requires allocation)
     * @param pPid  Process id of process to query
     */
    LinuxMemoryProfiler( [[maybe_unused]] InitOnly const & pInit, pid_t pPid = 0 )
    : mPidStatMQuerier(pInit)
    , mPid(pPid)
    {
    }

    /**
     * @brief Initializes the ProcQuerier
     *
     * @param pInit InitOnly Specifier
     */
    ErrorHandling::ReturnCode init( InitOnly const & pInit ) {

        auto rc = mPidStatMQuerier.init( pInit, mPid );
        if( not rc.success() ) { return TRACE_CODE( logger(), rc.error() ); }

        return {};
    }

    /**
     * @brief Registers a PODLike handle of MemInfo for use with Handle telemetry
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
     * @brief Opens the /proc/pid/statm file and populates the Info handle with its contents
     */
    ErrorHandling::Result<MemInfo> query() {

        auto memStatus = mPidStatMQuerier.query();
        if( not memStatus.success() ) { return TRACE_CODE( logger(), memStatus.error() ); }

        auto memValues = memStatus.value();

        MemInfo info;
        info.size = memValues.size;
        info.resident = memValues.resident;
        info.shared = memValues.shared;

        if( mHandle.isRegistered() ) {
            mHandle.set( info );
        }

        return info;
    }

private:

    ProcQuerier<LinuxPidStatMQuery> mPidStatMQuerier;
    DataModel::TypedDataHandle<MemInfo> mHandle;
    pid_t mPid{};
};

}


#endif
