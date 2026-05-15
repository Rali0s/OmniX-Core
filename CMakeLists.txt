cmake_minimum_required(VERSION 3.16)
project(OmniXCore VERSION 0.2.0 LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)
find_package(PkgConfig QUIET)

set(GHOSTLINE_BUILD_QT OFF CACHE BOOL "Build Ghostline Qt operator shell from OmniX" FORCE)
if(EXISTS "${CMAKE_SOURCE_DIR}/vendor/ghostline-gate/CMakeLists.txt")
    add_subdirectory(vendor/ghostline-gate)
endif()

if(APPLE)
    set(OMNIX_PLATFORM_NAME "macos")
elseif(UNIX)
    set(OMNIX_PLATFORM_NAME "linux")
else()
    set(OMNIX_PLATFORM_NAME "${CMAKE_SYSTEM_NAME}")
endif()

string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" OMNIX_ARCH_NAME)

add_library(xpp STATIC
    src/xpp_parser.cpp
    src/xpp_index.cpp
    src/xpp_emitter.cpp
)

target_include_directories(xpp PUBLIC include)
target_compile_features(xpp PUBLIC cxx_std_20)

add_library(tze STATIC
    src/analyst_flow_interpreter.cpp
    src/build_flow_interpreter.cpp
    src/build_executor.cpp
    src/cache_coordinator.cpp
    src/definition_engine.cpp
    src/definition_flow_interpreter.cpp
    src/io_runtime.cpp
    src/intent_resolver.cpp
    src/language_engine.cpp
    src/knowledge_engine.cpp
    src/memory_store.cpp
    src/native_tool_registry.cpp
    src/namespace_registry.cpp
    src/neural_math_engine.cpp
    src/neural_signal_router.cpp
    src/omni_bridge.cpp
    src/packet_capture_engine.cpp
    src/preprocessor_runtime.cpp
    src/project_alias_registry.cpp
    src/project_resolver.cpp
    src/recipe_authoring_engine.cpp
    src/security_manager.cpp
    src/self_review.cpp
    src/shell_lexicon.cpp
    src/sdk.cpp
    src/tool_flow_interpreter.cpp
    src/unix_evidence_parser.cpp
    src/processing_engine.cpp
    src/query_runtime.cpp
    src/reasoning_provider.cpp
    src/session_coordinator.cpp
    src/types.cpp
    src/operand.cpp
    src/workflow_support.cpp
)

target_include_directories(tze PUBLIC include)
target_compile_features(tze PUBLIC cxx_std_20)
target_link_libraries(tze PUBLIC xpp)
target_compile_definitions(tze PUBLIC
    OMNIX_VERSION="${PROJECT_VERSION}"
    OMNIX_BUILD_DIR="${CMAKE_BINARY_DIR}"
    OMNIX_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

if(PkgConfig_FOUND)
    pkg_check_modules(OMNIX_PCAP QUIET libpcap)
endif()
find_path(OMNIX_PCAP_INCLUDE_DIR
    NAMES pcap/pcap.h pcap.h
    HINTS ${OMNIX_PCAP_INCLUDE_DIRS} /opt/homebrew/opt/libpcap/include /usr/local/opt/libpcap/include
)
find_library(OMNIX_PCAP_LIBRARY
    NAMES pcap
    HINTS ${OMNIX_PCAP_LIBRARY_DIRS} /opt/homebrew/opt/libpcap/lib /usr/local/opt/libpcap/lib
)
if(OMNIX_PCAP_INCLUDE_DIR AND OMNIX_PCAP_LIBRARY)
    target_include_directories(tze PUBLIC ${OMNIX_PCAP_INCLUDE_DIR})
    target_link_libraries(tze PUBLIC ${OMNIX_PCAP_LIBRARY})
    target_compile_definitions(tze PUBLIC OMNIX_HAS_LIBPCAP=1)
endif()

add_executable(omnix
    main.cpp
)

target_include_directories(omnix PRIVATE include .)
target_link_libraries(omnix PRIVATE tze)

add_executable(versus_demo
    examples/versus_demo.cpp
    FunctionAnalyzer.cpp
    src/versus_comparator.cpp
)

target_include_directories(versus_demo PRIVATE include .)
target_compile_features(versus_demo PRIVATE cxx_std_20)

add_executable(omnix_tests
    tests/xpp_tests.cpp
)

target_include_directories(omnix_tests PRIVATE include .)
target_compile_features(omnix_tests PRIVATE cxx_std_20)
target_link_libraries(omnix_tests PRIVATE tze)
target_compile_definitions(omnix_tests PRIVATE
    OMNIX_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    OMNIX_BINARY_DIR="${CMAKE_BINARY_DIR}"
)

add_test(NAME omnix_unit COMMAND omnix_tests)
add_test(NAME omnix_cli_version COMMAND omnix --version)
add_test(NAME omnix_cli_map COMMAND omnix map ${CMAKE_SOURCE_DIR}/res/tze.txt)
add_test(NAME omnix_cli_search COMMAND omnix search "<~>" ${CMAKE_SOURCE_DIR}/res/tze.txt)
add_test(NAME omnix_cli_emit COMMAND omnix emit-cpp ${CMAKE_SOURCE_DIR}/res/tze.txt ${CMAKE_BINARY_DIR}/ctest-generated/xpp)
add_test(
    NAME omnix_cli_define
    COMMAND omnix define xProcessingCache ${CMAKE_SOURCE_DIR}/res/tze.txt --memory-root ${CMAKE_BINARY_DIR}/ctest-memory-define
)
add_test(
    NAME omnix_cli_ask_toolchain
    COMMAND omnix ask toolchain --memory-root ${CMAKE_BINARY_DIR}/ctest-memory-toolchain --source-map ${CMAKE_SOURCE_DIR}/res/tze.txt
)
add_test(
    NAME omnix_cli_preflight_nmap
    COMMAND omnix preflight nmap --memory-root ${CMAKE_BINARY_DIR}/ctest-memory-preflight --offline --source-map ${CMAKE_SOURCE_DIR}/res/tze.txt
)
add_test(
    NAME omnix_cli_doctor_nmap
    COMMAND omnix doctor nmap --memory-root ${CMAKE_BINARY_DIR}/ctest-memory-doctor --offline --source-map ${CMAKE_SOURCE_DIR}/res/tze.txt
)
add_test(
    NAME omnix_cli_build_current_repo
    COMMAND omnix build-cmake ${CMAKE_SOURCE_DIR} --target omnix --clean --build-dir ${CMAKE_BINARY_DIR}/ctest-build-flow --memory-root ${CMAKE_BINARY_DIR}/ctest-memory-build-current
)

install(TARGETS omnix
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/res/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/omnix/res
)
install(FILES ${CMAKE_SOURCE_DIR}/docs/smoke-tests.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/omnix/docs
)

if(APPLE OR UNIX)
    set(CPACK_GENERATOR "TGZ")
endif()
set(CPACK_PACKAGE_NAME "omnix")
set(CPACK_PACKAGE_VENDOR "OmniX")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "omnix-${CPACK_PACKAGE_VERSION}-${OMNIX_PLATFORM_NAME}-${OMNIX_ARCH_NAME}")
include(CPack)

