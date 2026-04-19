if(NOT DEFINED PACKAGE_DIR)
    message(FATAL_ERROR "PACKAGE_DIR is required.")
endif()

file(GLOB _packages "${PACKAGE_DIR}/*.tar.gz")
if(_packages STREQUAL "")
    message(FATAL_ERROR "No TGZ package was produced in ${PACKAGE_DIR}")
endif()

message(STATUS "Validated package artifacts in ${PACKAGE_DIR}")
