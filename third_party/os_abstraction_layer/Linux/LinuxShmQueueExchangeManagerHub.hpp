
///
/// @file
/// @brief Defines ManagerHub built on top of exec/manager SPSCQueue and SPSCQueueExchanger
///
#ifndef ENCORE_OS_LINUX_LinuxShmQueueExchangeManagerHub_HPP
#define ENCORE_OS_LINUX_LinuxShmQueueExchangeManagerHub_HPP

#include <algorithm>
#include <filesystem>
#include <map>
#include <tuple>
#include <vector>

#include "encore/ErrorHandling/ErrorGroup.hpp"
#include "encore/ErrorHandling/Result.hpp"
#include "encore/Utils/SPSCQueue.hpp"
#include "encore/Utils/SPSCQueueExchanger.hpp"
#include "encore/Sequencing/Directive.hpp"
#include "encore/Sequencing/IDirectiveExchanger.hpp"
#include "encore/Sequencing/IManagerHub.hpp"
#include "encore/Sequencing/Sequencing.hpp"
#include "encore/Sequencing/SequencingErrors.hpp"

#include "encore/OS/OS.hpp"
#include "encore/OS/Linux/LinuxShmRegion.hpp"

namespace Encore::OS {

///@brief Prefix of shm region names for each ExecId scheduling directives
std::string const kLinuxShmMHProducerPrefix = "EncoreMHExecQueue-";

///@brief Prefix of shm region names for each Manager receiving directives
std::string const kLinuxShmMHConsumerPrefix = "EncoreMHManQueue-";


/**
 * @brief Backend of ManagerHub meant to be driven by an SPSCQueueExchanger
 *
 * @tparam tIsMainExchanger A boolean indicating if implementation will be used as IDirectiveExchanger
 * @tparam tNumExec The number of Directives possible in each producer ExecId queue
 * @tparam tNumMan The number of Directives possible in each consumer ManagerId queue
 */
template <bool tIsMainExchanger, std::uint32_t tNumExec, std::uint32_t tNumMan = tNumExec>
class LinuxShmQueueExchangeManagerHub final : public Sequencing::IManagerHub, public Sequencing::IDirectiveExchanger {

    using Exchanger = Utils::SPSCQueueExchanger<Sequencing::Directive,ExecId,Sequencing::ManagerId,tNumExec,tNumMan>;

public: /*IManagerHub*/

    /**
     * @brief Allows an execution context to contribute to ManagerHub scheduling
     *
     * @param pInit INIT_ONLY specifier
     * @param pId The id of the execution context to register for scheduling
     */
    ErrorHandling::ReturnCode registerExecId( InitOnly const & pInit, ExecId pId ) final {

        auto itr = std::find_if( mProdQs.begin(), mProdQs.end(), [pId](auto && pPr){ return pId == pPr.first; } );
        if( itr != mProdQs.end() ) { return {}; } //no need to error -- an ExecId could register multiple times

        std::string regionName = kLinuxShmMHProducerPrefix + std::to_string(pId);

        auto pr = mProdQs.emplace( std::piecewise_construct
                                 , std::forward_as_tuple( pId )
                                 , std::forward_as_tuple( pInit, kNotThreadSafe, regionName, true )
                                 );

        auto & [execId,region] = *(pr.first);

        if constexpr( not tIsMainExchanger ) {

            region.release();

            mExchanger.addProducer( pInit, pId, *region.data() );
        }

        return {};
    }

    /**
     * @brief Allows a manager to retrieve from the ManagerHub exchange
     *
     * @param pInit  INIT_ONLY specifier
     * @param pId The id of the manager to register for retrieval
     */
    ErrorHandling::ReturnCode registerManager( InitOnly const & pInit, Sequencing::ManagerId pId ) final {

        auto itr = std::find_if( mConsQs.begin(), mConsQs.end(), [pId](auto && pPr){ return pId == pPr.first; } );
        if( itr != mConsQs.end() ) {
            return EG_ERROR_CODE_WITH_PAYLOAD( Sequencing::SequencingErrors, duplicate_manager, pId );
        }

        unsigned id = pId;
        std::string regionName = kLinuxShmMHConsumerPrefix + std::to_string(mExecId) + "-" + std::to_string(id);

        auto pr = mConsQs.emplace( std::piecewise_construct
                                 , std::forward_as_tuple( pId )
                                 , std::forward_as_tuple( pInit, kNotThreadSafe, regionName, true )
                                 );

        auto & [manId,region] = *(pr.first);

        if constexpr( not tIsMainExchanger ) {

            region.release();

            mExchanger.addConsumer( pInit, pId, *region.data() );
        }

        mManagerMapping[pId] = mExecId;

        return {};
    }

    /**
     * @brief Sets the current ExecId of the hub
     *
     * @param pInit INIT ONLY behavior
     * @param pId   ExecId to set
     */
    ErrorHandling::ReturnCode setExecId( [[maybe_unused]] InitOnly const & pInit, ExecId pId ) final {
        mExecId = pId;
        return {};
    }

