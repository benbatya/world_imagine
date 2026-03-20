# World Imagine — Implementation Plan

Point cloud processing and visualization application built in modern C++23.

---

## Project Directory Structure

```
world_imagine/
├── CMakeLists.txt
├── cmake/
│   ├── FindFFmpeg.cmake
│   ├── FindCOLMAP.cmake
│   └── CompilerFlags.cmake
├── third_party/
│   ├── imgui/                    # vendored ImGui + GLFW/Vulkan backends
│   └── imfiledialog/             # ImGuiFileDialog (header + cpp)
├── src/
│   ├── main.cpp
│   ├── app/
│   │   ├── Application.hpp/.cpp  # main loop, owns AppState
│   │   └── AppState.hpp          # shared state (shared_ptrs + atomics)
│   ├── ui/
│   │   ├── MainWindow.hpp/.cpp   # GLFW + ImGui context owner
│   │   ├── MenuOverlay.hpp/.cpp  # hover-triggered top-left menu
│   │   ├── Viewport3D.hpp/.cpp   # Vulkan offscreen render pass panel
│   │   └── ProgressModal.hpp/.cpp
│   ├── pipeline/
│   │   ├── VideoImporter.hpp/.cpp  # async orchestrator
│   │   ├── FrameExtractor.hpp/.cpp # FFmpeg frame extraction
│   │   ├── ColmapRunner.hpp/.cpp   # COLMAP SfM wrapper
│   │   └── SplatTrainer.hpp/.cpp   # OpenSplat training wrapper
│   ├── io/
│   │   ├── SplatIO.hpp/.cpp
│   │   ├── PlyParser.hpp/.cpp
│   │   └── SpzParser.hpp/.cpp
│   ├── render/
│   │   ├── VulkanContext.hpp/.cpp  # instance, device, queues, swapchain
│   │   ├── SplatRenderer.hpp/.cpp  # render pass, pipeline, descriptors
│   │   ├── OrbitCamera.hpp/.cpp    # orbit camera + CameraUBO definition
│   │   ├── FlyCamera.hpp/.cpp      # first-person fly camera (quaternion-based)
│   │   ├── VulkanPipeline.hpp/.cpp # SPIR-V shader loading + pipeline creation
│   │   └── GpuBuffer.hpp/.cpp      # VkBuffer / VmaAllocation RAII wrappers
│   ├── model/
│   │   ├── GaussianModel.hpp/.cpp  # torch::Tensor storage
│   │   └── PointCloud.hpp
│   └── util/
│       ├── AsyncJob.hpp            # atomic progress + cancel flag
│       ├── ThreadPool.hpp/.cpp
│       └── Logger.hpp/.cpp
└── shaders/
    ├── splat.vert.glsl / splat.frag.glsl   # compiled to SPIR-V at build time
    └── point.vert.glsl / point.frag.glsl
```

---

## Architecture Overview

### Central Data Flow

```
Import Video → FrameExtractor (FFmpeg) → ColmapRunner (SfM) → SplatTrainer (OpenSplat)
                                                                        ↓
                                                               GaussianModel (torch::Tensors)
                                                                        ↓
                                                              SplatRenderer (Vulkan render pass)
                                                                        ↓
                                                              Viewport3D (ImGui::Image)
```

### Core Classes

| Class | Responsibility |
|---|---|
| `AppState` | Shared state: `shared_ptr<GaussianModel>`, `shared_ptr<AsyncJob>`, status, `cameraMode` |
| `GaussianModel` | `torch::Tensor` fields: positions[N,3], scales[N,3], rotations[N,4], opacities[N,1], sh_coeffs[N,K,3] |
| `MenuOverlay` | Monitors `ImGui::GetMousePos()`, shows menu when in top-left 150×150px zone |
| `Viewport3D` | Manages Vulkan offscreen image, exposes it via `ImGui::Image()`, owns OrbitCamera + FlyCamera |
| `OrbitCamera` | Orbit-around-target camera; quaternion orientation to avoid gimbal lock |
| `FlyCamera` | First-person fly camera; quaternion orientation to support roll |
| `AsyncJob` | `std::atomic<float> progress`, `std::atomic<bool> cancelRequested`, `std::atomic<bool> done` |
| `VideoImporter` | Launches `std::jthread` running the 3-stage pipeline, returns `AsyncJob` |

