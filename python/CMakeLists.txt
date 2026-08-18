cmake_minimum_required(VERSION 3.16)
project(qimsdk_python)

# ---- Python site-packages path ----
# In Yocto, this is usually passed via -DPYTHON_SITEPACKAGES_DIR
# Fallback for non-Yocto builds
if(NOT PYTHON_SITEPACKAGES_DIR)
    set(PYTHON_SITEPACKAGES_DIR "lib/python3/site-packages")
endif()

# ---- Install Python module (qimsdk) ----
install(
    DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/qimsdk/
    DESTINATION ${PYTHON_SITEPACKAGES_DIR}/qimsdk
    FILES_MATCHING
    PATTERN "*.py"
)

# ---- Install reference scripts into /usr/bin ----
file(GLOB EXAMPLE_SCRIPTS
    ${CMAKE_CURRENT_SOURCE_DIR}/examples/reference-apps/*.py
)

foreach(script ${EXAMPLE_SCRIPTS})
    get_filename_component(script_name ${script} NAME)

    install(
        PROGRAMS ${script}
        DESTINATION bin
    )
endforeach()

# ---- Install sample scripts into /usr/bin ----
file(GLOB EXAMPLE_SCRIPTS
    ${CMAKE_CURRENT_SOURCE_DIR}/examples/sample-apps/*.py
)

foreach(script ${EXAMPLE_SCRIPTS})
    get_filename_component(script_name ${script} NAME)

    install(
        PROGRAMS ${script}
        DESTINATION bin
    )
endforeach()
