cmake_minimum_required(VERSION 3.16)
project(GST-CUSTOM-POSTPROCESS-CLASSIFICATION-EXAMPLE LANGUAGES C)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

find_package(PkgConfig)

# Get the pkgconfigs exported by the automake tools
pkg_check_modules(GST
  REQUIRED gstreamer-1.0>=${GST_VERSION_REQUIRED})

# ML types (GstMLFrame, GstMLClassifications, ...) used by the custom
# postprocess callback.
pkg_check_modules(GST_QCOM_ML REQUIRED gstreamer-qcom-oss-ml-1.0>=1.0.0)

# GStreamer gst-custom-postprocess-classification
set(GST_EXAMPLE_BIN gst-custom-postprocess-classification)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Werror")

add_executable(${GST_EXAMPLE_BIN}
  main.c
)

target_include_directories(${GST_EXAMPLE_BIN} PRIVATE
  ${GST_INCLUDE_DIRS}
  ${GST_QCOM_ML_INCLUDE_DIRS}
)

target_link_libraries(${GST_EXAMPLE_BIN} PRIVATE
  ${GST_LIBRARIES}
  ${GST_QCOM_ML_LIBRARIES}
  gstappsutils
  m
)

install(
  TARGETS ${GST_EXAMPLE_BIN}
  RUNTIME DESTINATION ${GST_PLUGINS_QTI_OSS_INSTALL_BINDIR}
  PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ
              GROUP_EXECUTE GROUP_READ
              WORLD_EXECUTE WORLD_READ
)
