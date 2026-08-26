file(GLOB_RECURSE VELOR_SOURCES CONFIGURE_DEPENDS *.cpp)

set(MAIN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/velor.cpp)

set(SRC_FILES ${VELOR_SOURCES})
list(REMOVE_ITEM SRC_FILES ${MAIN_FILE})

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(velor ${MAIN_FILE} ${SRC_FILES})
target_compile_features(velor PRIVATE cxx_std_17)
target_compile_options(velor PRIVATE -fpermissive)

# Project and third-party include directories.
target_include_directories(velor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/command
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_SOURCE_DIR}/lib/kissat/src
    ${CMAKE_SOURCE_DIR}/lib/glucose
    ${CMAKE_SOURCE_DIR}/lib/minisat
    ${CMAKE_SOURCE_DIR}/lib/cadical/src
)

target_link_libraries(velor PRIVATE alice mockturtle kissat glucosep minisat-lib-static cadical)

install(TARGETS velor RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
