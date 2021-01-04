cmake_minimum_required(VERSION 3.13.4)
project(GChecker)

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} in: ${LLVM_INSTALL_PREFIX}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")
find_package(Clang REQUIRED CONFIG HINTS "${LLVM_INSTALL_PREFIX}/lib/cmake/clang")
message(STATUS "Found Clang in: ${CLANG_INSTALL_PREFIX}")

if (${LLVM_VERSION_MAJOR} LESS 11)
  message(FATAL_ERROR "ERROR: LLVM version 11 or higher required")
endif()

if (NOT LLVM_ENABLE_EH)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-exceptions")
endif()
if (NOT LLVM_ENABLE_RTTI)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti")
endif()

if (MSVC)
  add_compile_options(/W4 /WX)
else()
  add_compile_options(-Wall -Wextra -Wcast-qual -Wimplicit-fallthrough -pedantic)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

add_library(gchecker_lib OBJECT src/GChecker.cpp)

add_executable(gchecker src/GCheckerMain.cpp)

target_link_libraries(gchecker gchecker_lib clang-cpp)
if (LLVM_LINK_LLVM_DYLIB)
  target_link_libraries(gchecker LLVM)
endif()

add_subdirectory(test)
