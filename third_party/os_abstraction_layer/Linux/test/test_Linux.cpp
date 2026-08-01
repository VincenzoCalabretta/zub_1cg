
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>

#include "encore/OS/Linux/Linux.hpp"

#include "encore/UnitTesting/UnitTestingCore.hpp"
#include "encore/ErrorHandling/test/UnitTestExtensions.hpp"

namespace {

namespace test = Encore::UnitTesting;
using namespace Encore;
using namespace Encore::ErrorHandling;

class SillyFD final : public OS::LinuxDescriptable {
public:

    using OS::LinuxDescriptable::setFd;
};

void micro( test::UnitTestConfig const & pConfig ) {

    test::UnitTest<test::ErrorHandlingExtension> ut{ pConfig };

    SillyFD desc;
    ut.areEqual( desc.getFd(), -1, "Initial FD" );
    
    desc.setFd( 1 );
    ut.areEqual( desc.getFd(), 1, "FD after first set" );
    
    ut.failsWithCode( desc.poll( 0, POLLOUT ), buildCode<SystemErrors>( SystemErrors::not_a_socket ), "Poll fails" );
    
    // A FD of -1 will always timeout per the workings of poll
    desc.setFd( -1 );
    ut.areEqual( desc.getFd(), -1, "FD after second set" );
    ut.succeedsWithValue( desc.poll( 0, POLLOUT ), false, "Poll times out" );
    
    int fd = ::socket( AF_INET, SOCK_DGRAM, 0 );
    desc.setFd( fd );
    ut.areEqual( desc.getFd(), fd, "FD after third set" );
    ut.succeedsWithValue( desc.poll( 0, POLLOUT ), true, "Dgram socket is ready" );
    
    ::close( fd );
    
#ifndef ENCORE_DYNAMIC_ANALYSIS_TYPE_VALGRIND // Valgrind catches this intentional FD exhaustion that we are trying to invoke
    rlimit limits;
    ::getrlimit( RLIMIT_NOFILE, &limits );    
    desc.setFd( static_cast<int>( limits.rlim_cur ) );
    ut.failsWithCode( desc.poll( 0, POLLOUT ), buildCode<SystemErrors>( SystemErrors::bad_file_descriptor ), "Induced a poll error" );
#endif
}

}

UNITTESTER_MAIN( test_Linux, int pArgc, char const ** pArgv ) {

    test::UnitTester tester( pArgc, pArgv );

    return tester.filterAndExec( { { test::TestTypeID::micro, "micro", &micro }, } );
}
