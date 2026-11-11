// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jadyx/game/motion_view.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <unordered_map>
#include <vector>

namespace Jadyx {
namespace {

constexpr float Pi = std::numbers::pi_v<float>;

Vec3 operator+(Vec3 a, Vec3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
Vec3 operator-(Vec3 a, Vec3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
Vec3 operator*(Vec3 a, float s) {
    return {a.x * s, a.y * s, a.z * s};
}
float Dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
Vec3 Cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Vec3 Normalize(Vec3 v) {
    const float n = std::sqrt(Dot(v, v));
    return n > 0.0f ? v * (1.0f / n) : Vec3{0, 0, 1};
}

/// The parts that move on their own.
enum Part : u8 {
    Body, StickCapL, StickCapR, FaceA, FaceB, FaceX, FaceY, BumperL, BumperR, TriggerL, TriggerR,
    DPad, ViewButton, MenuButton, PartCount,
};

/// How a surface takes the light.
enum class Material : u8 {
    Matte,  ///< The textured shell.
    Gloss,  ///< Polished plastic: face buttons, the stick collars.
    Glow,   ///< Lit or printed: the guide button, letters and icons. Not shaded.
};

struct Triangle {
    Vec3 a, b, c;
    Vec3 na, nb, nc; // Smoothed normals, filled in once the mesh is done.
    u32 color;       // 0xRRGGBB
    Part part;
    Material material;
    bool closed;     // Wound to face out of a closed solid: hidden when it faces away.
};

// The colors of the carbon black controller (from photos of a real one): soft black shell,
// polished black face buttons with colored letters, a lit guide button.
constexpr u32 ShellColor = 0x26282C;
constexpr u32 UnderColor = 0x212327;
constexpr u32 BumperColor = 0x2E3135;
constexpr u32 TriggerColor = 0x1F2124;
constexpr u32 CapRimColor = 0x2A2C30;
constexpr u32 CapDishColor = 0x1D1F22;
constexpr u32 ShaftColor = 0x18191C;
constexpr u32 CollarColor = 0x0E0F11;
constexpr u32 PadColor = 0x1C1E21;
constexpr u32 ButtonColor = 0x111214;
constexpr u32 SmallButtonColor = 0x17181B;
constexpr u32 IconColor = 0xA4A8AE;
constexpr u32 GuideRingColor = 0xF5F5F7;
constexpr u32 AColor = 0x8CC63F;
constexpr u32 BColor = 0xE0453A;
constexpr u32 XColor = 0x2E9BDB;
constexpr u32 YColor = 0xF2D22E;

// No card behind it: the controller floats over the game on a soft shadow, as the frontend's
// HUD does (ui_painter.h); rumble marks in the focal white.
constexpr float ShadowAlpha = 0.55f;
constexpr u32 AccentColor = 0xF5F5F7;
constexpr u32 RimColor = 0x9AA3AE;

// The model's frame is the pad's: X right, Y forward (away from the player), Z up out of its face.
// One unit is the controller's width.

struct P2 {
    float x, y;
};

/// The outline seen from above, traced from a top-down photo, counterclockwise: the right half
/// from the notch between the grips to the top edge's middle, the left half mirrored.
std::vector<P2> Outline() {
    const std::vector<P2> right{
        {0.000f, -0.315f}, {0.093f, -0.315f}, {0.185f, -0.322f}, {0.259f, -0.340f}, {0.306f, -0.389f},
        {0.352f, -0.444f}, {0.417f, -0.465f}, {0.468f, -0.442f}, {0.495f, -0.372f}, {0.502f, -0.278f},
        {0.488f, -0.139f}, {0.470f, 0.000f},  {0.446f, 0.093f},  {0.400f, 0.150f},  {0.326f, 0.182f},
        {0.185f, 0.196f},  {0.000f, 0.204f},
    };
    std::vector<P2> outline = right;
    for (std::size_t i = right.size() - 2; i >= 1; --i) {
        outline.push_back({-right[i].x, right[i].y});
    }
    // Corner cutting rounds it off.
    for (int round = 0; round < 2; ++round) {
        std::vector<P2> smooth;
        for (std::size_t i = 0; i < outline.size(); ++i) {
            const P2 a = outline[i];
            const P2 b = outline[(i + 1) % outline.size()];
            smooth.push_back({a.x * 0.75f + b.x * 0.25f, a.y * 0.75f + b.y * 0.25f});
            smooth.push_back({a.x * 0.25f + b.x * 0.75f, a.y * 0.25f + b.y * 0.75f});
        }
        outline = std::move(smooth);
    }
    return outline;
}

/// Each corner's outward normal on a counterclockwise outline.
std::vector<P2> OutwardNormals(const std::vector<P2>& outline) {
    const std::size_t n = outline.size();
    std::vector<P2> normals(n);
    const auto edge_normal = [](P2 a, P2 b) {
        const float dx = b.x - a.x, dy = b.y - a.y;
        const float len = std::max(std::sqrt(dx * dx + dy * dy), 1e-6f);
        return P2{dy / len, -dx / len};
    };
    for (std::size_t i = 0; i < n; ++i) {
        const P2 n0 = edge_normal(outline[(i + n - 1) % n], outline[i]);
        const P2 n1 = edge_normal(outline[i], outline[(i + 1) % n]);
        P2 avg{n0.x + n1.x, n0.y + n1.y};
        const float len = std::max(std::sqrt(avg.x * avg.x + avg.y * avg.y), 1e-6f);
        normals[i] = {avg.x / len, avg.y / len};
    }
    return normals;
}

std::vector<P2> Inset(const std::vector<P2>& outline, const std::vector<P2>& normals, float d) {
    std::vector<P2> inset(outline.size());
    for (std::size_t i = 0; i < outline.size(); ++i) {
        inset[i] = {outline[i].x - normals[i].x * d, outline[i].y - normals[i].y * d};
    }
    return inset;
}

/// The face's gentle dome: highest a little below the middle, lower toward the edges.
float FaceZ(float x, float y) {
    const float dy = y + 0.06f;
    return 0.050f + 0.030f * std::max(0.0f, 1.0f - 1.7f * x * x - 2.6f * dy * dy);
}

/// The underside: the grips hang lower than the middle, as on the real controller.
float BottomZ(float y) {
    const float t = std::clamp((-0.10f - y) / 0.32f, 0.0f, 1.0f);
    return -0.050f - 0.075f * t * t * (3.0f - 2.0f * t);
}

/// A point of a lathe profile: radius, height, and the color and finish of the band that ends
/// at it.
struct Lathe {
    float r, z;
    u32 color;
    Material material = Material::Matte;
};

class MeshBuilder {
public:
    std::vector<Triangle> triangles;

    /// `out` is the side the surface faces; triangles are wound to face it, so the ones that
    /// turn away from the camera can be skipped. A zero `out` keeps the triangle always drawn.
    void Tri(Vec3 a, Vec3 b, Vec3 c, u32 color, Part part, Material material, Vec3 out) {
        const bool closed = Dot(out, out) > 0.0f;
        if (closed && Dot(Cross(b - a, c - a), out) < 0.0f) std::swap(b, c);
        triangles.push_back({a, b, c, {}, {}, {}, color, part, material, closed});
    }
    void Quad(Vec3 a, Vec3 b, Vec3 c, Vec3 d, u32 color, Part part, Material material, Vec3 out) {
        Tri(a, b, c, color, part, material, out);
        Tri(a, c, d, color, part, material, out);
    }

    /// A surface of revolution around (x, y) through `profile`, from its outer bottom to its top.
    void Revolve(float x, float y, const std::vector<Lathe>& profile, Part part, int segments = 28) {
        for (std::size_t k = 1; k < profile.size(); ++k) {
            const Lathe& p0 = profile[k - 1];
            const Lathe& p1 = profile[k];
            for (int i = 0; i < segments; ++i) {
                const float a0 = 2.0f * Pi * static_cast<float>(i) / static_cast<float>(segments);
                const float a1 = 2.0f * Pi * static_cast<float>(i + 1) / static_cast<float>(segments);
                const Vec3 q00{x + std::cos(a0) * p0.r, y + std::sin(a0) * p0.r, p0.z};
                const Vec3 q01{x + std::cos(a1) * p0.r, y + std::sin(a1) * p0.r, p0.z};
                const Vec3 q10{x + std::cos(a0) * p1.r, y + std::sin(a0) * p1.r, p1.z};
                const Vec3 q11{x + std::cos(a1) * p1.r, y + std::sin(a1) * p1.r, p1.z};
                // The profile runs up from the outside: its outward side is (dz, -dr).
                const float am = 0.5f * (a0 + a1), dz = p1.z - p0.z, dr = p1.r - p0.r;
                const Vec3 out{std::cos(am) * dz, std::sin(am) * dz, -dr};
                // A band that closes on the axis is a fan of single triangles.
                if (p1.r <= 1e-6f) {
                    Tri(q00, q01, q10, p1.color, part, p1.material, out);
                    continue;
                }
                if (p0.r <= 1e-6f) {
                    Tri(q00, q11, q10, p1.color, part, p1.material, out);
                    continue;
                }
                Tri(q00, q01, q11, p1.color, part, p1.material, out);
                Tri(q00, q11, q10, p1.color, part, p1.material, out);
            }
        }
    }

    /// A flat stroke from a to b at height z: letters and icons.
    void Stroke(P2 a, P2 b, float width, float z, u32 color, Part part) {
        float dx = b.x - a.x, dy = b.y - a.y;
        const float len = std::max(std::sqrt(dx * dx + dy * dy), 1e-6f);
        dx /= len, dy /= len;
        const float h = width * 0.5f;
        // Square caps, so strokes meet without gaps.
        a = {a.x - dx * h, a.y - dy * h};
        b = {b.x + dx * h, b.y + dy * h};
        const P2 n{-dy * h, dx * h};
        Quad({a.x + n.x, a.y + n.y, z}, {a.x - n.x, a.y - n.y, z}, {b.x - n.x, b.y - n.y, z},
             {b.x + n.x, b.y + n.y, z}, color, part, Material::Glow, {0, 0, 1});
    }
    /// A polyline of strokes in a box of half size `s` around (x, y).
    void Glyph(float x, float y, float s, float z, const std::vector<P2>& points, float width, u32 color,
               Part part) {
        for (std::size_t i = 1; i < points.size(); ++i) {
            Stroke({x + points[i - 1].x * s, y + points[i - 1].y * s}, {x + points[i].x * s, y + points[i].y * s},
                   width, z, color, part);
        }
    }

    /// A flat-topped solid over a counterclockwise 2D outline: its top at `top(x, y)`, its sides
    /// down to `bottom`.
    template <typename Top>
    void Prism(const std::vector<P2>& outline, P2 center, float bottom, Top top, u32 color, u32 side_color,
               Part part) {
        const std::size_t n = outline.size();
        const Vec3 apex{center.x, center.y, top(center.x, center.y)};
        for (std::size_t i = 0; i < n; ++i) {
            const P2 a = outline[i], b = outline[(i + 1) % n];
            const Vec3 ta{a.x, a.y, top(a.x, a.y)}, tb{b.x, b.y, top(b.x, b.y)};
            Tri(apex, ta, tb, color, part, Material::Matte, {0, 0, 1});
            // The outline is counterclockwise, so (dy, -dx) points out of it.
            Quad({a.x, a.y, bottom}, {b.x, b.y, bottom}, tb, ta, side_color, part, Material::Matte,
                 {b.y - a.y, a.x - b.x, 0.0f});
        }
    }

    /// A closed `profile` (outward distance, height) swept along `path` with its outward
    /// normals: the bumpers and triggers that wrap round the top corners.
    void Sweep(const std::vector<P2>& path, const std::vector<P2>& normals, const std::vector<P2>& profile,
               u32 color, Part part) {
        const std::size_t m = profile.size();
        const auto at = [&](std::size_t i, std::size_t k) {
            return Vec3{path[i].x + normals[i].x * profile[k].x, path[i].y + normals[i].y * profile[k].x,
                        profile[k].y};
        };
        // The profile is counterclockwise in (outward, height): each edge faces (dz, -do).
        for (std::size_t i = 0; i + 1 < path.size(); ++i) {
            const P2 radial{normals[i].x + normals[i + 1].x, normals[i].y + normals[i + 1].y};
            for (std::size_t k = 0; k < m; ++k) {
                const std::size_t l = (k + 1) % m;
                const float dz = profile[l].y - profile[k].y, d_out = profile[l].x - profile[k].x;
                Quad(at(i, k), at(i + 1, k), at(i + 1, l), at(i, l), color, part, Material::Matte,
                     {radial.x * dz, radial.y * dz, -d_out * 2.0f});
            }
        }
        // Caps at both ends: fans over the profile, facing along the path.
        const std::size_t last = path.size() - 1;
        const Vec3 start_out{path[0].x - path[1].x, path[0].y - path[1].y, 0.0f};
        const Vec3 end_out{path[last].x - path[last - 1].x, path[last].y - path[last - 1].y, 0.0f};
        for (const std::size_t i : {std::size_t{0}, last}) {
            for (std::size_t k = 1; k + 1 < m; ++k) {
                Tri(at(i, 0), at(i, k), at(i, k + 1), color, part, Material::Matte, i == 0 ? start_out : end_out);
            }
        }
    }
};

/// Fills a flat polygon with rings shrunk toward `center` (the outline is star-shaped around a
/// point a little above its middle) and lifts it with `z(x, y)`. Rings keep the triangles
/// compact, which matters to the rasterizer: long slivers cost their whole bounding box.
template <typename Z>
void Surface(MeshBuilder& mesh, const std::vector<P2>& polygon, P2 center, int rings, Z z, u32 color, Vec3 out) {
    const std::size_t n = polygon.size();
    const auto at = [&](std::size_t i, int ring) {
        const float s = 1.0f - static_cast<float>(ring) / static_cast<float>(rings);
        const P2 p{center.x + (polygon[i % n].x - center.x) * s, center.y + (polygon[i % n].y - center.y) * s};
        return Vec3{p.x, p.y, z(p.x, p.y)};
    };
    for (int ring = 0; ring < rings; ++ring) {
        for (std::size_t i = 0; i < n; ++i) {
            if (ring + 1 == rings) {
                mesh.Tri(at(i, ring), at(i + 1, ring), at(i, rings), color, Body, Material::Matte, out);
            } else {
                mesh.Quad(at(i, ring), at(i + 1, ring), at(i + 1, ring + 1), at(i, ring + 1), color, Body,
                          Material::Matte, out);
            }
        }
    }
}

/// Averages the normals of triangles that share a corner, so curved plastic shades smoothly;
/// creases sharper than about 50 degrees stay sharp. Winding does not matter: normals are
/// lined up by sign before they are added.
void SmoothNormals(std::vector<Triangle>& triangles) {
    struct Key {
        std::int32_t x, y, z;
        u8 part;
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const {
            return (static_cast<std::size_t>(k.x) * 73856093u) ^ (static_cast<std::size_t>(k.y) * 19349663u) ^
                   (static_cast<std::size_t>(k.z) * 83492791u) ^ k.part;
        }
    };
    const auto key = [](Vec3 p, Part part) {
        return Key{static_cast<std::int32_t>(std::lround(p.x * 4000.0f)),
                   static_cast<std::int32_t>(std::lround(p.y * 4000.0f)),
                   static_cast<std::int32_t>(std::lround(p.z * 4000.0f)), part};
    };
    std::unordered_map<Key, Vec3, KeyHash> sums;
    std::vector<Vec3> faces(triangles.size());
    for (std::size_t i = 0; i < triangles.size(); ++i) {
        const Triangle& t = triangles[i];
        const Vec3 n = Cross(t.b - t.a, t.c - t.a);
        if (Dot(n, n) < 1e-14f) continue;
        faces[i] = Normalize(n);
        for (const Vec3& p : {t.a, t.b, t.c}) {
            Vec3& sum = sums[key(p, t.part)];
            sum = sum + (Dot(sum, faces[i]) < 0.0f ? faces[i] * -1.0f : faces[i]);
        }
    }
    for (std::size_t i = 0; i < triangles.size(); ++i) {
        Triangle& t = triangles[i];
        const Vec3 face = faces[i];
        const auto pick = [&](Vec3 p) {
            Vec3 n = Normalize(sums[key(p, t.part)]);
            if (Dot(n, face) < 0.0f) n = n * -1.0f;
            return Dot(n, face) > 0.64f ? n : face;
        };
        t.na = pick(t.a);
        t.nb = pick(t.b);
        t.nc = pick(t.c);
    }
}

std::vector<Triangle> BuildController() {
    MeshBuilder mesh;
    const std::vector<P2> outline = Outline();
    const std::vector<P2> normals = OutwardNormals(outline);
    const std::size_t n = outline.size();

    // The shell: the domed face, a rounded edge, the side, and an underside that swells into
    // the grips. Each ring is the outline moved inward and set at a height.
    enum class Height { Face, Side, Bottom };
    struct RingSpec {
        float inset;
        Height height;
        float dz;
    };
    std::vector<RingSpec> rings;
    constexpr float Round = 0.040f;
    constexpr int Steps = 4;
    for (int s = 0; s <= Steps; ++s) {
        const float angle = 0.5f * Pi * static_cast<float>(s) / Steps;
        rings.push_back({Round * (1.0f - std::sin(angle)), Height::Face, -Round * (1.0f - std::cos(angle))});
    }
    rings.push_back({0.0f, Height::Side, -0.012f});
    rings.push_back({0.004f, Height::Bottom, 0.022f});
    rings.push_back({0.016f, Height::Bottom, 0.007f});
    rings.push_back({0.034f, Height::Bottom, 0.0f});
    std::vector<std::vector<Vec3>> ring_points;
    for (const RingSpec& spec : rings) {
        const std::vector<P2> flat = Inset(outline, normals, spec.inset);
        // The face's height is taken at the bevel's inner edge, so each slice of it is level.
        const std::vector<P2> top = Inset(outline, normals, Round);
        std::vector<Vec3> points(n);
        for (std::size_t i = 0; i < n; ++i) {
            float z = spec.dz;
            if (spec.height == Height::Face) z += FaceZ(top[i].x, top[i].y);
            if (spec.height == Height::Bottom) z += BottomZ(flat[i].y);
            points[i] = {flat[i].x, flat[i].y, z};
        }
        ring_points.push_back(std::move(points));
    }
    for (std::size_t r = 0; r + 1 < ring_points.size(); ++r) {
        const u32 color = r >= static_cast<std::size_t>(Steps) + 1 ? UnderColor : ShellColor;
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t j = (i + 1) % n;
            const float d_out = rings[r].inset - rings[r + 1].inset;
            const float dz = ring_points[r + 1][i].z - ring_points[r][i].z;
            const Vec3 out{normals[i].x * -dz, normals[i].y * -dz, d_out};
            mesh.Quad(ring_points[r][i], ring_points[r][j], ring_points[r + 1][j], ring_points[r + 1][i], color, Body,
                      Material::Matte, out);
        }
    }
    // The face and the underside, finely split so the dome reads round. Their rims match the
    // first and last rings exactly.
    {
        const std::vector<P2> top = Inset(outline, normals, Round);
        constexpr P2 Middle{0.0f, 0.05f};
        Surface(mesh, top, Middle, 5, [](float x, float y) { return FaceZ(x, y); }, ShellColor, {0, 0, 1});
        // The ring heights came from the same points, so the seam is closed.
        const std::vector<P2> under = Inset(outline, normals, rings.back().inset);
        Surface(mesh, under, Middle, 2, [](float, float y) { return BottomZ(y); }, UnderColor, {0, 0, -1});
    }

    // Sticks: a polished collar in the shell, the shaft, and the cap with its grippy rim and
    // dished middle (which moves with the real stick).
    const auto stick = [&](float x, float y, Part cap) {
        const float f = FaceZ(x, y);
        mesh.Revolve(x, y,
                     {{0.090f, f - 0.004f, CollarColor},
                      {0.089f, f + 0.004f, CollarColor, Material::Gloss},
                      {0.083f, f + 0.009f, CollarColor, Material::Gloss},
                      {0.074f, f + 0.006f, CollarColor, Material::Gloss},
                      {0.040f, f - 0.004f, CollarColor, Material::Gloss},
                      {0.0f, f - 0.006f, CollarColor}},
                     Body);
        mesh.Revolve(x, y, {{0.025f, f - 0.006f, ShaftColor}, {0.025f, f + 0.050f, ShaftColor, Material::Gloss}}, cap, 16);
        mesh.Revolve(x, y,
                     {{0.025f, f + 0.048f, CapRimColor},
                      {0.060f, f + 0.054f, CapRimColor},
                      {0.067f, f + 0.062f, CapRimColor},
                      {0.066f, f + 0.071f, CapRimColor},
                      {0.058f, f + 0.075f, CapRimColor},
                      {0.046f, f + 0.073f, CapDishColor},
                      {0.026f, f + 0.068f, CapDishColor},
                      {0.0f, f + 0.066f, CapDishColor}},
                     cap);
    };
    stick(-0.296f, 0.028f, StickCapL);
    stick(0.139f, -0.171f, StickCapR);

    // D-pad: a shallow dish and the faceted cross on it.
    {
        const float x = -0.153f, y = -0.167f, f = FaceZ(x, y);
        mesh.Revolve(x, y,
                     {{0.090f, f - 0.004f, PadColor},
                      {0.089f, f + 0.006f, PadColor},
                      {0.083f, f + 0.010f, PadColor},
                      {0.040f, f + 0.007f, PadColor},
                      {0.0f, f + 0.006f, PadColor}},
                     DPad, 32);
        constexpr float W = 0.019f, L = 0.074f;
        const std::vector<P2> cross{
            {x + W, y + W}, {x + W, y + L}, {x - W, y + L}, {x - W, y + W}, {x - L, y + W}, {x - L, y - W},
            {x - W, y - W}, {x - W, y - L}, {x + W, y - L}, {x + W, y - W}, {x + L, y - W}, {x + L, y + W},
        };
        // The arms rise toward their tips, so each one catches the light on its own.
        mesh.Prism(cross, {x, y}, f + 0.006f,
                   [&](float px, float py) {
                       const float d = std::max(std::abs(px - x), std::abs(py - y));
                       return f + 0.012f + 0.006f * d / L;
                   },
                   0x26282C, 0x1A1B1E, DPad);
    }

    // A, B, X and Y: polished black domes with colored letters.
    {
        const float fx = 0.278f, fy = 0.018f, spread = 0.080f;
        const auto face = [&](float x, float y, u32 color, Part part, const std::vector<std::vector<P2>>& glyph) {
            const float f = FaceZ(x, y);
            mesh.Revolve(x, y,
                         {{0.044f, f - 0.004f, ButtonColor},
                          {0.043f, f + 0.011f, ButtonColor, Material::Gloss},
                          {0.039f, f + 0.018f, ButtonColor, Material::Gloss},
                          {0.030f, f + 0.0215f, ButtonColor, Material::Gloss},
                          {0.0f, f + 0.0225f, ButtonColor, Material::Gloss}},
                         part, 24);
            for (const auto& line : glyph) mesh.Glyph(x, y, 0.020f, f + 0.0232f, line, 0.0080f, color, part);
        };
        face(fx, fy - spread, AColor, FaceA, {{{-0.62f, -0.85f}, {0.0f, 0.85f}, {0.62f, -0.85f}}, {{-0.36f, -0.18f}, {0.36f, -0.18f}}});
        face(fx + spread, fy, BColor, FaceB,
             {{{-0.48f, -0.85f}, {-0.48f, 0.85f}, {0.18f, 0.85f}, {0.44f, 0.64f}, {0.44f, 0.22f}, {0.18f, 0.02f}, {-0.48f, 0.02f}},
              {{0.18f, 0.02f}, {0.52f, -0.20f}, {0.52f, -0.62f}, {0.24f, -0.85f}, {-0.48f, -0.85f}}});
        face(fx - spread, fy, XColor, FaceX, {{{-0.58f, -0.85f}, {0.58f, 0.85f}}, {{-0.58f, 0.85f}, {0.58f, -0.85f}}});
        face(fx, fy + spread, YColor, FaceY, {{{-0.58f, 0.85f}, {0.0f, 0.02f}, {0.58f, 0.85f}}, {{0.0f, 0.02f}, {0.0f, -0.85f}}});
    }

    // The guide button: a dark polished dome inside a lit ring, in the face under the top edge
    // (plain, no logo).
    {
        const float x = 0.0f, y = 0.112f, f = FaceZ(x, y);
        mesh.Revolve(x, y,
                     {{0.044f, f - 0.004f, CollarColor},
                      {0.044f, f + 0.003f, GuideRingColor, Material::Glow},
                      {0.039f, f + 0.005f, GuideRingColor, Material::Glow},
                      {0.036f, f + 0.006f, ButtonColor, Material::Gloss},
                      {0.026f, f + 0.012f, ButtonColor, Material::Gloss},
                      {0.0f, f + 0.014f, ButtonColor, Material::Gloss}},
                     Body, 32);
    }

    // View and Menu: small round buttons with their icons; Share: a pill under them.
    {
        const auto small = [&](float x, float y, Part part) {
            const float f = FaceZ(x, y);
            mesh.Revolve(x, y,
                         {{0.029f, f - 0.004f, SmallButtonColor},
                          {0.029f, f + 0.007f, SmallButtonColor, Material::Gloss},
                          {0.024f, f + 0.010f, SmallButtonColor, Material::Gloss},
                          {0.0f, f + 0.011f, SmallButtonColor, Material::Gloss}},
                         part, 20);
            return f + 0.0115f;
        };
        const float vx = -0.088f, mx = 0.088f, by = 0.036f;
        const float vz = small(vx, by, ViewButton);
        // View: two overlapping windows.
        mesh.Glyph(vx, by, 0.0135f, vz, {{-0.9f, -0.2f}, {-0.9f, 0.7f}, {0.2f, 0.7f}, {0.2f, 0.45f}}, 0.0034f, IconColor, ViewButton);
        mesh.Glyph(vx, by, 0.0135f, vz, {{-0.35f, -0.9f}, {-0.35f, 0.15f}, {0.9f, 0.15f}, {0.9f, -0.9f}, {-0.35f, -0.9f}}, 0.0034f,
                   IconColor, ViewButton);
        const float mz = small(mx, by, MenuButton);
        for (const float line : {-0.55f, 0.0f, 0.55f}) {
            mesh.Glyph(mx, by, 0.0135f, mz, {{-0.75f, line}, {0.75f, line}}, 0.0034f, IconColor, MenuButton);
        }
        const float sx = 0.0f, sy = -0.022f, f = FaceZ(sx, sy);
        std::vector<P2> pill;
        for (int i = 0; i <= 8; ++i) {
            const float a = -0.5f * Pi + Pi * static_cast<float>(i) / 8.0f;
            pill.push_back({sx + 0.018f + std::cos(a) * 0.016f, sy + std::sin(a) * 0.016f});
        }
        for (int i = 0; i <= 8; ++i) {
            const float a = 0.5f * Pi + Pi * static_cast<float>(i) / 8.0f;
            pill.push_back({sx - 0.018f + std::cos(a) * 0.016f, sy + std::sin(a) * 0.016f});
        }
        mesh.Prism(pill, {sx, sy}, f - 0.004f, [&](float, float) { return f + 0.010f; }, SmallButtonColor,
                   SmallButtonColor, Body);
        // Share: an arrow up out of a tray.
        mesh.Glyph(sx, sy, 0.010f, f + 0.0105f, {{-1.0f, 0.1f}, {-1.0f, -0.8f}, {1.0f, -0.8f}, {1.0f, 0.1f}}, 0.0024f, IconColor, Body);
        mesh.Glyph(sx, sy, 0.010f, f + 0.0105f, {{0.0f, -0.35f}, {0.0f, 0.85f}}, 0.0024f, IconColor, Body);
        mesh.Glyph(sx, sy, 0.010f, f + 0.0105f, {{-0.45f, 0.4f}, {0.0f, 0.85f}, {0.45f, 0.4f}}, 0.0024f, IconColor, Body);
    }

    // Bumpers wrap round the top corners; the triggers sit under and in front of them.
    {
        const auto arc = [&](bool right, float from_x, float down_to_y) {
            std::vector<P2> path, path_normals;
            for (std::size_t i = 0; i < n; ++i) {
                const P2 p = outline[i];
                const bool side = right ? p.x > from_x : p.x < -from_x;
                if (side && p.y > down_to_y) {
                    path.push_back(p);
                    path_normals.push_back(normals[i]);
                }
            }
            // The outline runs counterclockwise; walk each arc from the top edge outward.
            if (right) {
                std::reverse(path.begin(), path.end());
                std::reverse(path_normals.begin(), path_normals.end());
            }
            return std::pair{path, path_normals};
        };
        const std::vector<P2> bumper{{-0.016f, -0.004f}, {0.016f, -0.004f}, {0.030f, 0.010f}, {0.032f, 0.040f},
                                     {0.022f, 0.062f},   {-0.004f, 0.070f},  {-0.016f, 0.066f}};
        const std::vector<P2> trigger{{-0.004f, -0.080f}, {0.030f, -0.072f}, {0.052f, -0.050f},
                                      {0.058f, -0.020f},  {0.044f, -0.006f}, {-0.004f, -0.010f}};
        for (const bool right : {false, true}) {
            const auto [bumper_path, bumper_normals] = arc(right, 0.06f, 0.02f);
            mesh.Sweep(bumper_path, bumper_normals, bumper, BumperColor, right ? BumperR : BumperL);
            const auto [trigger_path, trigger_normals] = arc(right, 0.20f, 0.10f);
            mesh.Sweep(trigger_path, trigger_normals, trigger, TriggerColor, right ? TriggerR : TriggerL);
        }
    }
    SmoothNormals(mesh.triangles);
    return std::move(mesh.triangles);
}

const std::vector<Triangle>& Controller() {
    static const std::vector<Triangle> triangles = BuildController();
    return triangles;
}

/// The camera: high above the player's side, close to how one looks down at a controller in
/// their hands, with a long lens so it reads like a product shot.
struct Camera {
    Vec3 eye{0.0f, -1.05f, 1.95f};
    Vec3 forward, right, up;
    float focal{};
    float cx{}, cy{};

    Camera(float width, float height) {
        const Vec3 target{0.0f, -0.10f, -0.02f};
        forward = Normalize(target - eye);
        right = Normalize(Cross(forward, {0, 0, 1}));
        up = Cross(right, forward);
        focal = width * 1.55f;
        cx = width * 0.5f;
        cy = height * 0.50f;
    }
    /// Screen x, y and depth.
    [[nodiscard]] Vec3 Project(Vec3 p) const {
        const Vec3 d = p - eye;
        const float z = std::max(Dot(d, forward), 0.05f);
        return {cx + Dot(d, right) / z * focal, cy - Dot(d, up) / z * focal, z};
    }
};

struct Color3 {
    float r, g, b;
};

Color3 Unpack(u32 rgb) {
    return {static_cast<float>((rgb >> 16) & 0xFF), static_cast<float>((rgb >> 8) & 0xFF),
            static_cast<float>(rgb & 0xFF)};
}
u32 Pack(Color3 c) {
    const auto ch = [](float v) { return static_cast<u32>(std::clamp(v, 0.0f, 255.0f) + 0.5f); };
    return (ch(c.r) << 16) | (ch(c.g) << 8) | ch(c.b);
}

struct Raster {
    int width{}, height{};
    std::vector<u32> color; ///< Premultiplied by cover: transparent black where nothing is drawn.
    std::vector<float> depth;
    std::vector<float> cover; ///< How much of each sample is drawn on, 0 to 1.

    Raster(int w, int h) : width{w}, height{h}, color(static_cast<std::size_t>(w) * h),
                           depth(static_cast<std::size_t>(w) * h, 0.0f),
                           cover(static_cast<std::size_t>(w) * h, 0.0f) {}

    /// A triangle with a color at each corner, blended across it.
    void Triangle(Vec3 a, Vec3 b, Vec3 c, Color3 ca, Color3 cb, Color3 cc) {
        const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        if (std::abs(area) < 1e-6f) return;
        const int x0 = std::max(0, static_cast<int>(std::floor(std::min({a.x, b.x, c.x}))));
        const int x1 = std::min(width - 1, static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}))));
        const int y0 = std::max(0, static_cast<int>(std::floor(std::min({a.y, b.y, c.y}))));
        const int y1 = std::min(height - 1, static_cast<int>(std::ceil(std::max({a.y, b.y, c.y}))));
        if (x0 > x1 || y0 > y1) return;
        const float inv_area = 1.0f / area;
        // The buffer keeps 1/z, which is linear on the screen: nearer is larger.
        const float iza = 1.0f / a.z, izb = 1.0f / b.z, izc = 1.0f / c.z;
        // The weights are linear on the screen: they step along a row and down the rows.
        const float w0dx = -(c.y - b.y) * inv_area, w1dx = -(a.y - c.y) * inv_area;
        const float w0dy = (c.x - b.x) * inv_area, w1dy = (a.x - c.x) * inv_area;
        const float w2dx = -(w0dx + w1dx);
        const float px0 = static_cast<float>(x0) + 0.5f, py0 = static_cast<float>(y0) + 0.5f;
        float row_w0 = ((b.x - px0) * (c.y - py0) - (b.y - py0) * (c.x - px0)) * inv_area;
        float row_w1 = ((c.x - px0) * (a.y - py0) - (c.y - py0) * (a.x - px0)) * inv_area;
        const auto inverse = [](float d) { return std::abs(d) > 1e-9f ? 1.0f / d : 0.0f; };
        const float i0 = inverse(w0dx), i1 = inverse(w1dx), i2 = inverse(w2dx);
        // Colors are clamped once here, so blends of them need no clamp per pixel.
        const auto clamp3 = [](Color3 k) {
            return Color3{std::clamp(k.r, 0.0f, 255.0f), std::clamp(k.g, 0.0f, 255.0f), std::clamp(k.b, 0.0f, 255.0f)};
        };
        ca = clamp3(ca), cb = clamp3(cb), cc = clamp3(cc);
        for (int y = y0; y <= y1; ++y, row_w0 += w0dy, row_w1 += w1dy) {
            // Only the span of the row inside all three edges (a pixel of slack each side;
            // the tests below stay exact).
            float lo = 0.0f, hi = static_cast<float>(x1 - x0);
            const auto clip = [&](float w, float inv) {
                if (inv > 0.0f) {
                    lo = std::max(lo, -w * inv - 1.0f);
                } else if (inv < 0.0f) {
                    hi = std::min(hi, -w * inv + 1.0f);
                } else if (w < 0.0f) {
                    hi = -1.0f;
                }
            };
            clip(row_w0, i0);
            clip(row_w1, i1);
            clip(1.0f - row_w0 - row_w1, i2);
            if (hi < lo) continue;
            const int first = x0 + std::max(0, static_cast<int>(lo));
            const int last = std::min(x1, x0 + static_cast<int>(hi));
            const float skip = static_cast<float>(first - x0);
            float w0 = row_w0 + w0dx * skip, w1 = row_w1 + w1dx * skip;
            for (int x = first; x <= last; ++x, w0 += w0dx, w1 += w1dx) {
                const float w2 = 1.0f - w0 - w1;
                if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
                const float iz = w0 * iza + w1 * izb + w2 * izc;
                const std::size_t at = static_cast<std::size_t>(y) * width + x;
                if (iz <= depth[at]) continue;
                depth[at] = iz;
                cover[at] = 1.0f;
                const auto ch = [](float v) { return static_cast<u32>(v + 0.5f); };
                color[at] = (ch(ca.r * w0 + cb.r * w1 + cc.r * w2) << 16) |
                            (ch(ca.g * w0 + cb.g * w1 + cc.g * w2) << 8) | ch(ca.b * w0 + cb.b * w1 + cc.b * w2);
            }
        }
    }

    /// A filled disc blended over what is there.
    void Disc(float cx, float cy, float radius, u32 rgb, float alpha) {
        const int x0 = std::max(0, static_cast<int>(cx - radius - 1));
        const int x1 = std::min(width - 1, static_cast<int>(cx + radius + 1));
        const int y0 = std::max(0, static_cast<int>(cy - radius - 1));
        const int y1 = std::min(height - 1, static_cast<int>(cy + radius + 1));
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const float ddx = static_cast<float>(x) + 0.5f - cx, ddy = static_cast<float>(y) + 0.5f - cy;
                const float d = std::sqrt(ddx * ddx + ddy * ddy);
                const float amount = std::clamp(radius - d + 0.5f, 0.0f, 1.0f) * alpha;
                if (amount <= 0.0f) continue;
                const std::size_t at = static_cast<std::size_t>(y) * width + x;
                // "Over" in premultiplied color: mixing toward the dot is exactly that.
                color[at] = Mix(color[at], rgb, amount);
                cover[at] += (1.0f - cover[at]) * amount;
            }
        }
    }

    static u32 Mix(u32 a, u32 b, float t) {
        const Color3 ca = Unpack(a), cb = Unpack(b);
        return Pack({ca.r + (cb.r - ca.r) * t, ca.g + (cb.g - ca.g) * t, ca.b + (cb.b - ca.b) * t});
    }
};

