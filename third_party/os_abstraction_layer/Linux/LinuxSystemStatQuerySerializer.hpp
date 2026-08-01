
///
/// @file
/// @brief Serializer definition for LinuxSystemStatQuery
///
#ifndef ENCORE_OS_LinuxSystemStatQuerySerializer_HPP
#define ENCORE_OS_LinuxSystemStatQuerySerializer_HPP

#include "encore/Utils/Serializer.hpp"

#include "encore/OS/Linux/Linux.hpp"
#include "encore/OS/Linux/LinuxSystemStatQuery.hpp"


namespace Encore::Utils {

/**
 * @brief Specialization for serializing and deserialization a LinuxSystemStatQuery struct
 *
 * @tparam tPolicy The policy to use for serialization
 */
template <SerializationPolicy tPolicy>
class Serializer<OS::LinuxSystemStatQuery, tPolicy, std::enable_if_t<RequiresReflection<tPolicy>::value>> {
public:

    using Type = OS::LinuxSystemStatQuery; ///<@brief convenience definition

    /**
     * @brief Decompose a LinuxSystemStatQuery to bytes
     *
     * @param pSource The query to decompose
     * @param pDest The device to write to
     */
    ErrorHandling::Result<std::size_t> toBytes( Type const & pSource, IReaderWriter & pDest )
    {

        return serializeCluster<tPolicy>( pDest, pSource.cpu, pSource.user, pSource.nice, pSource.system
                                        , pSource.idle, pSource.iowait, pSource.irq, pSource.softirq, pSource.steal
                                        , pSource.guest, pSource.guest_nice );
    }

    /**
     * @brief Compose a LinuxSystemStatQuery from bytes
     *
     * @param pSource The device to read from
     * @param pDest The query to compose
     */
    ErrorHandling::Result<std::size_t> toObj( IReaderWriter & pSource, Type & pDest ) {

        return deserializeCluster<tPolicy>( pSource, pDest.cpu, pDest.user, pDest.nice, pDest.system, pDest.idle
                                          , pDest.iowait, pDest.irq, pDest.softirq, pDest.steal, pDest.guest
                                          , pDest.guest_nice );
    }
};

/**
 * @brief Specialization for serializing and deserialization a LinuxSystemStatQuery::CpuName name
 *
 * @tparam tPolicy The policy to use for serialization
 */
template <SerializationPolicy tPolicy>
class Serializer<OS::LinuxSystemStatQuery::CpuName, tPolicy, std::enable_if_t<RequiresReflection<tPolicy>::value>> {
public:

    using Type = OS::LinuxSystemStatQuery::CpuName; ///<@brief convenience definition

    /**
     * @brief Decompose a CpuName name to bytes
     *
     * @param pSource The query to decompose
     * @param pDest The device to write to
     */
    ErrorHandling::Result<std::size_t> toBytes( Type const & pSource, IReaderWriter & pDest ) {

        std::size_t srcSize = pSource.size();
        auto nullCharLoc = std::find( pSource.begin(), pSource.end(), '\0' );
        if( nullCharLoc != pSource.end() ) {
            srcSize = static_cast<std::size_t>(nullCharLoc - pSource.begin());
        }
        BytesView bv( pSource.data(), srcSize );
        return pDest.write( bv );
    }

    /**
     * @brief Compose a CpuName name from bytes
     *
     * @param pSource The device to read from
     * @param pDest The query to compose
     */
    ErrorHandling::Result<std::size_t> toObj( IReaderWriter & pSource, Type & pDest ) {

        pDest.fill('\0');
        BytesRange bv( pDest );
        return pSource.read( bv );

    }
};

}

#endif
