
#include "../LinuxProcQuerier.hpp"

#include "UnitTesting/UnitTester.hpp"
#include "UnitTesting/UnitTestConfig.hpp"
#include "UnitTesting/UnitTest.hpp"
#include "ErrorHandling/test/UnitTestExtensions.hpp"

#include "encore/Encore.hpp"
#include "Utils/IReaderWriter.hpp"
#include "OS/Linux/LinuxSystemStatQuerySerializer.hpp"
#include "OS/Linux/LinuxSystemStatQuery.hpp"
#include "OS/Linux/LinuxPidStatQuerySerializer.hpp"
#include "OS/Linux/LinuxPidStatQuery.hpp"
#include "OS/Linux/LinuxPidStatMQuerySerializer.hpp"
#include "OS/Linux/LinuxPidStatMQuery.hpp"
#include "Utils/ASCIISerializer.hpp"


namespace Encore::OS {

struct FakeProcQuery {
    std::uint32_t anInt;

    static constexpr std::string_view getFileName() { return "areallylongfilenamethatwouldnotexist"; }
    static constexpr LinuxProcQueryType qType() { return LinuxProcQueryType::process; }
};

struct FakeProcQuery2 {
    std::uint32_t anInt;

    static constexpr std::string_view getFileName() { return "stat"; }
    static constexpr LinuxProcQueryType qType() { return LinuxProcQueryType::system; }
};

inline bool operator==( LinuxPidStatQuery const & lhs, LinuxPidStatQuery const & rhs ) {
    return ( lhs.pid == rhs.pid && lhs.state == rhs.state && lhs.ppid == rhs.ppid && lhs.pgrp == rhs.pgrp &&
             lhs.session == rhs.session && lhs.tty_nr == rhs.tty_nr && lhs.tpgid == rhs.tpgid &&
             lhs.flags == rhs.flags && lhs.minflt == rhs.minflt && lhs.cminflt == rhs.cminflt &&
             lhs.majflt == rhs.majflt && lhs.cmajflt == rhs.cmajflt && lhs.utime == rhs.utime &&
             lhs.stime == rhs.stime && lhs.cutime == rhs.cutime && lhs.cstime == rhs.cstime &&
             lhs.priority == rhs.priority && lhs.nice == rhs.nice && lhs.num_threads == rhs.num_threads &&
             std::memcmp( &lhs.comm, &rhs.comm, sizeof(lhs.comm) ) == 0 );
}

inline bool operator==( LinuxPidStatMQuery const & lhs, LinuxPidStatMQuery const & rhs ) {
    return ( lhs.size == rhs.size && lhs.resident == rhs.resident && lhs.shared == rhs.shared );
}

inline bool operator==( LinuxSystemStatQuery const & lhs, LinuxSystemStatQuery const & rhs ) {
    return ( lhs.user == rhs.user && lhs.nice == rhs.nice && lhs.system == rhs.system && lhs.idle == rhs.idle &&
             lhs.iowait == rhs.iowait && lhs.irq == rhs.irq && lhs.softirq == rhs.softirq && lhs.steal == rhs.steal &&
             lhs.guest == rhs.guest && lhs.guest_nice == rhs.guest_nice &&
             std::memcmp( &lhs.cpu, &rhs.cpu, sizeof(lhs.cpu) ) == 0 );
}

}

namespace Encore::Utils {

///@brief Specialization of trait -- disallow trivial serialization for LinuxPidStatMQuery
template <>
struct EnableTrivialSerialization<OS::FakeProcQuery2> : std::false_type {};

/**
 * @brief Specialization for serializing and deserialization a LinuxPidStatMQuery struct
 *
 * @tparam tPolicy The policy to use for serialization
 */
template <SerializationPolicy tPolicy>
class Serializer<OS::FakeProcQuery2, tPolicy, std::enable_if_t<RequiresReflection<tPolicy>::value>> {
public:

    /**
     * @brief Decompose a LinuxPidStatMQuery to bytes
     *
     * @param pSource The query to decompose
     * @param pDestination The device to write to
     */
    ErrorHandling::Result<std::size_t> toBytes( OS::FakeProcQuery2 const & pSource, IReaderWriter & pDestination ) {

        return serializeCluster<tPolicy>( pDestination, pSource.anInt );
    }

    /**
     * @brief Compose a LinuxPidStatMQuery from bytes
     *
     * @param pSource The device to read from
     * @param pDest The query to compose
     */
    ErrorHandling::Result<std::size_t> toObj( IReaderWriter & pSource, OS::FakeProcQuery2 & pDest ) {

        return deserializeCluster<tPolicy>( pSource, pDest.anInt );
    }
};
}