---

## Threading Model

```
Main Thread (UI)            Pipeline Thread (std::jthread)
─────────────────           ──────────────────────────────
GLFW event poll             FrameExtractor  (FFmpeg, disk I/O)
ImGui rendering                    ↓
SplatRenderer::render()     ColmapRunner    (CPU-intensive SfM)
Reads AppState                     ↓
Checks AsyncJob::isDone()   SplatTrainer    (OpenSplat training)
  → if done: uploadSplats()         ↓
                            Writes AppState (under mutex)
                            Sets job.m_done = true
```

**Key rule**: Vulkan calls only on the main thread. Pipeline thread writes to `AppState::gaussianModel` (mutex-guarded `shared_ptr`), main thread picks it up the next frame and submits a `vkCmdCopyBuffer` to upload new splat data.

---

## CMakeLists.txt Structure

```cmake
cmake_minimum_required(VERSION 3.25)
project(world_imagine VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(cmake/CompilerFlags.cmake)

# 1. Vulkan (system SDK — requires VulkanSDK or vulkan-dev package)
find_package(Vulkan REQUIRED)

# 2. GLFW3 (built without OpenGL: GLFW_CLIENT_API = GLFW_NO_API)
find_package(glfw3 3.3 REQUIRED)

# 3. Vulkan Memory Allocator — header-only, vendored or via vcpkg
#    No find_package needed; just add include path
add_library(vma INTERFACE)
target_include_directories(vma INTERFACE third_party/vma/include)

# 4. libTorch — set CMAKE_PREFIX_PATH or Torch_DIR to libtorch install
find_package(Torch REQUIRED)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${TORCH_CXX_FLAGS}")  # CRITICAL: ABI flags

# 5. FFmpeg via custom cmake/FindFFmpeg.cmake
find_package(FFmpeg REQUIRED COMPONENTS avcodec avformat avutil swscale)

# 6. COLMAP (build with -DGUI_ENABLED=OFF, then cmake --install)
find_package(COLMAP REQUIRED)

# 7. OpenSplat (same libtorch version required)
find_package(OpenSplat REQUIRED)

# 8. ImGui — vendored source with Vulkan backend
add_library(imgui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_glfw.cpp
    third_party/imgui/backends/imgui_impl_vulkan.cpp
    third_party/imfiledialog/ImGuiFileDialog.cpp
)
target_include_directories(imgui PUBLIC
    third_party/imgui
    third_party/imgui/backends
    third_party/imfiledialog
)
target_link_libraries(imgui PRIVATE glfw Vulkan::Vulkan)

# 9. SPIR-V shader compilation (requires glslc from VulkanSDK)
find_program(GLSLC glslc REQUIRED HINTS "$ENV{VULKAN_SDK}/bin")
file(GLOB GLSL_SOURCES shaders/*.glsl)
foreach(glsl ${GLSL_SOURCES})
    get_filename_component(stem ${glsl} NAME_WLE)  # e.g. splat.vert
    set(spv "${CMAKE_BINARY_DIR}/shaders/${stem}.spv")
    add_custom_command(OUTPUT ${spv}
        COMMAND ${GLSLC} ${glsl} -o ${spv}
        DEPENDS ${glsl}
        COMMENT "Compiling ${stem} to SPIR-V")
    list(APPEND SPV_OUTPUTS ${spv})
endforeach()
add_custom_target(shaders ALL DEPENDS ${SPV_OUTPUTS})

# Main executable
add_executable(world_imagine)
target_sources(world_imagine PRIVATE <all .cpp files in src/>)
add_dependencies(world_imagine shaders)
target_link_libraries(world_imagine PRIVATE
    imgui vma glfw Vulkan::Vulkan
    "${TORCH_LIBRARIES}"
    FFmpeg::avcodec FFmpeg::avformat FFmpeg::avutil FFmpeg::swscale
    COLMAP::colmap
    OpenSplat::opensplat
)
```

---

## Class Designs

### `AppState`