/// Studio light: a key light from the upper left, a weak fill from the right, highlights in
/// white on top, and a cool rim where a surface turns away from the camera, which is what
/// draws the edge of black plastic against a dark background.
Color3 Light(u32 rgb, Material material, Vec3 normal, Vec3 view) {
    const Color3 base = Unpack(rgb);
    if (material == Material::Glow) return base;
    static const Vec3 key = Normalize({-0.45f, -0.35f, 1.0f});
    static const Vec3 fill = Normalize({0.8f, -0.2f, 0.35f});
    const float diffuse = std::max(Dot(normal, key), 0.0f) * 0.85f + std::max(Dot(normal, fill), 0.0f) * 0.25f;
    const bool gloss = material == Material::Gloss;
    const float ambient = gloss ? 0.30f : 0.42f;
    const float spec_strength = gloss ? 0.85f : 0.16f;
    const Vec3 half = Normalize(key + view);
    // Powers by squaring: 48 = 32 + 16 for gloss, 8 + 2 for the shell.
    const float h1 = std::max(Dot(normal, half), 0.0f), h2 = h1 * h1, h4 = h2 * h2, h8 = h4 * h4, h16 = h8 * h8;
    const float specular = (gloss ? h16 * h16 * h16 : h8 * h2) * spec_strength;
    const float facing = std::clamp(Dot(normal, view), 0.0f, 1.0f);
    const float away = 1.0f - facing;
    const float rim = away * away * away * 0.55f;
    const float lit = ambient + (gloss ? 0.6f : 0.9f) * diffuse;
    const Color3 rim_color = Unpack(RimColor);
    return {base.r * lit + 255.0f * specular + rim_color.r * rim, base.g * lit + 255.0f * specular + rim_color.g * rim,
            base.b * lit + 255.0f * specular + rim_color.b * rim};
}

} // namespace

