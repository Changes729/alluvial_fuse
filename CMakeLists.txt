cmake_minimum_required(VERSION 4.0.0)
project(alluvial_fuse VERSION 0.1.0)

## Compiler flags
include(CheckCXXCompilerFlag)
CHECK_CXX_COMPILER_FLAG("-std=c++20" COMPILER_SUPPORTS_CXX20)
CHECK_CXX_COMPILER_FLAG("-std=c++17" COMPILER_SUPPORTS_CXX17)
if(COMPILER_SUPPORTS_CXX20)
  set(CMAKE_CXX_STANDARD 20)
elseif(COMPILER_SUPPORTS_CXX17)
  set(CMAKE_CXX_STANDARD 17)
else()
  message(FATAL_ERROR "需要 C++17 及以上编译器")
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)

## extra compiler warnings（不启用 -Werror，便于跨 glibc/libfuse 版本构建）
add_compile_options(-Wall -Wextra)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

find_package(PkgConfig REQUIRED)

add_subdirectory(src)

## Install: default to user-local prefix (~/.local), binary lands in ~/.local/bin
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/.local" CACHE PATH "install prefix" FORCE)
endif()

install(TARGETS ${PROJECT_NAME}
  RUNTIME DESTINATION bin
)

## CPack packaging configuration
set(CPACK_PACKAGE_NAME "${PROJECT_NAME}")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VENDOR "alluvial_fuse")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "alluvial_fuse - userspace overlay filesystem with TCP control plane")
set(CPACK_GENERATOR "TGZ;ZIP")
include(CPack)
