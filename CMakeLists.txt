cmake_minimum_required(VERSION 3.16)
project(qimsdk LANGUAGES C CXX)

include(GNUInstallDirs)

# App Builder SDKs
option(ENABLE_APP_BUILDER_CPP "Enable C++ App Builder SDK" OFF)
option(ENABLE_APP_BUILDER_PYTHON "Enable Python App Builder SDK" OFF)

add_subdirectory(gstreamer)

if(ENABLE_APP_BUILDER_CPP)
  message(STATUS "Enabling C++ App Builder SDK")
  add_subdirectory(cpp)
endif()

if(ENABLE_APP_BUILDER_PYTHON)
  message(STATUS "Enabling Python App Builder SDK")
  add_subdirectory(python)
endif()

add_subdirectory(tools)
