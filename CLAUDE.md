# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Is

World Imagine is a Gaussian Splatting viewer and video-to-splat pipeline built with C++23, Vulkan, GLFW, ImGui (docking branch), and LibTorch. It imports PLY splat files (or eventually video via FFmpeg → COLMAP → OpenSplat), renders them as 3D Gaussian splats in a Vulkan offscreen pass, and displays them in an ImGui viewport.

## Build Commands

```bash
# Configure (required after adding new .cpp files — CMake uses GLOB_RECURSE)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/local/libtorch

# Build
cmake --build build -j$(nproc)

# Run
LD_LIBRARY_PATH=$HOME/local/libtorch/lib:/usr/local/cuda/lib64 ./build/world_imagine
```

There are no tests or linting commands configured.

## Architecture

### Data Flow

```
Import PLY → PlyParser → GaussianModel (torch::Tensors) → SplatRenderer (Vulkan offscreen FBO) → Viewport3D (ImGui::Image)
Import Video → FrameExtractor (FFmpeg) → ColmapRunner (SfM) → SplatTrainer (OpenSplat) → GaussianModel → ...
```

### Threading Model

- **Main thread**: All GLFW events, ImGui rendering, and Vulkan commands. Never call Vulkan from background threads.
- **Pipeline thread** (`std::jthread`): Runs async work (PLY parsing, video pipeline). Writes to `AppState` via mutex-guarded `shared_ptr` and atomics. Main thread detects changes each frame and does GPU uploads.
- `AsyncJob` provides atomic progress/cancel/done flags + mutex-guarded status text for main thread polling.

### Rendering Pipeline

The 3D viewport is an ImGui window backed by a Vulkan offscreen image (R8G8B8A8_UNORM color + D32_SFLOAT depth). Each frame:
1. GPU compute: depth calculation + bitonic sort (two compute shader passes) produce a sorted index buffer
2. Graphics pass: vertex shader reads splat data via index indirection from SSBO, emits instanced quads (6 verts/splat)
3. Fragment shader outputs premultiplied alpha with Gaussian falloff
4. `Viewport3D` displays the offscreen image via `ImGui::Image(descriptorSet)`

Splat data (14 floats/splat) is uploaded to a device-local SSBO once on load. Sorting runs entirely on GPU every frame.

### Key Subsystems

| Directory | Role |
|-----------|------|
| `src/app/` | `Application` (main loop), `AppState` (shared state with `gaussianModel` shared_ptr + mutex) |
| `src/render/` | `VulkanContext` (instance/device/swapchain/VMA), `SplatRenderer` (offscreen FBO + compute sort), `VulkanPipeline`, `GpuBuffer` (RAII), `OrbitCamera`/`FlyCamera` |
| `src/model/` | `GaussianModel` — torch::Tensor fields: positions[N,3], scales[N,3], rotations[N,4], opacities[N,1], sh_coeffs[N,K,3] |
| `src/io/` | `PlyParser` (binary LE + ASCII), `SplatIO` (thin wrapper) |
| `src/ui/` | `MainWindow` (GLFW+ImGui init), `Viewport3D` (renders + camera input), `MenuOverlay` (hover menu), `ProgressOverlay`, `ControlsOverlay`, `FpsOverlay` |
| `src/pipeline/` | `FrameExtractor`, `ColmapRunner`, `SplatTrainer`, `VideoImporter` (Phase 5 — in progress) |
| `src/util/` | `AsyncJob` (header-only) |
| `shaders/` | GLSL 450, compiled to SPIR-V at build time via glslangValidator |

### Camera System

Two cameras (`OrbitCamera` + `FlyCamera`), both quaternion-based, both produce a `CameraUBO` (view + proj + camPos + viewport). `AppState::cameraMode` selects which is active. Switching Orbit→Fly copies position/orientation so the view doesn't snap.

## Critical Gotchas

- **Torch + C++23**: `CMAKE_CUDA_STANDARD 17` must be set before `find_package(Torch)` — NVCC doesn't support C++23.
- **CMAKE_PREFIX_PATH**: Must be `$HOME/local/libtorch` at configure time or Torch won't be found.
- **New .cpp files**: Must re-run `cmake -S . -B build` before building (GLOB_RECURSE doesn't auto-detect new files).
- **Shader compilation**: Uses `glslangValidator -V -S <stage>` (not glslc). Files are named `*.vert.glsl`, `*.frag.glsl`, `*.comp.glsl`.
- **Shader path resolution**: Resolved relative to the executable (`/proc/self/exe`), not CWD.
- **VMA_IMPLEMENTATION**: Defined in exactly one TU (`src/render/VmaImpl.cpp`).
- **AppState.hpp**: Forward-declares `GaussianModel` to keep torch headers out of the transitive include chain. Do not include `GaussianModel.hpp` from `AppState.hpp`.
- **SplatRenderer descriptor pool**: Creates its own `VkDescriptorPool` (UNIFORM_BUFFER + STORAGE_BUFFER); does not share `VulkanContext`'s pool (COMBINED_IMAGE_SAMPLER only).
- **ImGui docking branch (post Sept 2025)**: `RenderPass` and `MSAASamples` are on `initInfo.PipelineInfoMain`, not top-level.
- **PlyParser**: Only entry point is `loadAsync(path, AsyncJob&)` — no synchronous load. Synchronous callers must create a throwaway `AsyncJob`.
- **FRAMES_IN_FLIGHT = 2** (defined in `VulkanContext.hpp`).

## Code Style

`.clang-format` is configured: LLVM base, 100-column limit, 2-space indent, attached braces, left-aligned pointers. Project headers first, then system headers.
