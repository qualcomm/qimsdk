cmake_minimum_required(VERSION 3.16)
project(qimsdk LANGUAGES C CXX)

include(GNUInstallDirs)

add_subdirectory(gstreamer)

if(ENABLE_APP_BUILDER_CPP)
  add_subdirectory(cpp)
endif()

if(ENABLE_APP_BUILDER_PYTHON)
  add_subdirectory(python)
endif()
