cmake_minimum_required(VERSION 3.25)
project(world_imagine VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(cmake/CompilerFlags.cmake)
include(FetchContent)

# ---------------------------------------------------------------------------
# 0. libTorch — must come first so TORCH_CXX_FLAGS affects all targets
# ---------------------------------------------------------------------------
# NVCC does not support C++23; cap CUDA at 17 before Torch probes the compiler
set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)
find_package(Torch REQUIRED)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${TORCH_CXX_FLAGS}")

# ---------------------------------------------------------------------------
# 1. Vulkan
# ---------------------------------------------------------------------------
find_package(Vulkan REQUIRED)

# ---------------------------------------------------------------------------
# 2. GLFW3 (no OpenGL context — GLFW_CLIENT_API = GLFW_NO_API at runtime)
# ---------------------------------------------------------------------------
find_package(glfw3 3.3 REQUIRED)

# ---------------------------------------------------------------------------
# 3. ImGui (docking branch) — vendored via FetchContent
# ---------------------------------------------------------------------------
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        docking
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui PUBLIC glfw Vulkan::Vulkan)
# Suppress warnings from vendored ImGui source
target_compile_options(imgui PRIVATE -w)

# ---------------------------------------------------------------------------
# 4. Vulkan Memory Allocator (header-only) — vendored via FetchContent
# ---------------------------------------------------------------------------
FetchContent_Declare(
    vma
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(vma)

add_library(vma INTERFACE)
target_include_directories(vma INTERFACE ${vma_SOURCE_DIR}/include)

# ---------------------------------------------------------------------------
# 5. Native File Dialog Extended (NFDe) — system open/save dialogs
# ---------------------------------------------------------------------------
FetchContent_Declare(
    nfd
    GIT_REPOSITORY https://github.com/btzy/nativefiledialog-extended.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)
set(NFD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nfd)

# ---------------------------------------------------------------------------
# 6. SPIR-V shader compilation via glslangValidator (renumbered)
# ---------------------------------------------------------------------------
find_program(GLSLANG_VALIDATOR glslangValidator REQUIRED)

file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/shaders)
file(GLOB GLSL_SOURCES "${CMAKE_SOURCE_DIR}/shaders/*.glsl")

foreach(glsl_file ${GLSL_SOURCES})
    get_filename_component(filename ${glsl_file} NAME)          # splat.vert.glsl
    get_filename_component(stem     ${glsl_file} NAME_WLE)      # splat.vert
    get_filename_component(ext      ${stem}       LAST_EXT)     # .vert
    string(SUBSTRING "${ext}" 1 -1 stage)                       # vert
    set(spv_file "${CMAKE_BINARY_DIR}/shaders/${stem}.spv")

    add_custom_command(
        OUTPUT  "${spv_file}"
        COMMAND "${GLSLANG_VALIDATOR}" -V -S "${stage}" "${glsl_file}" -o "${spv_file}"
        DEPENDS "${glsl_file}"
        COMMENT "Compiling ${filename} → ${stem}.spv"
        VERBATIM
    )
    list(APPEND SPV_OUTPUTS "${spv_file}")
endforeach()

add_custom_target(shaders ALL DEPENDS ${SPV_OUTPUTS})

# ---------------------------------------------------------------------------
# 7. Main executable
# ---------------------------------------------------------------------------
file(GLOB_RECURSE SOURCES "src/*.cpp")

add_executable(world_imagine ${SOURCES})
add_dependencies(world_imagine shaders)

target_include_directories(world_imagine PRIVATE src)
target_link_libraries(world_imagine PRIVATE
    imgui
    vma
    nfd
    glfw
    Vulkan::Vulkan
    "${TORCH_LIBRARIES}"
)
