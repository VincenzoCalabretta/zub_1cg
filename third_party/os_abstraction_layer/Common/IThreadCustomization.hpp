
///
/// @file
/// @brief Declares interface for a std::thread customization
///
#ifndef ENCORE_OS_IThreadCustomization_HPP
#define ENCORE_OS_IThreadCustomization_HPP

#include <cstdint>
#include <thread>

#include "encore/Encore.hpp"
#include "encore/ErrorHandling/Result.hpp"
#include "encore/ErrorHandling/ErrorHandling.hpp"


namespace Encore::OS {

//forward declaration
class IThreadCustomization;


/**
 * @brief Aggregate of thread customization info and attributes
 */
struct ThreadAttributes {

    typedef std::int32_t Priority; ///<@brief Convenience typedef for Thread Priority
    typedef std::uint32_t CPUAffinity; ///<@brief Convenience typedef for Thread CPU Affinity

    static constexpr Priority kDefaultPriority{50}; ///<@brief Default thread priority

    IThreadCustomization * customizer{nullptr}; ///<@brief Abstraction to allow for setting of thread attributes
    Priority priority{kDefaultPriority}; ///<@brief Desired priority of thread
    CPUAffinity cpuAffinity{0}; ///<@brief Desired CPU affinity of thread
};


/**
 * @brief Interface that allows for customization of running threads (priority, affinity, etc)
 *
 * @details Interface is meant to span multiple OSs - must remain simple
 */
class IThreadCustomization {
public:

    ///@brief Destructor
    virtual ~IThreadCustomization() = default;

    ///@brief Assigns numeric priority to a thread
    virtual ErrorHandling::ReturnCode setPriority( ThreadAttributes::Priority, std::thread * ) =0;

    ///@brief Associates the thread with a numbered cpu - boolean specifies whether to clear set or not
    virtual ErrorHandling::ReturnCode setCPUAffinity( ThreadAttributes::CPUAffinity, bool, std::thread * ) =0;

    ///@brief Retrieves if thread has cpu affinity for a given CPU
    virtual ErrorHandling::Result<bool> getCPUAffinity( ThreadAttributes::CPUAffinity, std::thread * ) =0;
};


/**
 * @brief A do-nothing level of thread customization
 */
class NoOpThreadCustomization final : public IThreadCustomization {
public:

    ///@brief Does nothing
    ErrorHandling::ReturnCode setPriority( ThreadAttributes::Priority, std::thread * ) final { return {}; }

    ///@brief Does nothing
    ErrorHandling::ReturnCode setCPUAffinity( ThreadAttributes::CPUAffinity, bool, std::thread * ) final { return {}; }

    ///@brief Does nothing
    ErrorHandling::Result<bool> getCPUAffinity( ThreadAttributes::CPUAffinity, std::thread * ) final { return true; }
};

}

#endif
