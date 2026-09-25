// Interpolation of transform lists (CSS Transforms 2 §"Interpolation of
// Transforms"): the two lists are padded to one length with identity
// functions, the leading pairs that share a primitive interpolate argument by
// argument, and everything from the first pair that does not is converted to
// a matrix on each side, decomposed, interpolated and recomposed.

#include "engine/css_interpolation.h"

#include <css/transform.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace bro::engine {

namespace {

constexpr double kPi = 3.14159265358979323846;

// One transform function as written: its name, its numeric arguments with
// their units, and its source text (for the matrix conversion).
struct TFn {
    std::string name;
    std::vector<double> args;
    std::vector<std::string> units;
    std::string text;
};

bool parseTransformList(const std::string& val, std::vector<TFn>& out) {
    out.clear();
    if (val.empty() || val == "none") return true;
    size_t pos = 0;
    while (pos < val.size()) {
        while (pos < val.size() && std::isspace(static_cast<unsigned char>(val[pos]))) ++pos;
        if (pos >= val.size()) break;
        const size_t start = pos;
        while (pos < val.size() && val[pos] != '(' && !std::isspace(static_cast<unsigned char>(val[pos])))
            ++pos;
        if (pos >= val.size() || val[pos] != '(') return false;
        TFn fn;
        fn.name = val.substr(start, pos - start);
        ++pos;
        while (pos < val.size() && val[pos] != ')') {
            while (pos < val.size() && (val[pos] == ' ' || val[pos] == ',' || val[pos] == '\t')) ++pos;
            if (pos >= val.size() || val[pos] == ')') break;
            char* end = nullptr;
            const double v = std::strtod(val.c_str() + pos, &end);
            if (end == val.c_str() + pos) return false;
            size_t u = static_cast<size_t>(end - val.c_str());
            std::string unit;
            while (u < val.size() && (std::isalpha(static_cast<unsigned char>(val[u])) || val[u] == '%'))
                unit += val[u++];
            fn.args.push_back(v);
            fn.units.push_back(unit);
            pos = u;
        }
        if (pos >= val.size()) return false;
        ++pos;  // ')'
        fn.text = val.substr(start, pos - start);
        out.push_back(std::move(fn));
    }
    return true;
}

std::string num(double v) {
    if (std::abs(v) < 1e-9) v = 0;
    std::ostringstream oss;
    oss << static_cast<float>(v);
    return oss.str();
}

double angleDeg(double v, const std::string& unit) {
    if (unit == "rad") return v * 180.0 / kPi;
    if (unit == "turn") return v * 360.0;
    if (unit == "grad") return v * 0.9;
    return v;
}

// A function reduced to its primitive (translate3d / scale3d / rotate3d /
// skew / perspective), with a flag for whether it was written in 3D.
enum class Prim { Translate, Scale, Rotate, Skew, Perspective, Matrix, Unknown };

struct PrimFn {
    Prim prim = Prim::Unknown;
    bool is3d = false;
    double v[4] = {0, 0, 0, 0};
    std::string unit[3];  // translate: per-axis length unit
};

PrimFn toPrimitive(const TFn& f) {
    PrimFn p;
    const auto& a = f.args;
    auto arg = [&](size_t i, double def) { return i < a.size() ? a[i] : def; };
    auto unit = [&](size_t i) { return i < f.units.size() ? f.units[i] : std::string(); };
    const std::string& n = f.name;
    if (n == "translate" || n == "translateX" || n == "translateY" || n == "translateZ" ||
        n == "translate3d") {
        p.prim = Prim::Translate;
        if (n == "translate") {
            p.v[0] = arg(0, 0); p.unit[0] = unit(0);
            p.v[1] = arg(1, 0); p.unit[1] = unit(1);
        } else if (n == "translateX") {
            p.v[0] = arg(0, 0); p.unit[0] = unit(0);
        } else if (n == "translateY") {
            p.v[1] = arg(0, 0); p.unit[1] = unit(0);
        } else if (n == "translateZ") {
            p.is3d = true;
            p.v[2] = arg(0, 0); p.unit[2] = unit(0);
        } else {
            p.is3d = true;
            for (size_t i = 0; i < 3; ++i) { p.v[i] = arg(i, 0); p.unit[i] = unit(i); }
        }
        return p;
    }
    if (n == "scale" || n == "scaleX" || n == "scaleY" || n == "scaleZ" || n == "scale3d") {
        p.prim = Prim::Scale;
        p.v[0] = p.v[1] = p.v[2] = 1;
        if (n == "scale") { p.v[0] = arg(0, 1); p.v[1] = arg(1, p.v[0]); }
        else if (n == "scaleX") p.v[0] = arg(0, 1);
        else if (n == "scaleY") p.v[1] = arg(0, 1);
        else if (n == "scaleZ") { p.is3d = true; p.v[2] = arg(0, 1); }
        else { p.is3d = true; p.v[0] = arg(0, 1); p.v[1] = arg(1, 1); p.v[2] = arg(2, 1); }
        // Percentages are numbers here.
        for (size_t i = 0; i < f.units.size(); ++i)
            if (f.units[i] == "%") p.v[std::min<size_t>(i, 2)] /= 100.0;
        return p;
    }
    if (n == "rotate" || n == "rotateZ" || n == "rotateX" || n == "rotateY" || n == "rotate3d") {
        p.prim = Prim::Rotate;
        if (n == "rotate3d") {
            p.is3d = true;
            double x = arg(0, 0), y = arg(1, 0), z = arg(2, 1);
            const double len = std::sqrt(x * x + y * y + z * z);
            if (len > 0) { x /= len; y /= len; z /= len; } else { x = 0; y = 0; z = 1; }
            p.v[0] = x; p.v[1] = y; p.v[2] = z;
            p.v[3] = angleDeg(arg(3, 0), unit(3));
        } else {
            p.is3d = n == "rotateX" || n == "rotateY";
            p.v[0] = n == "rotateX" ? 1 : 0;
            p.v[1] = n == "rotateY" ? 1 : 0;
            p.v[2] = (n == "rotate" || n == "rotateZ") ? 1 : 0;
            p.v[3] = angleDeg(arg(0, 0), unit(0));
        }
        return p;
    }
    if (n == "skew" || n == "skewX" || n == "skewY") {
        p.prim = Prim::Skew;
        if (n == "skew") { p.v[0] = angleDeg(arg(0, 0), unit(0)); p.v[1] = angleDeg(arg(1, 0), unit(1)); }
        else if (n == "skewX") p.v[0] = angleDeg(arg(0, 0), unit(0));
        else p.v[1] = angleDeg(arg(0, 0), unit(0));
        return p;
    }
    if (n == "perspective") {
        p.prim = Prim::Perspective;
        p.is3d = true;
        p.v[0] = arg(0, 0);  // 0 = none (infinite)
        return p;
    }
    if (n == "matrix" || n == "matrix3d") p.prim = Prim::Matrix;
    return p;
}

// The identity function of `f`'s primitive, written in `f`'s own form.
std::string identityOf(const TFn& f) {
    const std::string& n = f.name;
    std::ostringstream oss;
    oss << n << "(";
    if (n == "matrix") return "matrix(1, 0, 0, 1, 0, 0)";
    if (n == "matrix3d") return "matrix3d(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)";
    if (n == "rotate3d") {
        oss << num(f.args.size() > 0 ? f.args[0] : 0) << ", " << num(f.args.size() > 1 ? f.args[1] : 0)
            << ", " << num(f.args.size() > 2 ? f.args[2] : 1) << ", 0deg)";
        return oss.str();
    }
    if (n == "perspective") return "perspective(none)";
    const bool one = n.rfind("scale", 0) == 0;
    for (size_t i = 0; i < f.args.size(); ++i) {
        if (i) oss << ", ";
        oss << (one ? "1" : "0");
        if (!one && !f.units[i].empty() && f.units[i] != "%") oss << f.units[i];
        else if (!one && f.units[i] == "%") oss << "px";
    }
    oss << ")";
    return oss.str();
}

TFn parseOne(const std::string& text) {
    std::vector<TFn> fns;
    if (parseTransformList(text, fns) && fns.size() == 1) return fns[0];
    TFn f;
    f.name = "perspective";
    f.text = text;
    return f;
}

// Two length units a pair can lerp in: equal, or one side a unitless zero.
bool unitsJoin(double a, const std::string& ua, double b, const std::string& ub, std::string& out) {
    if (ua == ub) { out = ua; return true; }
    if (ua.empty() && a == 0) { out = ub; return true; }
    if (ub.empty() && b == 0) { out = ua; return true; }
    return false;
}

// Interpolate one pair that shares a primitive. False when it does not
// (different primitives, rotations about different axes, lengths of
// different units, a matrix).
bool interpolatePair(const TFn& fa, const TFn& fb, double t, std::string& out) {
    auto lerp = [t](double a, double b) { return a + (b - a) * t; };
    // The same function with arguments of the same units: argument by
    // argument, keeping the function as written.
    if (fa.name == fb.name && fa.args.size() == fb.args.size() && fa.name != "matrix" &&
        fa.name != "matrix3d" && fa.name != "rotate3d" && fa.name != "perspective") {
        std::ostringstream oss;
        oss << fa.name << "(";
        bool ok = true;
        for (size_t i = 0; i < fa.args.size() && ok; ++i) {
            std::string unit;
            ok = unitsJoin(fa.args[i], fa.units[i], fb.args[i], fb.units[i], unit);
            if (i) oss << ", ";
            oss << num(lerp(fa.args[i], fb.args[i])) << unit;
        }
        oss << ")";
        if (ok) { out = oss.str(); return true; }
    }

    const PrimFn a = toPrimitive(fa), b = toPrimitive(fb);
    if (a.prim != b.prim || a.prim == Prim::Matrix || a.prim == Prim::Unknown) return false;
    const bool is3d = a.is3d || b.is3d;
    std::ostringstream oss;
    switch (a.prim) {
        case Prim::Translate: {
            std::string u[3];
            for (int i = 0; i < 3; ++i)
                if (!unitsJoin(a.v[i], a.unit[i], b.v[i], b.unit[i], u[i])) return false;
            for (auto& s : u) if (s.empty()) s = "px";
            if (is3d)
                oss << "translate3d(" << num(lerp(a.v[0], b.v[0])) << u[0] << ", "
                    << num(lerp(a.v[1], b.v[1])) << u[1] << ", " << num(lerp(a.v[2], b.v[2])) << u[2] << ")";
            else
                oss << "translate(" << num(lerp(a.v[0], b.v[0])) << u[0] << ", "
                    << num(lerp(a.v[1], b.v[1])) << u[1] << ")";
            break;
        }
        case Prim::Scale:
            if (is3d)
                oss << "scale3d(" << num(lerp(a.v[0], b.v[0])) << ", " << num(lerp(a.v[1], b.v[1]))
                    << ", " << num(lerp(a.v[2], b.v[2])) << ")";
            else
                oss << "scale(" << num(lerp(a.v[0], b.v[0])) << ", " << num(lerp(a.v[1], b.v[1])) << ")";
            break;
        case Prim::Rotate: {
            // One axis for both: the same one, or the other's when an angle is
            // zero (a zero rotation has no axis of its own).
            const double* axis = a.v;
            const bool sameAxis = std::abs(a.v[0] - b.v[0]) < 1e-6 && std::abs(a.v[1] - b.v[1]) < 1e-6 &&
                                  std::abs(a.v[2] - b.v[2]) < 1e-6;
            if (!sameAxis) {
                if (a.v[3] == 0) axis = b.v;
                else if (b.v[3] != 0) return false;
            }
            const double angle = lerp(a.v[3], b.v[3]);
            if (!is3d && axis[2] == 1)
                oss << "rotate(" << num(angle) << "deg)";
            else
                oss << "rotate3d(" << num(axis[0]) << ", " << num(axis[1]) << ", " << num(axis[2])
                    << ", " << num(angle) << "deg)";
            break;
        }
        case Prim::Skew:
            oss << "skew(" << num(lerp(a.v[0], b.v[0])) << "deg, " << num(lerp(a.v[1], b.v[1])) << "deg)";
            break;
        case Prim::Perspective: {
            // perspective(none) is the identity: an infinite distance. Blend
            // the inverse distances, which is what the matrices hold.
            const double ia = a.v[0] > 0 ? 1.0 / a.v[0] : 0.0;
            const double ib = b.v[0] > 0 ? 1.0 / b.v[0] : 0.0;
            const double inv = lerp(ia, ib);
            if (inv <= 0) oss << "perspective(none)";
            else oss << "perspective(" << num(1.0 / inv) << "px)";
            break;
        }
        default:
            return false;
    }
    out = oss.str();
    return true;
}

// ---- Matrix decomposition ------------------------------------------------

// Column-major 4x4, m[col*4 + row], as htmlayout::css::Matrix3D.
using M4 = std::array<double, 16>;

M4 fromCss(const htmlayout::css::Matrix3D& m) {
    M4 r;
    for (int i = 0; i < 16; ++i) r[static_cast<size_t>(i)] = m.m[i];
    return r;
}

M4 mul(const M4& a, const M4& b) {
    M4 r{};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            double v = 0;
            for (int k = 0; k < 4; ++k) v += a[static_cast<size_t>(k * 4 + row)] * b[static_cast<size_t>(c * 4 + k)];
            r[static_cast<size_t>(c * 4 + row)] = v;
        }
    return r;
}

