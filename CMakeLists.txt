cmake_minimum_required(VERSION 3.17)
project(MachO2bfuscator VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Debug by default
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -O0")
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -g -O0")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3")
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O3")

set(ENABLE_TESTING ON CACHE BOOL "Enable testing")
# === Main library (all phases will add sources here) ===
set(PROJECT_NAME_LIB ${PROJECT_NAME}_lib)
set(PROJECT_NAME_CLI ${PROJECT_NAME})

add_library(${PROJECT_NAME_LIB}
    src/mach_reader.cpp
    src/objc_types.cpp
    src/objc_extractor.cpp
    src/symbol_sets.cpp
    src/mangler.cpp
    src/binary_patcher.cpp
    src/obfuscator.cpp
)

target_include_directories(${PROJECT_NAME_LIB} PUBLIC include)

# === CLI executable ===
add_executable(${PROJECT_NAME_CLI}
    src/main.cpp
)
target_link_libraries(${PROJECT_NAME_CLI} PRIVATE ${PROJECT_NAME_LIB})

if(ENABLE_TESTING)
    add_subdirectory(tests)
endif()
