# ── GoogleTest ──────────────────────────────────────────────────────
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
# Prevent GTest from overriding our compiler/linker settings on Windows
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# ── Tests ───────────────────────────────────────────────────────────
add_definitions(-DTEST_OBJC_ABSOLUTE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/assets/testckey_objc")

enable_testing()
include(GoogleTest)

macro(add_macho2fuscator_test test_name)
  add_executable(${test_name}
    ${test_name}.cpp
  )
  target_link_libraries(${test_name} PRIVATE ${PROJECT_NAME_LIB} GTest::gtest_main)
  gtest_discover_tests(${test_name})
endmacro()

add_macho2fuscator_test(test_mach_reader)
add_macho2fuscator_test(test_objc_extractor)
add_macho2fuscator_test(test_symbol_sets)
add_macho2fuscator_test(test_mangler)
add_macho2fuscator_test(test_binary_patcher)
add_macho2fuscator_test(test_obfuscator)
