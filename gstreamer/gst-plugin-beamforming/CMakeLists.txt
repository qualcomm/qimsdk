cmake_minimum_required(VERSION 3.16)
project(GST_PLUGIN_QTI_OSS_BEAMFORMING
  VERSION ${GST_PLUGINS_QTI_OSS_VERSION}
  LANGUAGES C CXX
)

set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig)

# Get the pkgconfigs exported by the automake tools
pkg_check_modules(GST
  REQUIRED gstreamer-1.0>=${GST_VERSION_REQUIRED})
pkg_check_modules(GST_BASE
  REQUIRED gstreamer-base-1.0>=${GST_VERSION_REQUIRED})
pkg_check_modules(GST_VIDEO
  REQUIRED gstreamer-video-1.0>=${GST_VERSION_REQUIRED})

# Generate configuration header file with plugin describing definitions
configure_file(config.h.in config.h @ONLY)

# Precompiler definitions.
add_definitions(-DHAVE_CONFIG_H)

# Common compiler flags.
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-unused-parameter")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-unused-parameter")

# GStreamer Beamforming fusion plugin.
set(GST_QTI_BEAMFORMING gstqtibeamforming)

add_library(${GST_QTI_BEAMFORMING} SHARED
  gstbeamforming.c
  beamformpads.c
  beamform_context.cc
  beamform/adoa_filter.cc
  beamform/doa_histogram/doa_histogram.c
  beamform/doa_histogram/histogram.c
  beamform/doa_histogram/circbuf.c
)

target_include_directories(${GST_QTI_BEAMFORMING} PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/beamform
  ${CMAKE_CURRENT_SOURCE_DIR}/beamform/doa_histogram
)

target_include_directories(${GST_QTI_BEAMFORMING} PUBLIC
  ${GST_INCLUDE_DIRS}
  ${GST_BASE_INCLUDE_DIRS}
  ${GST_VIDEO_INCLUDE_DIRS}
)

target_link_libraries(${GST_QTI_BEAMFORMING} PRIVATE
  ${GST_LIBRARIES}
  ${GST_BASE_LIBRARIES}
  ${GST_VIDEO_LIBRARIES}
  m
)

install(
  TARGETS ${GST_QTI_BEAMFORMING}
  LIBRARY DESTINATION ${GST_PLUGINS_QTI_OSS_INSTALL_LIBDIR}/gstreamer-1.0
  PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ
              GROUP_EXECUTE GROUP_READ
              WORLD_EXECUTE WORLD_READ
)
