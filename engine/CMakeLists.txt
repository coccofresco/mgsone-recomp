cmake_minimum_required(VERSION 3.20)
project(PS1Recomp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# AddressSanitizer (debug aid — heap/stack/global overflow detection).
# Enable with: cmake -B build-asan -DPS1RECOMP_ASAN=ON
# Run with:    ASAN_OPTIONS=detect_leaks=0 ./build-asan/ps1Runtime/ps1Runtime ...
# Cost: ~2x runtime, ~3x memory.  Catches the precise file:line where a
# heap corruption originates — the right tool for "free(): corrupted
# unsorted chunks" type failures (see ISSUES.md #1 for context).
option(PS1RECOMP_ASAN "Build with AddressSanitizer + UBSan" OFF)
if(PS1RECOMP_ASAN)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
    message(STATUS "ASan + UBSan enabled — runtime will be ~2x slower")
endif()

# Third-party libraries
add_subdirectory(third_party/ELFIO)
add_subdirectory(third_party/toml11)
add_subdirectory(third_party/fmt)

# Project components
add_subdirectory(ps1Analyzer)
add_subdirectory(ps1Recomp)
add_subdirectory(ps1Runtime)

# GUI Studio (optional, disabled by default — requires SDL2 + OpenGL)
option(PS1RECOMP_BUILD_INTERFACE "Build ps1Interface GUI studio" OFF)
if(PS1RECOMP_BUILD_INTERFACE)
    add_subdirectory(ps1Interface)
endif()

# Tests (optional, enabled by default)
option(PS1RECOMP_BUILD_TESTS "Build tests" ON)
if(PS1RECOMP_BUILD_TESTS)
    # GoogleTest — disable install to avoid polluting the project
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    add_subdirectory(third_party/googletest)
    include(GoogleTest)
    enable_testing()
    add_subdirectory(ps1Test)
endif()

# Documentation (optional — requires Doxygen)
# Build with: cmake --build build --target docs
# Output:     docs/api/html/index.html
find_package(Doxygen OPTIONAL_COMPONENTS dot)
if(DOXYGEN_FOUND)
    set(DOXYGEN_PROJECT_NAME        "ps1-recomp")
    set(DOXYGEN_PROJECT_BRIEF       "PlayStation 1 static recompiler — MIPS R3000A to native C++")
    set(DOXYGEN_EXTRACT_ALL         NO)
    set(DOXYGEN_EXTRACT_STATIC      YES)
    set(DOXYGEN_GENERATE_HTML       YES)
    set(DOXYGEN_GENERATE_LATEX      NO)
    set(DOXYGEN_OUTPUT_DIRECTORY    ${CMAKE_SOURCE_DIR}/docs/api)
    set(DOXYGEN_USE_MDFILE_AS_MAINPAGE README.md)
    set(DOXYGEN_WARN_IF_UNDOCUMENTED NO)
    set(DOXYGEN_QUIET               YES)
    set(DOXYGEN_STRIP_FROM_PATH     ${CMAKE_SOURCE_DIR})
    doxygen_add_docs(docs
        ${CMAKE_SOURCE_DIR}/ps1Runtime/include/runtime
        ${CMAKE_SOURCE_DIR}/ps1Recomp/include
        ${CMAKE_SOURCE_DIR}/ps1Analyzer/include
        COMMENT "Generating API documentation -> docs/api/html/index.html"
    )
else()
    message(STATUS "Doxygen not found — 'docs' target unavailable (install doxygen to enable)")
endif()
