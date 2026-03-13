# world_imagine

A Gaussian Splatting viewer built with Vulkan, GLFW, ImGui, and LibTorch.

## Dependencies

### System packages (Ubuntu/Debian)

```bash
sudo apt install \
    libvulkan-dev vulkan-validationlayers \
    libglfw3-dev \
    glslang-tools \
    cmake ninja-build build-essential
```

### CUDA + LibTorch

LibTorch 2.6.0 with CUDA support is required. Download and extract to `~/local/libtorch`:

```bash
# Example — adjust URL for your CUDA version
wget https://download.pytorch.org/libtorch/cu126/libtorch-cxx11-abi-shared-with-deps-2.6.0%2Bcu126.zip
unzip libtorch-*.zip -d ~/local/
```

CUDA toolkit must also be installed (tested with CUDA 12.6, sm_86).

## Build

```bash
cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_PREFIX_PATH=$HOME/local/libtorch

cmake --build build -j$(nproc)
```

SPIR-V shaders are compiled automatically into `build/shaders/` during the build.

> **Note:** If you add new `.cpp` files, re-run the `cmake -S . -B build` configure step before building so CMake picks them up via `GLOB_RECURSE`.

## Run

```bash
LD_LIBRARY_PATH=$HOME/local/libtorch/lib:/usr/local/cuda/lib64 ./build/world_imagine
```

## Usage

- **Import Splats** — File menu → Import Splats → enter path to a `.ply` file (3DGS format)
- **Export Splats** — File menu → Export Splats → enter output path (available after a model is loaded)
- **Orbit** — left-drag in the viewport
- **Pan** — right-drag in the viewport
- **Zoom** — scroll wheel

## Project Structure

```
src/
  app/        Application loop, AppState
  io/         PLY parser, SplatIO
  model/      GaussianModel (torch::Tensor storage)
  render/     Vulkan context, pipeline, GPU buffers, camera, splat renderer
  ui/         ImGui windows: main window, menu overlay, viewport
shaders/      GLSL source (compiled to SPIR-V at build time)
test_data/    Sample .ply file for testing
```
