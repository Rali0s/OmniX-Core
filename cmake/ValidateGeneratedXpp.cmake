if(NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "OUTPUT_DIR must be provided.")
endif()

if(NOT DEFINED COMPILER)
    message(FATAL_ERROR "COMPILER must be provided.")
endif()

set(MANIFEST_FILE "${OUTPUT_DIR}/manifest.txt")
if(NOT EXISTS "${MANIFEST_FILE}")
    message(FATAL_ERROR "Manifest not found: ${MANIFEST_FILE}")
endif()

file(STRINGS "${MANIFEST_FILE}" GENERATED_SOURCES)
if(NOT GENERATED_SOURCES)
    message(FATAL_ERROR "No generated sources were listed in ${MANIFEST_FILE}")
endif()

file(MAKE_DIRECTORY "${OUTPUT_DIR}/obj")

foreach(GENERATED_SOURCE IN LISTS GENERATED_SOURCES)
    get_filename_component(SOURCE_NAME_WE "${GENERATED_SOURCE}" NAME_WE)
    set(OBJECT_FILE "${OUTPUT_DIR}/obj/${SOURCE_NAME_WE}.o")

    execute_process(
        COMMAND "${COMPILER}" "-std=c++20" "-I${OUTPUT_DIR}" "-c" "${GENERATED_SOURCE}" "-o" "${OBJECT_FILE}"
        RESULT_VARIABLE COMPILE_RESULT
        OUTPUT_VARIABLE COMPILE_STDOUT
        ERROR_VARIABLE COMPILE_STDERR
    )

    if(NOT COMPILE_RESULT EQUAL 0)
        message(FATAL_ERROR
            "Failed compiling generated source: ${GENERATED_SOURCE}\n"
            "stdout:\n${COMPILE_STDOUT}\n"
            "stderr:\n${COMPILE_STDERR}")
    endif()
endforeach()

message(STATUS "Validated generated X++ sources in ${OUTPUT_DIR}")
