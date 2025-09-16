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
pkg_check_modules(LIBCEC REQUIRED libcec>=5.0.0)

# Find libsystemd for sd-bus
pkg_check_modules(LIBSYSTEMD REQUIRED libsystemd>=239)

# Define installation paths
include(GNUInstallDirs)

# Define application paths
set(CONFIG_DIR "/etc/cec-control")
set(LOG_DIR "/var/log/cec-control")
set(RUNTIME_DIR "/run/cec-control")

# Include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${LIBCEC_INCLUDE_DIR})
include_directories(${LIBSYSTEMD_INCLUDE_DIRS})

# Define source files
set(COMMON_SOURCES
    src/common/protocol.cpp
    src/common/logger.cpp
    src/common/buffer_pool.cpp
    src/common/buffer_manager.cpp
    src/common/config_manager.cpp
    src/common/system_paths.cpp
    src/common/event_poller.cpp
    src/common/application_mode.cpp
    src/common/argument_parser.cpp
    src/common/help_printer.cpp
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
    src/daemon/daemon_bootstrap.cpp
)

set(CLIENT_SOURCES
    src/client/cec_client.cpp
    src/client/command_mapper.cpp
    src/client/socket_client.cpp
)

set(BINARY_SOURCES
    src/main.cpp
    ${COMMON_SOURCES}
    ${DAEMON_SOURCES}
    ${CLIENT_SOURCES}
)

# Create unified executable
add_executable(cec-control ${BINARY_SOURCES})

# Link all necessary libraries for the unified binary
target_link_libraries(cec-control
    PRIVATE
        pthread
        stdc++fs
    PUBLIC
        ${LIBCEC_LIBRARIES}
        ${LIBSYSTEMD_LIBRARIES}
)

# Install unified binary
install(TARGETS cec-control RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Install documentation
install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/docs/configuration.md
    ${CMAKE_CURRENT_SOURCE_DIR}/docs/default-paths.md
    DESTINATION ${CMAKE_INSTALL_DOCDIR}
)

# Install configuration sample (don't overwrite existing)
install(
  FILES ${CMAKE_CURRENT_SOURCE_DIR}/etc/cec-control.conf
  DESTINATION ${CONFIG_DIR}
  RENAME "config.conf.sample"
  PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ
)

# Create necessary directories
install(CODE "
  # Create config directory
  if(NOT EXISTS \"${CONFIG_DIR}\")
    file(MAKE_DIRECTORY \"${CONFIG_DIR}\")
    execute_process(COMMAND chmod 755 \"${CONFIG_DIR}\")
    execute_process(COMMAND chown root:wheel \"${CONFIG_DIR}\")
  endif()
  
  # Create log directory
  if(NOT EXISTS \"${LOG_DIR}\")
    file(MAKE_DIRECTORY \"${LOG_DIR}\")
    execute_process(COMMAND chmod 755 \"${LOG_DIR}\")
    execute_process(COMMAND chown root:wheel \"${LOG_DIR}\")
  endif()
  
  # Copy sample config to real config only if it doesn't exist
  if(NOT EXISTS \"${CONFIG_DIR}/config.conf\")
    file(INSTALL \"${CMAKE_CURRENT_SOURCE_DIR}/etc/cec-control.conf\"
         DESTINATION \"${CONFIG_DIR}\"
         RENAME \"config.conf\"
         FILE_PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
    execute_process(COMMAND chown root:wheel \"${CONFIG_DIR}/config.conf\")
    message(STATUS \"Configuration installed at ${CONFIG_DIR}/config.conf\")
  else()
    message(STATUS \"Existing configuration found at ${CONFIG_DIR}/config.conf - not overwriting\")
  endif()
  
  # Make sure the sample config is owned by root too
  execute_process(COMMAND chown root:wheel \"${CONFIG_DIR}/config.conf.sample\")
")

# Install systemd service file
if(PKG_CONFIG_FOUND)
  # Get systemd unit dir
  execute_process(
    COMMAND ${PKG_CONFIG_EXECUTABLE} --variable=systemdsystemunitdir systemd
    OUTPUT_VARIABLE SYSTEMD_UNIT_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )

  # Set the user and group for the service file to root
  set(INSTALL_USER "root")
  set(INSTALL_GROUP "wheel")

  configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/etc/cec-control.service
    ${CMAKE_CURRENT_BINARY_DIR}/cec-control.service
    @ONLY
  )

  # Install systemd service file
  install(
    FILES ${CMAKE_CURRENT_BINARY_DIR}/cec-control.service
    DESTINATION ${SYSTEMD_UNIT_DIR}
    PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ
  )

  # Create message about enabling the service
  install(CODE "message(STATUS \"To enable the service, run: sudo systemctl enable --now cec-control.service\")")
  install(CODE "message(STATUS \"Note: The service is configured to run as root for hardware access\")")
endif()

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
message(STATUS "libsystemd version: ${LIBSYSTEMD_VERSION}")
message(STATUS "Install prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "Config directory: ${CONFIG_DIR}")
message(STATUS "Log directory: ${LOG_DIR}")
message(STATUS "Runtime directory: ${RUNTIME_DIR}")
message(STATUS "Building binary: cec-control")
