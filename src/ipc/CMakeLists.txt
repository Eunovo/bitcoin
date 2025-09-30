# Copyright (c) 2023-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

include(GNUInstallDirs)

add_library(bitcoin_ipc STATIC EXCLUDE_FROM_ALL
  capnp/mining.cpp
  capnp/protocol.cpp
  interfaces.cpp
  process.cpp
)

target_capnp_sources(bitcoin_ipc ${PROJECT_SOURCE_DIR}
  capnp/common.capnp
  capnp/echo.capnp
  capnp/init.capnp
  capnp/mining.capnp
)

target_link_libraries(bitcoin_ipc
  PRIVATE
    core_interface
    univalue
)

configure_file(.clang-tidy.in .clang-tidy USE_SOURCE_PERMISSIONS COPYONLY)

# Configure and install pkg-config file
configure_file(${PROJECT_SOURCE_DIR}/bitcoinipc.pc.in ${PROJECT_BINARY_DIR}/bitcoinipc.pc @ONLY)
install(FILES ${PROJECT_BINARY_DIR}/bitcoinipc.pc DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig" COMPONENT bitcoin_ipc)

# Install Cap'n Proto schema files for external applications
install(FILES
  capnp/common.capnp
  capnp/echo.capnp
  capnp/init.capnp
  capnp/mining.capnp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/bitcoin/ipc
  COMPONENT bitcoin_ipc
)

