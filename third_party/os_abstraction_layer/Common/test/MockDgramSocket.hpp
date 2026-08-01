
#ifndef ENCORE_OS_COMMON_TEST_MockDgramSocket_HPP
#define ENCORE_OS_COMMON_TEST_MockDgramSocket_HPP

#include "Utils/BytesView.hpp"
#include "OS/Common/IDgramSocket.hpp"

namespace Encore::OS {

class MockDgramSocket final : public IDgramSocket {
public: /*IDevice*/

    ErrorHandling::Result<bool> open() final { return true; }

    bool isOpen() const final { return true; }

    ErrorHandling::Result<bool> close() final { return true; }

public: /*IReaderWriter*/

    ErrorHandling::Result<std::size_t> write( Utils::BytesView const & pView ) final {

        ++numWrites;

        return pView.size();
    }

    ErrorHandling::Result<std::size_t> read( Utils::BytesRange pBytes ) final {

        ++numReads;

        return pBytes.size();
    }

public:

    int numWrites{0};
    int numReads{0};
};

}

#endif