    /**
     * @brief Gets the current ExecId of the hub
     *
     * @param pInit INIT ONLY behavior
     */
    ExecId getExecId( [[maybe_unused]] InitOnly const & pInit ) final { return mExecId; }

    /**
     * @brief Trivially inits
     *
     * @param pInit INIT ONLY behavior
     */
    ErrorHandling::ReturnCode init( [[maybe_unused]] InitOnly const & pInit ) final {

        if constexpr( tIsMainExchanger ) {

            std::filesystem::path shmDir{"/dev/shm/"};

            for( auto const & entry : std::filesystem::directory_iterator{shmDir} ) {

                auto const & entryStr = entry.path().filename().string();

                if( entryStr.find( kLinuxShmMHProducerPrefix ) != std::string::npos ) {

                    auto execIdIndex = entryStr.find_first_of('-');

                    auto execId = static_cast<ExecId>( std::stoul( entryStr.substr(execIdIndex + 1) ) );

                    auto [itr,done] = mProdQs.try_emplace( execId, pInit, kNotThreadSafe, entryStr, false );

                    itr->second.inherit();

                    mExchanger.addProducer( pInit, execId, *(itr->second.data()) );
                }
                else if( entryStr.find( kLinuxShmMHConsumerPrefix ) != std::string::npos ) {

                    auto execIdIndex = entryStr.find_first_of('-');
                    auto manIdIndex = entryStr.find_last_of('-');

                    auto execId = static_cast<ExecId>( std::stoul( entryStr.substr(execIdIndex + 1, manIdIndex) ) );
                    auto manId = static_cast<Sequencing::ManagerId>( std::stoul( entryStr.substr(manIdIndex + 1) ) );

                    auto [itr,done] = mConsQs.try_emplace( manId, pInit, kNotThreadSafe, entryStr, false );

                    itr->second.inherit();

                    mExchanger.addConsumer( pInit, manId, *(itr->second.data()) );

                    mManagersPerExecId[execId].push_back(manId);
                }
            }

        }

        return {};
    }

    /**
     * @brief Schedules directive on an ExecId
     *
     * @param pId The ExecId who scheduled the directive
     * @param pDir The directive to schedule
     */
    ErrorHandling::ReturnCode schedule( ExecId pId, Sequencing::Directive pDir ) final {

        using namespace Encore::Utils;
        using namespace Encore::Sequencing;

        auto manId = pDir.destination;

        auto itr = mManagerMapping.find(manId);
        if( itr != mManagerMapping.end() and itr->second == pId ) {

            auto itr2 = mConsQs.find(manId);
            auto & destQueue = *(itr2->second.data()); //guaranteed to exist (NO ALLOC) via registerManager()

            if( not destQueue.write( pDir ) ) {
                return EG_ERROR_CODE_WITH_PAYLOAD( SequencingErrors, directive_not_scheduled, underCast(pDir.id) );
            }

            return {};
        }

        if( not mExchanger.receiveFromProducer( pId, pDir ) ) {
            return EG_ERROR_CODE_WITH_PAYLOAD( SequencingErrors, directive_not_scheduled, underCast(pDir.id) );
        }

        return {};
    }

    /**
     * @brief Retrieves directive destined for manager (via passed id)
     *
     * @param pId The id of manager who will retrieve the directive
     */
    ErrorHandling::Result<Utils::Maybe<Sequencing::Directive>> retrieve( Sequencing::ManagerId pId ) final {

        Sequencing::Directive out;

        if( not mExchanger.releaseToConsumer( pId, out ) ) {
            return Utils::Maybe<Sequencing::Directive>();
        }

        return Utils::Maybe<Sequencing::Directive>(out);
    }

public: /*IDirectiveExchanger*/

    /**
     * @brief Gets a span of all registered ManagerId for a given execution context ExecId
     *
     * @param pId The ExecId for which to retrieve all known ManagerId
     */
    Utils::Maybe<Utils::Span<Sequencing::ManagerId const>> getManagersForExecId( ExecId pId ) const final {

        auto itr = mManagersPerExecId.find(pId);
        if( itr == mManagersPerExecId.end() ) { return Utils::Maybe<Utils::Span<Sequencing::ManagerId const>>(); }

        auto const & vec = itr->second;

        return Utils::Span<Sequencing::ManagerId const>( vec.data(), vec.size() );
    }

    /**
     * @brief Moves relevant directives from execution context queue to manager queue
     *
     * @param pId ExecId from which to transfer
     * @param pManId Destination manager to receive directives
     */
    void exchange( ExecId pId, Sequencing::ManagerId pManId ) final {
        mExchanger.exchangeData( kNotThreadSafe, pId, pManId );
    }

private:

    Exchanger mExchanger;
    std::map<ExecId               ,LinuxShmRegion<typename Exchanger::ProducerQueue>> mProdQs;
    std::map<Sequencing::ManagerId,LinuxShmRegion<typename Exchanger::ConsumerQueue>> mConsQs;
    std::unordered_map<Sequencing::ManagerId,ExecId> mManagerMapping;
    std::unordered_map<ExecId,std::vector<Sequencing::ManagerId>> mManagersPerExecId;
    ExecId mExecId{kMainExecId};
};

}

#endif
