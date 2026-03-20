#pragma once
#include <filesystem>
#include <functional>
#include <memory>

class GaussianModel;
class AsyncJob;

// Called by PlyParser::loadAsync every kBatchSize rows with the growing partial
// model and the number of splats committed so far.  The same shared_ptr is
// reused across all calls; tensors are replaced in-place under model->mutex.
using SplatBatchCallback = std::function<void(std::shared_ptr<GaussianModel>, size_t count)>;

// Reads and writes 3D Gaussian Splatting PLY files.
// Supports binary_little_endian and ASCII formats.
// Property names follow the standard 3DGS convention:
//   x,y,z, nx,ny,nz, f_dc_0..2, f_rest_*, opacity, scale_0..2, rot_0..3
class PlyParser {
public:
    // Load with progress reporting and cancellation support.
    // Calls job.setProgress(0..1) roughly every 5000 rows.
    // If onBatch is provided, it is called every 25000 rows (and at the end)
    // with a partial model so the caller can display splats as they arrive.
    // Returns nullptr (after calling job.markDone) if cancelled.
    std::shared_ptr<GaussianModel> loadAsync(const std::filesystem::path& path, AsyncJob& job,
                                             SplatBatchCallback onBatch = nullptr);

    void save(const GaussianModel& model, const std::filesystem::path& path);
};
