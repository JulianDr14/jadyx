// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// What reaches the rasterizer: primitive types, indices, instancing, indirect draws, vertex attribute
// formats, tessellation and geometry shaders.

#include "harness.h"
#include "scene_util.h"

namespace {

Vertex At(Gpu& gpu, float x, float y, const Color& color = Green) {
    return {{gpu.NdcX(x), gpu.NdcY(y), 0},
            {static_cast<float>(color.r), static_cast<float>(color.g), static_cast<float>(color.b),
             static_cast<float>(color.a)},
            {0, 0}};
}

void BindIndices(Gpu& gpu, DkIdxFormat format, const void* data, uint32_t size) {
    Slice slice = gpu.Alloc(size, 4);
    std::memcpy(slice.cpu, data, size);
    gpu.cmd.bindIdxBuffer(format, slice.gpu);
}

// The left half of the target as two triangles of four vertices, indexed.
void QuadVertices(Gpu& gpu) {
    const Vertex vertices[4] = {At(gpu, 0, 0), At(gpu, 0, 32), At(gpu, 16, 0), At(gpu, 16, 32)};
    gpu.BindVertices(vertices, 4);
}

void ExpectLeftHalf(Gpu& gpu) {
    gpu.Expect(8, 16, Green);
    gpu.Expect(24, 16, Black);
}

struct AttributeCase {
    const char* name;
    DkVtxAttribSize size;
    DkVtxAttribType type;
    bool bgra;
    int shader; // 0 float, 1 uint, 2 sint
    uint32_t words[4];
    Color expected;
};

constexpr double U8(int value) {
    return value / 255.0;
}
constexpr double U16(int value) {
    return value / 65535.0;
}

constexpr AttributeCase Attributes[] = {
    {"4x8_unorm", DkVtxAttribSize_4x8, DkVtxAttribType_Unorm, false, 0, {0xFF804000}, {0, U8(64), U8(128), 1}},
    {"4x8_snorm", DkVtxAttribSize_4x8, DkVtxAttribType_Snorm, false, 0, {0x80817F00}, {0, 1, -1, -1}},
    {"4x8_uscaled", DkVtxAttribSize_4x8, DkVtxAttribType_Uscaled, false, 0, {0x04030201}, {1, 2, 3, 4}},
    {"4x8_sscaled", DkVtxAttribSize_4x8, DkVtxAttribType_Sscaled, false, 0, {0xFC03FE01}, {1, -2, 3, -4}},
    {"4x8_uint", DkVtxAttribSize_4x8, DkVtxAttribType_Uint, false, 1, {0xC8030201}, {1, 2, 3, 200}},
    {"4x8_sint", DkVtxAttribSize_4x8, DkVtxAttribType_Sint, false, 2, {0xFC03FE01}, {1, -2, 3, -4}},
    {"4x8_unorm_bgra", DkVtxAttribSize_4x8, DkVtxAttribType_Unorm, true, 0, {0xFF804000}, {U8(128), U8(64), 0, 1}},
    {"3x8_unorm", DkVtxAttribSize_3x8, DkVtxAttribType_Unorm, false, 0, {0x00804000}, {0, U8(64), U8(128), 1}},
    {"2x8_unorm", DkVtxAttribSize_2x8, DkVtxAttribType_Unorm, false, 0, {0x00004080}, {U8(128), U8(64), 0, 1}},
    {"1x8_unorm", DkVtxAttribSize_1x8, DkVtxAttribType_Unorm, false, 0, {0x00000080}, {U8(128), 0, 0, 1}},
    {"1x8_uint", DkVtxAttribSize_1x8, DkVtxAttribType_Uint, false, 1, {0x00000005}, {5, 0, 0, 1}},
    {"4x16_unorm", DkVtxAttribSize_4x16, DkVtxAttribType_Unorm, false, 0, {0x40000000, 0xFFFF8000},
     {0, U16(0x4000), U16(0x8000), 1}},
    {"4x16_snorm", DkVtxAttribSize_4x16, DkVtxAttribType_Snorm, false, 0, {0x7FFF0000, 0x80008001}, {0, 1, -1, -1}},
    {"4x16_uscaled", DkVtxAttribSize_4x16, DkVtxAttribType_Uscaled, false, 0, {0x00020001, 0x00040003}, {1, 2, 3, 4}},
    {"4x16_sscaled", DkVtxAttribSize_4x16, DkVtxAttribType_Sscaled, false, 0, {0xFFFE0001, 0xFFFC0003}, {1, -2, 3, -4}},
    {"4x16_uint", DkVtxAttribSize_4x16, DkVtxAttribType_Uint, false, 1, {0x00020001, 0x03E80003}, {1, 2, 3, 1000}},
    {"4x16_sint", DkVtxAttribSize_4x16, DkVtxAttribType_Sint, false, 2, {0xFFFE0001, 0xFC180003}, {1, -2, 3, -1000}},
    {"4x16_float", DkVtxAttribSize_4x16, DkVtxAttribType_Float, false, 0, {0xC0003E00, 0x3C003400}, {1.5, -2, 0.25, 1}},
    {"3x16_unorm", DkVtxAttribSize_3x16, DkVtxAttribType_Unorm, false, 0, {0x40000000, 0x00008000},
     {0, U16(0x4000), U16(0x8000), 1}},
    {"2x16_float", DkVtxAttribSize_2x16, DkVtxAttribType_Float, false, 0, {0xC0003E00}, {1.5, -2, 0, 1}},
    {"1x16_unorm", DkVtxAttribSize_1x16, DkVtxAttribType_Unorm, false, 0, {0x00008000}, {U16(0x8000), 0, 0, 1}},
    {"4x32_float", DkVtxAttribSize_4x32, DkVtxAttribType_Float, false, 0,
     {0x3FC00000, 0xC0000000, 0x3E800000, 0x3F800000}, {1.5, -2, 0.25, 1}},
    {"4x32_uint", DkVtxAttribSize_4x32, DkVtxAttribType_Uint, false, 1, {1, 2, 3, 100000}, {1, 2, 3, 100000}},
    {"4x32_sint", DkVtxAttribSize_4x32, DkVtxAttribType_Sint, false, 2, {1, 0xFFFFFFFE, 3, 0xFFFE7960},
     {1, -2, 3, -100000}},
    {"3x32_float", DkVtxAttribSize_3x32, DkVtxAttribType_Float, false, 0, {0x3FC00000, 0xC0000000, 0x3E800000},
     {1.5, -2, 0.25, 1}},
    {"2x32_float", DkVtxAttribSize_2x32, DkVtxAttribType_Float, false, 0, {0x3FC00000, 0xC0000000}, {1.5, -2, 0, 1}},
    {"1x32_float", DkVtxAttribSize_1x32, DkVtxAttribType_Float, false, 0, {0x3FC00000}, {1.5, 0, 0, 1}},
    {"1x32_unorm", DkVtxAttribSize_1x32, DkVtxAttribType_Unorm, false, 0, {0x80000000}, {0.5, 0, 0, 1}},
    {"2x32_snorm", DkVtxAttribSize_2x32, DkVtxAttribType_Snorm, false, 0, {0x40000000, 0x80000001}, {0.5, -1, 0, 1}},
    {"4x8_uscaled_bgra", DkVtxAttribSize_4x8, DkVtxAttribType_Uscaled, true, 0, {0x04030201}, {3, 2, 1, 4}},
    {"4x8_uint_bgra", DkVtxAttribSize_4x8, DkVtxAttribType_Uint, true, 1, {0xC8030201}, {3, 2, 1, 200}},
    {"10_10_10_2_unorm", DkVtxAttribSize_10_10_10_2, DkVtxAttribType_Unorm, false, 0, {0x600003FF},
     {1, 0, 512.0 / 1023.0, 1.0 / 3.0}},
    {"10_10_10_2_snorm", DkVtxAttribSize_10_10_10_2, DkVtxAttribType_Snorm, false, 0, {0x400805FF}, {1, -1, 0, 1}},
    {"10_10_10_2_uint", DkVtxAttribSize_10_10_10_2, DkVtxAttribType_Uint, false, 1, {0x80701805}, {5, 6, 7, 2}},
    {"11_11_10_float", DkVtxAttribSize_11_11_10, DkVtxAttribType_Float, false, 0, {0x701F03C0}, {1, 1.5, 0.5, 1}},
};

struct AttributeVertex {
    float pos[3];
    uint32_t words[4];
};

void RunAttribute(Gpu& gpu, const AttributeCase& test) {
    gpu.Begin(DkImageFormat_RGBA32_Float);
    const std::array<DkVtxAttribState, 2> attributes{{
        {0, 0, offsetof(AttributeVertex, pos), DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
        {0, 0, offsetof(AttributeVertex, words), test.size, test.type, test.bgra ? 1u : 0u},
    }};
    const std::array<DkVtxBufferState, 1> buffers{{{sizeof(AttributeVertex), 0}}};
    gpu.cmd.bindVtxAttribState(attributes);
    gpu.cmd.bindVtxBufferState(buffers);
    static constexpr const char* Shaders[] = {"attr_vsh", "attr_uint_vsh", "attr_sint_vsh"};
    gpu.Shaders(Shaders[test.shader], "color_fsh");
    const float uniform[8] = {1, 1, 1, 1, 0, 0, 0, 0};
    gpu.Uniform(DkStage_Vertex, 0, uniform, sizeof(uniform));
    Slice slice = gpu.Alloc(sizeof(AttributeVertex) * 4);
    auto* vertices = static_cast<AttributeVertex*>(slice.cpu);
    constexpr float Corners[4][2] = {{-1, 1}, {-1, -1}, {1, 1}, {1, -1}};
    for (int i = 0; i < 4; ++i) {
        AttributeVertex vertex{{Corners[i][0], Corners[i][1], 0}, {}};
        std::memcpy(vertex.words, test.words, sizeof(vertex.words));
        std::memcpy(&vertices[i], &vertex, sizeof(vertex));
    }
    gpu.cmd.bindVtxBuffer(0, slice.gpu, slice.size);
    gpu.cmd.draw(DkPrimitive_TriangleStrip, 4, 1, 0, 0);
    gpu.Finish();
    gpu.Expect(16, 16, test.expected, 0.002);
}

struct PackedOffset {
    float offset[4];
    float color[4];
};

void BindInstanced(Gpu& gpu, uint32_t divisor) {
    const std::array<DkVtxAttribState, 3> attributes{{
        {0, 0, 0, DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
        {1, 0, offsetof(PackedOffset, offset), DkVtxAttribSize_4x32, DkVtxAttribType_Float, 0},
        {1, 0, offsetof(PackedOffset, color), DkVtxAttribSize_4x32, DkVtxAttribType_Float, 0},
    }};
    const std::array<DkVtxBufferState, 2> buffers{{{sizeof(float) * 3, 0}, {sizeof(PackedOffset), divisor}}};
    gpu.cmd.bindVtxAttribState(attributes);
    gpu.cmd.bindVtxBufferState(buffers);
    gpu.Shaders("instanced_vsh", "color_fsh");
    // A cell a quarter of the target wide at the top left.
    const float positions[4][3] = {{-1, 1, 0}, {-1, 0.5f, 0}, {-0.5f, 1, 0}, {-0.5f, 0.5f, 0}};
    Slice slice = gpu.Alloc(sizeof(positions));
    std::memcpy(slice.cpu, positions, sizeof(positions));
    gpu.cmd.bindVtxBuffer(0, slice.gpu, slice.size);
    PackedOffset instances[4]{};
    for (uint32_t i = 0; i < 4; ++i) {
        instances[i].offset[0] = 0.5f * i;
        instances[i].color[0] = (i + 1) / 4.0f;
        instances[i].color[3] = 1;
    }
    Slice per_instance = gpu.Alloc(sizeof(instances));
    std::memcpy(per_instance.cpu, instances, sizeof(instances));
    gpu.cmd.bindVtxBuffer(1, per_instance.gpu, per_instance.size);
}

} // namespace

void RegisterGeometry(std::vector<SceneEntry>& scenes) {
    scenes.push_back({"prim_triangles", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        const Vertex vertices[6] = {At(gpu, 0, 0),  At(gpu, 0, 32),  At(gpu, 16, 0),
                                    At(gpu, 16, 0), At(gpu, 0, 32), At(gpu, 16, 32)};
        gpu.BindVertices(vertices, 6);
        gpu.cmd.draw(DkPrimitive_Triangles, 6, 1, 0, 0);
        gpu.Finish();
        ExpectLeftHalf(gpu);
    }});
    scenes.push_back({"prim_triangle_strip", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        QuadVertices(gpu);
        gpu.cmd.draw(DkPrimitive_TriangleStrip, 4, 1, 0, 0);
        gpu.Finish();
        ExpectLeftHalf(gpu);
    }});
    scenes.push_back({"prim_triangle_fan", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        const Vertex vertices[4] = {At(gpu, 0, 0), At(gpu, 0, 32), At(gpu, 16, 32), At(gpu, 16, 0)};
        gpu.BindVertices(vertices, 4);
        gpu.cmd.draw(DkPrimitive_TriangleFan, 4, 1, 0, 0);
        gpu.Finish();
        ExpectLeftHalf(gpu);
    }});
    scenes.push_back({"prim_quads", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        const Vertex vertices[8] = {At(gpu, 0, 0),  At(gpu, 0, 16),  At(gpu, 16, 16), At(gpu, 16, 0),
                                    At(gpu, 0, 16), At(gpu, 0, 32), At(gpu, 16, 32), At(gpu, 16, 16)};
        gpu.BindVertices(vertices, 8);
        gpu.cmd.draw(DkPrimitive_Quads, 8, 1, 0, 0);
        gpu.Finish();
        gpu.Expect(8, 8, Green);
        gpu.Expect(8, 24, Green);
        gpu.Expect(24, 16, Black);
    }});
    scenes.push_back({"prim_quads_indexed", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        const Vertex vertices[4] = {At(gpu, 0, 0), At(gpu, 0, 32), At(gpu, 16, 32), At(gpu, 16, 0)};
        gpu.BindVertices(vertices, 4);
        const uint16_t indices[4] = {0, 1, 2, 3};
        BindIndices(gpu, DkIdxFormat_Uint16, indices, sizeof(indices));
        gpu.cmd.drawIndexed(DkPrimitive_Quads, 4, 1, 0, 0, 0);
        gpu.Finish();
        ExpectLeftHalf(gpu);
    }});
    scenes.push_back({"prim_quad_strip", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        const Vertex vertices[6] = {At(gpu, 0, 0),  At(gpu, 0, 32),  At(gpu, 8, 0),
                                    At(gpu, 8, 32), At(gpu, 16, 0), At(gpu, 16, 32)};
        gpu.BindVertices(vertices, 6);
        gpu.cmd.draw(DkPrimitive_QuadStrip, 6, 1, 0, 0);
        gpu.Finish();
        gpu.Expect(4, 16, Green);
        gpu.Expect(12, 16, Green);
        gpu.Expect(24, 16, Black);
    }});
    scenes.push_back({"prim_polygon", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        const Vertex vertices[5] = {At(gpu, 0, 0), At(gpu, 0, 32), At(gpu, 16, 32), At(gpu, 16, 16), At(gpu, 16, 0)};
        gpu.BindVertices(vertices, 5);
        gpu.cmd.draw(DkPrimitive_Polygon, 5, 1, 0, 0);
        gpu.Finish();
        ExpectLeftHalf(gpu);
    }});
    scenes.push_back({"prim_triangle_adjacency", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        // Without a geometry shader the adjacent vertices are dropped.
        const Vertex far = At(gpu, 31, 31, Red);
        const Vertex vertices[6] = {At(gpu, 0, 0), far, At(gpu, 0, 32), far, At(gpu, 16, 0), far};
        gpu.BindVertices(vertices, 6);
        gpu.cmd.draw(DkPrimitive_TrianglesAdjacency, 6, 1, 0, 0);
        gpu.Finish();
        gpu.Expect(4, 8, Green);
        gpu.Expect(24, 24, Black);
    }});
    scenes.push_back({"first_vertex", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        const Vertex vertices[8] = {At(gpu, 16, 0, Red),  At(gpu, 16, 32, Red), At(gpu, 32, 0, Red),
                                    At(gpu, 32, 32, Red), At(gpu, 0, 0),        At(gpu, 0, 32),
                                    At(gpu, 16, 0),       At(gpu, 16, 32)};
        gpu.BindVertices(vertices, 8);
        gpu.cmd.draw(DkPrimitive_TriangleStrip, 4, 1, 4, 0);
        gpu.Finish();
        ExpectLeftHalf(gpu);
    }});

    scenes.push_back({"index_uint8", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        QuadVertices(gpu);
        const uint8_t indices[6] = {0, 1, 2, 2, 1, 3};
        BindIndices(gpu, DkIdxFormat_Uint8, indices, sizeof(indices));
        gpu.cmd.drawIndexed(DkPrimitive_Triangles, 6, 1, 0, 0, 0);
        gpu.Finish();
        ExpectLeftHalf(gpu);
    }});
    scenes.push_back({"index_uint16", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        QuadVertices(gpu);
        const uint16_t indices[6] = {0, 1, 2, 2, 1, 3};
        BindIndices(gpu, DkIdxFormat_Uint16, indices, sizeof(indices));
        gpu.cmd.drawIndexed(DkPrimitive_Triangles, 6, 1, 0, 0, 0);
        gpu.Finish();
        ExpectLeftHalf(gpu);
    }});
    scenes.push_back({"index_uint32", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        QuadVertices(gpu);
        const uint32_t indices[6] = {0, 1, 2, 2, 1, 3};
        BindIndices(gpu, DkIdxFormat_Uint32, indices, sizeof(indices));
        gpu.cmd.drawIndexed(DkPrimitive_Triangles, 6, 1, 0, 0, 0);
        gpu.Finish();
        ExpectLeftHalf(gpu);
    }});
    scenes.push_back({"index_first_index", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        QuadVertices(gpu);
        // Skipped indices would draw over the right half if they were read.
        const uint16_t indices[9] = {0, 0, 0, 0, 1, 2, 2, 1, 3};
        BindIndices(gpu, DkIdxFormat_Uint16, indices, sizeof(indices));
        gpu.cmd.drawIndexed(DkPrimitive_Triangles, 6, 1, 3, 0, 0);
        gpu.Finish();
        ExpectLeftHalf(gpu);
    }});
    scenes.push_back({"index_vertex_offset", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        const Vertex vertices[8] = {At(gpu, 16, 0, Red),  At(gpu, 16, 32, Red), At(gpu, 32, 0, Red),
                                    At(gpu, 32, 32, Red), At(gpu, 0, 0),        At(gpu, 0, 32),
                                    At(gpu, 16, 0),       At(gpu, 16, 32)};
        gpu.BindVertices(vertices, 8);
        const uint16_t indices[6] = {0, 1, 2, 2, 1, 3};
        BindIndices(gpu, DkIdxFormat_Uint16, indices, sizeof(indices));
        gpu.cmd.drawIndexed(DkPrimitive_Triangles, 6, 1, 0, 4, 0);
        gpu.Finish();
        ExpectLeftHalf(gpu);
    }});
    scenes.push_back({"index_restart", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        const Vertex vertices[8] = {At(gpu, 0, 0),   At(gpu, 0, 32),  At(gpu, 8, 0),   At(gpu, 8, 32),
                                    At(gpu, 16, 0), At(gpu, 16, 32), At(gpu, 24, 0), At(gpu, 24, 32)};
        gpu.BindVertices(vertices, 8);
        gpu.cmd.setPrimitiveRestart(true, 0xFFFF);
        const uint16_t indices[9] = {0, 1, 2, 3, 0xFFFF, 4, 5, 6, 7};
        BindIndices(gpu, DkIdxFormat_Uint16, indices, sizeof(indices));
        gpu.cmd.drawIndexed(DkPrimitive_TriangleStrip, 9, 1, 0, 0, 0);
        gpu.Finish();
        gpu.Expect(4, 16, Green);
        gpu.Expect(12, 16, Black); // The gap the restart leaves.
        gpu.Expect(20, 16, Green);
    }});
    // Games pick restart values other than the all-ones D3D12 knows.
    scenes.push_back({"index_restart_custom_value", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        const Vertex vertices[8] = {At(gpu, 0, 0),   At(gpu, 0, 32),  At(gpu, 8, 0),   At(gpu, 8, 32),
                                    At(gpu, 16, 0), At(gpu, 16, 32), At(gpu, 24, 0), At(gpu, 24, 32)};
        gpu.BindVertices(vertices, 8);
        gpu.cmd.setPrimitiveRestart(true, 0x7FFF);
        const uint16_t indices[9] = {0, 1, 2, 3, 0x7FFF, 4, 5, 6, 7};
        BindIndices(gpu, DkIdxFormat_Uint16, indices, sizeof(indices));
        gpu.cmd.drawIndexed(DkPrimitive_TriangleStrip, 9, 1, 0, 0, 0);
        gpu.Finish();
        gpu.Expect(4, 16, Green);
        gpu.Expect(12, 16, Black);
        gpu.Expect(20, 16, Green);
    }});
    scenes.push_back({"index_restart_off_keeps_value", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        const Vertex vertices[4] = {At(gpu, 0, 0), At(gpu, 0, 32), At(gpu, 16, 0), At(gpu, 16, 32)};
        gpu.BindVertices(vertices, 4);
        gpu.cmd.setPrimitiveRestart(false, 0);
        // Index 0 is the restart value of a disabled restart: it must draw as a normal index.
        const uint16_t indices[4] = {0, 1, 2, 3};
        BindIndices(gpu, DkIdxFormat_Uint16, indices, sizeof(indices));
        gpu.cmd.drawIndexed(DkPrimitive_TriangleStrip, 4, 1, 0, 0, 0);
        gpu.Finish();
        ExpectLeftHalf(gpu);
    }});

    scenes.push_back({"instancing_divisor", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        BindInstanced(gpu, 1);
        gpu.cmd.draw(DkPrimitive_TriangleStrip, 4, 4, 0, 0);
        gpu.Finish();
        for (uint32_t i = 0; i < 4; ++i) {
            gpu.Expect(i * 8 + 4, 4, {(i + 1) / 4.0, 0, 0, 1});
        }
        gpu.Expect(4, 12, Black);
    }});
    scenes.push_back({"instancing_divisor_two", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        BindInstanced(gpu, 2);
        gpu.cmd.draw(DkPrimitive_TriangleStrip, 4, 4, 0, 0);
        gpu.Finish();
        gpu.Expect(4, 4, {0.25, 0, 0, 1});
        gpu.Expect(12, 4, {0.5, 0, 0, 1});
        gpu.Expect(20, 4, Black);
    }});
    scenes.push_back({"instancing_first_instance", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        BindInstanced(gpu, 1);
        gpu.cmd.draw(DkPrimitive_TriangleStrip, 4, 2, 0, 2);
        gpu.Finish();
        gpu.Expect(4, 4, Black);
        gpu.Expect(12, 4, Black);
        gpu.Expect(20, 4, {0.75, 0, 0, 1});
        gpu.Expect(28, 4, {1, 0, 0, 1});
    }});
    scenes.push_back({"instance_id", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        gpu.cmd.bindVtxAttribState({});
        gpu.cmd.bindVtxBufferState({});
        gpu.Shaders("ids_vsh", "color_fsh");
        const int mode[4] = {0, 0, 0, 0};
        gpu.Uniform(DkStage_Vertex, 0, mode, sizeof(mode));
        gpu.cmd.draw(DkPrimitive_TriangleStrip, 4, 8, 0, 0);
        gpu.Finish();
        for (uint32_t i = 0; i < 8; ++i) {
            gpu.Expect(i * 4 + 2, 2, {(i + 1) / 8.0, 1.0 / 8.0, 0, 1});
        }
    }});
    scenes.push_back({"vertex_id_indexed", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        gpu.cmd.bindVtxAttribState({});
        gpu.cmd.bindVtxBufferState({});
        gpu.Shaders("ids_vsh", "color_fsh");
        const int mode[4] = {1, 0, 0, 0};
        gpu.Uniform(DkStage_Vertex, 0, mode, sizeof(mode));
        uint16_t indices[6 * 4];
        for (uint16_t quad = 0; quad < 4; ++quad) {
            const uint16_t first = quad * 4;
            const uint16_t order[6] = {first, static_cast<uint16_t>(first + 1), static_cast<uint16_t>(first + 2),
                                       static_cast<uint16_t>(first + 2), static_cast<uint16_t>(first + 1),
                                       static_cast<uint16_t>(first + 3)};
            std::memcpy(&indices[quad * 6], order, sizeof(order));
        }
        BindIndices(gpu, DkIdxFormat_Uint16, indices, sizeof(indices));
        gpu.cmd.drawIndexed(DkPrimitive_Triangles, 24, 1, 0, 0, 0);
        gpu.Finish();
        for (uint32_t quad = 0; quad < 4; ++quad) {
            gpu.Expect(quad * 4 + 2, 2, {1.0 / 8.0, (quad + 1) / 8.0, 0, 1});
        }
    }});
    scenes.push_back({"vertex_id_first_vertex", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        gpu.cmd.bindVtxAttribState({});
        gpu.cmd.bindVtxBufferState({});
        gpu.Shaders("ids_vsh", "color_fsh");
        const int mode[4] = {1, 0, 0, 0};
        gpu.Uniform(DkStage_Vertex, 0, mode, sizeof(mode));
        gpu.cmd.draw(DkPrimitive_TriangleStrip, 4, 1, 8, 0); // Vertex ids 8..11: the third cell.
        gpu.Finish();
        gpu.Expect(10, 2, {1.0 / 8.0, 3.0 / 8.0, 0, 1});
        gpu.Expect(2, 2, Black);
    }});

    scenes.push_back({"indirect_draw", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        QuadVertices(gpu);
        Slice args = gpu.Alloc(sizeof(DkDrawIndirectData), 4);
        const DkDrawIndirectData data{4, 1, 0, 0};
        std::memcpy(args.cpu, &data, sizeof(data));
        gpu.cmd.drawIndirect(DkPrimitive_TriangleStrip, args.gpu);
        gpu.Finish();
        ExpectLeftHalf(gpu);
    }});
    scenes.push_back({"indirect_draw_indexed", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        QuadVertices(gpu);
        const uint16_t indices[6] = {0, 1, 2, 2, 1, 3};
        BindIndices(gpu, DkIdxFormat_Uint16, indices, sizeof(indices));
        Slice args = gpu.Alloc(sizeof(DkDrawIndexedIndirectData), 4);
        const DkDrawIndexedIndirectData data{6, 1, 0, 0, 0};
        std::memcpy(args.cpu, &data, sizeof(data));
        gpu.cmd.drawIndexedIndirect(DkPrimitive_Triangles, args.gpu);
        gpu.Finish();
        ExpectLeftHalf(gpu);
    }});
    // Arguments the GPU wrote a moment ago, as games that cull in compute do.
    scenes.push_back({"indirect_draw_gpu_written", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        QuadVertices(gpu);
        Slice args = gpu.Alloc(sizeof(DkDrawIndirectData), 4);
        const DkDrawIndirectData wrong{0, 0, 0, 0};
        std::memcpy(args.cpu, &wrong, sizeof(wrong));
        const DkDrawIndirectData right{4, 1, 0, 0};
        gpu.cmd.pushData(args.gpu, &right, sizeof(right));
        gpu.cmd.barrier(DkBarrier_Full, DkInvalidateFlags_L2Cache);
        gpu.cmd.drawIndirect(DkPrimitive_TriangleStrip, args.gpu);
        gpu.Finish();
        ExpectLeftHalf(gpu);
    }});

    for (const AttributeCase& test : Attributes) {
        scenes.push_back({Named("attrib_", test.name), [&test](Gpu& gpu) { RunAttribute(gpu, test); }});
    }
    scenes.push_back({"attrib_stride_and_offset", [](Gpu& gpu) {
        struct Padded {
            uint32_t pad0;
            float pos[3];
            uint32_t pad1[3];
            uint8_t color[4];
            uint32_t pad2;
        };
        gpu.Begin();
        gpu.ClearColor(Black);
        const std::array<DkVtxAttribState, 2> attributes{{
            {0, 0, offsetof(Padded, pos), DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
            {0, 0, offsetof(Padded, color), DkVtxAttribSize_4x8, DkVtxAttribType_Unorm, 0},
        }};
        const std::array<DkVtxBufferState, 1> buffers{{{sizeof(Padded), 0}}};
        gpu.cmd.bindVtxAttribState(attributes);
        gpu.cmd.bindVtxBufferState(buffers);
        Slice slice = gpu.Alloc(sizeof(Padded) * 4);
        const float corners[4][2] = {{-1, 1}, {-1, -1}, {0, 1}, {0, -1}};
        for (int i = 0; i < 4; ++i) {
            Padded vertex{0xFFFFFFFF, {corners[i][0], corners[i][1], 0}, {1, 2, 3}, {0, 255, 0, 255}, 0xFFFFFFFF};
            std::memcpy(static_cast<Padded*>(slice.cpu) + i, &vertex, sizeof(vertex));
        }
        gpu.cmd.bindVtxBuffer(0, slice.gpu, slice.size);
        gpu.cmd.draw(DkPrimitive_TriangleStrip, 4, 1, 0, 0);
        gpu.Finish();
        ExpectLeftHalf(gpu);
    }});
    scenes.push_back({"attrib_fixed_value", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        // A fixed attribute reads as zero instead of reading a buffer: alpha 0, color 0.
        const std::array<DkVtxAttribState, 3> attributes{{
            {0, 0, offsetof(Vertex, pos), DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
            {0, 1, 0, DkVtxAttribSize_4x32, DkVtxAttribType_Float, 0},
            {0, 0, offsetof(Vertex, uv), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
        }};
        gpu.cmd.bindVtxAttribState(attributes);
        const Vertex vertices[4] = {At(gpu, 0, 0, Red), At(gpu, 0, 32, Red), At(gpu, 16, 0, Red), At(gpu, 16, 32, Red)};
        gpu.BindVertices(vertices, 4);
        gpu.cmd.draw(DkPrimitive_TriangleStrip, 4, 1, 0, 0);
        gpu.Finish();
        gpu.Check("red channel", gpu.Read(8, 16).r, 0.0, 0.01);
    }});
    scenes.push_back({"vertex_buffer_cpu_overwrite", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        Slice slice = gpu.Alloc(sizeof(Vertex) * 4);
        const Vertex red[4] = {At(gpu, 0, 0, Red), At(gpu, 0, 32, Red), At(gpu, 16, 0, Red), At(gpu, 16, 32, Red)};
        std::memcpy(slice.cpu, red, sizeof(red));
        gpu.cmd.bindVtxBuffer(0, slice.gpu, slice.size);
        gpu.cmd.draw(DkPrimitive_TriangleStrip, 4, 1, 0, 0);
        gpu.Submit();
        // The same address with new contents: the buffer cache must see the write.
        const Vertex green[4] = {At(gpu, 16, 0), At(gpu, 16, 32), At(gpu, 32, 0), At(gpu, 32, 32)};
        std::memcpy(slice.cpu, green, sizeof(green));
        gpu.cmd.bindVtxBuffer(0, slice.gpu, slice.size);
        gpu.cmd.draw(DkPrimitive_TriangleStrip, 4, 1, 0, 0);
        gpu.Finish();
        gpu.Expect(8, 16, Red);
        gpu.Expect(24, 16, Green);
    }});
    scenes.push_back({"index_buffer_cpu_overwrite", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        const Vertex vertices[8] = {At(gpu, 0, 0),  At(gpu, 0, 32),  At(gpu, 16, 0),  At(gpu, 16, 32),
                                    At(gpu, 16, 0), At(gpu, 16, 32), At(gpu, 32, 0), At(gpu, 32, 32)};
        gpu.BindVertices(vertices, 8);
        Slice slice = gpu.Alloc(12, 4);
        const uint16_t left[6] = {0, 1, 2, 2, 1, 3};
        std::memcpy(slice.cpu, left, sizeof(left));
        gpu.cmd.bindIdxBuffer(DkIdxFormat_Uint16, slice.gpu);
        gpu.cmd.drawIndexed(DkPrimitive_Triangles, 6, 1, 0, 0, 0);
        gpu.Finish();
        ExpectLeftHalf(gpu);
        const uint16_t right[6] = {4, 5, 6, 6, 5, 7};
        std::memcpy(slice.cpu, right, sizeof(right));
        gpu.cmd.drawIndexed(DkPrimitive_Triangles, 6, 1, 0, 0, 0);
        gpu.Finish();
        gpu.Expect(24, 16, Green);
    }});

    scenes.push_back({"tess_triangles", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        gpu.cmd.bindShaders(DkStageFlag_GraphicsMask, {gpu.Shader("basic_vsh"), gpu.Shader("tess_tcsh"),
                                                       gpu.Shader("tess_tesh"), gpu.Shader("color_fsh")});
        gpu.cmd.setPatchSize(3);
        const Vertex vertices[3] = {At(gpu, 0, 0, Red), At(gpu, 0, 32, Green), At(gpu, 32, 0, Blue)};
        gpu.BindVertices(vertices, 3);
        gpu.cmd.draw(DkPrimitive_Patches, 3, 1, 0, 0);
        gpu.Finish();
        // Color interpolated over the patch; a little off where tessellation splits it.
        gpu.Expect(2, 2, {1, 0, 0, 1}, 0.25);
        gpu.Expect(2, 28, {0, 1, 0, 1}, 0.25);
        gpu.Expect(28, 2, {0, 0, 1, 1}, 0.25);
        gpu.Expect(28, 28, Black);
    }});
    // No control shader: the levels come from fixed state, which D3D12 has no equivalent of.
    scenes.push_back({"tess_quads_fixed_levels", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        gpu.cmd.bindShaders(DkStageFlag_GraphicsMask,
                            {gpu.Shader("pos_only_vsh"), gpu.Shader("tess_quads_tesh"), gpu.Shader("color_fsh")});
        gpu.cmd.setPatchSize(4);
        const float outer[4] = {4, 4, 4, 4};
        const float inner[2] = {4, 4};
        gpu.cmd.setTessOuterLevels(outer[0], outer[1], outer[2], outer[3]);
        gpu.cmd.setTessInnerLevels(inner[0], inner[1]);
        const Vertex vertices[4] = {At(gpu, 0, 0), At(gpu, 0, 32), At(gpu, 32, 0), At(gpu, 32, 32)};
        gpu.BindVertices(vertices, 4);
        gpu.cmd.draw(DkPrimitive_Patches, 4, 1, 0, 0);
        gpu.Finish();
        gpu.Expect(8, 16, {0.25, 0.5, 0, 1}, 0.05);
        gpu.Expect(24, 8, {0.75, 0.25, 0, 1}, 0.05);
    }});
    // The same patch, its corners from the captured vertices, from 16-bit indices: out of order, after a base vertex, from an odd first
    // index (not word-aligned) whose skipped index is larger than any the draw reads.
    scenes.push_back({"tess_quads_indexed_u16", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        gpu.cmd.bindShaders(DkStageFlag_GraphicsMask,
                            {gpu.Shader("pos_only_vsh"), gpu.Shader("tess_quads_pos_tesh"), gpu.Shader("color_fsh")});
        gpu.cmd.setPatchSize(4);
        gpu.cmd.setTessOuterLevels(4, 4, 4, 4);
        gpu.cmd.setTessInnerLevels(4, 4);
        const Vertex vertices[6] = {At(gpu, 16, 16, Red), At(gpu, 16, 16, Red), At(gpu, 32, 32),
                                    At(gpu, 0, 0),         At(gpu, 32, 0),         At(gpu, 0, 32)};
        gpu.BindVertices(vertices, 6);
        const uint16_t indices[6] = {9, 1, 3, 2, 0, 9};
        BindIndices(gpu, DkIdxFormat_Uint16, indices, sizeof(indices));
        gpu.cmd.drawIndexed(DkPrimitive_Patches, 4, 1, 1, 2, 0);
        gpu.Finish();
        gpu.Expect(8, 16, {0.25, 0.5, 0, 1}, 0.05);
        gpu.Expect(24, 8, {0.75, 0.25, 0, 1}, 0.05);
    }});
    // Two patches from 32-bit indices sharing vertices, drawn twice as instances.
    scenes.push_back({"tess_quads_indexed_u32_instanced", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        gpu.cmd.bindShaders(DkStageFlag_GraphicsMask,
                            {gpu.Shader("pos_only_vsh"), gpu.Shader("tess_quads_pos_tesh"), gpu.Shader("color_fsh")});
        gpu.cmd.setPatchSize(4);
        gpu.cmd.setTessOuterLevels(4, 4, 4, 4);
        gpu.cmd.setTessInnerLevels(4, 4);
        // Left half (x 0..16) and right half (x 16..32), sharing the middle edge.
        const Vertex vertices[6] = {At(gpu, 0, 0),  At(gpu, 0, 32),  At(gpu, 16, 0),
                                    At(gpu, 16, 32), At(gpu, 32, 0), At(gpu, 32, 32)};
        gpu.BindVertices(vertices, 6);
        const uint32_t indices[8] = {0, 1, 2, 3, 2, 3, 4, 5};
        BindIndices(gpu, DkIdxFormat_Uint32, indices, sizeof(indices));
        gpu.cmd.drawIndexed(DkPrimitive_Patches, 8, 2, 0, 0, 0);
        gpu.Finish();
        // Each patch spans 16 pixels in x: u runs from 0 to 1 within its half.
        gpu.Expect(4, 16, {0.25, 0.5, 0, 1}, 0.05);
        gpu.Expect(20, 16, {0.25, 0.5, 0, 1}, 0.05);
        gpu.Expect(12, 8, {0.75, 0.25, 0, 1}, 0.05);
        gpu.Expect(28, 8, {0.75, 0.25, 0, 1}, 0.05);
    }});
    scenes.push_back({"geometry_point_to_quad", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        gpu.cmd.bindShaders(DkStageFlag_GraphicsMask,
                            {gpu.Shader("basic_vsh"), gpu.Shader("quad_gsh"), gpu.Shader("color_fsh")});
        const Vertex point = At(gpu, 16, 16);
        gpu.BindVertices(&point, 1);
        gpu.cmd.draw(DkPrimitive_Points, 1, 1, 0, 0);
        gpu.Finish();
        gpu.Expect(16, 16, Green);
        gpu.Expect(13, 13, Green);
        gpu.Expect(10, 16, Black);
        gpu.Expect(22, 16, Black);
    }});
}
