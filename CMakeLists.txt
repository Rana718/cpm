cmake_minimum_required(VERSION 3.20)
project(cpm VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Pass version to source code
add_compile_definitions(CPM_VERSION="${PROJECT_VERSION}")

# All source files (explicit list — safer than GLOB for incremental builds)
set(CPM_SOURCES
    src/main.cpp
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
)

add_executable(cpm ${CPM_SOURCES})

target_include_directories(cpm PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(cpm PRIVATE pthread)

# Install
install(TARGETS cpm DESTINATION bin)