M4 identity4() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }

bool invert4(const M4& m, M4& inv) {
    const double* a = m.data();
    double r[16];
    r[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    r[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    r[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    r[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
    r[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    r[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    r[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    r[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
    r[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
    r[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    r[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    r[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
    r[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
    r[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
    r[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
    r[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
    const double det = a[0] * r[0] + a[1] * r[4] + a[2] * r[8] + a[3] * r[12];
    if (std::abs(det) < 1e-12) return false;
    for (int i = 0; i < 16; ++i) inv[static_cast<size_t>(i)] = r[i] / det;
    return true;
}

// CSS Transforms 1 §"Decomposing a 2D matrix", in column terms: the linear
// part is R(angle) · K · S, K the leftover (skew) 2x2, S the scales.
struct Decomp2D {
    double tx = 0, ty = 0, sx = 1, sy = 1, angle = 0;  // angle in degrees
    double k11 = 1, k12 = 0, k21 = 0, k22 = 1;          // K columns (k11,k12), (k21,k22)
};

bool decompose2D(const M4& m, Decomp2D& d) {
    double a = m[0], b = m[1], c = m[4], dd = m[5];
    d.tx = m[12];
    d.ty = m[13];
    d.sx = std::sqrt(a * a + b * b);
    d.sy = std::sqrt(c * c + dd * dd);
    // A flip: negate one scale (the spec's choice of which).
    if (a * dd - b * c < 0) {
        if (a < dd) d.sx = -d.sx;
        else d.sy = -d.sy;
    }
    if (d.sx != 0) { a /= d.sx; b /= d.sx; }
    if (d.sy != 0) { c /= d.sy; dd /= d.sy; }
    const double ang = std::atan2(b, a);
    const double cs = std::cos(ang), sn = std::sin(ang);
    // Rotate the columns by -angle.
    d.k11 = cs * a + sn * b;
    d.k12 = -sn * a + cs * b;
    d.k21 = cs * c + sn * dd;
    d.k22 = -sn * c + cs * dd;
    d.angle = ang * 180.0 / kPi;
    return true;
}

M4 recompose2D(const Decomp2D& d) {
    const double r = d.angle * kPi / 180.0, cs = std::cos(r), sn = std::sin(r);
    // R · K
    const double a0 = cs * d.k11 - sn * d.k12, b0 = sn * d.k11 + cs * d.k12;
    const double c0 = cs * d.k21 - sn * d.k22, d0 = sn * d.k21 + cs * d.k22;
    M4 m = identity4();
    m[0] = a0 * d.sx; m[1] = b0 * d.sx;
    m[4] = c0 * d.sy; m[5] = d0 * d.sy;
    m[12] = d.tx; m[13] = d.ty;
    return m;
}

Decomp2D interpolate2D(Decomp2D a, Decomp2D b, double t) {
    // CSS Transforms 1: a flip on different axes is a rotation by 180°.
    if ((a.sx < 0 && b.sy < 0) || (a.sy < 0 && b.sx < 0)) {
        a.sx = -a.sx;
        a.sy = -a.sy;
        a.angle += a.angle < 0 ? 180 : -180;
    }
    // Do not rotate the long way around.
    if (a.angle == 0) a.angle = 360;
    if (b.angle == 0) b.angle = 360;
    if (std::abs(a.angle - b.angle) > 180) {
        if (a.angle > b.angle) a.angle -= 360;
        else b.angle -= 360;
    }
    auto l = [t](double x, double y) { return x + (y - x) * t; };
    Decomp2D r;
    r.tx = l(a.tx, b.tx); r.ty = l(a.ty, b.ty);
    r.sx = l(a.sx, b.sx); r.sy = l(a.sy, b.sy);
    r.angle = l(a.angle, b.angle);
    r.k11 = l(a.k11, b.k11); r.k12 = l(a.k12, b.k12);
    r.k21 = l(a.k21, b.k21); r.k22 = l(a.k22, b.k22);
    return r;
}

// CSS Transforms 2 §"Decomposing a 3D matrix", in column terms: M =
// P · T · R · K · S, P the perspective, K the skews (upper unit triangle).
struct Decomp3D {
    double translate[3] = {0, 0, 0};
    double scale[3] = {1, 1, 1};
    double skew[3] = {0, 0, 0};  // XY, XZ, YZ
    double perspective[4] = {0, 0, 0, 1};
    double quat[4] = {0, 0, 0, 1};  // x, y, z, w
};

double dot3(const double* a, const double* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

bool decompose3D(M4 m, Decomp3D& d) {
    if (m[15] == 0) return false;
    for (double& v : m) v /= m[15];
    // The affine part (bottom row 0 0 0 1) must be invertible.
    M4 affine = m;
    affine[3] = affine[7] = affine[11] = 0;
    affine[15] = 1;
    M4 inv;
    if (!invert4(affine, inv)) return false;
    if (m[3] != 0 || m[7] != 0 || m[11] != 0) {
        // Bottom row of M = p^T · affine, so p^T = bottom · affine^-1.
        const double bottom[4] = {m[3], m[7], m[11], m[15]};
        for (int j = 0; j < 4; ++j) {
            double v = 0;
            for (int i = 0; i < 4; ++i) v += bottom[i] * inv[static_cast<size_t>(j * 4 + i)];
            d.perspective[j] = v;
        }
    }
    for (int i = 0; i < 3; ++i) d.translate[i] = m[static_cast<size_t>(12 + i)];
    // Columns of the linear part.
    double col[3][3];
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r) col[c][r] = m[static_cast<size_t>(c * 4 + r)];
    auto len = [](const double* v) { return std::sqrt(dot3(v, v)); };
    auto normalize = [&](double* v) {
        const double l = len(v);
        if (l != 0) for (int i = 0; i < 3; ++i) v[i] /= l;
    };
    auto combine = [](double* a, const double* b, double sb) {
        for (int i = 0; i < 3; ++i) a[i] += b[i] * sb;
    };
    d.scale[0] = len(col[0]);
    normalize(col[0]);
    d.skew[0] = dot3(col[0], col[1]);
    combine(col[1], col[0], -d.skew[0]);
    d.scale[1] = len(col[1]);
    normalize(col[1]);
    if (d.scale[1] != 0) d.skew[0] /= d.scale[1];
    d.skew[1] = dot3(col[0], col[2]);
    combine(col[2], col[0], -d.skew[1]);
    d.skew[2] = dot3(col[1], col[2]);
    combine(col[2], col[1], -d.skew[2]);
    d.scale[2] = len(col[2]);
    normalize(col[2]);
    if (d.scale[2] != 0) { d.skew[1] /= d.scale[2]; d.skew[2] /= d.scale[2]; }
    // A left-handed basis: negate everything.
    const double cross[3] = {col[1][1] * col[2][2] - col[1][2] * col[2][1],
                             col[1][2] * col[2][0] - col[1][0] * col[2][2],
                             col[1][0] * col[2][1] - col[1][1] * col[2][0]};
    if (dot3(col[0], cross) < 0) {
        for (int i = 0; i < 3; ++i) {
            d.scale[i] = -d.scale[i];
            for (int j = 0; j < 3; ++j) col[i][j] = -col[i][j];
        }
    }
    // R[row][c] = col[c][row].
    auto R = [&](int row, int c) { return col[c][row]; };
    d.quat[0] = 0.5 * std::sqrt(std::max(1 + R(0, 0) - R(1, 1) - R(2, 2), 0.0));
    d.quat[1] = 0.5 * std::sqrt(std::max(1 - R(0, 0) + R(1, 1) - R(2, 2), 0.0));
    d.quat[2] = 0.5 * std::sqrt(std::max(1 - R(0, 0) - R(1, 1) + R(2, 2), 0.0));
    d.quat[3] = 0.5 * std::sqrt(std::max(1 + R(0, 0) + R(1, 1) + R(2, 2), 0.0));
    if (R(2, 1) < R(1, 2)) d.quat[0] = -d.quat[0];
    if (R(0, 2) < R(2, 0)) d.quat[1] = -d.quat[1];
    if (R(1, 0) < R(0, 1)) d.quat[2] = -d.quat[2];
    return true;
}

M4 recompose3D(const Decomp3D& d) {
    M4 P = identity4();
    P[3] = d.perspective[0]; P[7] = d.perspective[1]; P[11] = d.perspective[2]; P[15] = d.perspective[3];
    M4 T = identity4();
    T[12] = d.translate[0]; T[13] = d.translate[1]; T[14] = d.translate[2];
    const double x = d.quat[0], y = d.quat[1], z = d.quat[2], w = d.quat[3];
    M4 R = identity4();
    auto set = [](M4& m, int row, int c, double v) { m[static_cast<size_t>(c * 4 + row)] = v; };
    set(R, 0, 0, 1 - 2 * (y * y + z * z));
    set(R, 0, 1, 2 * (x * y - z * w));
    set(R, 0, 2, 2 * (x * z + y * w));
    set(R, 1, 0, 2 * (x * y + z * w));
    set(R, 1, 1, 1 - 2 * (x * x + z * z));
    set(R, 1, 2, 2 * (y * z - x * w));
    set(R, 2, 0, 2 * (x * z - y * w));
    set(R, 2, 1, 2 * (y * z + x * w));
    set(R, 2, 2, 1 - 2 * (x * x + y * y));
    M4 K = identity4();
    set(K, 0, 1, d.skew[0]);
    set(K, 0, 2, d.skew[1]);
    set(K, 1, 2, d.skew[2]);
    M4 S = identity4();
    S[0] = d.scale[0]; S[5] = d.scale[1]; S[10] = d.scale[2];
    return mul(mul(mul(mul(P, T), R), K), S);
}

Decomp3D interpolate3D(const Decomp3D& a, const Decomp3D& b, double t) {
    auto l = [t](double x, double y) { return x + (y - x) * t; };
    Decomp3D r;
    for (int i = 0; i < 3; ++i) {
        r.translate[i] = l(a.translate[i], b.translate[i]);
        r.scale[i] = l(a.scale[i], b.scale[i]);
        r.skew[i] = l(a.skew[i], b.skew[i]);
    }
    for (int i = 0; i < 4; ++i) r.perspective[i] = l(a.perspective[i], b.perspective[i]);
    // Spherical linear interpolation of the rotations.
    double product = 0;
    for (int i = 0; i < 4; ++i) product += a.quat[i] * b.quat[i];
    product = std::clamp(product, -1.0, 1.0);
    if (std::abs(product) >= 1.0) {
        for (int i = 0; i < 4; ++i) r.quat[i] = a.quat[i];
        return r;
    }
    const double theta = std::acos(product);
    const double w = std::sin(t * theta) / std::sqrt(1 - product * product);
    const double wa = std::cos(t * theta) - product * w;
    for (int i = 0; i < 4; ++i) r.quat[i] = a.quat[i] * wa + b.quat[i] * w;
    return r;
}

bool is2D(const M4& m) {
    return m[2] == 0 && m[3] == 0 && m[6] == 0 && m[7] == 0 && m[8] == 0 && m[9] == 0 &&
           m[10] == 1 && m[11] == 0 && m[14] == 0 && m[15] == 1;
}

// Interpolate two function lists (the tail from the first pair without a
// common primitive) through their matrices.
bool interpolateMatrices(const std::string& fromText, const std::string& toText, double t,
                         std::string& out) {
    // Percentages resolve against the box, which a value string does not
    // know; such a pair stays discrete.
    if (fromText.find('%') != std::string::npos || toText.find('%') != std::string::npos)
        return false;
    const M4 ma = fromCss(htmlayout::css::parseTransform3D(fromText, 0, 0));
    const M4 mb = fromCss(htmlayout::css::parseTransform3D(toText, 0, 0));
    std::ostringstream oss;
    if (is2D(ma) && is2D(mb)) {
        Decomp2D da, db;
        decompose2D(ma, da);
        decompose2D(mb, db);
        const M4 r = recompose2D(interpolate2D(da, db, t));
        oss << "matrix(" << num(r[0]) << ", " << num(r[1]) << ", " << num(r[4]) << ", "
            << num(r[5]) << ", " << num(r[12]) << ", " << num(r[13]) << ")";
    } else {
        Decomp3D da, db;
        if (!decompose3D(ma, da) || !decompose3D(mb, db)) return false;
        const M4 r = recompose3D(interpolate3D(da, db, t));
        oss << "matrix3d(";
        for (int i = 0; i < 16; ++i) oss << (i ? ", " : "") << num(r[static_cast<size_t>(i)]);
        oss << ")";
    }
    out = oss.str();
    return true;
}

}  // namespace

bool interpolateTransformLists(const std::string& from, const std::string& to, float t,
                               std::string& out) {
    std::vector<TFn> a, b;
    if (!parseTransformList(from, a) || !parseTransformList(to, b)) return false;
    if (a.empty() && b.empty()) { out = "none"; return true; }
    // Pad the shorter list with the identities of the longer one's functions.
    while (a.size() < b.size()) a.push_back(parseOne(identityOf(b[a.size()])));
    while (b.size() < a.size()) b.push_back(parseOne(identityOf(a[b.size()])));

    std::string result;
    size_t i = 0;
    for (; i < a.size(); ++i) {
        std::string piece;
        if (!interpolatePair(a[i], b[i], t, piece)) break;
        if (!result.empty()) result += ' ';
        result += piece;
    }
    if (i < a.size()) {
        std::string restA, restB;
        for (size_t j = i; j < a.size(); ++j) {
            if (j > i) { restA += ' '; restB += ' '; }
            restA += a[j].text;
            restB += b[j].text;
        }
        std::string piece;
        if (!interpolateMatrices(restA, restB, t, piece)) return false;
        if (!result.empty()) result += ' ';
        result += piece;
    }
    out = std::move(result);
    return true;
}

}  // namespace bro::engine
