cmake_minimum_required(VERSION 3.16)
project(tools LANGUAGES C CXX)

if(ENABLE_APP_BUILDER_CPP OR ENABLE_APP_BUILDER_PYTHON)
  add_subdirectory(appbuilders-configs)
endif()
