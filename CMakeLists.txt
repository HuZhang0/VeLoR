cmake_minimum_required(VERSION 3.15)
project(velor VERSION 0.1.0 LANGUAGES C CXX)

# The bundled Alice/pybind11 sources predate CMake 3.5.  CMake 4.x no
# longer enables compatibility with such old policy versions implicitly;
# set the compatibility floor from the top-level project instead of editing
# the vendored subrepositories.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING
    "Minimum CMake policy version for bundled dependencies")

include(GNUInstallDirs)

# Require C++17.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default to Release for single-config generators.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose build type" FORCE)
endif()

# Common optimization flags.  Haswell is an x86-specific target; applying it
# unconditionally prevents compiler feature checks (including Threads) from
# working on ARM builders.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|i[3-6]86)$")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=haswell -mtune=haswell -fno-plt")
endif()
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pipe -fno-omit-frame-pointer")

# LTO is disabled because some bundled ABC C objects are not link-friendly with it.
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -ffast-math -funroll-loops -falign-functions=32")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g -march=native -mtune=native")
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g")

# Clang-specific tuning.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -mllvm -inline-threshold=100")
endif()

# Colored diagnostics and quieter Release builds.
if(UNIX)
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-fcolor-diagnostics" HAS_FCOLOR_DIAGNOSTICS)

    if(HAS_FCOLOR_DIAGNOSTICS)
        add_compile_options(-fcolor-diagnostics)
    endif()

    add_compile_options($<$<CONFIG:Release>:-w>)
endif()

# Show compiler diagnostics in a consistent format.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-pipe -fdiagnostics-show-option)
endif()

# pybind11 in the bundled Alice dependency still expects distutils, which is
# removed in Python 3.12. Prepend a small compatibility shim for configure time.
set(VELOR_PYTHON_SHIM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cmake/python")
if(EXISTS "${VELOR_PYTHON_SHIM_DIR}")
    if(DEFINED ENV{PYTHONPATH} AND NOT "$ENV{PYTHONPATH}" STREQUAL "")
        set(ENV{PYTHONPATH} "${VELOR_PYTHON_SHIM_DIR}:$ENV{PYTHONPATH}")
    else()
        set(ENV{PYTHONPATH} "${VELOR_PYTHON_SHIM_DIR}")
    endif()
endif()

# Third-party libraries and project sources.
add_subdirectory(lib)
add_subdirectory(src)

install(FILES
    ${CMAKE_SOURCE_DIR}/README.md
    ${CMAKE_SOURCE_DIR}/LICENSE
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/${PROJECT_NAME}
)