add_test(
    NAME omnix_package_install_layout
    COMMAND ${CMAKE_COMMAND} --install ${CMAKE_BINARY_DIR} --prefix ${CMAKE_BINARY_DIR}/ctest-install
)
add_test(
    NAME omnix_package_install_validate
    COMMAND ${CMAKE_COMMAND}
            -DINSTALL_DIR=${CMAKE_BINARY_DIR}/ctest-install
            -P ${CMAKE_SOURCE_DIR}/cmake/ValidateInstallLayout.cmake
)
set_tests_properties(omnix_package_install_validate PROPERTIES DEPENDS omnix_package_install_layout)

add_test(
    NAME omnix_package_tgz
    COMMAND ${CMAKE_COMMAND} -E chdir ${CMAKE_BINARY_DIR} ${CMAKE_CPACK_COMMAND} -G TGZ -B ${CMAKE_BINARY_DIR}/ctest-packages
)
add_test(
    NAME omnix_package_tgz_validate
    COMMAND ${CMAKE_COMMAND}
            -DPACKAGE_DIR=${CMAKE_BINARY_DIR}/ctest-packages
            -P ${CMAKE_SOURCE_DIR}/cmake/ValidatePackageArtifacts.cmake
)
set_tests_properties(omnix_package_tgz_validate PROPERTIES DEPENDS omnix_package_tgz)

add_custom_target(release_local
    COMMAND /bin/sh ${CMAKE_SOURCE_DIR}/scripts/release_local.sh --build-dir ${CMAKE_BINARY_DIR}
    DEPENDS omnix omnix_tests
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Building local OmniX release artifacts"
)

add_test(
    NAME omnix_release_local
    COMMAND /bin/sh ${CMAKE_SOURCE_DIR}/scripts/release_local.sh --build-dir ${CMAKE_BINARY_DIR} --skip-tests
)
add_test(
    NAME omnix_release_local_validate
    COMMAND ${CMAKE_COMMAND}
            -DRELEASE_DIR=${CMAKE_BINARY_DIR}/release
            -P ${CMAKE_SOURCE_DIR}/cmake/ValidateReleaseArtifacts.cmake
)
set_tests_properties(omnix_release_local_validate PROPERTIES DEPENDS omnix_release_local)

add_test(
    NAME omnix_smoke_helper
    COMMAND ${CMAKE_COMMAND}
            -DPACKAGE_DIR=${CMAKE_BINARY_DIR}/ctest-packages
            -DSMOKE_SCRIPT=${CMAKE_SOURCE_DIR}/scripts/smoke_test_package.sh
            -DEVIDENCE_DIR=${CMAKE_BINARY_DIR}/ctest-smoke-evidence
            -P ${CMAKE_SOURCE_DIR}/cmake/RunSmokeHelper.cmake
)
set_tests_properties(omnix_smoke_helper PROPERTIES DEPENDS omnix_package_tgz)

add_custom_target(validate_generated_xpp
    COMMAND $<TARGET_FILE:omnix> emit-cpp ${CMAKE_SOURCE_DIR}/res/tze.txt ${CMAKE_BINARY_DIR}/generated/xpp
    COMMAND ${CMAKE_COMMAND}
            -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/generated/xpp
            -DCOMPILER=${CMAKE_CXX_COMPILER}
            -P ${CMAKE_SOURCE_DIR}/cmake/ValidateGeneratedXpp.cmake
    DEPENDS omnix
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Generating and compiling X++ stub artifacts"
)

add_custom_target(validate_tze_conformance
    COMMAND $<TARGET_FILE:omnix_tests>
    DEPENDS omnix_tests
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running source-vs-runtime TZE conformance checks"
)
