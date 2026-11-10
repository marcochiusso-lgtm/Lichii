// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "common/page_table.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/kernel.h"
#include "core/memory.h"
#include "tests/core/system_test_context.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/engines/maxwell_dma.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/memory_manager.h"
#include "video_core/rasterizer_interface.h"

namespace {

using Tegra::Engines::Maxwell3D;

class NoDma final : public Tegra::Engines::AccelerateDMAInterface {
public:
    bool BufferCopy(GPUVAddr, GPUVAddr, u64) override { return true; }
    bool BufferClear(GPUVAddr, u64, u32) override { return true; }
    bool ImageToBuffer(const Tegra::DMA::ImageCopy&, const Tegra::DMA::ImageOperand&,
                       const Tegra::DMA::BufferOperand&) override { return true; }
    bool BufferToImage(const Tegra::DMA::ImageCopy&, const Tegra::DMA::BufferOperand&,
                       const Tegra::DMA::ImageOperand&) override { return true; }
};

/// Records the vertex range of each draw, and the draws made with a GPU-read task count.
class DrawRasterizer final : public VideoCore::RasterizerInterface {
public:
    struct Range {
        u32 first;
        u32 count;
        bool operator==(const Range&) const = default;
    };
    std::vector<Range> draws;
    std::vector<DAddr> gpu_count_draws;
    bool draws_from_gpu{}; ///< What DrawMeshTasksFromGpu answers
    Maxwell3D* engine{};

    void Draw(bool, u32) override {
        const auto& vertex_buffer = engine->draw_manager.draw_state.vertex_buffer;
        draws.push_back({vertex_buffer.first, vertex_buffer.count});
    }
    bool DrawMeshTasksFromGpu(DAddr count_addr) override {
        if (draws_from_gpu) {
            gpu_count_draws.push_back(count_addr);
        }
        return draws_from_gpu;
    }
    void DrawTexture() override {}
    void Clear(u32) override {}
    void DispatchCompute() override {}
    void ResetCounter(VideoCommon::QueryType) override {}
    void Query(GPUVAddr, VideoCommon::QueryType, VideoCommon::QueryPropertiesFlags, u32,
               u32) override {}
    void BindGraphicsUniformBuffer(size_t, u32, GPUVAddr, u32) override {}
    void DisableGraphicsUniformBuffer(size_t, u32) override {}
    void SignalFence(std::function<void()>&& func) override { func(); }
    void SyncOperation(std::function<void()>&& func) override { func(); }
    void SignalSyncPoint(u32) override {}
    void SignalReference() override {}
    void ReleaseFences(bool) override {}
    void FlushAll() override {}
    void FlushRegion(DAddr, u64, VideoCommon::CacheType) override {}
    bool MustFlushRegion(DAddr, u64, VideoCommon::CacheType) override { return false; }
    VideoCore::RasterizerDownloadArea GetFlushArea(DAddr addr, u64 size) override {
        return {addr, addr + size, false};
    }
    void InvalidateRegion(DAddr, u64, VideoCommon::CacheType) override {}
    void OnCacheInvalidation(PAddr, u64) override {}
    bool OnCPUWrite(PAddr, u64) override { return false; }
    void InvalidateGPUCache() override {}
    void UnmapMemory(DAddr, u64) override {}
    void ModifyGPUMemory(size_t, GPUVAddr, u64) override {}
    void FlushAndInvalidateRegion(DAddr, u64, VideoCommon::CacheType) override {}
    void WaitForIdle() override {}
    void FragmentBarrier() override {}
    void TiledCacheBarrier() override {}
    void FlushCommands() override {}
    void TickFrame() override {}
    Tegra::Engines::AccelerateDMAInterface& AccessAccelerateDMA() override { return dma; }
    void AccelerateInlineToMemory(GPUVAddr, size_t, std::span<const u8>) override {}

private:
    NoDma dma;
};

constexpr DAddr DeviceBase = 0x10000;
constexpr GPUVAddr GpuBase = 0x100000;
constexpr u32 ArgumentsOffset = 0x40; ///< Where the GPU "wrote" {count, first}

/// A 3D engine whose GPU memory at GpuBase is guest memory the test writes directly.
struct ArgumentDrawFixture {
    ScopedSystemTestContext context;
    Core::System system;
    Core::DeviceMemory physical;
    Tegra::MaxwellDeviceMemoryManager device{physical};
    DrawRasterizer rasterizer;
    std::unique_ptr<Kernel::KProcess> process;
    Core::Asid asid{};
    u8* bytes{};
    std::optional<Tegra::MemoryManager> memory;
    std::optional<Maxwell3D> maxwell3d;

