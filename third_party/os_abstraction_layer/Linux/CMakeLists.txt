
file( GLOB HPP_FILES *hpp )

install(FILES ${HPP_FILES} DESTINATION include/encore/OS/Linux/)

add_subdirectory( test )
