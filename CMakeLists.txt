cmake_minimum_required(VERSION 3.20)
project(cpm VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
find_package(Threads REQUIRED)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Pass version to source code
add_compile_definitions(CPM_VERSION="${PROJECT_VERSION}")

# All source files (explicit list — safer than GLOB for incremental builds)
set(CPM_CORE_SOURCES
    src/package_manager.cpp
    src/installer.cpp
    src/builder.cpp
    src/downloader.cpp
    src/toml_parser.cpp
    src/resolver.cpp
    src/environment.cpp
    src/nix_env.cpp
    src/config.cpp
    src/progress.cpp
    src/process.cpp
)

add_library(cpm_core STATIC ${CPM_CORE_SOURCES})
target_include_directories(cpm_core PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(cpm_core PUBLIC Threads::Threads)

add_executable(cpm src/main.cpp)
target_link_libraries(cpm PRIVATE cpm_core)

option(CPM_ENABLE_SANITIZERS "Enable address and undefined-behavior sanitizers" OFF)
if(CPM_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(cpm_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(cpm_core PUBLIC -fsanitize=address,undefined)
endif()

if(BUILD_TESTING)
    add_executable(cpm_core_tests tests/core_tests.cpp)
    target_link_libraries(cpm_core_tests PRIVATE cpm_core)
    add_test(NAME cpm_core_tests COMMAND cpm_core_tests)
endif()

# Install
install(TARGETS cpm DESTINATION bin)