```cpp
struct AppState {
    std::shared_ptr<GaussianModel> gaussianModel;   // null until loaded/trained
    std::shared_ptr<PointCloud>    sparseCloud;      // from COLMAP (optional display)
    std::shared_ptr<AsyncJob>      activeJob;
    bool        showProgressModal{false};
    std::string statusMessage;
};
```

### `GaussianModel`

```cpp
class GaussianModel {
public:
    torch::Tensor positions;   // [N, 3] float32 — XYZ centers
    torch::Tensor scales;      // [N, 3] float32 — log-scale
    torch::Tensor rotations;   // [N, 4] float32 — quaternions
    torch::Tensor opacities;   // [N, 1] float32 — pre-sigmoid opacity
    torch::Tensor sh_coeffs;   // [N, K, 3] float32 — spherical harmonics

    size_t numSplats() const { return positions.size(0); }

    // Flatten to CPU float* for Vulkan staging buffer upload
    std::vector<float> toVertexBuffer() const;
    // Load from flattened buffer (after file import)
    void fromVertexBuffer(const std::vector<float>& buf, size_t N);
};
```

### `AsyncJob`

```cpp
class AsyncJob {
public:
    void  setProgress(float p);
    float progress() const;
    void  setStatusText(std::string s);
    std::string statusText() const;
    void  requestCancel();
    bool  cancelRequested() const;
    bool  isDone() const;
    std::exception_ptr exception() const;

private:
    std::atomic<float>     m_progress{0.f};
    std::atomic<bool>      m_cancelRequested{false};
    std::atomic<bool>      m_done{false};
    mutable std::mutex     m_textMu;
    std::string            m_statusText;
    std::exception_ptr     m_exception;
};
```

### `VideoImporter`

```cpp
class VideoImporter {
public:
    std::shared_ptr<AsyncJob> importVideo(
        std::filesystem::path videoPath,
        std::filesystem::path workDir,
        AppState& state);

private:
    void runPipeline(std::filesystem::path videoPath,
                     std::filesystem::path workDir,
                     AsyncJob& job,
                     AppState& state);
    FrameExtractor m_frameExtractor;
    ColmapRunner   m_colmapRunner;
    SplatTrainer   m_splatTrainer;
};
```

### `SplatRenderer`

```cpp
class SplatRenderer {
public:
    void init(VulkanContext& ctx);
    void uploadSplats(const GaussianModel& model);
    void render(const Camera& cam, int viewportW, int viewportH);
    void resize(int w, int h);
    VkDescriptorSet getOutputDescriptorSet() const;  // ImGui::Image handle

private:
    // Offscreen render targets
    VkImage        m_colorImage;
    VkImageView    m_colorView;
    VmaAllocation  m_colorAlloc;
    VkImage        m_depthImage;
    VkImageView    m_depthView;
    VmaAllocation  m_depthAlloc;
    VkSampler      m_sampler;

    VkRenderPass         m_renderPass;
    VkFramebuffer        m_framebuffer;
    VkDescriptorSet      m_imguiTexDescSet;  // registered with ImGui_ImplVulkan

    // Splat geometry
    VkBuffer       m_vertexBuffer;
    VmaAllocation  m_vertexAlloc;
    VkBuffer       m_stagingBuffer;
    VmaAllocation  m_stagingAlloc;

    VulkanPipeline m_pipeline;
    size_t         m_splatCount{0};
    bool           m_dirty{true};
};
```

### `SplatIO`

```cpp
class SplatIO {
public:
    std::shared_ptr<GaussianModel> importPLY(std::filesystem::path path);
    std::shared_ptr<GaussianModel> importSPZ(std::filesystem::path path);
    void exportPLY(const GaussianModel& model, std::filesystem::path path);
    void exportSPZ(const GaussianModel& model, std::filesystem::path path);

private:
    PlyParser m_plyParser;
    SpzParser m_spzParser;
};
```

PLY property names follow the 3DGS convention:
`x, y, z, nx, ny, nz, f_dc_0, f_dc_1, f_dc_2, f_rest_*`, `opacity`, `scale_0/1/2`, `rot_0/1/2/3`.

---

## Rendering Approach

### Vulkan Offscreen Render Pass

