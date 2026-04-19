if(NOT DEFINED PACKAGE_DIR)
    message(FATAL_ERROR "PACKAGE_DIR is required.")
endif()
if(NOT DEFINED SMOKE_SCRIPT)
    message(FATAL_ERROR "SMOKE_SCRIPT is required.")
endif()
if(NOT DEFINED EVIDENCE_DIR)
    message(FATAL_ERROR "EVIDENCE_DIR is required.")
endif()

file(GLOB smoke_packages "${PACKAGE_DIR}/omnix-*.tar.gz")
list(LENGTH smoke_packages smoke_package_count)
if(smoke_package_count EQUAL 0)
    message(FATAL_ERROR "No package found in ${PACKAGE_DIR}")
endif()

list(SORT smoke_packages)
math(EXPR smoke_package_index "${smoke_package_count} - 1")
list(GET smoke_packages ${smoke_package_index} smoke_package)
execute_process(
    COMMAND /bin/sh "${SMOKE_SCRIPT}" --package "${smoke_package}" --evidence-dir "${EVIDENCE_DIR}" --offline --skip-builds
    RESULT_VARIABLE smoke_result
    OUTPUT_VARIABLE smoke_stdout
    ERROR_VARIABLE smoke_stderr
)

if(NOT smoke_result EQUAL 0)
    message(FATAL_ERROR "Smoke helper failed.\nSTDOUT:\n${smoke_stdout}\nSTDERR:\n${smoke_stderr}")
endif()

file(GLOB smoke_evidence "${EVIDENCE_DIR}/*")
list(LENGTH smoke_evidence smoke_evidence_count)
if(smoke_evidence_count EQUAL 0)
    message(FATAL_ERROR "Smoke helper did not create evidence under ${EVIDENCE_DIR}")
endif()

message(STATUS "Smoke helper completed successfully.")
