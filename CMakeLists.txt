cmake_minimum_required(VERSION 3.20)
project(NuevoAuto)

if(WIN32)
    enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 20)

include(FetchContent)

FetchContent_Declare(
        SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-3.4.14
)

# Force SDL3 to build as a static library
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(SDL3)

FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG master
)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/imgui_demo.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp
)
target_include_directories(imgui PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui PUBLIC SDL3::SDL3-static)

add_executable(NuevoAuto
        src/main.cpp
        src/november.cpp
        src/november.h
        src/memory.cpp
        src/memory.h
        src/fileutil.cpp
        src/fileutil.h
        src/NACPU.cpp
        src/NACPU.h
        src/sysres_gen.cpp
        src/sysres_gen.h
        src/afterburnerII.cpp
        src/afterburnerII.h
        src/input.cpp
        src/input.h
        src/debugger.cpp
        src/debugger.h
        src/font_std.h
        src/charmap.h
        $<$<PLATFORM_ID:Windows>:${CMAKE_SOURCE_DIR}/src/app.rc>
)

target_include_directories(NuevoAuto PRIVATE
        ${CMAKE_SOURCE_DIR}/src
)

# Place the built executable and runtime assets into a dedicated 'bin' folder
# inside the build directory. This makes the exe and shipped asset folders
# appear in <build-dir>/bin instead of the default target file directory.
set_target_properties(NuevoAuto PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Optional: fully static binary (works best on MinGW; on Linux may need extra care)
if(MINGW OR WIN32)
    target_compile_options(NuevoAuto PRIVATE -static)
    target_link_options(NuevoAuto PRIVATE -static -static-libgcc -static-libstdc++)

    # MinGW setjmp / vsnprintf remapping (from original CMakeLists)
    target_link_options(NuevoAuto PRIVATE
            "-Wl,-u,__mingw_vsnprintf"
            "-Wl,--defsym,__ms_vsnprintf=__mingw_vsnprintf"
            "-Wl,-u,_setjmp"
            "-Wl,--defsym,__intrinsic_setjmpex=_setjmp"
    )
endif()

target_link_libraries(NuevoAuto PRIVATE SDL3::SDL3-static imgui)

# Ship asset directories next to the built exe
add_custom_command(TARGET NuevoAuto POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/bin"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/disk"
        "${CMAKE_BINARY_DIR}/bin/disk"
        COMMENT "Copying disk assets into bin"
)
if(EXISTS ${CMAKE_SOURCE_DIR}/emures)
    add_custom_command(TARGET NuevoAuto POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/bin"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/emures"
            "${CMAKE_BINARY_DIR}/bin/emures"
            COMMENT "Copying emures assets into bin"
    )
endif()

# Also copy the top-level disk image file (if present) into the bin folder
if(EXISTS ${CMAKE_SOURCE_DIR}/disk.nkg)
    add_custom_command(TARGET NuevoAuto POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/bin"
            COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_SOURCE_DIR}/disk.nkg"
            "${CMAKE_BINARY_DIR}/bin/disk.nkg"
            COMMENT "Copying disk.nkg into bin"
    )
endif()