The 3D viewport is an ImGui child window backed by a Vulkan offscreen image:

1. `VulkanContext` creates the Vulkan instance, picks a physical device, creates a logical device with graphics + transfer queues, and initializes VMA (`VmaAllocator`).
2. `SplatRenderer::init()` allocates:
   - A `VK_FORMAT_R8G8B8A8_UNORM` color image (`VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT`)
   - A `VK_FORMAT_D32_SFLOAT` depth image
   - A `VkRenderPass` with one color + one depth attachment
   - A `VkFramebuffer` binding both images
   - A `VkSampler` and a `VkDescriptorSet` registered with `ImGui_ImplVulkan_AddTexture()`
3. Each frame, `SplatRenderer::render()` records commands into a `VkCommandBuffer`: begin render pass, bind pipeline, draw, end render pass. Transitions the color image to `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL` for ImGui sampling.
4. `Viewport3D::draw()` calls `ImGui::Image(m_imguiTexDescSet, size)`.
5. On resize, destroy and recreate the color/depth images and framebuffer at the new size; re-register with ImGui.

### `VulkanContext` Bootstrap

```cpp
class VulkanContext {
public:
    void init(GLFWwindow* window);
    void destroy();

    VkInstance       instance;
    VkPhysicalDevice physicalDevice;
    VkDevice         device;
    VkQueue          graphicsQueue;
    uint32_t         graphicsQueueFamily;
    VkCommandPool    commandPool;
    VkDescriptorPool descriptorPool;   // shared pool for ImGui + renderer
    VmaAllocator     allocator;        // VMA allocator
    VkSurfaceKHR     surface;
    VkSwapchainKHR   swapchain;        // used by imgui_impl_vulkan for presentation
};
```

GLFW provides the surface via `glfwCreateWindowSurface()`. ImGui uses the swapchain for presenting the final composited frame; the splat render pass writes to a separate offscreen image.

### Splat Shaders (SPIR-V)

Written in GLSL 450, compiled to SPIR-V via `glslc` at build time (see CMake step above).

**Strategy**: Instanced quads — 6 vertices per splat, `gl_VertexID / 6` gives the splat index. No geometry shader needed.

Vertex shader (`splat.vert.glsl`) responsibilities:
- Read per-splat data from a storage buffer (SSBO) bound at set 0, binding 0
- Compute 2D projected covariance from 3D scale + rotation via projection Jacobian
- Emit screen-space billboard corner position and UV

Fragment shader (`splat.frag.glsl`):
```glsl
#version 450
layout(location = 0) in  vec2 inUV;
layout(location = 1) in  vec4 inColor;
layout(location = 0) out vec4 outColor;

void main() {
    float alpha = inColor.a * exp(-0.5 * dot(inUV, inUV));
    outColor = vec4(inColor.rgb * alpha, alpha);  // premultiplied alpha
}
```

Pipeline blend state: `srcColorBlendFactor = VK_BLEND_FACTOR_ONE`, `dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA`. Requires back-to-front depth sort.

### Splat Data Upload

Splat geometry is stored in a `VkBuffer` with `VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT` (device-local via VMA `VMA_MEMORY_USAGE_GPU_ONLY`). Upload uses a CPU-visible staging buffer:

```cpp
// On main thread after pipeline completes:
auto flat = model.toVertexBuffer();
// memcpy into staging buffer (VMA_MEMORY_USAGE_CPU_TO_GPU)
// vkCmdCopyBuffer(cmdBuf, stagingBuf, vertexBuf, ...)
// pipeline barrier: transfer → vertex shader read
```

### Depth Sorting

Sort splats by distance to camera before each upload using torch:

```cpp
auto depths = (positions - cam_pos).norm(2, 1);
auto sorted_idx = depths.argsort(0, /*descending=*/true);
auto sorted_positions = positions.index_select(0, sorted_idx);
// repeat for scales, rotations, opacities, sh_coeffs
```

Fast for ≤500k splats. For larger scenes, run sort on a background thread and double-buffer the vertex `VkBuffer` with a fence-guarded swap.

### Camera System