namespace {

namespace test = Encore::UnitTesting;
using namespace Encore::ErrorHandling;
using namespace Encore::Utils;
using namespace Encore::OS;
using namespace Encore;


void linuxSystemStatQuery( test::UnitTestConfig const & pConfig ) {
    test::UnitTest<test::ErrorHandlingExtension> ut( pConfig );

    LinuxSystemStatQuery zeros{};

    ProcQuerier<LinuxSystemStatQuery> procQuery( kTestInitOnly );
    ut.failsWithCode( procQuery.query(), EG_ERROR_CODE(FileErrors, not_open), "can't read before opening" );
    ut.isSuccessful( procQuery.init( kTestInitOnly ), "query inits successfully" );

    auto qResult = procQuery.query();
    ut.isSuccessful( qResult, "Stats query success" );
    ut.notEqual( qResult.value(), zeros, "Stats are non zero" );
}

void linuxPidStatQuery( test::UnitTestConfig const & pConfig ) {
    test::UnitTest<test::ErrorHandlingExtension> ut( pConfig );

    LinuxPidStatQuery zeros{};

    ProcQuerier<LinuxPidStatQuery> procQuery( kTestInitOnly );
    ut.isSuccessful( procQuery.init( kTestInitOnly, 0 ), "query inits successfully" );

    auto qResult = procQuery.query();
    ut.isSuccessful( qResult, "Stats query success" );

    ProcQuerier<LinuxPidStatQuery> procQueryByPid( kTestInitOnly );
    ut.isSuccessful( procQueryByPid.init( kTestInitOnly, qResult.value().pid ), "query with pid inits successfully" );
    qResult = procQueryByPid.query();
    ut.notEqual( qResult.value(), zeros, "pid Stats also non zero" );
}

void linuxPidStatMQuery( test::UnitTestConfig const & pConfig ) {
    test::UnitTest<test::ErrorHandlingExtension> ut( pConfig );

    LinuxPidStatMQuery zeros{};

    ProcQuerier<LinuxPidStatMQuery> procQuery( kTestInitOnly );
    ut.isSuccessful( procQuery.init( kTestInitOnly, 0, 80 ), "query inits successfully" );

    auto qResult = procQuery.query();
    ut.isSuccessful( qResult, "Stats query success" );

    ProcQuerier<LinuxPidStatQuery> procStatQuery( kTestInitOnly );
    ut.isSuccessful( procStatQuery.init( kTestInitOnly ), "pid query inits successfully" );
    auto qResultStat = procStatQuery.query();
    ut.isSuccessful( qResultStat, "pid Stats query success" );

    ProcQuerier<LinuxPidStatMQuery> procQueryByPid( kTestInitOnly );
    ut.isSuccessful( procQueryByPid.init( kTestInitOnly, qResultStat.value().pid ), "query by pid inits successfully" );
    qResult = procQueryByPid.query();
    ut.notEqual( qResult.value(), zeros, "Stats by pid don't change" );
}

void linuxProcQueryBadDeserialize( test::UnitTestConfig const & pConfig ) {
    test::UnitTest<test::ErrorHandlingExtension> ut( pConfig );

    ProcQuerier<FakeProcQuery2> procQuery( kTestInitOnly );
    ut.isSuccessful( procQuery.init( kTestInitOnly, 0, 80 ), "query inits successfully" );
    ut.failsWithCode( procQuery.query(), EG_ERROR_CODE(StdFswErrors, range_error), "can't read before opening" );
}

void linuxProcQueryBadInit( test::UnitTestConfig const & pConfig ) {
    test::UnitTest<test::ErrorHandlingExtension> ut( pConfig );

    ProcQuerier<FakeProcQuery> procQuery( kTestInitOnly );
    ut.failsWithCode( procQuery.init( kTestInitOnly, std::numeric_limits<pid_t>::max() )
                                    , EG_ERROR_CODE(ErrorHandling::StdFswErrors, length_error)
                                    , "query fails init" );

    ProcQuerier<FakeProcQuery> procQuery2( kTestInitOnly );
    ut.failsWithCode( procQuery2.init( kTestInitOnly )
                                    , EG_ERROR_CODE(ErrorHandling::StdFswErrors, length_error)
                                    , "query fails init" );
}

}


UNITTESTER_MAIN( test_LinuxProcQuerier, int pArgc, char const** pArgv ) {

    test::UnitTester  tester( pArgc, pArgv );

    std::vector<test::TestDescriptor> tests;

    tests.push_back( { test::TestTypeID::micro, "LinuxSystemStatQuery", &linuxSystemStatQuery } );
    tests.push_back( { test::TestTypeID::micro, "LinuxPidStatQuery", &linuxPidStatQuery } );
    tests.push_back( { test::TestTypeID::micro, "LinuxPidStatMQuery", &linuxPidStatMQuery } );
    tests.push_back( { test::TestTypeID::micro, "LinuxProcQueryBadDeserialize", &linuxProcQueryBadDeserialize } );
    tests.push_back( { test::TestTypeID::micro, "LinuxProcQueryBadInit", &linuxProcQueryBadInit } );

    return tester.filterAndExec( tests );

}
