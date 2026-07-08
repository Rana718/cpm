cmake_minimum_required(VERSION 3.20)
project(cpm VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Source files
file(GLOB_RECURSE CPM_SOURCES "src/*.cpp")

add_executable(cpm ${CPM_SOURCES})

target_include_directories(cpm PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(cpm PRIVATE pthread)

# Install
install(TARGETS cpm DESTINATION bin)
