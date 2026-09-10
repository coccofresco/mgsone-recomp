# ps1Interface -- PS1 Recompiler Studio GUI

cmake_minimum_required(VERSION 3.20)

include(FetchContent)

# ImGui (docking branch)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        docking
    GIT_SHALLOW    TRUE
)

# ImGuiColorTextEdit
FetchContent_Declare(
    imgui_colortextedit
    GIT_REPOSITORY https://github.com/BalazsJako/ImGuiColorTextEdit.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)

# imgui_club (memory editor header)
FetchContent_Declare(
    imgui_club
    GIT_REPOSITORY https://github.com/ocornut/imgui_club.git
    GIT_TAG        main
    GIT_SHALLOW    TRUE
)

# ImGuiFileDialog
FetchContent_Declare(
    imgui_file_dialog
    GIT_REPOSITORY https://github.com/aiekick/ImGuiFileDialog.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)

# SDL2
FetchContent_Declare(
    SDL2
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-2.30.9
    GIT_SHALLOW    TRUE
)

set(SDL_SHARED ON  CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(imgui imgui_colortextedit imgui_club SDL2)

FetchContent_GetProperties(imgui_file_dialog)
if(NOT imgui_file_dialog_POPULATED)
    FetchContent_Populate(imgui_file_dialog)
endif()

find_package(OpenGL REQUIRED)

# imgui static lib
set(IMGUI_SOURCES
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)

add_library(imgui_lib STATIC ${IMGUI_SOURCES})
target_include_directories(imgui_lib PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui_lib PUBLIC SDL2::SDL2 OpenGL::GL)

# ImGuiColorTextEdit
add_library(imgui_colortextedit_lib STATIC
    ${imgui_colortextedit_SOURCE_DIR}/TextEditor.cpp
)
target_include_directories(imgui_colortextedit_lib PUBLIC
    ${imgui_colortextedit_SOURCE_DIR}
)
target_link_libraries(imgui_colortextedit_lib PUBLIC imgui_lib)

# imgui_memory_editor (header-only)
add_library(imgui_memory_editor_lib INTERFACE)
target_include_directories(imgui_memory_editor_lib INTERFACE
    ${imgui_club_SOURCE_DIR}/imgui_memory_editor
)
target_link_libraries(imgui_memory_editor_lib INTERFACE imgui_lib)

# ImGuiFileDialog
add_library(imgui_filedialog_lib STATIC
    ${imgui_file_dialog_SOURCE_DIR}/ImGuiFileDialog.cpp
)
target_include_directories(imgui_filedialog_lib PUBLIC
    ${imgui_file_dialog_SOURCE_DIR}
    ${imgui_SOURCE_DIR}
)
target_link_libraries(imgui_filedialog_lib PUBLIC imgui_lib)

# ps1Interface executable
file(GLOB_RECURSE INTERFACE_SOURCES "src/*.cpp")

add_executable(ps1Interface ${INTERFACE_SOURCES})

target_include_directories(ps1Interface PRIVATE
    include
)

target_link_libraries(ps1Interface PRIVATE
    ps1Analyzer_lib
    imgui_lib
    imgui_colortextedit_lib
    imgui_memory_editor_lib
    imgui_filedialog_lib
    fmt::fmt
    toml11::toml11
    SDL2::SDL2
    OpenGL::GL
)

if(WIN32)
    target_link_libraries(ps1Interface PRIVATE SDL2::SDL2main)
endif()
