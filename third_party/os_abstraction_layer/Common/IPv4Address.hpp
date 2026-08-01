
///
/// @file
/// @brief Defines IPv4 class for use in Encore
///
#ifndef ENCORE_OS_NETWORK_IPv4Address_HPP
#define ENCORE_OS_NETWORK_IPv4Address_HPP

#include <cstdint>
#include <cstdlib>

#include <array>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

#include "encore/ErrorHandling/ErrorGroup.hpp"
#include "encore/ErrorHandling/Result.hpp"
#include "encore/ErrorHandling/StdFswErrors.hpp"

#include "encore/OS/Common/Network.hpp"


namespace Encore::OS {

/**
 * @brief Function to parse an IPv4 string
 *
 * @param pIP The string to parse
 *
 * @return A Result containing array of 4 bytes representing numeric octets of address or an ErrorCode
 */
inline ErrorHandling::Result<std::array<std::uint8_t,4>> parseIPv4( std::string const & pIP ) {

    std::array<std::uint8_t, 4> addr{};
    std::string_view ip( pIP.data(), pIP.size() );

    std::size_t count{0};

    for( auto & octet : addr ) {

        ++count;

        auto i = ip.find_first_of('.');

        if( i == std::string_view::npos and count < addr.size() ) {
            return EG_ERROR_CODE( ErrorHandling::StdFswErrors, invalid_argument );
        }

        int val = std::atoi( ip.substr(0,i).data() );

        if( val < 0 or val > std::numeric_limits<std::uint8_t>::max() ) {
            return EG_ERROR_CODE( ErrorHandling::StdFswErrors, invalid_argument );
        }

        octet = static_cast<std::uint8_t>(val);

        ip = ip.substr(i+1);
    }

    return addr;
}


/**
 * @brief Simple representation of an IPv4 address
 */
class IPv4Address {
public:

    /// @brief Default constructor
    constexpr IPv4Address() noexcept = default; //LCOV_EXCL_LINE -- We very clearly invoke this...

    /**
     * @brief Constructor from 4 byte unsigned integer
     *
     * @param pAddr The integral representation of the address
     */
    constexpr IPv4Address( std::uint32_t pAddr ) noexcept {

        octet1() = static_cast<std::uint8_t>( pAddr >> kOctet1Shift );
        octet2() = static_cast<std::uint8_t>( pAddr >> kOctet2Shift );
        octet3() = static_cast<std::uint8_t>( pAddr >> kOctet3Shift );
        octet4() = static_cast<std::uint8_t>( pAddr );
    }

    /**
     * @brief Constructor from array
     *
     * @param pAddr Representation of address as 4 octets
     */
    constexpr IPv4Address( std::array<std::uint8_t,4> pAddr ) noexcept
    : mAddr{ pAddr }
    {}

    /**
     * @brief Constructor from IP string
     *
     * @param pInit Signifies INIT ONLY usage
     * @param pAddr The string rep X.Y.Z.W of the address
     *
     * @details If string is "invalid", exception is thrown
     */
    IPv4Address( [[maybe_unused]] InitOnly const & pInit, std::string pAddr ) {

        auto addr = parseIPv4( pAddr );
        if( not addr.success() ) {
            throw std::invalid_argument("IP string format invalid: " + pAddr +
                                        "\nShould be: [0-255].[0-255].[0-255].[0-255]");
        }

        mAddr = addr.value();
    }

    /// @brief Returns address represented as array
    constexpr std::array<std::uint8_t,4> asArray() const noexcept { return mAddr; }

    /// @brief Returns address represented as 32 bit unsigned integer
    constexpr std::uint32_t asInteger() const noexcept {

        return static_cast<std::uint32_t>( (octet1() << kOctet1Shift)
                                         | (octet2() << kOctet2Shift)
                                         | (octet3() << kOctet3Shift)
                                         |  octet4() );
    }

private:

    static constexpr int kOctet1Shift = 24;
    static constexpr int kOctet2Shift = 16;
    static constexpr int kOctet3Shift = 8;

    constexpr std::uint8_t & octet1() { return mAddr[0]; }
    constexpr std::uint8_t & octet2() { return mAddr[1]; }
    constexpr std::uint8_t & octet3() { return mAddr[2]; }
    constexpr std::uint8_t & octet4() { return mAddr[3]; }

    constexpr std::uint8_t octet1() const { return mAddr[0]; }
    constexpr std::uint8_t octet2() const { return mAddr[1]; }
    constexpr std::uint8_t octet3() const { return mAddr[2]; }
    constexpr std::uint8_t octet4() const { return mAddr[3]; }

    // NOLINTBEGIN(customchecks-avoid-magic-numbers) -- Intent of default octet value is clear.
    std::array<std::uint8_t,4> mAddr{127,0,0,1};
    // NOLINTEND(customchecks-avoid-magic-numbers)
};


/**
 * @brief Equality operator for IPv4Address
 *
 * @param pLHS LHS of operator
 * @param pRHS RHS of operator
 */
constexpr bool operator==(IPv4Address pLHS, IPv4Address pRHS) {
    return pLHS.asInteger() == pRHS.asInteger();
}


///@brief The INADDR_LOOPBACK equivalent
inline constexpr IPv4Address kLoopBack;


/**
 * @brief An IPv4Address and a NetworkPort
 */
struct IPAndPort {
    IPv4Address  addr;     ///<@brief The IPv4Address
    NetworkPort  port{0};  ///<@brief The NetworkPort
};


/**
 * @brief Equality comparison operator for IPAndPort
 *
 * @param pLHS LHS operand
 * @param pRHS RHS operand
 */
constexpr bool operator==( IPAndPort const & pLHS, IPAndPort const & pRHS ) noexcept {
    return (pLHS.addr == pRHS.addr) and (pLHS.port == pRHS.port);
}


///@brief Useful constant
inline constexpr IPAndPort kNullIP = { kLoopBack, 0 };


///@brief Useful typedef - larger 4 bytes are IP addr and smaller 4 are port
typedef std::uint64_t IPKey;


/**
 * @brief Generates a unique key from IP addr and port
 *
 * @param pIPAndPort The IPAndPort from which to build the key
 */
constexpr IPKey makeIPKey( IPAndPort const & pIPAndPort ) noexcept {

    return ( static_cast<IPKey>(pIPAndPort.addr.asInteger()) << 32)
           | static_cast<IPKey>(pIPAndPort.port);
}


/**
 * @brief Extract IP addr and port from a key
 *
 * @param pKey The key from which to extract info
 */
constexpr IPAndPort makeIPAndPort( IPKey pKey ) noexcept {

    IPAndPort out;

    out.addr = static_cast<std::uint32_t>( pKey >> 32 );
    out.port = static_cast<NetworkPort>( pKey );

    return out;
}


/**
 * @brief A pair of IPAndPort
 */
struct IPConnection {
    IPAndPort  recv; ///<@brief From where data is being received
    IPAndPort  send; ///<@brief To where data is being sent
};

}


#endif
