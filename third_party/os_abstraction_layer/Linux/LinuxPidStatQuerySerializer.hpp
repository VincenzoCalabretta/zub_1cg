
///
/// @file LinuxPidStatQuerySerializer.hpp
/// @brief Defines serializers for LinuxPidStatQeury
///
#ifndef ENCORE_OS_LinuxPidStatQuerySerializer_HPP
#define ENCORE_OS_LinuxPidStatQuerySerializer_HPP

#include "encore/Utils/Serializer.hpp"

#include "encore/OS/Linux/Linux.hpp"
#include "encore/OS/Linux/LinuxPidStatQuery.hpp"

namespace Encore::Utils {

/**
 * @brief Specialization for serializing and deserialization a LinuxPidStatQuery struct
 *
 * @tparam tPolicy The policy to use for serialization
 */
template <SerializationPolicy tPolicy>
class Serializer<OS::LinuxPidStatQuery, tPolicy, std::enable_if_t<RequiresReflection<tPolicy>::value>> {
public:

    using Type = OS::LinuxPidStatQuery; ///<@brief convenience definition

    /**
     * @brief Decompose a LinuxPidStatQuery to bytes
     *
     * @param pSource The query to decompose
     * @param pDest The device to write to
     */
    ErrorHandling::Result<std::size_t> toBytes( Type const & pSource, IReaderWriter & pDest ) {

        return serializeCluster<tPolicy>( pDest, pSource.pid, pSource.comm, pSource.state, pSource.ppid
                                        , pSource.pgrp, pSource.session, pSource.tty_nr, pSource.tpgid, pSource.flags
                                        , pSource.minflt, pSource.cminflt, pSource.majflt, pSource.cmajflt
                                        , pSource.utime, pSource.stime, pSource.cutime, pSource.cstime, pSource.priority
                                        , pSource.nice, pSource.num_threads );
    }

    /**
     * @brief Compose a LinuxPidStatQuery from bytes
     *
     * @param pSource The device to read from
     * @param pDest The query to compose
     */
    ErrorHandling::Result<std::size_t> toObj( IReaderWriter & pSource, Type & pDest ) {

        return deserializeCluster<tPolicy>( pSource, pDest.pid, pDest.comm, pDest.state, pDest.ppid, pDest.pgrp
                                          , pDest.session, pDest.tty_nr, pDest.tpgid, pDest.flags, pDest.minflt
                                          , pDest.cminflt, pDest.majflt, pDest.cmajflt, pDest.utime, pDest.stime
                                          , pDest.cutime, pDest.cstime, pDest.priority, pDest.nice, pDest.num_threads );
    }
};

/**
 * @brief Specialization for serializing and deserialization a LinuxPidStatQuery::CommArray name
 *
 * @tparam tPolicy The policy to use for serialization
 */
template <SerializationPolicy tPolicy>
class Serializer<OS::LinuxPidStatQuery::CommArray, tPolicy, std::enable_if_t<RequiresReflection<tPolicy>::value>> {
public:

    using Type = OS::LinuxPidStatQuery::CommArray; ///<@brief convenience definition

    /**
     * @brief Decompose a CommArray name to bytes
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
     * @brief Compose a CommArray name from bytes
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
