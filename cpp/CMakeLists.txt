cmake_minimum_required(VERSION 3.8.2)
project(GST_QTI_QIMSDK
  VERSION 1.0.0
  LANGUAGES CXX
  DESCRIPTION "GStreamer QTI C++ SDK"
)

# Common point for setting version of all libs of gst-plugins-qcom package
set(GST_VERSION_REQUIRED "1.16")

set(QIMSDK_BINDIR "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}")
set(QIMSDK_LIBDIR "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

find_package(PkgConfig REQUIRED)

set(PKG_CONFIG_USE_CMAKE_PREFIX_PATH ON)
set(CMAKE_PREFIX_PATH "${PKG_CONFIG_LOCAL_PATH}")

# Get the pkgconfigs exported by the automake tools.
pkg_check_modules(GST REQUIRED gstreamer-1.0>=${GST_VERSION_REQUIRED})

# Resolve required QTI GStreamer base libraries from pkg-config.
pkg_check_modules(GST_QTI_MLBASE REQUIRED gstreamer-qcom-oss-ml-1.0>=1.0.0)
pkg_check_modules(GST_QTI_VIDEOBASE REQUIRED gstreamer-qcom-oss-video-1.0>=1.0.0)

# Resolve required yaml-cpp library from pkg-config.
pkg_check_modules(YAML_CPP REQUIRED yaml-cpp)

# Common compiler flags.
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wpedantic")

set(QTI_QIMSDK qimsdk-app-builder)

set(QTI_PUBLIC_HEADERS
  include/qti/qimsdk-appsink.h
  include/qti/qimsdk-appsrc.h
  include/qti/qimsdk-camsrc.h
  include/qti/qimsdk-ml-types.h
  include/qti/qimsdk-preprocess-base.h
  include/qti/qimsdk-postprocess-base.h
  include/qti/qimsdk-mlvconverter.h
  include/qti/qimsdk-mlpostprocess.h
  include/qti/qimsdk-mlvideotflitebin.h
  include/qti/qimsdk-mlvideoqnnbin.h
  include/qti/qimsdk-mlvideosnpebin.h
  include/qti/qimsdk-mlvideoonnxbin.h
  include/qti/qimsdk-buffer.h
  include/qti/qimsdk-element.h
  include/qti/qimsdk-logging.h
  include/qti/qimsdk-pipeline.h
  include/qti/qimsdk-stream-filter.h
  include/qti/qimsdk.h
)

add_library(${QTI_QIMSDK} SHARED
  src/elements/qimsdk-element.cc
  src/elements/qimsdk-appsink.cc
  src/elements/qimsdk-appsrc.cc
  src/elements/qimsdk-camsrc.cc
  src/elements/qimsdk-preprocess-base.cc
  src/elements/qimsdk-postprocess-base.cc
  src/elements/qimsdk-mlvconverter.cc
  src/elements/qimsdk-mlpostprocess.cc
  src/elements/qimsdk-mlvideotflitebin.cc
  src/elements/qimsdk-mlvideoqnnbin.cc
  src/elements/qimsdk-mlvideosnpebin.cc
  src/elements/qimsdk-mlvideoonnxbin.cc
  src/qimsdk-element-registry.cc
  src/qimsdk-logging.cc
  src/qimsdk-runtime.cc
  src/qimsdk-stream-filter.cc
  src/qimsdk-buffer.cc
  src/qimsdk-pipeline.cc
  src/qimsdk-pipeline-config.cc
)

set_target_properties(${QTI_QIMSDK} PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
)

target_include_directories(${QTI_QIMSDK} PRIVATE
  ${GST_INCLUDE_DIRS}
  ${GST_QTI_MLBASE_INCLUDE_DIRS}
  ${GST_QTI_VIDEOBASE_INCLUDE_DIRS}
  ${YAML_CPP_INCLUDE_DIRS}
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_include_directories(${QTI_QIMSDK} PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(${QTI_QIMSDK} PRIVATE
  ${GST_LIBRARIES}
  ${GST_QTI_MLBASE_LIBRARIES}
  ${GST_QTI_VIDEOBASE_LIBRARIES}
  ${YAML_CPP_LIBRARIES}
)

install(
  TARGETS ${QTI_QIMSDK}
  LIBRARY DESTINATION ${QIMSDK_LIBDIR}
  PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ
              GROUP_EXECUTE GROUP_READ
              WORLD_EXECUTE WORLD_READ
)

install(
  FILES ${QTI_PUBLIC_HEADERS}
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/qti
)

# QIMSDK test applications.
add_subdirectory(examples)
