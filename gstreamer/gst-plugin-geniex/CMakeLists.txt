cmake_minimum_required(VERSION 3.16)
project(GST_PLUGIN_QTI_OSS_GENIEX
  VERSION ${GST_PLUGINS_QTI_OSS_VERSION}
  LANGUAGES C
)

find_package(PkgConfig REQUIRED)

pkg_check_modules(GST REQUIRED gstreamer-1.0>=${GST_VERSION_REQUIRED})

set(GENIEX_ROOT "" CACHE PATH "GenieX pkg-geniex package root")
if(NOT GENIEX_ROOT AND DEFINED ENV{GENIEX_ROOT})
  set(GENIEX_ROOT "$ENV{GENIEX_ROOT}")
endif()

find_path(GENIEX_INCLUDE_DIR geniex.h HINTS "${GENIEX_ROOT}/include")
find_path(GENIEX_MODEL_INCLUDE_DIR geniex_model.h HINTS "${GENIEX_ROOT}/include")
find_library(GENIEX_LIBRARY NAMES geniex HINTS "${GENIEX_ROOT}/lib")
if(NOT GENIEX_ROOT OR NOT GENIEX_INCLUDE_DIR OR
   NOT GENIEX_MODEL_INCLUDE_DIR OR NOT GENIEX_LIBRARY)
  message(FATAL_ERROR
    "GenieX package not found. Set GENIEX_ROOT to the pkg-geniex directory.")
endif()

configure_file(config.h.in config.h @ONLY)

add_library(gstgeniex SHARED geniex.c)
target_compile_definitions(gstgeniex PRIVATE HAVE_CONFIG_H)
target_compile_options(gstgeniex PRIVATE
  -Wall -Wextra -Werror -Wno-unused-parameter
  ${GST_CFLAGS_OTHER}
)
target_include_directories(gstgeniex PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}"
  "${GENIEX_INCLUDE_DIR}"
  "${GENIEX_MODEL_INCLUDE_DIR}"
  ${GST_INCLUDE_DIRS}
)
target_link_directories(gstgeniex PRIVATE ${GST_LIBRARY_DIRS})
target_link_libraries(gstgeniex PRIVATE ${GST_LIBRARIES} "${GENIEX_LIBRARY}")
target_link_options(gstgeniex PRIVATE ${GST_LDFLAGS_OTHER})
set_target_properties(gstgeniex PROPERTIES
  BUILD_RPATH "${GENIEX_ROOT}/lib"
  INSTALL_RPATH "$ORIGIN/.."
)

install(
  TARGETS gstgeniex
  LIBRARY DESTINATION ${GST_PLUGINS_QTI_OSS_INSTALL_LIBDIR}/gstreamer-1.0
  PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ
              GROUP_EXECUTE GROUP_READ
              WORLD_EXECUTE WORLD_READ
)
