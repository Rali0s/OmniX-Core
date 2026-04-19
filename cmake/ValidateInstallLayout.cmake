if(NOT DEFINED INSTALL_DIR)
    message(FATAL_ERROR "INSTALL_DIR is required.")
endif()

set(_binary "${INSTALL_DIR}/bin/omnix")
set(_data "${INSTALL_DIR}/share/omnix/res/tze.txt")

if(NOT EXISTS "${_binary}")
    message(FATAL_ERROR "Installed omnix binary not found at ${_binary}")
endif()

if(NOT EXISTS "${_data}")
    message(FATAL_ERROR "Installed runtime data not found at ${_data}")
endif()

message(STATUS "Validated install layout in ${INSTALL_DIR}")
