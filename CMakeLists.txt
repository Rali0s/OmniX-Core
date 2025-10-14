cmake_minimum_required(VERSION 3.16)
project(OmniXCore LANGUAGES CXX)

add_library(tze STATIC
    src/cache_coordinator.cpp
    src/knowledge_engine.cpp
    src/security_manager.cpp
    src/processing_engine.cpp
    src/operand.cpp
)

target_include_directories(tze PUBLIC include)
target_compile_features(tze PUBLIC cxx_std_20)

add_executable(omnix
    main.cpp
    FunctionAnalyzer.cpp
    src/versus_comparator.cpp
)

target_include_directories(omnix PRIVATE include .)
target_link_libraries(omnix PRIVATE tze)
