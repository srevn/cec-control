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

# Define XDG installation paths based on GNU standards
include(GNUInstallDirs)

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
    src/common/xdg_paths.cpp
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
target_link_libraries(cec-daemon ${LIBCEC_LIBRARIES} ${DBUS_LIBRARIES} pthread stdc++fs)
target_link_libraries(cec-client ${LIBCEC_LIBRARIES} pthread stdc++fs)

# Install targets
install(TARGETS cec-daemon DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS cec-client DESTINATION ${CMAKE_INSTALL_BINDIR})

# Add installation option for system-wide installation
option(SYSTEM_WIDE_INSTALL "Install for all users (requires root privileges)" OFF)

# Define system-wide and user-specific paths
set(SYSTEM_CONFIG_DIR "/etc/cec-control")
set(SYSTEM_LOG_DIR "/var/log/cec-control")
set(SYSTEM_RUNTIME_DIR "/run/cec-control")

# Install targets
install(TARGETS cec-daemon cec-client 
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Install documentation
install(FILES 
        ${CMAKE_CURRENT_SOURCE_DIR}/docs/configuration.md
        ${CMAKE_CURRENT_SOURCE_DIR}/docs/default-paths.md
        DESTINATION ${CMAKE_INSTALL_DOCDIR})

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

    configure_file(
      ${CMAKE_CURRENT_SOURCE_DIR}/scripts/cec-daemon.service.in
      ${CMAKE_CURRENT_BINARY_DIR}/cec-daemon.service
      @ONLY
    )

    # Install systemd service file for system-wide installation
    if(SYSTEM_WIDE_INSTALL)
      install(
        FILES ${CMAKE_CURRENT_BINARY_DIR}/cec-daemon.service
        DESTINATION ${SYSTEMD_UNIT_DIR}
        PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ
      )
      
      # Create message about enabling the service
      install(CODE "message(STATUS \"To enable the service, run: sudo systemctl enable --now cec-daemon.service\")")
    else()
      # For user installation, install to user systemd directory
      if(DEFINED ENV{USER})
        set(INSTALL_USER $ENV{USER})
      else()
        set(INSTALL_USER $ENV{LOGNAME})
      endif()
      
      execute_process(
        COMMAND ${PKG_CONFIG_EXECUTABLE} --variable=systemduserunitdir systemd
        OUTPUT_VARIABLE SYSTEMD_USER_UNIT_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
      )
      
      if(SYSTEMD_USER_UNIT_DIR)
        install(
          FILES ${CMAKE_CURRENT_BINARY_DIR}/cec-daemon.service
          DESTINATION ${SYSTEMD_USER_UNIT_DIR}
          PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ
          OPTIONAL
        )
        
        # Create message about enabling the user service
        install(CODE "message(STATUS \"To enable the user service, run: systemctl --user enable --now cec-daemon.service\")")
      endif()
    endif()
  else()
    message(STATUS "systemd not found, skipping service installation")
  endif()
endif()

# System-wide installation
if(SYSTEM_WIDE_INSTALL)
  # Install system-wide configuration
  install(
    FILES ${CMAKE_CURRENT_SOURCE_DIR}/etc/cec-control.conf
    DESTINATION ${SYSTEM_CONFIG_DIR}
    RENAME "config.conf.default"
    PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ
  )
  
  # Create system directories
  install(CODE "
    # Create system config directory if it doesn't exist
    if(NOT EXISTS \"${SYSTEM_CONFIG_DIR}\")
      file(MAKE_DIRECTORY \"${SYSTEM_CONFIG_DIR}\")
      execute_process(COMMAND chmod 755 \"${SYSTEM_CONFIG_DIR}\")
    endif()
    
    # Create system log directory if it doesn't exist
    if(NOT EXISTS \"${SYSTEM_LOG_DIR}\")
      file(MAKE_DIRECTORY \"${SYSTEM_LOG_DIR}\")
      execute_process(COMMAND chmod 755 \"${SYSTEM_LOG_DIR}\")
    endif()
    
    # Don't override existing config
    if(NOT EXISTS \"${SYSTEM_CONFIG_DIR}/config.conf\")
      file(INSTALL \"${CMAKE_CURRENT_SOURCE_DIR}/etc/cec-control.conf\"
           DESTINATION \"${SYSTEM_CONFIG_DIR}\"
           RENAME \"config.conf\"
           FILE_PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
      message(STATUS \"System configuration installed at ${SYSTEM_CONFIG_DIR}/config.conf\")
    else()
      message(STATUS \"Existing system configuration found at ${SYSTEM_CONFIG_DIR}/config.conf - not overwriting\")
    endif()
  ")
  
  # Create message about checking permissions
  install(CODE "message(STATUS \"Ensure the system directories have appropriate permissions:\")")
  install(CODE "message(STATUS \"  sudo chown -R root:root ${SYSTEM_CONFIG_DIR}\")")
  install(CODE "message(STATUS \"  sudo chmod 755 ${SYSTEM_CONFIG_DIR}\")")
  install(CODE "message(STATUS \"  sudo mkdir -p ${SYSTEM_LOG_DIR}\")")
  install(CODE "message(STATUS \"  sudo chown -R root:root ${SYSTEM_LOG_DIR}\")")
  install(CODE "message(STATUS \"  sudo chmod 755 ${SYSTEM_LOG_DIR}\")")
  
# User-specific installation
else()
  # Use XDG directories for user installation
  install(CODE "
    # Determine XDG_CONFIG_HOME
    set(XDG_CONFIG_HOME \"$ENV{XDG_CONFIG_HOME}\")
    if(NOT XDG_CONFIG_HOME)
      set(XDG_CONFIG_HOME \"$ENV{HOME}/.config\")
    endif()
    
    # Determine XDG_CACHE_HOME for log files
    set(XDG_CACHE_HOME \"$ENV{XDG_CACHE_HOME}\")
    if(NOT XDG_CACHE_HOME)
      set(XDG_CACHE_HOME \"$ENV{HOME}/.cache\")
    endif()
    
    # Create user config directory
    set(USER_CONFIG_DIR \"\${XDG_CONFIG_HOME}/cec-control\")
    file(MAKE_DIRECTORY \"\${USER_CONFIG_DIR}\")
    
    # Create user cache directory for logs
    set(USER_CACHE_DIR \"\${XDG_CACHE_HOME}/cec-control\")
    file(MAKE_DIRECTORY \"\${USER_CACHE_DIR}\")
    
    # Check if config file already exists
    if(EXISTS \"\${USER_CONFIG_DIR}/config.conf\")
      # Install as .sample file since the real config exists
      file(INSTALL \"${CMAKE_CURRENT_SOURCE_DIR}/etc/cec-control.conf\"
           DESTINATION \"\${USER_CONFIG_DIR}\"
           RENAME \"config.conf.sample\"
           FILE_PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
      message(STATUS \"Existing configuration found. Sample configuration installed at \${USER_CONFIG_DIR}/config.conf.sample\")
    else()
      # Install as the main config file
      file(INSTALL \"${CMAKE_CURRENT_SOURCE_DIR}/etc/cec-control.conf\"
           DESTINATION \"\${USER_CONFIG_DIR}\"
           RENAME \"config.conf\"
           FILE_PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
      message(STATUS \"Configuration installed at \${USER_CONFIG_DIR}/config.conf\")
    endif()
  ")
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
message(STATUS "Binary install location: ${CMAKE_INSTALL_FULL_BINDIR}")
message(STATUS "System-wide installation: ${SYSTEM_WIDE_INSTALL}")