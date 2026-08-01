
// NOLINTBEGIN(cppcoreguidelines-macro-usage) -- overriding system calls for testing purposes
#define pthread_getschedparam mock_getschedparam
#define sched_get_priority_min mock_get_priority_min
#define sched_get_priority_max mock_get_priority_max
#define pthread_setschedprio mock_setschedprio
#define pthread_getaffinity_np mock_getaffinity_np
#define pthread_setaffinity_np mock_setaffinity_np
// NOLINTEND(cppcoreguidelines-macro-usage)
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#include "../LinuxThreadCustomization.hpp"

#include <pthread.h>

#include <sched.h>
#include <csignal>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "encore/ErrorHandling/test/UnitTestExtensions.hpp"
#include "encore/UnitTesting/UnitTest.hpp"
#include "encore/UnitTesting/UnitTestConfig.hpp"
#include "encore/UnitTesting/UnitTester.hpp"

namespace Encore::UnitTesting {

struct MockLinuxPthread {
    std::function<int()>
        getSchedParamImpl{ []() noexcept { return 0; } };
    std::function<int()> schedGetPriorityMinImpl{ []() noexcept { return 0; } };
    std::function<int()> schedGetPriorityMaxImpl{ []() noexcept { return 99; } };
    std::function<int()> setSchedPrioImpl{ []() noexcept { return 0; } };

    std::function<int(cpu_set_t*)> getAffinityNpImpl{ []( cpu_set_t* ) noexcept { return 0; } };
    std::function<int(const cpu_set_t*)> setAffinityNpImpl{ []( const cpu_set_t* ) noexcept { return 0; } };

    void reset() {
        getSchedParamImpl = []() noexcept { return 0; };
        schedGetPriorityMinImpl = []() noexcept { return 0; };
        schedGetPriorityMaxImpl = []() noexcept { return 99; };
        setSchedPrioImpl = []() noexcept { return 0; };

        getAffinityNpImpl = [](cpu_set_t*) noexcept { return 0; };
        setAffinityNpImpl = [](const cpu_set_t*) noexcept { return 0; };
    }
};

static inline MockLinuxPthread gMockPthread{};
}

extern "C" int mock_getschedparam( pthread_t, int*, struct sched_param* ) noexcept {
    return Encore::UnitTesting::gMockPthread.getSchedParamImpl();
}

extern "C" int mock_get_priority_min( int ) noexcept {
    return Encore::UnitTesting::gMockPthread.schedGetPriorityMinImpl();
}

extern "C" int mock_get_priority_max( int ) noexcept {
    return Encore::UnitTesting::gMockPthread.schedGetPriorityMaxImpl();
}

extern "C" int mock_setschedprio( pthread_t, int ) noexcept {
    return Encore::UnitTesting::gMockPthread.setSchedPrioImpl();
}

extern "C" int mock_getaffinity_np( pthread_t, size_t, cpu_set_t* cpu_set ) noexcept {
    return Encore::UnitTesting::gMockPthread.getAffinityNpImpl( cpu_set );
}

extern "C" int mock_setaffinity_np( pthread_t, size_t, const cpu_set_t* cpu_set ) noexcept {
    return Encore::UnitTesting::gMockPthread.setAffinityNpImpl( cpu_set );
}

