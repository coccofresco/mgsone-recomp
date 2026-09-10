# ps1Analyzer -- PS1 ELF Analyzer

# Library target (for linking into tests)
add_library(ps1Analyzer_lib
    src/elf_parser.cpp
    src/function_finder.cpp
    src/psyq_signatures.cpp
    src/config_generator.cpp
    src/disc_reader.cpp
    src/overlay_scanner.cpp
)

target_include_directories(ps1Analyzer_lib
    PUBLIC include
    PRIVATE src
)

target_link_libraries(ps1Analyzer_lib
    PUBLIC
        elfio
        fmt::fmt
        toml11::toml11
)

# Default search root for psyq_signatures.toml / psyq_metadata.toml. The
# matcher first honors PS1RECOMP_DATA_DIR from the environment, then this
# baked-in path (set to the in-tree data dir at build time).
target_compile_definitions(ps1Analyzer_lib
    PRIVATE
        PS1RECOMP_DATA_DIR_DEFAULT="${CMAKE_SOURCE_DIR}/ps1Analyzer/data"
)

# Executable target
add_executable(ps1Analyzer
    src/main.cpp
)

target_link_libraries(ps1Analyzer
    PRIVATE
        ps1Analyzer_lib
        toml11::toml11
)