    ArgumentDrawFixture() {
        system.Initialize();
        process = std::make_unique<Kernel::KProcess>(system.Kernel());
        auto& table = process->GetPageTable().GetImpl();
        table.Resize(24, 12);
        bytes = physical.GetPointerFromRaw<u8>(DeviceBase);
        std::memset(bytes, 0, 0x10000);
        for (u64 page = 0; page < 16; ++page) {
            table.entries.GetAndFault((DeviceBase >> 12) + page)
                .Store(false, Common::PageType::Memory, 1,
                       reinterpret_cast<uintptr_t>(bytes) - DeviceBase);
        }
        process->GetMemory().SetCurrentPageTable(*process);
        asid = device.RegisterProcess(&process->GetMemory());
        device.BindInterface(&rasterizer);
        device.Map(DeviceBase, DeviceBase, 0x10000, asid);
        memory.emplace(system, device, 32, 1ULL << 31, 16);
        memory->BindRasterizer(&rasterizer);
        memory->Map(GpuBase, DeviceBase, 0x10000, Tegra::PTEKind::PITCH, true);
        maxwell3d.emplace(*memory);
        maxwell3d->BindRasterizer(&rasterizer);
        rasterizer.engine = &*maxwell3d;
    }
    ~ArgumentDrawFixture() {
        maxwell3d.reset();
        memory.reset();
        device.Unmap(DeviceBase, 0x10000);
        device.UnregisterProcess(asid);
        system.Kernel().UnregisterKernelObject(process.get());
    }

    void Write(u32 method, u32 value) {
        maxwell3d->CallMethod(system, method, value, true);
    }
    /// The words a culling dispatch left for the macro: its first parameter (the count), then
    /// the second (the first vertex).
    void SetArguments(u32 count, u32 first) {
        const std::array<u32, 2> words{count, first};
        std::memcpy(bytes + ArgumentsOffset, words.data(), sizeof(words));
    }
    /// What the split back runs for a draw macro fed by GPU-written parameters.
    void DrawFromGpuArguments() {
        Write(MAXWELL3D_REG_INDEX(draw.begin), 0x4); // first instance, triangles
        maxwell3d->DrawFromGpuArguments(system, GpuBase + ArgumentsOffset,
                                        DeviceBase + ArgumentsOffset, 0);
    }
    /// A draw that changes neither vertex register.
    void DrawAgain() {
        Write(MAXWELL3D_REG_INDEX(draw.begin), 0x4);
        Write(MAXWELL3D_REG_INDEX(draw.end), 0);
    }
    bool Pending() const {
        return maxwell3d->draw_manager.pending_vertex_arguments.has_value();
    }
};

using Range = DrawRasterizer::Range;

} // namespace

TEST_CASE("A draw from GPU arguments the rasterizer can't make reads the words first",
          "[gpu_argument_draws]") {
    ArgumentDrawFixture f;
    f.SetArguments(6, 3);
    f.DrawFromGpuArguments();
    REQUIRE(f.rasterizer.draws == std::vector<Range>{{3, 6}});
    CHECK(f.rasterizer.gpu_count_draws.empty());
    CHECK(f.maxwell3d->regs.vertex_buffer.first == 3);
    CHECK(f.maxwell3d->regs.vertex_buffer.count == 6);
    CHECK_FALSE(f.Pending());
}

TEST_CASE("A draw from GPU arguments leaves the registers pending until a draw reads them",
          "[gpu_argument_draws]") {
    ArgumentDrawFixture f;
    f.rasterizer.draws_from_gpu = true;
    f.SetArguments(6, 3);
    f.DrawFromGpuArguments();
    REQUIRE(f.rasterizer.gpu_count_draws == std::vector<DAddr>{DeviceBase + ArgumentsOffset});
    CHECK(f.rasterizer.draws.empty());
    REQUIRE(f.Pending());

    // The next draw that uses the registers reads them, as the macro would have set them.
    f.rasterizer.draws_from_gpu = false;
    f.DrawAgain();
    CHECK(f.rasterizer.draws == std::vector<Range>{{3, 6}});
    CHECK_FALSE(f.Pending());
}

TEST_CASE("A vertex register written after a draw from GPU arguments keeps its new value",
          "[gpu_argument_draws]") {
    ArgumentDrawFixture f;
    f.rasterizer.draws_from_gpu = true;
    f.SetArguments(6, 3);
    f.DrawFromGpuArguments();
    f.rasterizer.draws_from_gpu = false;

    // Only the count is written again: the first vertex still comes from the words.
    f.Write(MAXWELL3D_REG_INDEX(vertex_buffer.count), 9);
    REQUIRE(f.Pending());
    f.DrawAgain();
    CHECK(f.rasterizer.draws == std::vector<Range>{{3, 9}});

    // Both written again: the words are not read at all (they changed meanwhile).
    f.rasterizer.draws_from_gpu = true;
    f.DrawFromGpuArguments();
    f.rasterizer.draws_from_gpu = false;
    f.Write(MAXWELL3D_REG_INDEX(vertex_buffer.first), 0);
    f.Write(MAXWELL3D_REG_INDEX(vertex_buffer.count), 2);
    CHECK_FALSE(f.Pending());
    f.SetArguments(100, 100);
    f.DrawAgain();
    CHECK(f.rasterizer.draws.back() == Range{0, 2});
}
