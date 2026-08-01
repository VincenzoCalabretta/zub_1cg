
///
/// @file
/// @brief Defines serializers for application profiling structures
///
#ifndef ENCORE_OS_AppProfilingSerializers_HPP
#define ENCORE_OS_AppProfilingSerializers_HPP

#include <cstdint>

#include <string_view>

#include "encore/Encore.hpp"
#include "encore/OS/Common/AppProfiling.hpp"
#include "encore/Utils/Serializer.hpp"


namespace Encore::Utils {

/**
 * @brief Partial specialization of Serializer to Exec::MemInfo
 *
 * @tparam tPolicy The serialization policy
 */
template <SerializationPolicy tPolicy>
class Serializer<OS::MemInfo, tPolicy, std::enable_if_t<RequiresReflection<tPolicy>::value>> {
public:

    ///@brief Convenience alias
    using Type = OS::MemInfo;

    /**
     * @brief Serializes object
     *
     * @param pSource      The object to serialize
     * @param pDestination The device to store the data
     *
     * @return A Result indicating the total bytes serialized/de-serialized
     */
    ErrorHandling::Result<std::size_t> toBytes( Type const & pSource, IReaderWriter & pDestination ) {

        return serializeCluster<tPolicy>( pDestination, pSource.size, pSource.resident, pSource.shared );
    }

    /**
     * @brief Deserializes object
     *
     * @param pSource      The device from which to read the data
     * @param pDestination The object to populate
     *
     * @return A Result indicating the total bytes serialized/de-serialized
     */
    ErrorHandling::Result<std::size_t> toObj( IReaderWriter & pSource, Type & pDestination ) {

        return deserializeCluster<tPolicy>( pSource, pDestination.size, pDestination.resident, pDestination.shared );
    }
};

/**
 * @brief Partial specialization of Serializer to ProcessInfo
 *
 * @tparam tPolicy The serialization policy
 */
template <SerializationPolicy tPolicy>
class Serializer<OS::ProcessInfo, tPolicy, std::enable_if_t<RequiresReflection<tPolicy>::value>> {
public:

    ///@brief Convenience alias
    using Type = OS::ProcessInfo;

    /**
     * @brief Serializes object
     *
     * @param pSource      The object to serialize
     * @param pDestination The device to store the data
     *
     * @return A Result indicating the total bytes serialized/de-serialized
     */
    ErrorHandling::Result<std::size_t> toBytes( Type const & pSource, IReaderWriter & pDestination ) {

        return serializeCluster<tPolicy>( pDestination, pSource.cpuUsage, pSource.systemUsage, pSource.threads );
    }

    /**
     * @brief Deserializes object
     *
     * @param pSource      The device from which to read the data
     * @param pDestination The object to populate
     *
     * @return A Result indicating the total bytes serialized/de-serialized
     */
    ErrorHandling::Result<std::size_t> toObj( IReaderWriter & pSource, Type & pDestination ) {

        return deserializeCluster<tPolicy>( pSource
                                          , pDestination.cpuUsage
                                          , pDestination.systemUsage
                                          , pDestination.threads
                                          );
    }
};

}

#endif
