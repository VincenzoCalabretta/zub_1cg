
file( GLOB HPP_FILES *hpp )

install(FILES ${HPP_FILES} DESTINATION include/encore/OS/Common/)

add_subdirectory( test )
