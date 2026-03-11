#include "PlyParser.hpp"

#include "model/GaussianModel.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

enum class PlyFormat { BinaryLE, BinaryBE, Ascii };

struct PropDef {
    std::string name;
    std::string type; // "float", "double", "int", "uint", "uchar", etc.
    int byteSize{4};
    int byteOffset{0}; // byte offset within one element (computed after parsing)
};

static int plyTypeSize(const std::string& t) {
    if (t == "float" || t == "float32" || t == "int" || t == "uint" || t == "int32" ||
        t == "uint32")
        return 4;
    if (t == "double" || t == "float64" || t == "int64" || t == "uint64")
        return 8;
    if (t == "short" || t == "ushort" || t == "int16" || t == "uint16")
        return 2;
    if (t == "char" || t == "uchar" || t == "int8" || t == "uint8")
        return 1;
    return 4;
}

// Read a float from raw bytes, handling type conversion
static float readFloat(const char* data, const std::string& type) {
    if (type == "float" || type == "float32") {
        float v;
        std::memcpy(&v, data, 4);
        return v;
    }
    if (type == "double" || type == "float64") {
        double v;
        std::memcpy(&v, data, 8);
        return static_cast<float>(v);
    }
    if (type == "int" || type == "int32") {
        int32_t v;
        std::memcpy(&v, data, 4);
        return static_cast<float>(v);
    }
    if (type == "uint" || type == "uint32") {
        uint32_t v;
        std::memcpy(&v, data, 4);
        return static_cast<float>(v);
    }
    if (type == "short" || type == "int16") {
        int16_t v;
        std::memcpy(&v, data, 2);
        return static_cast<float>(v);
    }
    if (type == "ushort" || type == "uint16") {
        uint16_t v;
        std::memcpy(&v, data, 2);
        return static_cast<float>(v);
    }
    if (type == "uchar" || type == "uint8") {
        uint8_t v;
        std::memcpy(&v, data, 1);
        return static_cast<float>(v);
    }
    if (type == "char" || type == "int8") {
        int8_t v;
        std::memcpy(&v, data, 1);
        return static_cast<float>(v);
    }
    float v;
    std::memcpy(&v, data, 4);
    return v;
}

// ---------------------------------------------------------------------------
// Header parser
// ---------------------------------------------------------------------------

struct PlyHeader {
    PlyFormat format{PlyFormat::BinaryLE};
    int numVertices{0};
    std::vector<PropDef> props;
    int stride{0}; // total bytes per element

    // Lookup: property name → index in props[]
    std::unordered_map<std::string, int> propIndex;

    int indexOf(const std::string& name) const {
        auto it = propIndex.find(name);
        return it != propIndex.end() ? it->second : -1;
    }

    float getFloat(const char* elemData, const std::string& name) const {
        int idx = indexOf(name);
        if (idx < 0)
            return 0.f;
        const PropDef& p = props[idx];
        return readFloat(elemData + p.byteOffset, p.type);
    }
};

static PlyHeader parseHeader(std::ifstream& f, std::streampos& dataStart) {
    PlyHeader hdr;
    std::string line;

    // First line must be "ply"
    std::getline(f, line);
    if (line != "ply" && line != "ply\r")
        throw std::runtime_error("Not a PLY file");

    bool inVertex = false;
    while (std::getline(f, line)) {
        // Strip trailing \r
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        if (line == "end_header")
            break;

        std::istringstream ss(line);
        std::string tok;
        ss >> tok;

        if (tok == "format") {
            std::string fmt;
            ss >> fmt;
            if (fmt == "ascii")
                hdr.format = PlyFormat::Ascii;
            else if (fmt == "binary_little_endian")
                hdr.format = PlyFormat::BinaryLE;
            else if (fmt == "binary_big_endian")
                hdr.format = PlyFormat::BinaryBE;
        } else if (tok == "element") {
            std::string elem;
            ss >> elem;
            inVertex = (elem == "vertex");
            if (inVertex)
                ss >> hdr.numVertices;
        } else if (tok == "property" && inVertex) {
            std::string typeStr, nameStr;
            ss >> typeStr;
            if (typeStr == "list") {
                // Skip list properties (not used in 3DGS)
                continue;
            }
            ss >> nameStr;
            PropDef pd;
            pd.name     = nameStr;
            pd.type     = typeStr;
            pd.byteSize = plyTypeSize(typeStr);
            hdr.props.push_back(pd);
        }
    }

    // Compute byte offsets
    int offset = 0;
    for (int i = 0; i < (int)hdr.props.size(); ++i) {
        hdr.props[i].byteOffset = offset;
        offset += hdr.props[i].byteSize;
        hdr.propIndex[hdr.props[i].name] = i;
    }
    hdr.stride = offset;

    dataStart = f.tellg();
    return hdr;
}

// ---------------------------------------------------------------------------
// Load
// ---------------------------------------------------------------------------

