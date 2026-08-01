
#include "OS/Linux/LinuxProcQuerier.hpp"

#include "UnitTesting/UnitTestingCore.hpp"

using namespace Encore;
using namespace Encore::OS;


struct NotQuery {

    int x{0};
};


//no-op test for static assertion
UNITTESTER_MAIN( test_LinuxProcQuerier, int, char const** ) {

#ifdef IS_LINUX_PROC_QUERY
    ProcQuerier<NotQuery> lpq( kTestInitOnly );
#endif

    return 0;
}
