#include "SplatIO.hpp"

#include "PlyParser.hpp"
#include "util/AsyncJob.hpp"

std::shared_ptr<GaussianModel> SplatIO::importPLY(const std::filesystem::path& path,
                                                   AsyncJob&                    job) {
    PlyParser parser;
    return parser.loadAsync(path, job);
}

void SplatIO::exportPLY(const GaussianModel& model, const std::filesystem::path& path) {
    PlyParser parser;
    parser.save(model, path);
}
