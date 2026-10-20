# Add library subdirectories

# Add alice (command shell library)
add_subdirectory(alice)

# Add mockturtle (logic synthesis library)
add_subdirectory(mockturtle)

# Add kissat SAT solver
add_subdirectory(kissat)

# Add glucose SAT solver
add_subdirectory(glucose)

# Add minisat SAT solver
add_subdirectory(minisat)

# Keep the vendored MiniSAT sources buildable with modern GCC without
# modifying the MiniSAT submodule itself.
target_compile_options(minisat-lib-static PRIVATE -fpermissive)
target_compile_options(minisat-lib-shared PRIVATE -fpermissive)
target_compile_options(minisat_core PRIVATE -fpermissive)
target_compile_options(minisat_simp PRIVATE -fpermissive)

# CaDiCaL ships a configure/make build but no CMake project.  Build its
# reusable solver sources directly so a fresh checkout does not depend on a
# separately generated CaDiCaL build directory.
file(GLOB VELOR_CADICAL_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/cadical/src/*.cpp")
file(GLOB VELOR_CADICAL_C_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/cadical/src/*.c")
list(APPEND VELOR_CADICAL_SOURCES ${VELOR_CADICAL_C_SOURCES})
list(FILTER VELOR_CADICAL_SOURCES EXCLUDE REGEX "/(cadical|mobical)\\.cpp$")

add_library(cadical STATIC ${VELOR_CADICAL_SOURCES})
target_compile_features(cadical PUBLIC cxx_std_11)
target_compile_definitions(cadical PRIVATE NBUILD)
# CaDiCaL and Kissat vendor different Kitten implementations with the same
# C symbol names.  Keep CaDiCaL's ABI private so both solvers can coexist.
set(CADICAL_KITTEN_SYMBOLS
    kitten_init kitten_clear kitten_release kitten_set_logging
    kitten_track_antecedents kitten_shuffle_clauses kitten_flip_phases
    kitten_randomize_phases kitten_assume kitten_assume_signed kitten_clause
    citten_clause_with_id kitten_unit kitten_binary
    kitten_clause_with_id_and_exception citten_clause_with_id_and_exception
    citten_clause_with_id_and_equivalence kitten_no_ticks_limit
    kitten_set_ticks_limit kitten_current_ticks kitten_no_terminator
    kitten_set_terminator kitten_solve kitten_status kitten_value
    kitten_signed_value kitten_fixed kitten_fixed_signed kitten_failed
    kitten_flip_literal kitten_flip_signed_literal kitten_compute_clausal_core
    kitten_shrink_to_clausal_core kitten_traverse_core_ids
    kitten_traverse_core_clauses kitten_traverse_core_clauses_with_id
    kitten_trace_core kitten_compute_prime_implicant
    kitten_add_prime_implicant kitten_flip_and_implicant_for_signed_literal
    new_learned_klause completely_backtrack_to_root_level)
foreach(symbol IN LISTS CADICAL_KITTEN_SYMBOLS)
    target_compile_definitions(cadical PRIVATE
        "${symbol}=cadical_${symbol}")
endforeach()
target_include_directories(cadical PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/cadical/src")
