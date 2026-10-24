# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile the production shared-buffer geometry and publisher without a guest/kernel."""

import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]


class SharedBufferStrideTest(unittest.TestCase):
    def test_boot_switch_is_explicit_and_session_only(self):
        settings = (ROOT / "src/common/settings.h").read_text()
        boot = (ROOT / "src/jadyx/app/uwp_boot.cpp").read_text()
        self.assertIn("bool shared_buffer_stride_fix{};", settings)
        self.assertIn("bool shared_buffer_stride_fix{};", boot)
        self.assertIn('line == "shared_buffer_stride_fix=1"', boot)
        self.assertIn('line == "shared_buffer_stride_fix=0"', boot)
        self.assertIn(
            "Settings::values.shared_buffer_stride_fix = config.shared_buffer_stride_fix;",
            boot,
        )
        self.assertIn("Settings::values.shared_buffer_stride_fix = stride_fix;", boot)

    def test_published_geometry(self):
        source = (
            ROOT / "src/core/hle/service/vi/shared_buffer_manager.cpp"
        ).read_text()
        geometry = source[
            source.index("constexpr auto SharedBufferBlockLinearFormat") : source.index(
                "constexpr SharedMemoryPoolLayout"
            )
        ]
        publisher = source[
            source.index("void MakeGraphicBuffer(") : source.index("\n} // namespace")
        ]
        harness = r"""
#include <cassert>
#include <cstdint>
#include <memory>
using u32 = std::uint32_t;
#define ASSERT assert
namespace Settings {
struct Values { bool shared_buffer_stride_fix{}; };
inline Values values;
}
namespace android {
enum class PixelFormat { Rgba8888 };
enum class Status { NoError };
struct NvGraphicBuffer {
    u32 width{}, height{}, stride{}, buffer_id{}, offset{};
    PixelFormat format{}, external_format{};
};
struct BufferQueueProducer {
    std::shared_ptr<NvGraphicBuffer> published;
    Status SetPreallocatedBuffer(u32, std::shared_ptr<NvGraphicBuffer> buffer) {
        published = std::move(buffer);
        return Status::NoError;
    }
};
}
"""
        harness += (
            geometry
            + publisher
            + r"""

int main() {
    android::BufferQueueProducer producer;
    assert(!SharedBufferAsync);
    assert(SharedBufferSlotSize == 3932160);
    assert(SharedBufferSize == 27525120);
    for (const bool enabled : {false, true, false}) {
        Settings::values.shared_buffer_stride_fix = enabled;
        for (u32 slot = 0; slot < SharedBufferNumSlots; ++slot) {
            MakeGraphicBuffer(producer, 0, slot, 42);
            const auto& buffer = *producer.published;
            assert(buffer.stride == (enabled ? 1280u : 5120u));
            assert(buffer.width == 1280 && buffer.height == 720);
            assert(buffer.buffer_id == 42 && buffer.offset == slot * SharedBufferSlotSize);
            if (enabled) {
                // Consumers multiply pixel stride by bpp exactly once, including padded capture
                // rows.
                assert(buffer.stride * SharedBufferBlockLinearBpp == 5120);
                assert(buffer.stride * SharedBufferBlockLinearBpp * SharedBufferBlockLinearHeight ==
                       SharedBufferSlotSize);
            }
            assert(buffer.offset + SharedBufferSlotSize <= SharedBufferSize);
        }
    }
}
"""
        )
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory)
            (path / "test.cpp").write_text(harness)
            subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-Wshadow",
                    "-Wconversion",
                    str(path / "test.cpp"),
                    "-o",
                    str(path / "test"),
                ],
                check=True,
            )
            subprocess.run([str(path / "test")], check=True)


if __name__ == "__main__":
    unittest.main()
