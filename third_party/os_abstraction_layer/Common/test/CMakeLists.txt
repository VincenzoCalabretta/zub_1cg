
encore_unit_test( test_IPv4Address )
encore_unit_test( test_UDP )
encore_unit_test( test_DgramSocketClient )
encore_unit_test( test_TCP )
encore_unit_test( test_TCPClientSocketFactory )
encore_unit_test( test_IThreadCustomization )
encore_unit_test( test_IStreamProtocol )
encore_unit_test( test_StreamSocketClient )
encore_unit_test( test_FileErrors )
encore_unit_test( test_IProcessProfiler )
encore_unit_test( test_AppProfiling )

encore_static_test( test_TCPClientSocketFactoryStatic
    MACRO "NOT_ITCPCLIENT"
    MSG "Socket must be an IStreamSocket, an ITCPClient, and must be default constructible"
)

encore_static_test( test_TCPClientSocketFactoryStatic
    MACRO "NOT_ISTREAMSOCKET"
    MSG "Socket must be an IStreamSocket, an ITCPClient, and must be default constructible"
)

encore_static_test( test_TCPClientSocketFactoryStatic
    MACRO "NOT_DEFAULT_CONSTRUCTIBLE"
    MSG "Socket must be an IStreamSocket, an ITCPClient, and must be default constructible"
)
