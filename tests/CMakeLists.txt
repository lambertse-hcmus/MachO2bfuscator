macro(add_macho2fuscator_test test_name)
  add_executable(${test_name}
    ${test_name}.cpp
  )
  target_link_libraries(${test_name} PRIVATE ${PROJECT_NAME_LIB})
  add_test(NAME ${test_name} COMMAND ${test_name})
endmacro()

enable_testing()
add_macho2fuscator_test(test_mach_reader)
add_macho2fuscator_test(test_objc_extractor)
add_macho2fuscator_test(test_symbol_sets)
add_macho2fuscator_test(test_mangler)
add_macho2fuscator_test(test_binary_patcher)