`Viewport3D` owns both cameras. `AppState::cameraMode` (an `atomic<CameraMode>`) selects the active one. The menu's Orbit/Fly radio buttons write this field; `Viewport3D::draw()` reads it each frame.

#### Shared GPU interface

Both cameras produce a `CameraUBO` (defined in `OrbitCamera.hpp`, reused by `FlyCamera`):

```cpp
struct CameraUBO {
    glm::mat4 view;
    glm::mat4 proj;       // Vulkan-convention: Y-flipped, depth in [0, 1]
    glm::vec4 camPos;     // world-space camera position (w unused)
    glm::vec4 viewport;   // xy = width, height in pixels (zw unused)
};
```

`Viewport3D::draw()` calls `activeCamera.makeUBO(aspect, w, h)` and stores the result in `m_currentUBO`. `renderOffscreen()` passes it directly to `SplatRenderer::render()`, which memcpys it into the persistently-mapped UBO buffer each frame. `SplatRenderer::uploadSplats()` takes `glm::vec3 camPos` for depth-sorting — extracted from the active camera's `position()`.

#### OrbitCamera (`src/render/OrbitCamera.hpp/.cpp`)

Orbits around a `target` point at a given `distance`. Orientation is stored as a `glm::quat` to avoid gimbal lock.

```
orientation_ = Ry(azimuth) * Rx(-elevation)   // initialized at azimuth=0, elevation=−0.3 rad
```

| Method | What it does |
|--------|-------------|
| `orbit(dx, dy)` | Pre-multiply azimuth (world Y), post-multiply elevation (local X); 0.005 rad/px |
| `pan(dx, dy)` | Translate `target` along camera-local right/up; scale = 0.001 × distance/px |
| `dolly(delta)` | Multiplicative zoom: `distance *= (1 − delta × 0.001)`; floor at 0.01 |
| `fitToBounds(center, radius)` | Sets `target = center`, `distance = 2.5 × radius`, adjusts `zNear/zFar` |
| `resetOrientation(az, el)` | Rebuilds quaternion from explicit angles (used at init) |

Controls wired in `Viewport3D` (Orbit mode, viewport hovered):

| Input | Action |
|-------|--------|
| RMB drag | Orbit |
| MMB drag | Pan |
| Scroll | Dolly |

#### FlyCamera (`src/render/FlyCamera.hpp/.cpp`)

First-person camera with full 6-DOF including roll. Orientation stored as a `glm::quat`; local axes derived from it each frame:

```cpp
forward() = normalize(orientation_ * vec3(0, 0, -1))
right()   = normalize(orientation_ * vec3(1, 0, 0))
up()      = normalize(orientation_ * vec3(0, 1, 0))
```

View matrix: `mat4_cast(inverse(orientation_)) * translate(-position_)`.

| Method | What it does |
|--------|-------------|
| `look(dx, dy)` | Yaw around **world Y**, pitch around **camera-local right**; 0.003 rad/px |
| `roll(dr)` | Rotate around camera-local forward; 0.02 rad/step |
| `move(fwd, rgt, upv, dt)` | Translate along local forward/right + world Y; scaled by `moveSpeed_ × dt` |
| `pan(dx, dy)` | Translate along local right/up; scaled by `moveSpeed_ × 0.005`/px |
| `dolly(ticks)` | Move along local forward; `ticks × moveSpeed_ × 0.5` units |
| `adjustSpeed(factor)` | Multiply `moveSpeed_` by factor (clamped ≥ 0.01) |
| `setFromOrbit(orbit)` | Copy `position()` and `orientation()` from an `OrbitCamera` |

Controls wired in `Viewport3D` (Fly mode, viewport hovered):

| Input | Action |
|-------|--------|
| RMB drag | Look (yaw + pitch) |
| Arrow keys | Look (200 px/s equivalent) |
| WASD | Move forward/back/strafe |
| MMB drag | Pan (lateral + vertical) |
| Q / E | Move up / down |
| R / F | Roll left / right |
| Scroll | Dolly along forward |
| `=` / `-` | Speed × 1.2 / ÷ 1.2 |

**Mode switching**: when `cameraMode` changes from Orbit → Fly, `Viewport3D` calls `m_flyCamera.setFromOrbit(m_camera)` so the view does not snap. Switching back to Orbit leaves the `OrbitCamera` state untouched.

