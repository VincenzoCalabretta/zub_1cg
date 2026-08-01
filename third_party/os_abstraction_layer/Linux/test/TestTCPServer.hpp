
#ifndef ENCORE_UNITTESTING_TestTCPServer_HPP
#define ENCORE_UNITTESTING_TestTCPServer_HPP

#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>

#include <errno.h>

#include "encore/Encore.hpp"
#include "encore/OS/Common/IPv4Address.hpp"

namespace Encore::UnitTesting {

class TestTCPServer {
public:

    TestTCPServer( OS::IPAndPort pAddress ) {
        mAddr.sin_family      = AF_INET;
        mAddr.sin_port        = ::htons( pAddress.port );
        mAddr.sin_addr.s_addr = ::htonl( pAddress.addr.asInteger() );
    }

    int start() {

        mFd = ::socket( AF_INET, SOCK_STREAM, 0 );

        if( mFd < 0 ) { return errno; }

        int const opt{1};
        auto rc = ::setsockopt( mFd, IPPROTO_IP, TCP_NODELAY, &opt, sizeof(opt) );
        if( rc != 0 ) { return errno; }

        rc = ::bind( mFd, reinterpret_cast<sockaddr const *>(&mAddr), sizeof(mAddr) );
        if( rc != 0 ) { return errno; }

        rc = ::listen( mFd, 1 );
        if( rc != 0 ) { return errno; }

        socklen_t len{ sizeof(mAddr) };
        rc = ::getsockname( mFd, reinterpret_cast<sockaddr *>(&mAddr), &len );
        if( rc != 0 ) { return errno; }

        return 0;
    }

    int stop() {
        auto rc = ::close( mFd );
        mFd = -1;
        return rc;
    }

    int accept() {

        sockaddr_in addr{};
        socklen_t len{ sizeof(addr) };

        auto acceptedFd = ::accept( mFd, reinterpret_cast<sockaddr *>(&addr), &len );
        if( acceptedFd < 0 ) {
            return errno;
        }

        return acceptedFd;
    }

    int port() const {
        return ntohs(mAddr.sin_port);
    }

private:

    int mFd{-1};

    sockaddr_in mAddr{};
};

}

#endif