D3D12::MenuImage DrawMotionView(const MotionViewState& state, std::pair<u32, u32> screen, float seconds) {
    const auto [screen_width, screen_height] = screen;
    if (screen_width < 64 || screen_height < 64) return {};
    const u32 height = std::max<u32>(96, screen_height * 26 / 100);
    const u32 width = height * 4 / 3;
    constexpr int SS = 2;
    const int rw = static_cast<int>(width) * SS, rh = static_cast<int>(height) * SS;

    Raster raster(rw, rh);
    const Camera camera(static_cast<float>(rw), static_cast<float>(rh));

    // Where the controller is: the gyro's orientation, the shake's swing and the rumble's buzz.
    Quat orientation = state.orientation;
    Vec3 offset = {state.shake.x * 0.10f, 0.0f, state.shake.z * 0.06f};
    orientation = (Quat::AxisAngle({0, 1, 0}, state.shake.x * 0.25f) * orientation).Normalized();
    const float rumble = std::clamp(state.rumble, 0.0f, 1.0f);
    if (rumble > 0.01f) {
        offset = offset + Vec3{std::sin(seconds * 91.0f) * 0.010f * rumble, std::sin(seconds * 67.0f + 2.0f) * 0.006f * rumble,
                               std::sin(seconds * 113.0f + 1.0f) * 0.008f * rumble};
        orientation = (Quat::AxisAngle({0, 0, 1}, std::sin(seconds * 79.0f) * 0.035f * rumble) * orientation).Normalized();
    }

    // Each part's own movement: pressed buttons sink, triggers pull in, caps follow the sticks.
    namespace Btn = MotionViewButton;
    std::array<Vec3, PartCount> part_offset{};
    const u32 held = state.buttons;
    const auto sink = [&](Part part, bool down) {
        if (down) part_offset[part] = {0.0f, 0.0f, -0.010f};
    };
    // The Switch's A is the right face button by position, the Xbox A by letter.
    const bool pos = state.by_position;
    sink(pos ? FaceB : FaceA, (held & Btn::A) != 0);
    sink(pos ? FaceA : FaceB, (held & Btn::B) != 0);
    sink(pos ? FaceY : FaceX, (held & Btn::X) != 0);
    sink(pos ? FaceX : FaceY, (held & Btn::Y) != 0);
    if (held & Btn::L) part_offset[BumperL] = {0.0f, -0.006f, -0.008f};
    if (held & Btn::R) part_offset[BumperR] = {0.0f, -0.006f, -0.008f};
    sink(ViewButton, (held & Btn::Minus) != 0);
    sink(MenuButton, (held & Btn::Plus) != 0);
    if (held & Btn::ZL) part_offset[TriggerL] = {0.0f, -0.022f, 0.010f};
    if (held & Btn::ZR) part_offset[TriggerR] = {0.0f, -0.022f, 0.010f};
    if (held & (Btn::Up | Btn::Down | Btn::Left | Btn::Right)) part_offset[DPad] = {0.0f, 0.0f, -0.006f};
    part_offset[StickCapL] = {state.left_x * 0.022f, state.left_y * 0.022f, (held & Btn::StickL) ? -0.010f : 0.0f};
    part_offset[StickCapR] = {state.right_x * 0.022f, state.right_y * 0.022f, (held & Btn::StickR) ? -0.010f : 0.0f};

    for (const Triangle& t : Controller()) {
        const Vec3 local = part_offset[t.part];
        const Vec3 a = orientation.Rotate(t.a + local) + offset;
        const Vec3 b = orientation.Rotate(t.b + local) + offset;
        const Vec3 c = orientation.Rotate(t.c + local) + offset;
        // Lit from the side the camera sees, whichever way the triangle was wound.
        const Vec3 centroid = (a + b + c) * (1.0f / 3.0f);
        const Vec3 view = Normalize(camera.eye - centroid);
        const float side = Dot(Cross(b - a, c - a), view) < 0.0f ? -1.0f : 1.0f;
        if (t.closed && side < 0.0f) continue;
        const auto shade = [&](Vec3 n) {
            Vec3 normal = orientation.Rotate(n) * side;
            // A smoothed normal can lean past the silhouette; keep it facing the camera.
            if (Dot(normal, view) < 0.0f) normal = Normalize(normal - view * Dot(normal, view));
            return Light(t.color, t.material, normal, view);
        };
        raster.Triangle(camera.Project(a), camera.Project(b), camera.Project(c), shade(t.na), shade(t.nb), shade(t.nc));
    }

    // Rumble: arcs of white dots beside the grips, as strong as the game asks.
    if (rumble > 0.05f) {
        for (const float side : {-1.0f, 1.0f}) {
            const Vec3 grip = camera.Project(orientation.Rotate({side * 0.47f, -0.34f, 0.0f}) + offset);
            for (int wave = 0; wave < 3; ++wave) {
                const float radius = static_cast<float>(rh) * (0.08f + 0.05f * static_cast<float>(wave));
                const float alpha = rumble * (0.9f - 0.25f * static_cast<float>(wave));
                for (int dot = -3; dot <= 3; ++dot) {
                    const float angle = static_cast<float>(dot) * 0.17f;
                    const float x = grip.x + side * std::cos(angle) * radius;
                    const float y = grip.y + std::sin(angle) * radius;
                    raster.Disc(x, y, static_cast<float>(SS) * 1.4f, AccentColor, alpha);
                }
            }
        }
    }

    // Down to the image's size: premultiplied RGBA (the renderer blends it as such).
    const std::size_t count = static_cast<std::size_t>(width) * height;
    std::vector<float> alpha(count);
    std::vector<u32> rgb(count);
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            u32 r = 0, g = 0, b = 0;
            float a = 0.0f;
            for (int sy = 0; sy < SS; ++sy) {
                for (int sx = 0; sx < SS; ++sx) {
                    const std::size_t at = (static_cast<std::size_t>(y) * SS + sy) * rw + x * SS + sx;
                    const u32 c = raster.color[at];
                    r += (c >> 16) & 0xFF;
                    g += (c >> 8) & 0xFF;
                    b += c & 0xFF;
                    a += raster.cover[at];
                }
            }
            const std::size_t at = static_cast<std::size_t>(y) * width + x;
            rgb[at] = ((r / (SS * SS)) << 16) | ((g / (SS * SS)) << 8) | (b / (SS * SS));
            alpha[at] = a / static_cast<float>(SS * SS);
        }
    }
    // The shadow: the silhouette dropped a little and blurred (three box passes each way, close
    // to a gaussian), in black, so it lifts the black controller off a bright scene.
    std::vector<float> shadow(count, 0.0f);
    {
        const int drop = std::max(2, static_cast<int>(height) / 40);
        const int radius = std::max(2, static_cast<int>(height) / 45);
        for (u32 y = static_cast<u32>(drop); y < height; ++y) {
            std::copy_n(alpha.begin() + static_cast<std::ptrdiff_t>((y - drop) * width), width,
                        shadow.begin() + static_cast<std::ptrdiff_t>(y * width));
        }
        std::vector<float> line(std::max(width, height));
        const auto blur = [&](std::size_t start, std::size_t stride, int length) {
            for (int pass = 0; pass < 3; ++pass) {
                float sum = 0.0f;
                for (int i = 0; i < length; ++i) line[i] = shadow[start + i * stride];
                for (int i = 0; i <= radius && i < length; ++i) sum += line[i];
                for (int i = 0; i < length; ++i) {
                    shadow[start + i * stride] = sum / static_cast<float>(2 * radius + 1);
                    if (i + radius + 1 < length) sum += line[i + radius + 1];
                    if (i - radius >= 0) sum -= line[i - radius];
                }
            }
        };
        for (u32 y = 0; y < height; ++y) blur(static_cast<std::size_t>(y) * width, 1, static_cast<int>(width));
        for (u32 x = 0; x < width; ++x) blur(x, width, static_cast<int>(height));
    }
    D3D12::MenuImage image;
    image.width = width;
    image.height = height;
    image.pixels.resize(count * 4);
    for (std::size_t at = 0; at < count; ++at) {
        const float a = alpha[at];
        // Black shadow under the controller: it adds coverage, not color.
        const float out = a + (1.0f - a) * shadow[at] * ShadowAlpha;
        u8* px = image.pixels.data() + at * 4;
        const u32 cap = static_cast<u32>(std::clamp(out * 255.0f + 0.5f, 0.0f, 255.0f));
        // Premultiplied: no channel above the alpha, whatever the rounding did.
        px[0] = static_cast<u8>(std::min((rgb[at] >> 16) & 0xFF, cap));
        px[1] = static_cast<u8>(std::min((rgb[at] >> 8) & 0xFF, cap));
        px[2] = static_cast<u8>(std::min(rgb[at] & 0xFF, cap));
        px[3] = static_cast<u8>(cap);
    }
    const u32 margin = std::max<u32>(8, screen_height / 40);
    image.placed = true;
    image.x = margin;
    image.y = screen_height > height + margin ? screen_height - height - margin : 0;
    return image;
}

} // namespace Jadyx
