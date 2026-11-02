set(TARGET_NAME _gi_gst_qti)

Python3_add_library(${TARGET_NAME} MODULE
  gstmodule.c
)

target_include_directories(${TARGET_NAME} PRIVATE
  ${GLIB_INCLUDE_DIRS}
  ${GST_INCLUDE_DIRS}
  ${PYGOBJECT_INCLUDE_DIRS}
  ${Python3_INCLUDE_DIRS}
)

target_include_directories(${TARGET_NAME} INTERFACE
  $<BUILD_INTERFACE:${QTI_PLUGINS_BASE_SOURCE_DIR}>
)

target_link_libraries(${TARGET_NAME} PRIVATE
  ${GLIB_LIBRARIES}
  ${GST_LIBRARIES}
  ${PYGOBJECT_LIBRARIES}
  gstqtimlbase
  gstqtivideobase
)

install(
  TARGETS ${TARGET_NAME}
  LIBRARY DESTINATION ${PYTHON_SITEPACKAGES_DIR}/gi/overrides
  PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ
              GROUP_EXECUTE GROUP_READ
              WORLD_EXECUTE WORLD_READ
)

# Install the overrides so 'from gi.repository import' picks them up.
install(
  FILES ${CMAKE_CURRENT_SOURCE_DIR}/GstQtiML.py
        ${CMAKE_CURRENT_SOURCE_DIR}/GstQtiVideo.py
  DESTINATION ${PYTHON_SITEPACKAGES_DIR}/gi/overrides
)
find_file(QIMSDK_GSTQTIML_TYPELIB "GstQtiML-1.0.typelib"
  PATH_SUFFIXES "lib/girepository-1.0" "usr/lib/girepository-1.0"
)

if(TARGET GstQtiML-1.0 OR QIMSDK_GSTQTIML_TYPELIB)
  set(QIMSDK_PLATFORM_RUNTIME_DISTINFO
    "${CMAKE_CURRENT_BINARY_DIR}/qimsdk_platform_runtime-${QIMSDK_PLATFORM_RUNTIME_VERSION}.dist-info")

  configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/qimsdk-platform-runtime.METADATA.in
    ${QIMSDK_PLATFORM_RUNTIME_DISTINFO}/METADATA
    @ONLY
  )

  file(WRITE ${QIMSDK_PLATFORM_RUNTIME_DISTINFO}/INSTALLER "cmake\n")

  install(
    DIRECTORY ${QIMSDK_PLATFORM_RUNTIME_DISTINFO}
    DESTINATION ${PYTHON_SITEPACKAGES_DIR}
  )
endif()
