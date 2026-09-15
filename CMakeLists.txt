cmake_minimum_required(VERSION 3.20)
project(yyasio LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Debug flags for coroutine development
add_compile_options(-g -O0 -fsanitize=address -fsanitize=undefined)
add_link_options(-fsanitize=address -fsanitize=undefined)

add_executable(usage usage.cpp)
target_link_libraries(usage PRIVATE uring)

option(BUILD_TEST "Build unit tests" OFF)
if(BUILD_TEST)
    find_package(Boost REQUIRED COMPONENTS unit_test_framework)
    add_executable(yyasio_unittest yyasio_unittest.cpp)
    target_link_libraries(yyasio_unittest PRIVATE uring Boost::unit_test_framework)
endif()
