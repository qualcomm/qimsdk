cmake_minimum_required(VERSION 3.16)
project(GST-SPEAKER-FRAMING-DOA-EXAMPLE LANGUAGES CXX)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

# pkgconfigs are exported by the parent CMakeLists
pkg_check_modules(GST
  REQUIRED gstreamer-1.0>=${GST_VERSION_REQUIRED})
pkg_check_modules(GST_VIDEO
  REQUIRED gstreamer-video-1.0>=${GST_VERSION_REQUIRED})
pkg_check_modules(PW
  REQUIRED libpipewire-0.3)

# GStreamer speaker framing DOA validation app
set(GST_EXAMPLE_BIN gst-speaker-framing-doa-example)

add_executable(${GST_EXAMPLE_BIN}
  main.cc
)

target_include_directories(${GST_EXAMPLE_BIN} PRIVATE
  ${GST_INCLUDE_DIRS}
  ${GST_VIDEO_INCLUDE_DIRS}
  ${PW_INCLUDE_DIRS}
)

target_link_libraries(${GST_EXAMPLE_BIN} PRIVATE
  pthread
  ${GST_LIBRARIES}
  ${GST_VIDEO_LIBRARIES}
  ${PW_LIBRARIES}
)

install(
  TARGETS ${GST_EXAMPLE_BIN}
  RUNTIME DESTINATION ${GST_PLUGINS_QTI_OSS_INSTALL_BINDIR}
  PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ
              GROUP_EXECUTE GROUP_READ
              WORLD_EXECUTE WORLD_READ
)
