// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Runs the scenes one after another and tells the host each step in sdmc:/renderer_suite/results.txt.
// The host finds there which scene a crash or a hang stopped at, adds it to skip.txt and runs the rest.
// A non-empty only.txt runs just the scenes it names.
// Without mode.txt saying otherwise it runs the scenes; "perf <frames>" runs the performance loop and
// "tess <frames> <draws> <patches> <level> <flat>" the tessellation one (perf_tess.cpp).

#include "harness.h"

#include <set>
#include <sys/stat.h>

namespace {

std::set<std::string> ReadNames(const char* path) {
    std::set<std::string> names;
    if (FILE* file = fopen(path, "r")) {
        char line[128];
        while (fgets(line, sizeof(line), file)) {
            std::string name{line};
            while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) name.pop_back();
            if (!name.empty()) names.insert(name);
        }
        fclose(file);
    }
    return names;
}

void RunList(const std::vector<SceneEntry>& scenes, const std::set<std::string>& skip,
             const std::set<std::string>& only, uint32_t device_flags) {
    Gpu gpu{device_flags};
    for (const SceneEntry& entry : scenes) {
        if (skip.count(entry.name) || (!only.empty() && !only.count(entry.name))) {
            Report("SKIP %s", entry.name.c_str());
            continue;
        }
        gpu.BeginScene(entry.name.c_str());
        entry.run(gpu);
        gpu.EndScene();
    }
}

struct Mode {
    uint32_t frames = 0;
    bool tess = false;
    TessPerformance tess_config;
};

Mode ReadMode() {
    Mode mode;
    if (FILE* file = fopen("sdmc:/renderer_suite/mode.txt", "r")) {
        char line[128] = {};
        if (fgets(line, sizeof(line), file)) {
            unsigned frames = 0, draws = 0, patches = 0, level = 0, flat = 0;
            if (sscanf(line, "tess %u %u %u %u %u", &frames, &draws, &patches, &level, &flat) == 5) {
                mode = {frames, true, {draws, patches, level, flat != 0}};
            } else if (sscanf(line, "perf %u", &frames) == 1) {
                mode.frames = frames;
            }
        }
        fclose(file);
    }
    return mode;
}

} // namespace

int main(int, char**) {
    mkdir("sdmc:/renderer_suite", 0777);
    // The boot's liveness check: the guest is running.
    static const char alive[] = "JADYX_JIT_ALIVE";
    svcOutputDebugString(alive, sizeof(alive) - 1);

    if (const Mode mode = ReadMode(); mode.frames && mode.tess) {
        RunTessPerformanceLoop(mode.frames, mode.tess_config);
    } else if (mode.frames) {
        RunPerformanceLoop(mode.frames);
    } else {
        const std::set<std::string> skip = ReadNames("sdmc:/renderer_suite/skip.txt");
        const std::set<std::string> only = ReadNames("sdmc:/renderer_suite/only.txt");
        std::vector<SceneEntry> scenes;
        RegisterClears(scenes);
        RegisterBlend(scenes);
        RegisterDepthStencil(scenes);
        RegisterRaster(scenes);
        RegisterGeometry(scenes);
        RegisterTextures(scenes);
        RegisterShaders(scenes);
        RegisterTransfers(scenes);
        RunList(scenes, skip, only, DkDeviceFlags_DepthZeroToOne | DkDeviceFlags_OriginUpperLeft);

        std::vector<SceneEntry> gl_scenes;
        RegisterGlConventions(gl_scenes);
        RunList(gl_scenes, skip, only, DkDeviceFlags_DepthMinusOneToOne | DkDeviceFlags_OriginLowerLeft);
    }

    Report("ALL_DONE");
    static const char done[] = "JADYX_GFX_DONE";
    svcOutputDebugString(done, sizeof(done) - 1);
    return 0;
}
