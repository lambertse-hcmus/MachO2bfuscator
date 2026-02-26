cmake_minimum_required(VERSION 3.17)
project(MachO2fuscation VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# === Main library (all phases will add sources here) ===
set(PROJECT_NAME_LIB ${PROJECT_NAME}_lib)
set(PROJECT_NAME_CLI ${PROJECT_NAME})

add_library(${PROJECT_NAME_LIB}
    src/mach_reader.cpp
)

target_include_directories(${PROJECT_NAME_LIB} PUBLIC include)

# === CLI executable ===
add_executable(${PROJECT_NAME_CLI}
    src/main.cpp
)
target_link_libraries(${PROJECT_NAME_CLI} PRIVATE ${PROJECT_NAME_LIB})

# === Tests ===
enable_testing()
add_executable(test_mach_reader
    tests/test_mach_reader.cpp
)
target_link_libraries(test_mach_reader PRIVATE ${PROJECT_NAME_LIB})
add_test(NAME MachReaderTests COMMAND test_mach_reader)
