# Kissat SAT Solver Integration
cmake_minimum_required(VERSION 3.15)

# 使用 ExternalProject 来构建 Kissat
include(ExternalProject)

set(KISSAT_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(KISSAT_BUILD_DIR ${CMAKE_CURRENT_BINARY_DIR}/kissat-build)

# 配置 Kissat 外部项目
ExternalProject_Add(
    kissat_external
    SOURCE_DIR ${KISSAT_SOURCE_DIR}
    BINARY_DIR ${KISSAT_BUILD_DIR}
    # Kissat's configure script creates a source-tree symlink.  Remove a
    # stale/dangling link first so repeated CMake builds remain idempotent.
    CONFIGURE_COMMAND ${CMAKE_COMMAND} -E chdir ${KISSAT_SOURCE_DIR}
                      sh -c "rm -f src/makefile && ./configure"
    # Build only the library.  The standalone Kissat executable expects its
    # own embedded Kitten object, which would collide with CaDiCaL's Kitten
    # implementation when both static libraries are linked into VeLoR.
    BUILD_COMMAND ${CMAKE_COMMAND} -E chdir ${KISSAT_SOURCE_DIR}/build
                  make libkissat.a
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS ${KISSAT_SOURCE_DIR}/build/libkissat.a
)

# 设置 Kissat 库路径
set(KISSAT_LIB_PATH ${KISSAT_SOURCE_DIR}/build/libkissat.a)

# 创建导入的静态库目标
add_library(kissat STATIC IMPORTED GLOBAL)
set_target_properties(kissat PROPERTIES
    IMPORTED_LOCATION ${KISSAT_LIB_PATH}
    IMPORTED_LOCATION_RELEASE ${KISSAT_LIB_PATH}
    IMPORTED_LOCATION_DEBUG ${KISSAT_LIB_PATH}
    INTERFACE_INCLUDE_DIRECTORIES ${KISSAT_SOURCE_DIR}/src
)

# 添加依赖关系
add_dependencies(kissat kissat_external)

# 设置数学库链接（Kissat 需要 libm）
target_link_libraries(kissat INTERFACE m)

# 确保库文件存在时才能使用
set_property(TARGET kissat PROPERTY IMPORTED_LOCATION_EXISTS TRUE)