---

### Incremental Splat Display During Load

PLY files are parsed and displayed incrementally so the user sees splats appear
as they load rather than waiting for the full file.

**How it works:**

`PlyParser::loadAsync` accepts an optional `SplatBatchCallback`:
```cpp
using SplatBatchCallback = std::function<void(std::shared_ptr<GaussianModel>, size_t count)>;
```
Inside the parse loop, every `kBatchSize = 25000` rows (and at the final row),
a `commitBatch(count)` helper clones the first `count` entries from the raw
float vectors into a reused `GaussianModel` under `model->mutex`, then invokes
the callback.  The same `shared_ptr<GaussianModel>` is passed on every call —
tensors are replaced in-place, so the pointer published to `AppState` never
changes during a single load.  When a callback is registered the final return
value is that reused model (no second tensor allocation).

`SplatIO::loadAsync` constructs the callback and wires it to `AppState`:
- **First batch**: acquires `gaussianMutex`, sets `state.gaussianModel = partial`.
  This is the only time the pointer changes.
- **Every batch**: stores `count` into `state.committedSplatCount` (atomic
  release-store) so `Viewport3D` can detect growth without locking.

`AppState` gains one field:
```cpp
std::atomic<size_t> committedSplatCount{0};
```

`Viewport3D::draw` checks both new-model and growth conditions each frame:
```cpp
bool isNewModel = current && current != m_lastModel;
bool hasGrown   = current == m_lastModel && committedCount > m_lastSplatCount;
```
- `isNewModel` → auto-fit camera (quantile bounding box) + upload splats.
- `hasGrown` → upload splats only; camera is **not** re-fit, so the user can
  orbit freely while loading continues.
- Both paths call `m_renderer.uploadSplats()`, which acquires `model->mutex`
  internally before reading the tensors.

**Thread-safety summary:**

| Shared state | Writer | Reader | Guard |
|---|---|---|---|
| `model->positions` etc. | parser thread (`commitBatch`) | main thread (`uploadSplats`) | `model->mutex` |
| `state.gaussianModel` (ptr) | parser thread (first batch only) | main thread | `state.gaussianMutex` |
| `state.committedSplatCount` | parser thread (each batch) | main thread | `std::atomic` |

No Vulkan calls are made from the background thread; all GPU work remains on
the main thread.

---

## Implementation Phases

### Phase 1 — Foundation
1. Set up `CMakeLists.txt` with Vulkan + GLFW + VMA + ImGui and the `glslc` SPIR-V compilation step; build a blank window with ImGui demo
2. Implement `VulkanContext`: instance with validation layers, device selection, graphics queue, VMA allocator, GLFW surface, swapchain
3. Implement `MainWindow`: `glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API)`, `ImGui_ImplGlfw_InitForVulkan()`, `ImGui_ImplVulkan_Init()` with the shared descriptor pool
4. Implement `Application` main loop (acquire swapchain image → ImGui frame → submit → present)
5. Implement `MenuOverlay` with hover detection (log clicks, no file dialog yet)

### Phase 2 — File I/O and Model
5. Implement `GaussianModel` with `torch::Tensor` fields; verify `torch::zeros` links correctly
6. Implement `PlyParser` for standard 3DGS `.ply` format; test with a known splat file
7. Implement `SplatIO::importPLY` and `exportPLY`; add SPZ after PLY works
8. Wire "Import Splats" and "Export Splats" menu items to file dialogs and `SplatIO`

### Phase 3 — Rendering
9. Implement `GpuBuffer` RAII wrappers for `VkBuffer` + `VmaAllocation`; implement staging upload helpers
10. Implement `VulkanPipeline`: load SPIR-V from `.spv` files, create `VkShaderModule`, define vertex input / rasterization / blend state, create `VkPipelineLayout` + `VkPipeline`
11. Write `splat.vert.glsl` and `splat.frag.glsl`; verify they compile with `glslc`
12. Implement `SplatRenderer::init()`: offscreen color + depth images, render pass, framebuffer, ImGui texture descriptor set
13. Implement `SplatRenderer::uploadSplats()` via staging buffer + `vkCmdCopyBuffer`; implement `render()` recording into a command buffer
14. Implement `Viewport3D` using `ImGui::Image(m_imguiTexDescSet, size)`
15. Implement `Camera` orbit controls; test by loading a `.ply` and orbiting

