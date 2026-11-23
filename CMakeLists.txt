cmake_minimum_required(VERSION 3.18)
project(hauntedcats_holdable CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Inline hooking library (used to hook GorillaTagger.Update)
include(FetchContent)
FetchContent_Declare(dobby GIT_REPOSITORY https://github.com/jmpews/Dobby.git GIT_TAG master)
set(DOBBY_GENERATE_SHARED OFF CACHE BOOL "" FORCE)
set(DOBBY_DEBUG OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(dobby)

add_library(hauntedcats_holdable SHARED src/holdable.cpp)
target_include_directories(hauntedcats_holdable PRIVATE ${dobby_SOURCE_DIR}/include)
target_compile_options(hauntedcats_holdable PRIVATE -fvisibility=hidden -O2)
target_link_libraries(hauntedcats_holdable PRIVATE dobby_static log dl)
# output: libhauntedcats_holdable.so

