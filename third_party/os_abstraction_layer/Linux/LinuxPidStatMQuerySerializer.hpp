
///
/// @file LinuxPidStatMQuerySerializer.hpp
/// @brief Defines serializers for the LinuxPidStatMQuery structure
///
#ifndef ENCORE_OS_LinuxPidStatMQuerySerializer_HPP
#define ENCORE_OS_LinuxPidStatMQuerySerializer_HPP

#include "encore/Utils/ASCIISerializer.hpp"

#include "encore/OS/Linux/Linux.hpp"
#include "encore/OS/Linux/LinuxPidStatMQuery.hpp"


namespace Encore::Utils {

/**
 * @brief Specialization for serializing and deserialization a LinuxPidStatMQuery struct
 *
 * @tparam tPolicy The policy to use for serialization
 */
template <SerializationPolicy tPolicy>
class Serializer<OS::LinuxPidStatMQuery, tPolicy, std::enable_if_t<RequiresReflection<tPolicy>::value>> {
public:

    using Type = OS::LinuxPidStatMQuery; ///<@brief convenience definition

    /**
     * @brief Decompose a LinuxPidStatMQuery to bytes
     *
     * @param pSource The query to decompose
     * @param pDest The device to write to
     */
    ErrorHandling::Result<std::size_t> toBytes( Type const & pSource, IReaderWriter & pDest ) {

        return serializeCluster<tPolicy>( pDest, pSource.size, pSource.resident, pSource.shared );
    }

    /**
     * @brief Compose a LinuxPidStatMQuery from bytes
     *
     * @param pSource The device to read from
     * @param pDest The query to compose
     */
    ErrorHandling::Result<std::size_t> toObj( IReaderWriter & pSource, Type & pDest ) {

        return deserializeCluster<tPolicy>( pSource, pDest.size, pDest.resident, pDest.shared );
    }
};

}


#endif
