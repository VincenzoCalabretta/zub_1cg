
#include "../TCPClientSocketFactory.hpp"

#include "encore/UnitTesting/UnitTestingCore.hpp"
#include "encore/ErrorHandling/test/UnitTestExtensions.hpp"

namespace {

namespace test = Encore::UnitTesting;
using namespace Encore;

class NoOpStreamSocket final : public OS::IStreamSocket {};

class NoOpTCPClient final : public OS::ITCPClient {};

class NotConstructibleSocket final : public OS::IStreamSocket, public OS::ITCPClient {
public:

    NotConstructibleSocket() = delete;

    NotConstructibleSocket(int);
};

}

UNITTESTER_MAIN( test_TCPClientSocketFactoryStatic, int, char const ** ) {

#ifdef NOT_ITCPCLIENT
    OS::FixedTCPClientSocketFactory<NoOpStreamSocket, 3> factory;
#endif
    
#ifdef NOT_ISTREAMSOCKET
    OS::FixedTCPClientSocketFactory<NoOpTCPClient, 3> factory;
#endif
    
#ifdef NOT_DEFAULT_CONSTRUCTIBLE
    OS::FixedTCPClientSocketFactory<NotConstructibleSocket, 3> factory;
#endif

    return 0;
}

