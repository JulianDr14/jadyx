// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The rasterizer's fixed-function switches: culling, fill modes, viewports, scissors, clipping,
// points and lines.

#include "harness.h"
#include "scene_util.h"

namespace {

void DrawVertices(Gpu& gpu, DkPrimitive primitive, const Vertex* vertices, uint32_t count) {
    gpu.BindVertices(vertices, count);
    gpu.cmd.draw(primitive, count, 1, 0, 0);
}

// In the upper-left conventions Rect winds counterclockwise on screen, and swapping the sides reverses it.
void CullCase(Gpu& gpu, DkFace cull, DkFrontFace front, bool ccw_shows, bool cw_shows) {
    gpu.Begin();
    gpu.ClearColor(Black);
    dk::RasterizerState rasterizer;
    rasterizer.setCullMode(cull).setFrontFace(front);
    gpu.cmd.bindRasterizerState(rasterizer);
    gpu.Rect(0, 0, 16, 32, Green);
    gpu.Rect(32, 0, 16, 32, Green);
    gpu.Finish();
    gpu.Expect(8, 16, ccw_shows ? Green : Black);
    gpu.Expect(24, 16, cw_shows ? Green : Black);
}

} // namespace

void RegisterRaster(std::vector<SceneEntry>& scenes) {
    scenes.push_back({"cull_none", [](Gpu& gpu) { CullCase(gpu, DkFace_None, DkFrontFace_CCW, true, true); }});
    scenes.push_back({"cull_back_ccw_front", [](Gpu& gpu) { CullCase(gpu, DkFace_Back, DkFrontFace_CCW, true, false); }});
    scenes.push_back({"cull_front_ccw_front", [](Gpu& gpu) { CullCase(gpu, DkFace_Front, DkFrontFace_CCW, false, true); }});
    scenes.push_back({"cull_back_cw_front", [](Gpu& gpu) { CullCase(gpu, DkFace_Back, DkFrontFace_CW, false, true); }});
    scenes.push_back({"cull_front_cw_front", [](Gpu& gpu) { CullCase(gpu, DkFace_Front, DkFrontFace_CW, true, false); }});
    scenes.push_back({"cull_both", [](Gpu& gpu) { CullCase(gpu, DkFace_FrontAndBack, DkFrontFace_CCW, false, false); }});
    scenes.push_back({"front_facing_value", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        gpu.Shaders("basic_vsh", "front_facing_fsh");
        gpu.Rect(0, 0, 16, 32, White);
        gpu.Rect(32, 0, 16, 32, White);
        gpu.Finish();
        gpu.Expect(8, 16, Green);
        gpu.Expect(24, 16, Red);
    }});
    scenes.push_back({"front_facing_value_cw", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        dk::RasterizerState rasterizer;
        rasterizer.setCullMode(DkFace_None).setFrontFace(DkFrontFace_CW);
        gpu.cmd.bindRasterizerState(rasterizer);
        gpu.Shaders("basic_vsh", "front_facing_fsh");
        gpu.Rect(0, 0, 16, 32, White);
        gpu.Rect(32, 0, 16, 32, White);
        gpu.Finish();
        gpu.Expect(8, 16, Red);
        gpu.Expect(24, 16, Green);
    }});
    scenes.push_back({"rasterizer_discard", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        dk::RasterizerState rasterizer;
        rasterizer.setCullMode(DkFace_None).setRasterizerEnable(false);
        gpu.cmd.bindRasterizerState(rasterizer);
        gpu.Full(Red);
        gpu.Finish();
        gpu.Expect(16, 16, Black);
    }});
    scenes.push_back({"polygon_mode_line", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        dk::RasterizerState rasterizer;
        rasterizer.setCullMode(DkFace_None).setPolygonMode(DkPolygonMode_Line);
        gpu.cmd.bindRasterizerState(rasterizer);
        // Half a pixel in: an edge on a pixel boundary may land on either column.
        const float l = gpu.NdcX(4.5f), r = gpu.NdcX(28), t = gpu.NdcY(4), b = gpu.NdcY(28);
        const Vertex triangle[3] = {
            {{l, t, 0}, {0, 1, 0, 1}, {0, 0}}, {{l, b, 0}, {0, 1, 0, 1}, {0, 0}}, {{r, b, 0}, {0, 1, 0, 1}, {0, 0}}};
        DrawVertices(gpu, DkPrimitive_Triangles, triangle, 3);
        gpu.Finish();
        gpu.Expect(4, 16, Green, 0.5);   // On the left edge.
        gpu.Expect(10, 20, Black);       // Inside: nothing fills it.
    }});
    scenes.push_back({"polygon_mode_point", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        dk::RasterizerState rasterizer;
        rasterizer.setCullMode(DkFace_None).setPolygonMode(DkPolygonMode_Point);
        gpu.cmd.bindRasterizerState(rasterizer);
        gpu.cmd.setPointSize(4.0f);
        gpu.Rect(8, 8, 24, 24, Green);
        gpu.Finish();
        gpu.Expect(8, 8, Green, 0.5);
        gpu.Expect(16, 16, Black);
    }});
    scenes.push_back({"fill_rectangle", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        dk::RasterizerState rasterizer;
        rasterizer.setCullMode(DkFace_None).setFillRectangleEnable(true);
        gpu.cmd.bindRasterizerState(rasterizer);
        const float l = gpu.NdcX(4), r = gpu.NdcX(28), t = gpu.NdcY(4), b = gpu.NdcY(28);
        const Vertex triangle[3] = {
            {{l, t, 0}, {0, 1, 0, 1}, {0, 0}}, {{l, b, 0}, {0, 1, 0, 1}, {0, 0}}, {{r, b, 0}, {0, 1, 0, 1}, {0, 0}}};
        DrawVertices(gpu, DkPrimitive_Triangles, triangle, 3);
        gpu.Finish();
        // The corner the triangle does not reach is filled too.
        gpu.Expect(26, 6, Green);
        gpu.Expect(2, 2, Black);
    }});
    scenes.push_back({"scissor", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        gpu.cmd.setScissors(0, {{8, 4, 16, 8}});
        gpu.Full(Green);
        gpu.Finish();
        gpu.Expect(8, 4, Green);
        gpu.Expect(23, 11, Green);
        gpu.Expect(7, 8, Black);
        gpu.Expect(24, 8, Black);
        gpu.Expect(12, 3, Black);
        gpu.Expect(12, 12, Black);
    }});
    scenes.push_back({"scissor_changes_between_draws", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        for (uint32_t i = 0; i < 4; ++i) {
            gpu.cmd.setScissors(0, {{i * 8, 0, 8, 32}});
            gpu.Full({(i + 1) / 4.0, 0, 0, 1});
        }
        gpu.Finish();
        for (uint32_t i = 0; i < 4; ++i) {
            gpu.Expect(i * 8 + 4, 16, {(i + 1) / 4.0, 0, 0, 1});
        }
    }});
    scenes.push_back({"viewport_offset", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        gpu.cmd.setViewports(0, {{16.0f, 8.0f, 16.0f, 8.0f, 0.0f, 1.0f}});
        gpu.Full(Green);
        gpu.Finish();
        gpu.Expect(24, 12, Green);
        gpu.Expect(8, 12, Black);
        gpu.Expect(24, 20, Black);
    }});
    scenes.push_back({"viewport_scales_rect", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        gpu.cmd.setViewports(0, {{0.0f, 0.0f, 16.0f, 16.0f, 0.0f, 1.0f}});
        // The upper-left quarter of the viewport is the upper-left sixteenth of the target.
        gpu.Rect(0, 0, 16, 16, Green);
        gpu.Finish();
        gpu.Expect(4, 4, Green);
        gpu.Expect(12, 12, Black);
    }});
    scenes.push_back({"viewport_flipped_height", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        gpu.cmd.setViewports(0, {{0.0f, 32.0f, 32.0f, -32.0f, 0.0f, 1.0f}});
        gpu.Rect(0, 0, 32, 8, Green);
        gpu.Finish();
        gpu.Expect(16, 28, Green);
        gpu.Expect(16, 4, Black);
    }});
    scenes.push_back({"viewport_swizzle_negative_y", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        const DkViewportSwizzle swizzle{DkSwizzle_PositiveX, DkSwizzle_NegativeY, DkSwizzle_PositiveZ, DkSwizzle_PositiveW};
        gpu.cmd.setViewportSwizzles(0, swizzle);
        gpu.Rect(0, 0, 32, 8, Green);
        gpu.Finish();
        gpu.Expect(16, 28, Green);
        gpu.Expect(16, 4, Black);
    }});
    scenes.push_back({"viewport_swizzle_xy", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        const DkViewportSwizzle swizzle{DkSwizzle_PositiveY, DkSwizzle_PositiveX, DkSwizzle_PositiveZ, DkSwizzle_PositiveW};
        gpu.cmd.setViewportSwizzles(0, swizzle);
        gpu.Rect(0, 0, 8, 32, Green); // The left column becomes the top row.
        gpu.Finish();
        gpu.Expect(16, 28, Green);
        gpu.Expect(16, 4, Black);
    }});
    scenes.push_back({"viewport_index_from_geometry", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        gpu.cmd.setViewports(0, {{0.0f, 0.0f, 16.0f, 32.0f, 0.0f, 1.0f}, {16.0f, 0.0f, 16.0f, 32.0f, 0.0f, 1.0f}});
        gpu.cmd.setScissors(0, {{0, 0, 16, 32}, {16, 0, 16, 32}});
        gpu.cmd.bindShaders(DkStageFlag_GraphicsMask,
                            {gpu.Shader("basic_vsh"), gpu.Shader("viewport_gsh"), gpu.Shader("color_fsh")});
        gpu.Rect(0, 0, 16, 32, Green); // The left half of each viewport.
        gpu.Finish();
        gpu.Expect(4, 16, Green);
        gpu.Expect(12, 16, Black);
        gpu.Expect(20, 16, Green);
        gpu.Expect(28, 16, Black);
    }});
    scenes.push_back({"clip_distance", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        gpu.Shaders("clip_vsh", "color_fsh");
        const float t = gpu.NdcY(0), b = gpu.NdcY(32);
        const Vertex vertices[4] = {
            {{-1, t, 0}, {0, 1, 0, 1}, {-1, 0}},
            {{-1, b, 0}, {0, 1, 0, 1}, {-1, 0}},
            {{1, t, 0}, {0, 1, 0, 1}, {1, 0}},
            {{1, b, 0}, {0, 1, 0, 1}, {1, 0}},
        };
        // The distance enable is state dk has no switch for: every distance the shader writes is on.
        DrawVertices(gpu, DkPrimitive_TriangleStrip, vertices, 4);
        gpu.Finish();
        gpu.Expect(8, 16, Black);
        gpu.Expect(24, 16, Green);
    }});
    scenes.push_back({"clip_near_and_far", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        gpu.Rect(0, 0, 10, 32, Red, -0.5f); // In front of the near plane of the 0..1 range.
        gpu.Rect(10, 0, 20, 32, Green, 0.5f);
        gpu.Rect(20, 0, 32, 32, Red, 1.5f); // Behind the far plane.
        gpu.Finish();
        gpu.Expect(5, 16, Black);
        gpu.Expect(15, 16, Green);
        gpu.Expect(25, 16, Black);
    }});
    scenes.push_back({"guard_band_big_triangle", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        const Vertex triangle[3] = {
            {{-1, 1, 0}, {0, 1, 0, 1}, {0, 0}}, {{-1, -3, 0}, {0, 1, 0, 1}, {0, 0}}, {{3, 1, 0}, {0, 1, 0, 1}, {0, 0}}};
        DrawVertices(gpu, DkPrimitive_Triangles, triangle, 3);
        gpu.Finish();
        gpu.Expect(1, 1, Green);
        gpu.Expect(30, 30, Green);
    }});
    scenes.push_back({"frag_coord", [](Gpu& gpu) {
        gpu.Begin(DkImageFormat_RGBA16_Unorm);
        gpu.Shaders("basic_vsh", "frag_coord_fsh");
        gpu.Full(White, 0.5f);
        gpu.Finish();
        gpu.Expect(4, 20, {4.5 / 32.0, 20.5 / 32.0, 0.5, 1.0}, 0.002);
    }});
    scenes.push_back({"points_fixed_size", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        gpu.cmd.setPointSize(6.0f);
        const Vertex point{{gpu.NdcX(16), gpu.NdcY(16), 0}, {0, 1, 0, 1}, {0, 0}};
        DrawVertices(gpu, DkPrimitive_Points, &point, 1);
        gpu.Finish();
        gpu.Expect(14, 14, Green);
        gpu.Expect(17, 17, Green);
        gpu.Expect(11, 16, Black);
        gpu.Expect(20, 16, Black);
    }});
    scenes.push_back({"points_size_from_shader", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        gpu.Shaders("point_vsh", "color_fsh");
        const Vertex point{{gpu.NdcX(16), gpu.NdcY(16), 0}, {0, 1, 0, 1}, {0, 0}};
        DrawVertices(gpu, DkPrimitive_Points, &point, 1);
        gpu.Finish();
        gpu.Expect(15, 15, Green);
        gpu.Expect(16, 16, Green);
        gpu.Expect(12, 16, Black);
        gpu.Expect(19, 16, Black);
    }});
    scenes.push_back({"points_coord", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        gpu.cmd.setPointSize(16.0f);
        gpu.Shaders("basic_vsh", "point_coord_fsh");
        const Vertex point{{gpu.NdcX(16), gpu.NdcY(16), 0}, {0, 1, 0, 1}, {0, 0}};
        DrawVertices(gpu, DkPrimitive_Points, &point, 1);
        gpu.Finish();
        // Pixel 8 is the first of the point, so coordinates grow from 0.5/16.
        gpu.Expect(8, 8, {0.5 / 16.0, 0.5 / 16.0, 0.0, 1.0}, 0.02);
        gpu.Expect(23, 23, {15.5 / 16.0, 15.5 / 16.0, 0.0, 1.0}, 0.02);
    }});
    scenes.push_back({"lines", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        const Vertex line[2] = {{{-1, gpu.NdcY(16.5f), 0}, {0, 1, 0, 1}, {0, 0}},
                                {{1, gpu.NdcY(16.5f), 0}, {0, 1, 0, 1}, {0, 0}}};
        DrawVertices(gpu, DkPrimitive_Lines, line, 2);
        gpu.Finish();
        gpu.Expect(16, 16, Green);
        gpu.Expect(16, 12, Black);
        gpu.Expect(16, 20, Black);
    }});
    scenes.push_back({"line_strip_and_loop", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        const float l = gpu.NdcX(8.5f), r = gpu.NdcX(24.5f), t = gpu.NdcY(8.5f), b = gpu.NdcY(24.5f);
        const Vertex path[3] = {{{l, t, 0}, {0, 1, 0, 1}, {0, 0}},
                                {{r, t, 0}, {0, 1, 0, 1}, {0, 0}},
                                {{r, b, 0}, {0, 1, 0, 1}, {0, 0}}};
        DrawVertices(gpu, DkPrimitive_LineStrip, path, 3);
        gpu.Finish();
        gpu.Expect(16, 8, Green);
        gpu.Expect(24, 16, Green);
        gpu.Expect(16, 16, Black);
        gpu.Begin();
        gpu.ClearColor(Black);
        const Vertex loop[4] = {{{l, t, 0}, {0, 1, 0, 1}, {0, 0}},
                                {{r, t, 0}, {0, 1, 0, 1}, {0, 0}},
                                {{r, b, 0}, {0, 1, 0, 1}, {0, 0}},
                                {{l, b, 0}, {0, 1, 0, 1}, {0, 0}}};
        DrawVertices(gpu, DkPrimitive_LineLoop, loop, 4);
        gpu.Finish();
        gpu.Expect(8, 16, Green); // The closing segment.
        gpu.Expect(16, 8, Green);
        gpu.Expect(16, 16, Black);
    }});
    scenes.push_back({"line_width", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        gpu.cmd.setLineWidth(5.0f);
        const Vertex line[2] = {{{-1, gpu.NdcY(16.5f), 0}, {0, 1, 0, 1}, {0, 0}},
                                {{1, gpu.NdcY(16.5f), 0}, {0, 1, 0, 1}, {0, 0}}};
        DrawVertices(gpu, DkPrimitive_Lines, line, 2);
        gpu.Finish();
        gpu.Expect(16, 14, Green);
        gpu.Expect(16, 18, Green);
        gpu.Expect(16, 11, Black);
        gpu.Expect(16, 21, Black);
    }});
    scenes.push_back({"line_stipple", [](Gpu& gpu) {
        gpu.Begin();
        gpu.ClearColor(Black);
        gpu.cmd.setLineStipple(true, 4, 0x5555);
        const Vertex line[2] = {{{-1, gpu.NdcY(16.5f), 0}, {0, 1, 0, 1}, {0, 0}},
                                {{1, gpu.NdcY(16.5f), 0}, {0, 1, 0, 1}, {0, 0}}};
        DrawVertices(gpu, DkPrimitive_Lines, line, 2);
        // deko3d checks the factor even when the stipple is off.
        gpu.cmd.setLineStipple(false, 1, 0xffff);
        gpu.Finish();
        // Runs of 4 on, 4 off from the start of the line.
        gpu.Expect(1, 16, Green);
        gpu.Expect(5, 16, Black);
        gpu.Expect(9, 16, Green);
        gpu.Expect(13, 16, Black);
    }});
    scenes.push_back({"polygon_stipple", [](Gpu& gpu) {
        uint32_t pattern[32];
        for (uint32_t i = 0; i < 32; ++i) {
            pattern[i] = (i & 1) ? 0xFFFFFFFFu : 0;
        }
        gpu.Begin();
        gpu.ClearColor(Black);
        // This deko3d loads a pattern but only ever writes the enable register to turn it off:
        // record that immediate word and flip its value. 112 is where the library keeps its write
        // pointer (see dkCmdBufSetPolygonStipple).
        gpu.cmd.setPolygonStipple(pattern);
        gpu.cmd.setPolygonStipple(nullptr);
        DkCmdBuf raw = gpu.cmd;
        uint32_t* const written = *reinterpret_cast<uint32_t**>(reinterpret_cast<char*>(raw) + 112);
        written[-1] = 0x800105A3;
        dk::RasterizerState rasterizer;
        rasterizer.setCullMode(DkFace_None);
        gpu.cmd.bindRasterizerState(rasterizer);
        gpu.Full(Green);
        gpu.cmd.setPolygonStipple(nullptr);
        gpu.Finish();
        // Which rows are on depends on the origin; that they alternate does not.
        const bool first = gpu.Read(16, 8).g > 0.5;
        gpu.Check("row after", gpu.Read(16, 9).g, first ? 0.0 : 1.0, 0.01);
        gpu.Check("row two after", gpu.Read(16, 10).g, first ? 1.0 : 0.0, 0.01);
    }});
}
