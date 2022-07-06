#cmake_minimum_required (VERSION 2.8)
#project (final)
#set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
#set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=c99")
#set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fpermissive")
#set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fpermissive")
#add_executable (final main.cpp fd_passing.h fd_passing.c)

cmake_minimum_required (VERSION 3.5)

project (final LANGUAGES CXX)

set (CMAKE_CXX_STANDARD 17)
set (CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories (${PROJECT_SOURCE_DIR})
add_executable (final main.cpp fd_passing.h fd_passing.cpp)
