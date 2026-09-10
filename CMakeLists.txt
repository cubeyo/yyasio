cmake_minimum_required(VERSION 3.20)
project(yyasio LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(usage usage.cpp)
target_link_libraries(usage PRIVATE uring)
