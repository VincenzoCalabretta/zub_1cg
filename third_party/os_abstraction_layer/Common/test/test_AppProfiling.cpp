
#include "../AppProfiling.hpp"
#include "../AppProfilingSerializers.hpp"

#include <cstdint>
#include <cstdio>

#include <string_view>

#include "encore/ErrorHandling/test/UnitTestExtensions.hpp"
#include "encore/UnitTesting/UnitTestingCore.hpp"

namespace {

namespace test = Encore::UnitTesting;
using namespace Encore;
using namespace Encore::OS;
using namespace Encore::Utils;


void execTypes( test::UnitTestConfig const & pConfig ) {

    test::UnitTest<test::ErrorHandlingExtension> ut{ pConfig };

    using namespace std::string_view_literals;

    ut.areEqual(MemInfo::getType(), "encore_OS_MemInfo", "MemInfo Custom Type Def" );
    ut.areEqual(ProcessInfo::getType(), "encore_OS_ProcessInfo", "ProcessInfo Custom Type Def" );
}

void memInfoSerializer( test::UnitTestConfig const & pConfig ) {

    test::UnitTest<test::ErrorHandlingExtension> ut{ pConfig };

    MemInfo mem{32, 132, 232};
    Utils::Serializer<MemInfo, Utils::SerializationPolicy::packed_big_endian> seri;
    std::array<EncoreByte, 24> buf;
    BytesRangeBuffer brb( buf );
    ut.isSuccessful( seri.toBytes( mem, brb ), "Serialize MemInfo" );
    ut.areEqual( buf, {0, 0, 0, 0, 0, 0, 0, 32, 0, 0, 0, 0, 0, 0, 0, 132, 0, 0, 0, 0, 0, 0, 0, 232 }, "bytes match" );

    MemInfo memOut{};
    ut.isSuccessful( seri.toObj( brb, memOut ), "Deserialize MemInfo" );
    ut.areEqual( mem.size, memOut.size, "Size matches" );
    ut.areEqual( mem.resident, memOut.resident, "Resident size matches" );
    ut.areEqual( mem.shared, memOut.shared, "Shared size matches" );
}

void processInfoSerializer( test::UnitTestConfig const & pConfig ) {

    test::UnitTest<test::ErrorHandlingExtension> ut{ pConfig };

    ProcessInfo proc{32., 99., 4};
    Utils::Serializer<ProcessInfo, Utils::SerializationPolicy::packed_big_endian> seri;
    std::array<EncoreByte, 20> buf;
    BytesRangeBuffer brb( buf );
    ut.isSuccessful( seri.toBytes( proc, brb ), "Serialize ProcessInfo" );
    ut.areEqual( buf, {0x40, 0x40, 0, 0, 0, 0, 0, 0, 0x40, 0x58, 0xC0, 0, 0, 0, 0, 0, 0, 0, 0, 4 }, "bytes match" );

    ProcessInfo procOut{};
    ut.isSuccessful( seri.toObj( brb, procOut ), "Deserialize ProcessInfo" );
    ut.areEqual( proc.cpuUsage, procOut.cpuUsage, "cpuUsage matches" );
    ut.areEqual( proc.systemUsage, procOut.systemUsage, "systemUsage matches" );
    ut.areEqual( proc.threads, procOut.threads, "threads matches" );
}

}

UNITTESTER_MAIN( test_Exec, int pArgc, char const** pArgv ) {

    test::UnitTester tester( pArgc, pArgv );

    return tester.filterAndExec( {
        { test::TestTypeID::misc, "ExecTypes", &execTypes },
        { test::TestTypeID::misc, "MemInfoSerializer", &memInfoSerializer },
        { test::TestTypeID::misc, "ProcessInfoSerializer", &processInfoSerializer },
    } );
}
