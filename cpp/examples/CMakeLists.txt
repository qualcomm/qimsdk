# QIMSDK test apps and helpers.

# Shared test utilities library.
add_library(qti-qimsdk-test-utils STATIC
  qimsdk-test-utils.cc
)

target_include_directories(qti-qimsdk-test-utils PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)

# Test applications.
function(add_example_subdirs parent_dir)
  file(GLOB children RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/${parent_dir}"
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/${parent_dir}/*")

  foreach(child IN LISTS children)
    if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${parent_dir}/${child}"
       AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${parent_dir}/${child}/CMakeLists.txt")
      add_subdirectory("${parent_dir}/${child}")
    endif()
  endforeach()
endfunction()

add_example_subdirs(reference-apps)
add_example_subdirs(demo-apps)