namespace {

namespace test = Encore::UnitTesting;
using namespace Encore;
using namespace Encore::OS;


void oneCoreChildThread( test::UnitTestConfig const & pConfig ) {

    test::UnitTest<test::ErrorHandlingExtension> ut ( pConfig );

    test::gMockPthread.reset();

    cpu_set_t affinity;
    test::gMockPthread.setAffinityNpImpl = [&]( const cpu_set_t* cpu_set ) noexcept { affinity = *cpu_set; return 0; };
    test::gMockPthread.getAffinityNpImpl = [&]( cpu_set_t* cpu_set ) noexcept { *cpu_set = affinity; return 0; };

    sched_param sch;
    sch.sched_priority = 0;
    sched_setscheduler( 0, SCHED_RR, &sch );
    LinuxThreadCustomization threadCustomizer{};

    std::atomic<bool> flag{false};

    std::thread t1( [&flag](){
                                  std::this_thread::sleep_for( std::chrono::milliseconds( 125 ) );
                                  while( not flag.load() ) { std::this_thread::yield(); }
                             } );

    ut.isSuccessful( threadCustomizer.setCPUAffinity( 0, true, &t1 ) );
    ut.isSuccessful( threadCustomizer.setPriority( 10, &t1 ) );
    auto result = threadCustomizer.getCPUAffinity( 0, &t1 );
    ut.isSuccessful ( result, "CPU0 success check - t1" );
    ut.areEqual( true, result.value(), "CPU0 affinity is set - t1" );
    for ( unsigned int i = 1; i < std::thread::hardware_concurrency(); ++i ) {
        std::string cpuStr = "CPU" + std::to_string( i );
        result = threadCustomizer.getCPUAffinity( i, &t1 );
        ut.isSuccessful ( result, cpuStr + "success check - t1" );
        ut.areEqual( false, result.value(), cpuStr + "affinity is not set - t1" );
    }

    std::thread t2( [&flag](){ std::this_thread::sleep_for( std::chrono::milliseconds(250) ); flag.store( true ); } );

    ut.isSuccessful( threadCustomizer.setCPUAffinity( 0, true, &t2 ) );
    ut.isSuccessful( threadCustomizer.setPriority( 20, &t2 ) );
    result = threadCustomizer.getCPUAffinity( 0, &t2 );
    ut.isSuccessful ( result, "CPU0 success check - t2" );
    ut.areEqual( true, result.value(), "CPU0 affinity is set - t2" );
    for ( unsigned int i = 1; i < std::thread::hardware_concurrency(); ++i ) {
        std::string cpuStr = "CPU" + std::to_string( i );
        result = threadCustomizer.getCPUAffinity( i, &t1 );
        ut.isSuccessful ( result, cpuStr + "success check - t2" );
        ut.areEqual( false, result.value(), cpuStr + "affinity is not set - t2" );
    }

    t1.join();
    t2.join();
}

void oneCoreCurThread( test::UnitTestConfig const & pConfig ) {

    test::UnitTest<test::ErrorHandlingExtension> ut ( pConfig );

    test::gMockPthread.reset();

    LinuxThreadCustomization threadCustomizer{};

    cpu_set_t affinity;
    test::gMockPthread.setAffinityNpImpl = [&]( const cpu_set_t* cpu_set ) noexcept { affinity = *cpu_set; return 0; };
    test::gMockPthread.getAffinityNpImpl = [&]( cpu_set_t* cpu_set ) noexcept { *cpu_set = affinity; return 0; };

    ut.isSuccessful( threadCustomizer.setCPUAffinity( 0, true ) );
    ut.isSuccessful( threadCustomizer.setPriority( 10 ) );

    auto result = threadCustomizer.getCPUAffinity( 0 );
    ut.isSuccessful ( result, "CPU0 success check" );
    ut.areEqual( true, result.value(), "CPU0 affinity is set" );
    for ( unsigned int i = 1; i < std::thread::hardware_concurrency(); ++i ) {
        std::string cpuStr = "CPU" + std::to_string( i );
        result = threadCustomizer.getCPUAffinity( i );
        ut.isSuccessful ( result, cpuStr + "success check" );
        ut.areEqual( false, result.value(), cpuStr + "affinity is not set" );
    }
}

void setPriorityFailures( test::UnitTestConfig const & pConfig ) {

    test::UnitTest<test::ErrorHandlingExtension> ut ( pConfig );

    test::gMockPthread.reset();

    LinuxThreadCustomization threadCustomizer{};

    test::gMockPthread.setSchedPrioImpl = []() noexcept { return 1; };
    ut.notSuccessful( threadCustomizer.setPriority( 10 ) );

    test::gMockPthread.schedGetPriorityMinImpl = []() noexcept { return -1; };
    ut.notSuccessful( threadCustomizer.setPriority( 10 ) );

    test::gMockPthread.schedGetPriorityMaxImpl = []() noexcept { return -1; };
    ut.notSuccessful( threadCustomizer.setPriority( 10 ) );

    test::gMockPthread.schedGetPriorityMinImpl = []() noexcept { return 0; };
    ut.notSuccessful( threadCustomizer.setPriority( 10 ) );

    test::gMockPthread.getSchedParamImpl = []() noexcept { return 1; };
    ut.notSuccessful( threadCustomizer.setPriority( 10 ) );

}

void CPUAffinityFailures( test::UnitTestConfig const & pConfig ) {

    test::UnitTest<test::ErrorHandlingExtension> ut ( pConfig );

    test::gMockPthread.reset();

    LinuxThreadCustomization threadCustomizer{};

    test::gMockPthread.setAffinityNpImpl = []( const cpu_set_t* ) noexcept { return 1; };
    ut.notSuccessful( threadCustomizer.setCPUAffinity( ThreadAttributes::CPUAffinity{1}, false ) );

    test::gMockPthread.getAffinityNpImpl = []( cpu_set_t* ) noexcept { return 1; };
    ut.notSuccessful( threadCustomizer.setCPUAffinity( ThreadAttributes::CPUAffinity{1}, true ) );
    ut.notSuccessful( threadCustomizer.getCPUAffinity( ThreadAttributes::CPUAffinity{1} ) );

}

}

UNITTESTER_MAIN( test_LinuxThreadCustomization, int pArgc, char const** pArgv ) {

    test::UnitTester tester( pArgc, pArgv );

    return tester.filterAndExec( { { test::TestTypeID::micro, "OneCoreChildThread", &oneCoreChildThread }
                                 , { test::TestTypeID::micro, "OneCoreCurThread", &oneCoreCurThread }
                                 , { test::TestTypeID::micro, "SetPriorityFailures", &setPriorityFailures }
                                 , { test::TestTypeID::micro, "CPUAffinityFailures", &CPUAffinityFailures }
                                 } );


}
