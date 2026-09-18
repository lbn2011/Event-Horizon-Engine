#include "ehe/core/noise.h"

#include <cmath>

namespace ehe::core {
namespace {

/// 平滑步 3t²−2t³（与 shader 的 f*f*(3-2f) 同式）
double smooth_step(double t) {
    return t * t * (3.0 - 2.0 * t);
}

/// 与 GLSL 一致的 fract
double fract(double x) {
    return x - std::floor(x);
}

}  // namespace

double hash13(const Vec3& p) {
    // 与 shaders/common/noise.glsl 的 hash13 同式：
    //   p = fract(p * 0.1031); p += dot(p, p.yzx + 33.33); return fract((p.x + p.y) * p.z);
    Vec3 q{fract(p.x * 0.1031), fract(p.y * 0.1031), fract(p.z * 0.1031)};
    const double d = q.x * (q.y + 33.33) + q.y * (q.z + 33.33) + q.z * (q.x + 33.33);
    q.x += d;
    q.y += d;
    q.z += d;
    return fract((q.x + q.y) * q.z);
}

double value_noise(const Vec3& p) {
    const Vec3 i{std::floor(p.x), std::floor(p.y), std::floor(p.z)};
    const Vec3 f{p.x - i.x, p.y - i.y, p.z - i.z};
    const Vec3 w{smooth_step(f.x), smooth_step(f.y), smooth_step(f.z)};

    auto corner = [&](double dx, double dy, double dz) {
        return hash13(Vec3{i.x + dx, i.y + dy, i.z + dz});
    };

    const double n000 = corner(0.0, 0.0, 0.0);
    const double n100 = corner(1.0, 0.0, 0.0);
    const double n010 = corner(0.0, 1.0, 0.0);
    const double n110 = corner(1.0, 1.0, 0.0);
    const double n001 = corner(0.0, 0.0, 1.0);
    const double n101 = corner(1.0, 0.0, 1.0);
    const double n011 = corner(0.0, 1.0, 1.0);
    const double n111 = corner(1.0, 1.0, 1.0);

    const double nx00 = n000 + (n100 - n000) * w.x;
    const double nx10 = n010 + (n110 - n010) * w.x;
    const double nx01 = n001 + (n101 - n001) * w.x;
    const double nx11 = n011 + (n111 - n011) * w.x;
    const double nxy0 = nx00 + (nx10 - nx00) * w.y;
    const double nxy1 = nx01 + (nx11 - nx01) * w.y;
    return nxy0 + (nxy1 - nxy0) * w.z;
}

double fbm(const Vec3& p, int octaves) {
    double sum = 0.0;
    double amplitude = 0.5;
    double normalization = 0.0;
    Vec3 q = p;
    for (int i = 0; i < octaves; ++i) {
        sum += amplitude * value_noise(q);
        normalization += amplitude;
        q = Vec3{q.x * 2.02, q.y * 2.02, q.z * 2.02};  // 非整数倍频，避免格点对齐产生方块感
        amplitude *= 0.5;
    }
    return (normalization > 0.0) ? (sum / normalization) : 0.0;
}

double kepler_omega(double r) {
    // Ω = r^{−3/2}；r ≤ 3 处时序圆轨道不存在（光子球），返回 0 表示"不动"
    if (r <= 3.0) {
        return 0.0;
    }
    return 1.0 / (r * std::sqrt(r));
}

Vec3 derotate_for_disk(const Vec3& position, double r, double time) {
    const double omega = kepler_omega(r);
    if (omega == 0.0 || time == 0.0) {
        return position;
    }
    // 盘图案按 +Ω 顺行旋转 ⇒ 采样坐标反向旋转 −Ω·t
    const double angle = -omega * time;
    const double c = std::cos(angle);
    const double s = std::sin(angle);
    return Vec3{c * position.x - s * position.y, s * position.x + c * position.y, position.z};
}

}  // namespace ehe::core
