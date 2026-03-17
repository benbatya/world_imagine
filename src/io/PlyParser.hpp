#pragma once
#include <filesystem>
#include <memory>

class GaussianModel;
class AsyncJob;

// Reads and writes 3D Gaussian Splatting PLY files.
// Supports binary_little_endian and ASCII formats.
// Property names follow the standard 3DGS convention:
//   x,y,z, nx,ny,nz, f_dc_0..2, f_rest_*, opacity, scale_0..2, rot_0..3
class PlyParser {
public:
    // Load with progress reporting and cancellation support.
    // Calls job.setProgress(0..1) roughly every 5000 rows.
    // Returns nullptr (after calling job.markDone) if cancelled.
    std::shared_ptr<GaussianModel> loadAsync(const std::filesystem::path& path, AsyncJob& job);

    void save(const GaussianModel& model, const std::filesystem::path& path);
};