std::shared_ptr<GaussianModel> PlyParser::load(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        throw std::runtime_error("Cannot open: " + path.string());

    std::streampos dataStart;
    PlyHeader hdr = parseHeader(f, dataStart);

    if (hdr.numVertices <= 0)
        throw std::runtime_error("PLY has no vertices");

    // Figure out SH layout
    // f_dc_0/1/2 are DC (k=0); f_rest_* are higher degrees (k=1..)
    // f_rest are stored as: first all R coefficients, then G, then B
    int numRest = 0;
    for (auto& p : hdr.props) {
        if (p.name.rfind("f_rest_", 0) == 0)
            ++numRest;
    }
    // numRest = 3*(K-1) where K = num_sh_bases
    int numHigherBases = (numRest > 0 && numRest % 3 == 0) ? numRest / 3 : 0;
    int numBases       = 1 + numHigherBases;

    const int N = hdr.numVertices;

    // Raw storage (we'll fill, then copy to tensors)
    std::vector<float> positions(N * 3);
    std::vector<float> scales(N * 3);
    std::vector<float> rotations(N * 4);
    std::vector<float> opacities(N);
    std::vector<float> sh(N * numBases * 3, 0.f);

    auto fillRow = [&](const char* elem, int i) {
        positions[i * 3 + 0] = hdr.getFloat(elem, "x");
        positions[i * 3 + 1] = hdr.getFloat(elem, "y");
        positions[i * 3 + 2] = hdr.getFloat(elem, "z");

        scales[i * 3 + 0] = hdr.getFloat(elem, "scale_0");
        scales[i * 3 + 1] = hdr.getFloat(elem, "scale_1");
        scales[i * 3 + 2] = hdr.getFloat(elem, "scale_2");

        rotations[i * 4 + 0] = hdr.getFloat(elem, "rot_0");
        rotations[i * 4 + 1] = hdr.getFloat(elem, "rot_1");
        rotations[i * 4 + 2] = hdr.getFloat(elem, "rot_2");
        rotations[i * 4 + 3] = hdr.getFloat(elem, "rot_3");

        opacities[i] = hdr.getFloat(elem, "opacity");

        // SH: sh[i * numBases*3 + k*3 + c]
        // DC
        sh[i * numBases * 3 + 0 * 3 + 0] = hdr.getFloat(elem, "f_dc_0");
        sh[i * numBases * 3 + 0 * 3 + 1] = hdr.getFloat(elem, "f_dc_1");
        sh[i * numBases * 3 + 0 * 3 + 2] = hdr.getFloat(elem, "f_dc_2");

        // Higher SH: f_rest_{j} where j in [0, numHigherBases) → R channel, basis k=j+1
        //            f_rest_{numHigherBases+j}                  → G channel
        //            f_rest_{2*numHigherBases+j}                → B channel
        for (int j = 0; j < numHigherBases; ++j) {
            int k                            = j + 1;
            sh[i * numBases * 3 + k * 3 + 0] = hdr.getFloat(elem, "f_rest_" + std::to_string(j));
            sh[i * numBases * 3 + k * 3 + 1] =
                hdr.getFloat(elem, "f_rest_" + std::to_string(numHigherBases + j));
            sh[i * numBases * 3 + k * 3 + 2] =
                hdr.getFloat(elem, "f_rest_" + std::to_string(2 * numHigherBases + j));
        }
    };

    if (hdr.format == PlyFormat::Ascii) {
        std::string line;
        for (int i = 0; i < N; ++i) {
            if (!std::getline(f, line))
                throw std::runtime_error("PLY: unexpected end of ASCII data");
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            // Build a fake binary element from parsed tokens
            std::istringstream ss(line);
            std::vector<float> vals(hdr.props.size());
            for (int j = 0; j < (int)hdr.props.size(); ++j) {
                if (!(ss >> vals[j]))
                    throw std::runtime_error("PLY: malformed ASCII row at vertex " +
                                             std::to_string(i));
            }

            // Re-use getFloat via a temporary binary buffer
            std::vector<char> elemBuf(hdr.stride);
            for (int j = 0; j < (int)hdr.props.size(); ++j) {
                std::memcpy(elemBuf.data() + hdr.props[j].byteOffset, &vals[j], 4);
                // Patch type to float so readFloat works correctly
            }
            // Temporarily treat all as float for ascii path
            auto savedTypes = hdr.props;
            for (auto& p : hdr.props)
                p.type = "float";
            fillRow(elemBuf.data(), i);
            hdr.props = savedTypes;
        }
    } else {
        // Binary (big-endian swapping not implemented — 3DGS files are always LE)
        if (hdr.format == PlyFormat::BinaryBE)
            throw std::runtime_error("PLY: big-endian format not supported");

        std::vector<char> elemBuf(hdr.stride);
        for (int i = 0; i < N; ++i) {
            f.read(elemBuf.data(), hdr.stride);
            if (!f)
                throw std::runtime_error("PLY: unexpected end of binary data at vertex " +
                                         std::to_string(i));
            fillRow(elemBuf.data(), i);
        }
    }

    auto model = std::make_shared<GaussianModel>();
    auto opts  = torch::TensorOptions().dtype(torch::kFloat32);

    model->positions = torch::from_blob(positions.data(), {N, 3}, opts).clone();
    model->scales    = torch::from_blob(scales.data(), {N, 3}, opts).clone();
    model->rotations = torch::from_blob(rotations.data(), {N, 4}, opts).clone();
    model->opacities = torch::from_blob(opacities.data(), {N, 1}, opts).clone();
    model->sh_coeffs = torch::from_blob(sh.data(), {N, numBases, 3}, opts).clone();

    return model;
}