### Phase 4 — Async Infrastructure
16. Implement `AsyncJob` with atomics
17. Implement `ProgressModal` that polls `AsyncJob`
18. Implement pipeline `std::jthread` wiring in `VideoImporter`


### Phase 5 — Video Pipeline
19. Implement `FrameExtractor` using FFmpeg C API; test on `raw_camera/VID_20260309_111720.mp4`
20. Implement `ColmapRunner`; first via subprocess (`fork+exec colmap`) to de-risk, then switch to library API
21. Implement `SplatTrainer` using OpenSplat; test with a small COLMAP output
22. Implement `VideoImporter` to chain all three stages with progress reporting
23. Wire "Import Video" menu item; show `ProgressModal` during import

### Phase 6 — Export and Polish
24. Implement `exportPLY` and `exportSPZ`; wire "Export Splats"
25. Add error modal: show `ImGui::OpenPopup("Error")` when `AsyncJob::exception()` is set
26. Move depth sort to a background thread with double-buffered `VkBuffer` (fence-guarded swap) for large scenes
27. Add cancel support: pipeline checks `AsyncJob::cancelRequested()` between stages

---

## Dependency Integration Notes

### libTorch (PyTorch C++)
- Download pre-built libtorch `.zip` from `download.pytorch.org` (CPU or CUDA). Do NOT build from source.
- Set `CMAKE_PREFIX_PATH=/path/to/libtorch` or `-DTorch_DIR=/path/to/libtorch/share/cmake/Torch`.
- **Critical**: `TORCH_CXX_FLAGS` must be propagated to ALL translation units that include torch headers. Missing this causes ABI mismatch linker errors (`_GLIBCXX_USE_CXX11_ABI`).
- On Linux, link `gomp` before torch to avoid `GOMP` symbol conflicts: `-Wl,--no-as-needed gomp`.
- Guard all tensor writes in `GaussianModel` behind a `std::mutex` — tensor ops are not thread-safe across threads sharing a tensor.

### COLMAP Library API
- Build COLMAP with `-DGUI_ENABLED=OFF` (removes Qt dependency) and `cmake --install`.
- Entry point: `colmap::AutomaticReconstructionController` from `<colmap/controllers/automatic_reconstruction.h>`. Set `image_path` and `workspace_path`, call `Start()`, then `Wait()`.
- Read output via `colmap::ReadBinaryModel()` from `<colmap/base/reconstruction.h>` to get `colmap::Reconstruction` containing `Points3D()`.
- Transitive dependencies: Ceres Solver, Eigen, Boost, SQLite, FLANN. On Ubuntu: `apt install libceres-dev libeigen3-dev libboost-all-dev libsqlite3-dev`.

### OpenSplat
- Clone `https://github.com/pierotofy/OpenSplat`. Use identical libtorch version as the main project.
- Integrate via `add_subdirectory(third_party/opensplat)` or `find_package(OpenSplat)` after install.
- Training entry point: construct `OpenSplat::Dataset` from COLMAP path, construct `OpenSplat::GaussianCloud`, call `cloud.train(numIterations)`.
- Register a per-iteration callback to check `AsyncJob::cancelRequested()` for cooperative cancellation.
- GPU training requires CUDA. Without CUDA, training runs on CPU ATen kernels — functional but significantly slower.

### FFmpeg C API
- Install dev headers: `apt install libavcodec-dev libavformat-dev libswscale-dev libavutil-dev`.
- Video frames often arrive as `AV_PIX_FMT_YUV420P`. Always use `SwsContext` to convert to `AV_PIX_FMT_RGB24` before saving.
- Frame extraction loop: `av_read_frame()` → check stream index → `avcodec_send_packet()` → `avcodec_receive_frame()`. Handle `AVERROR(EAGAIN)` correctly.
- Use RAII wrappers with custom deleters: `std::unique_ptr<AVFrame, decltype(&av_frame_free)>`.
- Extract every Nth frame (e.g., every 5th at 30fps) to produce 100–300 frames for COLMAP — ideal for reconstruction quality vs. runtime.

