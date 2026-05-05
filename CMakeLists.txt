add_library(quectel_bc660kgl INTERFACE)

target_include_directories(quectel_bc660kgl
    INTERFACE
        include
        include/bc660kgl
)

target_sources(quectel_bc660kgl
    INTERFACE
        src/QUECTEL_BC660KGL.cpp
        src/QUECTEL_BC660KGL_CellularContext.cpp
        src/QUECTEL_BC660KGL_CellularNetwork.cpp
        src/QUECTEL_BC660KGL_CellularStack.cpp
        src/QUECTEL_BC660KGL_CellularInformation.cpp
        src/QUECTEL_BC660KGL_SocketBuffer.cpp
)

target_link_libraries(quectel_bc660kgl
    INTERFACE
    mbed-core-flags
    mbed-cellular
)