cmake_minimum_required(VERSION 3.10)
project(cec-control VERSION 1.0.0 LANGUAGES CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Set build type if not specified
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Compiler flags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -O0")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3")

# Find libcec package
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBCEC REQUIRED libcec>=4.0.0)

# Find D-Bus
pkg_check_modules(DBUS REQUIRED dbus-1>=1.6)

# Include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${LIBCEC_INCLUDE_DIR})
include_directories(${DBUS_INCLUDE_DIRS})

# Define source files
set(COMMON_SOURCES
    src/common/protocol.cpp
    src/common/logger.cpp
)

set(DAEMON_SOURCES
    src/daemon/cec_daemon.cpp
    src/daemon/cec_manager.cpp
    src/daemon/cec_adapter.cpp
    src/daemon/command_throttler.cpp
    src/daemon/device_operations.cpp
    src/daemon/socket_server.cpp
    src/daemon/command_queue.cpp
    src/daemon/cec_operation.cpp
    src/daemon/thread_pool.cpp
    src/daemon/dbus_monitor.cpp
    src/daemon/main.cpp
    ${COMMON_SOURCES}
)

set(CLIENT_SOURCES
    src/client/cec_client.cpp
    src/client/command_builder.cpp
    src/client/socket_client.cpp
    src/client/main.cpp
    ${COMMON_SOURCES}
)

# Create executables
add_executable(cec-daemon ${DAEMON_SOURCES})
add_executable(cec-client ${CLIENT_SOURCES})

# Link libraries
target_link_libraries(cec-daemon ${LIBCEC_LIBRARIES} ${DBUS_LIBRARIES} pthread)
target_link_libraries(cec-client ${LIBCEC_LIBRARIES} pthread)

# Install targets
install(TARGETS cec-daemon DESTINATION bin)
install(TARGETS cec-client DESTINATION bin)

# Install systemd service file
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/scripts/cec-daemon.service.in
    ${CMAKE_CURRENT_BINARY_DIR}/cec-daemon.service
    @ONLY
)

install(
    FILES ${CMAKE_CURRENT_BINARY_DIR}/cec-daemon.service
    DESTINATION /lib/systemd/system/
    PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ
    OPTIONAL
)

# Create uninstall target
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cmake_uninstall.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
    IMMEDIATE @ONLY)

add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake)

# Print configuration summary
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "libcec version: ${LIBCEC_VERSION}")
message(STATUS "dbus version: ${DBUS_VERSION}")