// ---------------------------------------------------------------------------
// Save (always binary_little_endian)
// ---------------------------------------------------------------------------

void PlyParser::save(const GaussianModel& model, const std::filesystem::path& path) {
    const int N = static_cast<int>(model.numSplats());
    if (N == 0)
        throw std::runtime_error("GaussianModel is empty — nothing to save");

    std::ofstream f(path, std::ios::binary);
    if (!f)
        throw std::runtime_error("Cannot write: " + path.string());

    const int K     = model.shBases();
    const int nRest = 3 * (K - 1); // f_rest count

    // --- Header ---
    f << "ply\n";
    f << "format binary_little_endian 1.0\n";
    f << "element vertex " << N << "\n";
    f << "property float x\n";
    f << "property float y\n";
    f << "property float z\n";
    f << "property float nx\n";
    f << "property float ny\n";
    f << "property float nz\n";
    f << "property float f_dc_0\n";
    f << "property float f_dc_1\n";
    f << "property float f_dc_2\n";
    for (int j = 0; j < nRest; ++j)
        f << "property float f_rest_" << j << "\n";
    f << "property float opacity\n";
    f << "property float scale_0\n";
    f << "property float scale_1\n";
    f << "property float scale_2\n";
    f << "property float rot_0\n";
    f << "property float rot_1\n";
    f << "property float rot_2\n";
    f << "property float rot_3\n";
    f << "end_header\n";

    // --- Data ---
    auto pos = model.positions.contiguous().cpu();
    auto sc  = model.scales.contiguous().cpu();
    auto rot = model.rotations.contiguous().cpu();
    auto op  = model.opacities.contiguous().cpu();
    auto shc = model.sh_coeffs.contiguous().cpu(); // [N, K, 3]

    const float* pPos = pos.data_ptr<float>();
    const float* pSc  = sc.data_ptr<float>();
    const float* pRot = rot.data_ptr<float>();
    const float* pOp  = op.data_ptr<float>();
    const float* pSH  = shc.data_ptr<float>();

    const float zero         = 0.f;
    const int numHigherBases = K - 1;

    for (int i = 0; i < N; ++i) {
        // x y z
        f.write(reinterpret_cast<const char*>(pPos + i * 3), 3 * sizeof(float));
        // nx ny nz (always 0)
        f.write(reinterpret_cast<const char*>(&zero), sizeof(float));
        f.write(reinterpret_cast<const char*>(&zero), sizeof(float));
        f.write(reinterpret_cast<const char*>(&zero), sizeof(float));
        // f_dc_0/1/2  — sh_coeffs[i, 0, 0/1/2]
        f.write(reinterpret_cast<const char*>(pSH + i * K * 3 + 0 * 3 + 0), sizeof(float));
        f.write(reinterpret_cast<const char*>(pSH + i * K * 3 + 0 * 3 + 1), sizeof(float));
        f.write(reinterpret_cast<const char*>(pSH + i * K * 3 + 0 * 3 + 2), sizeof(float));
        // f_rest: first R(1..K-1), then G(1..K-1), then B(1..K-1)
        for (int j = 0; j < numHigherBases; ++j)
            f.write(reinterpret_cast<const char*>(pSH + i * K * 3 + (j + 1) * 3 + 0),
                    sizeof(float));
        for (int j = 0; j < numHigherBases; ++j)
            f.write(reinterpret_cast<const char*>(pSH + i * K * 3 + (j + 1) * 3 + 1),
                    sizeof(float));
        for (int j = 0; j < numHigherBases; ++j)
            f.write(reinterpret_cast<const char*>(pSH + i * K * 3 + (j + 1) * 3 + 2),
                    sizeof(float));
        // opacity
        f.write(reinterpret_cast<const char*>(pOp + i), sizeof(float));
        // scale_0/1/2
        f.write(reinterpret_cast<const char*>(pSc + i * 3), 3 * sizeof(float));
        // rot_0/1/2/3
        f.write(reinterpret_cast<const char*>(pRot + i * 4), 4 * sizeof(float));
    }

    if (!f)
        throw std::runtime_error("PLY write failed: " + path.string());
}
