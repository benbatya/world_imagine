#include "SplatIO.hpp"

#include "PlyParser.hpp"

std::shared_ptr<GaussianModel> SplatIO::importPLY(const std::filesystem::path& path) {
    PlyParser parser;
    return parser.load(path);
}

void SplatIO::exportPLY(const GaussianModel& model, const std::filesystem::path& path) {
    PlyParser parser;
    parser.save(model, path);
}
