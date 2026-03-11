#pragma once
#include <filesystem>
#include <memory>

class GaussianModel;

class SplatIO {
public:
  std::shared_ptr<GaussianModel> importPLY(const std::filesystem::path& path);
  void exportPLY(const GaussianModel& model, const std::filesystem::path& path);
};
