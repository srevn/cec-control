cmake_minimum_required(VERSION 3.10)
project(cec-control VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to Release for single-config generators. Multi-config generators
# (Ninja Multi-Config, Visual Studio) pick the type at build time and must
# not have the cache variable forced here.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    message(STATUS "Build type defaulted to Release")
endif()

set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -O0")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3")

# External dependencies via pkg-config. IMPORTED_TARGET produces
# PkgConfig::LIBCEC / PkgConfig::LIBSYSTEMD, which carry include paths and
# link flags transitively — no explicit ${..._INCLUDE_DIRS} /
# ${..._LIBRARIES} plumbing required.
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBCEC     REQUIRED IMPORTED_TARGET libcec>=5.0.0)
pkg_check_modules(LIBSYSTEMD REQUIRED IMPORTED_TARGET libsystemd>=239)

include(GNUInstallDirs)

# Application paths
set(CONFIG_DIR  "/etc/cec-control")
set(LOG_DIR     "/var/log/cec-control")
set(RUNTIME_DIR "/run/cec-control")

add_executable(cec-control)

target_include_directories(cec-control PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(cec-control PRIVATE -Wall -Wextra)

target_sources(cec-control PRIVATE
    src/main.cpp
)

target_sources(cec-control PRIVATE
    src/common/argument_parser.cpp
    src/common/command_registry.cpp
    src/common/config_manager.cpp
    src/common/event_loop.cpp
    src/common/event_poller.cpp
    src/common/help_printer.cpp
    src/common/logger.cpp
    src/common/main_thread_work.cpp
    src/common/messages.cpp
    src/common/signal_source.cpp
    src/common/system_paths.cpp
    src/common/systemd_env.cpp
    src/common/timer_source.cpp
    src/common/unix_socket.cpp
)

target_sources(cec-control PRIVATE
    src/daemon/app_config.cpp
    src/daemon/cec/adapter_worker.cpp
    src/daemon/cec/libcec_adapter.cpp
    src/daemon/cec/operations.cpp
    src/daemon/cec_daemon.cpp
    src/daemon/command_router.cpp
    src/daemon/command_throttler.cpp
    src/daemon/daemon_bootstrap.cpp
    src/daemon/dbus_monitor.cpp
    src/daemon/power/adapter_reconnect.cpp
    src/daemon/power/power_lifecycle.cpp
    src/daemon/power/power_supervisor.cpp
    src/daemon/power/suspend_queue.cpp
    src/daemon/socket_server.cpp
)

target_sources(cec-control PRIVATE
    src/client/cec_client.cpp
    src/client/client_runner.cpp
    src/client/socket_client.cpp
)

target_link_libraries(cec-control PRIVATE
    pthread
    stdc++fs
    PkgConfig::LIBCEC
    PkgConfig::LIBSYSTEMD
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
