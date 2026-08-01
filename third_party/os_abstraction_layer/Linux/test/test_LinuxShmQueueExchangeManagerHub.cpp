
#include "OS/Linux/LinuxShmQueueExchangeManagerHub.hpp"

#include <wait.h>

#include "UnitTesting/UnitTestingCore.hpp"
#include "ErrorHandling/ErrorGroup.hpp"
#include "ErrorHandling/test/UnitTestExtensions.hpp"
#include "Sequencing/Directive.hpp"


namespace {

namespace test = Encore::UnitTesting;
using namespace Encore;
using namespace Encore::ErrorHandling;
using namespace Encore::Sequencing;
using namespace Encore::OS;

void basicAPI( test::UnitTestConfig const & pConfig ) {

    test::MixinUnitTest<test::ErrorHandlingExtension> ut(pConfig);

    LinuxShmRegion<int> otherRegion(kTestInitOnly,kNotThreadSafe,"test-region",true);
    (void)otherRegion; //just needed to create it

    LinuxShmQueueExchangeManagerHub<true,10> hub;

    ut.isSuccessful( hub.setExecId(kTestInitOnly,ExecId{1}), "SetExecId" );
    ut.areEqual( hub.getExecId(kTestInitOnly), ExecId{1}, "GetExecId" );

    ut.isSuccessful( hub.registerExecId(kTestInitOnly,ExecId{1}), "RegisterExecId" );
    ut.isSuccessful( hub.registerExecId(kTestInitOnly,ExecId{1}), "Duplicate registerExecId ok" );

    ManagerId man{0};
    ut.isSuccessful( hub.registerManager(kTestInitOnly, man), "RegisterManager" );

    ut.isSuccessful( hub.init(kTestInitOnly), "Init" );

    auto mansOn1 = hub.getManagersForExecId( ExecId{1} );
    ut.assertTrue( mansOn1.hasValue(), "We have managers on context 1..." );
    ut.areEqual( mansOn1.value().size(), 1u, "...we have 1 manager..." );
    ut.areEqual( mansOn1.value()[0], ManagerId{0}, "...and the id is expected" );

    ut.assertFalse( hub.getManagersForExecId( ExecId{2} ).hasValue(), "No managers on context 2" );

    ut.isSuccessful( hub.schedule(ExecId{1},Directive{man}), "Schedule" );

    auto maybeDir = hub.retrieve(man);
    ut.isSuccessful( maybeDir, "Retrieve..." );
    ut.assertTrue( maybeDir.value().hasValue(), "...has value" );
}


void scheduleCases( test::UnitTestConfig const & pConfig ) {

    test::MixinUnitTest<test::ErrorHandlingExtension> ut(pConfig);

    LinuxShmQueueExchangeManagerHub<true,2> hub;

    ut.isSuccessful( hub.registerExecId(kTestInitOnly,ExecId{1}), "ExecId1" );
    ut.isSuccessful( hub.registerExecId(kTestInitOnly,ExecId{2}), "ExecId2" );

    ut.isSuccessful( hub.setExecId(kTestInitOnly,ExecId{1}), "E1" );
    ut.isSuccessful( hub.registerManager(kTestInitOnly,ManagerId{1}), "Man1" );

    ut.isSuccessful( hub.setExecId(kTestInitOnly,ExecId{2}), "E2" );
    ut.isSuccessful( hub.registerManager(kTestInitOnly,ManagerId{2}), "Man2" );

    ut.isSuccessful( hub.init(kTestInitOnly), "Init" );

    ut.isSuccessful( hub.schedule(ExecId{1},Directive{ManagerId{1}}), "Schedule directive1 for Man1 from Exec1" );
    ut.isSuccessful( hub.schedule(ExecId{1},Directive{ManagerId{1}}), "Schedule directive2 for Man1 from Exec1" );
    ut.isSuccessful( hub.schedule(ExecId{1},Directive{ManagerId{2}}), "Schedule directive1 for Man2 from Exec1" );
    ut.isSuccessful( hub.schedule(ExecId{1},Directive{ManagerId{2}}), "Schedule directive2 for Man2 from Exec1" );

    ut.failsWithCode( hub.schedule(ExecId{1},Directive{ManagerId{1}})
                    , buildCode<SequencingErrors>(SequencingErrors::directive_not_scheduled)
                    , "Cannot schedule directive3 for Man1 from Exec1" );

    ut.failsWithCode( hub.schedule(ExecId{1},Directive{ManagerId{2}})
                    , buildCode<SequencingErrors>(SequencingErrors::directive_not_scheduled)
                    , "Cannot schedule directive3 for Man2 from Exec1" );

    hub.exchange( ExecId{1}, ManagerId{2} ); //moves directives from execid1 queue to manager2 queue

    auto dir = hub.retrieve( ManagerId{1} );
    ut.assertTrue( dir.value().hasValue(), "Dir 1 for Man1" );
    dir = hub.retrieve( ManagerId{1} );
    ut.assertTrue( dir.value().hasValue(), "Dir 2 for Man1" );
    dir = hub.retrieve( ManagerId{1} );
    ut.isSuccessful( dir, "Dir 3 successful ..." );
    ut.assertFalse( dir.value().hasValue(), "... but no more dir for Man1" );

    dir = hub.retrieve( ManagerId{2} );
    ut.assertTrue( dir.value().hasValue(), "Dir 1 for Man2" );
    dir = hub.retrieve( ManagerId{2} );
    ut.assertTrue( dir.value().hasValue(), "Dir 2 for Man2" );

    ut.isSuccessful( hub.schedule(ExecId{1},Directive{ManagerId{1}}), "Schedule directive3 for Man1 from Exec1" );
    ut.isSuccessful( hub.schedule(ExecId{1},Directive{ManagerId{2}}), "Schedule directive3 for Man2 from Exec1" );
    ut.isSuccessful( hub.schedule(ExecId{1},Directive{ManagerId{2}}), "Schedule directive4 for Man2 from Exec1" );

    ut.failsWithCode( hub.schedule(ExecId{1},Directive{ManagerId{3}})
                    , buildCode<SequencingErrors>(SequencingErrors::directive_not_scheduled)
                    , "Cannot schedule directive with full queue -- branch hit on bypass to direct consumer" );
}


void multiProcess( test::UnitTestConfig const & pConfig ) {

    auto mainHub = std::make_unique<LinuxShmQueueExchangeManagerHub<true,2>>();

    auto pid = ::fork();

    if( pid == -1 ) {

        throw std::runtime_error("Couldn't fork 1");
    }
    else if( pid == 0 ) {

        test::UnitTest<test::ErrorHandlingExtension> ut( "LinuxShmRegion::Micro::Child", pConfig );

        LinuxShmQueueExchangeManagerHub<false,2> procHub;

        ut.isSuccessful( procHub.registerExecId(kTestInitOnly,ExecId{1}), "Register Exec1" );

        ut.isSuccessful( procHub.setExecId(kTestInitOnly,ExecId{1}), "Set Exec1" );

        ut.isSuccessful( procHub.registerManager(kTestInitOnly,ManagerId{1}), "Register Manager1" );

        ut.isSuccessful( procHub.schedule( ExecId{1}, Directive{ManagerId{1}} ), "Good schedule 1" );
        ut.isSuccessful( procHub.schedule( ExecId{1}, Directive{ManagerId{2}} ), "Good schedule 2" );

        auto dir = procHub.retrieve( ManagerId{1} );
        ut.isSuccessful( dir, "Good retrieve man1/exec1" );
        ut.assertTrue( dir.value().hasValue(), "Actual directive retrieved" );

        dir = procHub.retrieve( ManagerId{1} );
        ut.isSuccessful( dir, "Another good retrieve man1/exec1" );
        ut.assertFalse( dir.value().hasValue(), "No more directives for man1/exec1" );
    }
    else {

        test::UnitTest<test::ErrorHandlingExtension> ut( "LinuxShmRegion::Micro::Parent", pConfig );

        int wstatus{0};
        ::wait(&wstatus);

        ut.isSuccessful( mainHub->registerExecId(kTestInitOnly,ExecId{2}), "Register Exec2" );

        ut.isSuccessful( mainHub->setExecId(kTestInitOnly,ExecId{2}), "Set Exec2" );
        ut.isSuccessful( mainHub->registerManager(kTestInitOnly,ManagerId{2}), "Register Manager2" );

        auto code = mainHub->init(kTestInitOnly);
        ut.isSuccessful( code, "Good init" );

        auto dir = mainHub->retrieve( ManagerId{2} );
        ut.isSuccessful( dir, "Good retrieve man2/exec2" );
        ut.assertFalse( dir.value().hasValue(), "No directive retrieved yet" );

        mainHub->exchange( ExecId{1}, ManagerId{2} );

        dir = mainHub->retrieve( ManagerId{2} );
        ut.isSuccessful( dir, "Another good retrieve man2/exec2" );
        ut.assertTrue( dir.value().hasValue(), "Directive retrieved after exchange" );
    }
}


void nonMainCoverage( test::UnitTestConfig const & pConfig ) {

    auto ut = std::make_unique<test::UnitTest<test::ErrorHandlingExtension>>(pConfig);

    LinuxShmQueueExchangeManagerHub<true,2> mainHub;

    LinuxShmQueueExchangeManagerHub<false,2> procHub;

    ut->isSuccessful( procHub.registerExecId( kTestInitOnly, ExecId{1} ), "RegisterExecId" );
    ut->isSuccessful( procHub.registerManager(kTestInitOnly, ManagerId{1} ), "RegisterManager" );

    ut->isSuccessful( mainHub.init(kTestInitOnly), "Init Main" );
}


void duplicateManager( test::UnitTestConfig const & pConfig ) {

    test::MixinUnitTest<test::ErrorHandlingExtension> ut(pConfig);

    LinuxShmQueueExchangeManagerHub<true,10> hub;

    ManagerId man{0};
    ut.isSuccessful( hub.registerManager(kTestInitOnly, man), "RegisterManager" );

    ut.failsWithCode( hub.registerManager(kTestInitOnly, man)
                    , buildCode<SequencingErrors>(SequencingErrors::duplicate_manager)
                    , "Cannot register a manager twice" );
}

}

UNITTESTER_MAIN( test_LinuxShmQueueExchangeManagerHub, int pArgc, char const** pArgv ) {

    test::UnitTester tester( pArgc, pArgv );

    return tester.filterAndExec( { { test::TestTypeID::micro, "BasicAPI", &basicAPI }
                                 , { test::TestTypeID::micro, "ScheduleCases", &scheduleCases }
                                 , { test::TestTypeID::misc, "NonMainCoverage", &nonMainCoverage }
                                 , { test::TestTypeID::misc, "DuplicateManager", &duplicateManager }
                                 , { test::TestTypeID::isolated, "MultiProcess", &multiProcess }
                                 } );
}
