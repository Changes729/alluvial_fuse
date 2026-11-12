include_directories(.)
include_directories(${CMAKE_CURRENT_BINARY_DIR})
include_directories(overlay)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FUSE3 REQUIRED fuse3)

add_compile_definitions(FUSE_USE_VERSION=31)

add_executable( ${PROJECT_NAME}
  main.cpp

  overlay/atfs.cpp
  overlay/overlay.cpp
  overlay/util.cpp
  overlay/ops.cpp
)
target_include_directories(${PROJECT_NAME} PRIVATE ${FUSE3_INCLUDE_DIRS})
target_link_directories(${PROJECT_NAME} PRIVATE ${FUSE3_LIBRARY_DIRS})
target_link_libraries(${PROJECT_NAME} PRIVATE ${FUSE3_LIBRARIES} pthread)
target_compile_options(${PROJECT_NAME} PRIVATE ${FUSE3_CFLAGS_OTHER})
