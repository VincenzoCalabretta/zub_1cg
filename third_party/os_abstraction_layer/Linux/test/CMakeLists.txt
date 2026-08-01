
install( FILES TestTCPServer.hpp DESTINATION include/encore/OS/Linux/test )

encore_unit_test( test_BaseLinuxTCPSocket )
encore_unit_test( test_Linux )
encore_unit_test( test_LinuxTCPClientSocket )
encore_unit_test( test_LinuxTCPServer )
encore_unit_test( test_LinuxTCPServerSocket )
encore_unit_test( test_LinuxUDPSocket )
encore_unit_test( test_LinuxShmRegion )
encore_unit_test( test_LinuxShmBufferRegistry )
encore_unit_test( test_LinuxShmQueueExchangeManagerHub )
encore_unit_test( test_LinuxThreadCustomization )
encore_unit_test( test_LinuxSerialDevice )
encore_unit_test( test_LinuxProcQuerier )
encore_unit_test( test_LinuxFile )
encore_unit_test( test_LinuxPidStatMQuery )
encore_unit_test( test_LinuxPidStatQuery )
encore_unit_test( test_LinuxSystemStatQuery )
encore_unit_test( test_ProcFileReader )
encore_unit_test( test_LinuxStatProcessProfiler )
encore_unit_test( test_LinuxMemoryProfiler )


encore_static_test( test_LinuxShmRegionStatic
    MACRO SHARED_MEM_DEFAULT_CONSTRUCTIBLE
    MSG "ShmRegion type must be default constructible"
)
encore_static_test( test_LinuxShmBufferRegistryStatic
    MACRO "UNDERSIZED_BUFFER"
    MSG "Buffer must support tNumHandles 64-bit fields"
)
encore_static_test( test_LinuxProcQuerierStatic
    MACRO IS_LINUX_PROC_QUERY
    MSG "Must be procfs query type"
)
