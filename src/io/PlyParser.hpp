#pragma once
#include <filesystem>
#include <memory>

class GaussianModel;

// Reads and writes 3D Gaussian Splatting PLY files.
// Supports binary_little_endian and ASCII formats.
// Property names follow the standard 3DGS convention:
//   x,y,z, nx,ny,nz, f_dc_0..2, f_rest_*, opacity, scale_0..2, rot_0..3
class PlyParser {
  public:
    std::shared_ptr<GaussianModel> load(const std::filesystem::path& path);
    void save(const GaussianModel& model, const std::filesystem::path& path);
};
