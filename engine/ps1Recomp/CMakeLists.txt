# ps1Recomp -- PS1 Static Recompiler

# Library target (for linking into tests)
add_library(ps1Recomp_lib
    src/mips_decoder.cpp
    src/instruction_emitter.cpp
    src/gte_emitter.cpp
    src/overlay_handler.cpp
    src/hle_emitter.cpp
    src/dispatch_emitter.cpp
)

target_include_directories(ps1Recomp_lib
    PUBLIC include
    PRIVATE src
)

target_link_libraries(ps1Recomp_lib
    PUBLIC
        fmt::fmt
        toml11::toml11
        ps1Analyzer_lib
)

# Executable target
add_executable(ps1Recomp
    src/main.cpp
)

target_link_libraries(ps1Recomp
    PRIVATE
        ps1Recomp_lib
        toml11::toml11
        ps1Analyzer_lib
)