### Dear ImGui
- Vendor ImGui v1.91.x (docking branch recommended). Pin to a specific commit SHA.
- Backends needed: `imgui_impl_glfw.cpp` + `imgui_impl_vulkan.cpp`.
- GLFW must be initialized with `glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API)` — no OpenGL context.
- `ImGui_ImplVulkan_Init()` requires a pre-created `VkDescriptorPool` with enough sets for ImGui's internal textures plus one per splat viewport.
- Register the offscreen color image as an ImGui texture with `ImGui_ImplVulkan_AddTexture(sampler, imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)` — this returns a `VkDescriptorSet` used directly as `ImTextureID`.
- File dialog: use `ImGuiFileDialog` (single header+cpp) or `nfd` (native file dialog via `NFD_OpenDialogN()`).
- **ImGui is not thread-safe.** All ImGui calls must be on the main thread. Use `AppState` atomics/mutexes for pipeline→UI communication.

### Vulkan / VMA
- Install Vulkan SDK (`apt install vulkan-sdk` or download from lunarg.com). Provides `Vulkan::Vulkan` CMake target and `glslc`.
- Enable validation layers (`VK_LAYER_KHRONOS_validation`) in debug builds via `VkInstanceCreateInfo::ppEnabledLayerNames`. This catches all API misuse.
- Use **Vulkan Memory Allocator (VMA)** for all image and buffer allocations. It is header-only: vendor `VulkanMemoryAllocator/include/vk_mem_alloc.h` and define `VMA_IMPLEMENTATION` in one `.cpp`.
- Image layout transitions must be explicit. Use pipeline barriers to transition the offscreen color image between `VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL` (during render) and `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL` (for ImGui sampling) each frame.
- For the splat SSBO, use `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER` at set 0, binding 0. Update the descriptor set once after each `uploadSplats()` call.
- Double-buffer command buffers (one per swapchain frame-in-flight) to avoid stalling the GPU pipeline.

---

## C++23 Features to Use

| Feature | Usage |
|---|---|
| `std::jthread` | Pipeline thread with stop token for cooperative cancellation |
| `std::expected<T, E>` | Return type from each pipeline stage instead of exceptions |
| `std::format` | Log messages and progress status strings |
| `std::ranges::sort` | Depth-sort splat indices with a projection |
| `std::flat_map` | PLY property name-to-index mapping in `PlyParser` |
| `std::mdspan` | View `GaussianModel` tensor data as 2D span without copying |

---

## Critical Risks

| Risk | Mitigation |
|---|---|
| libTorch ABI mismatch | Propagate `TORCH_CXX_FLAGS` to all targets that include torch headers |
| COLMAP transitive deps | Build headless (`-DGUI_ENABLED=OFF`); install system packages for Ceres/Boost/SQLite |
| OpenSplat + torch version mismatch | Use identical libtorch for both; integrate via `add_subdirectory` |
| Vulkan calls from pipeline thread | Strict rule: only main thread submits Vulkan commands; pipeline writes `AppState`, main thread does staging upload |
| FFmpeg memory leaks | RAII `unique_ptr` wrappers with custom deleters for `AVFrame`/`AVPacket` |
| Long training blocks cancel | Register per-iteration OpenSplat callback to check `AsyncJob::cancelRequested()` |

---

## Key Files (Most Critical First)

1. **`CMakeLists.txt`** — All dependency wiring; libTorch ABI flags and COLMAP transitive deps are the hardest part
2. **`src/model/GaussianModel.hpp`** — Defines the tensor layout; all pipeline stages produce it, all I/O serializes it, renderer consumes it
3. **`shaders/splat.vert.glsl`** — Most technically demanding; must compute 2D projected covariance from 3D Gaussian parameters for correct billboard sizing; compiled to SPIR-V at build time
4. **`src/app/Application.hpp`** — Top-level lifecycle owner; controls main loop and wires all subsystems
5. **`src/pipeline/VideoImporter.hpp`** — Async orchestrator; threading, progress, and cancellation logic lives here
