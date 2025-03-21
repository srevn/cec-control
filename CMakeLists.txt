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

# Find D-Bus
pkg_check_modules(DBUS REQUIRED dbus-1>=1.6)

# Define installation paths
include(GNUInstallDirs)

# Define application paths
set(CONFIG_DIR "/etc/cec-control")
set(LOG_DIR "/var/log/cec-control")
set(RUNTIME_DIR "/run/cec-control")

# Include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${LIBCEC_INCLUDE_DIR})
include_directories(${DBUS_INCLUDE_DIRS})

# Define source files
set(COMMON_SOURCES
    src/common/protocol.cpp
    src/common/logger.cpp
    src/common/buffer_pool.cpp
    src/common/buffer_manager.cpp
    src/common/config_manager.cpp
    src/common/system_paths.cpp
    src/common/event_poller.cpp
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
    src/client/command_mapper.cpp
    src/client/socket_client.cpp
    src/client/main.cpp
    ${COMMON_SOURCES}
)

# Create executables
add_executable(cec-daemon ${DAEMON_SOURCES})
add_executable(cec-client ${CLIENT_SOURCES})

# Link libraries for cec-daemon
target_link_libraries(cec-daemon
    PRIVATE
        pthread
        stdc++fs
    PUBLIC
        ${LIBCEC_LIBRARIES}
        ${DBUS_LIBRARIES}
)

# Link libraries for cec-client
target_link_libraries(cec-client
    PRIVATE
        pthread
        stdc++fs
    PUBLIC
        ${LIBCEC_LIBRARIES}
)

# Install targets
install(TARGETS cec-client RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS cec-daemon RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

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

# Install systemd service file if systemd is available
if(PKG_CONFIG_FOUND)
  pkg_check_modules(SYSTEMD systemd)
  if(SYSTEMD_FOUND)
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
      ${CMAKE_CURRENT_SOURCE_DIR}/etc/cec-daemon.service
      ${CMAKE_CURRENT_BINARY_DIR}/cec-daemon.service
      @ONLY
    )

    # Install systemd service file
    install(
      FILES ${CMAKE_CURRENT_BINARY_DIR}/cec-daemon.service
      DESTINATION ${SYSTEMD_UNIT_DIR}
      PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ
    )
    
    # Create message about enabling the service
    install(CODE "message(STATUS \"To enable the service, run: sudo systemctl enable --now cec-daemon.service\")")
    install(CODE "message(STATUS \"Note: The service is configured to run as root for hardware access\")")
  else()
    message(STATUS "systemd not found, skipping service installation")
  endif()
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
message(STATUS "dbus version: ${DBUS_VERSION}")
message(STATUS "Install prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "Config directory: ${CONFIG_DIR}")
message(STATUS "Log directory: ${LOG_DIR}")
message(STATUS "Runtime directory: ${RUNTIME_DIR}")
