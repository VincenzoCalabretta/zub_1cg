
file( GLOB HPP_FILES *hpp )

install( FILES ${HPP_FILES} DESTINATION include/encore/OS )

#Common OS abstraction layer
add_subdirectory( Common )

#OS instances
if( ENCORE_TARGET_OS STREQUAL "LINUX" )
    add_subdirectory( Linux )
endif()

if( ENCORE_TARGET_OS STREQUAL "THREADX" )
    add_subdirectory( ThreadX )
endif()
