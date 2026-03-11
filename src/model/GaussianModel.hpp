#pragma once
#include <cstddef>
#include <mutex>
#include <torch/torch.h>
#include <vector>

// Stores all per-splat data for a 3D Gaussian Splatting scene.
// All tensors are CPU float32 (uploaded to GPU only by SplatRenderer in Phase 3).
// Guard tensor writes with `mutex` when modifying from a background thread.
class GaussianModel {
public:
  torch::Tensor positions; // [N, 3]    float32 — XYZ centers
  torch::Tensor scales;    // [N, 3]    float32 — log-scale per axis
  torch::Tensor rotations; // [N, 4]    float32 — quaternion (w, x, y, z)
  torch::Tensor opacities; // [N, 1]    float32 — pre-sigmoid opacity
  torch::Tensor sh_coeffs; // [N, K, 3] float32 — K SH bases, 3 channels (RGB)

  mutable std::mutex mutex;

  // Number of splats (0 if not yet loaded).
  size_t numSplats() const;

  // Number of SH basis functions inferred from sh_coeffs (0 if undefined).
  int shBases() const;

  // Inferred SH degree: 0→1 base, 1→4, 2→9, 3→16.
  int shDegree() const;

  // Flatten all fields to a compact CPU float buffer for Vulkan staging.
  // Layout per splat: [x,y,z, s0,s1,s2, r0,r1,r2,r3, opacity, dc_r,dc_g,dc_b]
  // (14 floats; higher SH degrees are omitted — renderer fills in Phase 3.)
  std::vector<float> toVertexBuffer() const;
};
