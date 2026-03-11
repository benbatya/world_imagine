#include "GaussianModel.hpp"
#include <cmath>
#include <stdexcept>

size_t GaussianModel::numSplats() const {
    return positions.defined() ? static_cast<size_t>(positions.size(0)) : 0;
}

int GaussianModel::shBases() const {
    return sh_coeffs.defined() ? static_cast<int>(sh_coeffs.size(1)) : 0;
}

int GaussianModel::shDegree() const {
    int k = shBases();
    if (k >= 16) return 3;
    if (k >= 9)  return 2;
    if (k >= 4)  return 1;
    return 0;
}

std::vector<float> GaussianModel::toVertexBuffer() const {
    const size_t N = numSplats();
    if (N == 0) return {};

    // 14 floats per splat: pos(3) + scale(3) + rot(4) + opacity(1) + dc_sh(3)
    std::vector<float> buf(N * 14);

    auto pos = positions.contiguous().cpu();
    auto sc  = scales.contiguous().cpu();
    auto rot = rotations.contiguous().cpu();
    auto op  = opacities.contiguous().cpu();

    const float* pPos = pos.data_ptr<float>();
    const float* pSc  = sc.data_ptr<float>();
    const float* pRot = rot.data_ptr<float>();
    const float* pOp  = op.data_ptr<float>();

    // DC SH (first basis, all 3 channels)
    const float* pDC = nullptr;
    torch::Tensor dc;
    if (sh_coeffs.defined() && sh_coeffs.size(1) > 0) {
        dc  = sh_coeffs.select(1, 0).contiguous().cpu(); // [N, 3]
        pDC = dc.data_ptr<float>();
    }

    for (size_t i = 0; i < N; ++i) {
        float* out = buf.data() + i * 14;
        out[0]  = pPos[i * 3 + 0];
        out[1]  = pPos[i * 3 + 1];
        out[2]  = pPos[i * 3 + 2];
        out[3]  = pSc[i * 3 + 0];
        out[4]  = pSc[i * 3 + 1];
        out[5]  = pSc[i * 3 + 2];
        out[6]  = pRot[i * 4 + 0];
        out[7]  = pRot[i * 4 + 1];
        out[8]  = pRot[i * 4 + 2];
        out[9]  = pRot[i * 4 + 3];
        out[10] = pOp[i];
        out[11] = pDC ? pDC[i * 3 + 0] : 0.f;
        out[12] = pDC ? pDC[i * 3 + 1] : 0.f;
        out[13] = pDC ? pDC[i * 3 + 2] : 0.f;
    }

    return buf;
}
