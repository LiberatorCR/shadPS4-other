// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/diagnostic_env.h"
#include "common/debug.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_hle.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/texture_cache.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <unordered_set>

namespace Vulkan {

static Shader::PushData MakeUserData(const AmdGpu::Regs& regs) {
    // TODO(roamic): Add support for multiple viewports and geometry shaders when ViewportIndex
    // is encountered and implemented in the recompiler.
    Shader::PushData push_data{};
    push_data.xoffset = regs.viewport_control.xoffset_enable ? regs.viewports[0].xoffset : 0.f;
    push_data.xscale = regs.viewport_control.xscale_enable ? regs.viewports[0].xscale : 1.f;
    push_data.yoffset = regs.viewport_control.yoffset_enable ? regs.viewports[0].yoffset : 0.f;
    push_data.yscale = regs.viewport_control.yscale_enable ? regs.viewports[0].yscale : 1.f;
    return push_data;
}

namespace {

constexpr u32 DiagDrawTraceMaxSubmits = 2000;
u32 diag_got_last_origin_x = ~0u;
u32 diag_got_last_origin_y = ~0u;

struct DiagDrawTrace {
    struct SceneDescriptor {
        u64 srt{};
        u64 table_base{};
        u64 first_image{};
        u64 image_address_hash{14695981039346656037ull};
        u32 table_stride{};
        u32 table_records{};
        u32 descriptor_valid{};
        u32 descriptor_unmapped{};
        u32 image_null{};
        u32 image_unmapped{};
        u32 image_one_by_one{};
        u32 payload_checked{};
        u32 payload_zero{};
        u32 payload_nonzero{};
        u32 payload_unmapped{};
        u64 payload_hash{14695981039346656037ull};
    };
    bool verbose{Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_DRAW_TRACE">() != nullptr};
    bool enabled{verbose || Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_HEAD_VOLUME_CAPTURE">() != nullptr ||
                 Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_COLOR0_READBACK">() != nullptr ||
                 Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_GBUFFER_ALL_READBACK">() != nullptr ||
                 Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_TEMPORAL_SCENE_INPUT">() != nullptr ||
                 Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_ADAPTIVE_VOLUME_READBACK">() != nullptr ||
                 Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_MATERIAL_HOST_IMAGE">() != nullptr ||
                 Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_INDIRECT_WRITERS">() != nullptr ||
                 Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SCENE_STEADY">() != nullptr ||
                 Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_PRODUCER_CONSTANTS">() != nullptr ||
                 Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_TILE_READBACK">() != nullptr ||
                 Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_ATLAS_SEQUENCE">() != nullptr ||
                 Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_TARGET_TILE_IMAGE">() != nullptr ||
                 Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_TEMPORAL_SCENE_READBACK">() != nullptr ||
                 Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_NOISE_READBACK">() != nullptr ||
                 Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_WEATHER_READBACK">() != nullptr ||
                 Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_WEATHER_POST_BIND_READBACK">() != nullptr ||
                 Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_LUT_SOURCE_READBACK">() != nullptr ||
                 Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_INDIRECT_ARGS">() != nullptr ||
                 Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_INDIRECT_ARGS">() != nullptr ||
                 Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_LERP_CONSTANTS">() != nullptr ||
                 Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_LUT_RAW">() != nullptr ||
                 Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_GROUND_DRAW_CENSUS">() != nullptr ||
                 Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_NOISE_SOURCE_READBACK">() != nullptr ||
                 Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_DRAW_STATE">() != nullptr ||
                 Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_GROUND_DEPTH_POINTS">() != nullptr ||
                 Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_B0_SPLIT_PPM">() != nullptr ||
                 Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_DRAW_GATE_COUNTS">() != nullptr};
    u32 submit_index{};
    u32 attempted{};
    u32 filtered{};
    u32 no_pipeline{};
    u32 bind_failed{};
    u32 issued{};
    u32 scene_issued{};
    std::array<SceneDescriptor, 32> scene_descriptors{};
    u32 draw_index{};
    u32 repeat_count{};
    bool pending{};
    bool has_draws{};
    u32 first_draw_index{};
    u32 last_fs_hash{};
    u32 last_vs_hash{};
    u32 last_mrt_mask{};
    u64 last_cb0_addr{};
    u32 last_cb0_target_mask{};
    u32 last_num_indices{};
    u32 last_num_instances{};
};

DiagDrawTrace diag_draw_trace;
u32 diag_got_scene_draw_submit = ~0u;
u32 diag_got_full_scene_submit = ~0u;
VideoCore::ImageId diag_got_sword_rt0_id{};
VideoCore::ImageId diag_got_sky_tile_id{};
VideoCore::ImageId diag_got_sky_noise_output_id{};
struct DiagGotIndirectDraw {
    u64 stage;
    VAddr target;
    u32 mask;
    VAddr args;
    u32 offset;
    u32 stride;
    u32 max_count;
    VAddr count;
    bool indexed;
};
std::vector<DiagGotIndirectDraw> diag_got_indirect_draws;

float DiagHalfToFloat(u16 bits) {
    const u32 exponent = (bits >> 10) & 31;
    const u32 mantissa = bits & 1023;
    const float sign = (bits & 0x8000) ? -1.f : 1.f;
    if (exponent == 31) {
        return 0.f;
    }
    if (exponent == 0) {
        return sign * std::ldexp(static_cast<float>(mantissa), -24);
    }
    return sign * std::ldexp(static_cast<float>(1024 + mantissa),
                             static_cast<int>(exponent) - 25);
}

void DiagDrawTraceFlush(const char* reason) {
    if (!diag_draw_trace.pending) {
        return;
    }
    LOG_INFO(Render_Vulkan,
             "DiagDraw s={} d={} x{} FS={:#x} VS={:#x} mrt={:#x} cb0={:#x} tmask={:#x} "
             "idx={} inst={} ({})",
             diag_draw_trace.submit_index, diag_draw_trace.first_draw_index,
             diag_draw_trace.repeat_count + 1, diag_draw_trace.last_fs_hash,
             diag_draw_trace.last_vs_hash, diag_draw_trace.last_mrt_mask,
             diag_draw_trace.last_cb0_addr, diag_draw_trace.last_cb0_target_mask,
             diag_draw_trace.last_num_indices, diag_draw_trace.last_num_instances, reason);
    diag_draw_trace.pending = false;
}

} // namespace

Rasterizer::Rasterizer(const Instance& instance_, Scheduler& scheduler_, Runtime& runtime_,
                       AmdGpu::Liverpool* liverpool_)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_}, page_manager{this},
      buffer_cache{instance, scheduler, runtime, liverpool_, texture_cache, page_manager},
      texture_cache{instance, scheduler, runtime, liverpool_, buffer_cache, page_manager},
      liverpool{liverpool_}, memory{Core::Memory::Instance()},
      pipeline_cache{instance, scheduler, liverpool, buffer_cache.GetSparsePageShift()},
      host_markers_enabled{EmulatorSettings.IsVkHostMarkersEnabled()},
      guest_markers_enabled{EmulatorSettings.IsVkGuestMarkersEnabled()} {
    if (!EmulatorSettings.IsNullGPU()) {
        liverpool->BindRasterizer(this);
    }
    memory->SetRasterizer(this);

    scheduler.SetSubmitCallback([this](Vulkan::SubmitInfo& info) {
        runtime.FlushBarriers();
        buffer_cache.SubmitPendingArenaBinds(info);
    });
}

Rasterizer::~Rasterizer() = default;

bool Rasterizer::FilterDraw() {
    const auto& regs = liverpool->regs;
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::EliminateFastClear) {
        // Clears the render target if FCE is launched before any draws
        EliminateFastClear();
        return false;
    }
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::FmaskDecompress) {
        // TODO: check for a valid MRT1 to promote the draw to the resolve pass.
        LOG_TRACE(Render_Vulkan, "FMask decompression pass skipped");
        ScopedMarkerInsert("FmaskDecompress");
        return false;
    }
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Resolve) {
        LOG_TRACE(Render_Vulkan, "Resolve pass");
        Resolve();
        return false;
    }
    if (regs.primitive_type == AmdGpu::PrimitiveType::None) {
        LOG_TRACE(Render_Vulkan, "Primitive type 'None' skipped");
        ScopedMarkerInsert("PrimitiveTypeNone");
        return false;
    }

    const bool cb_disabled =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;
    const auto depth_copy =
        regs.depth_render_override.force_z_dirty && regs.depth_render_override.force_z_valid &&
        regs.depth_buffer.DepthValid() && regs.depth_buffer.DepthWriteValid() &&
        regs.depth_buffer.DepthAddress() != regs.depth_buffer.DepthWriteAddress();
    const auto stencil_copy =
        regs.depth_render_override.force_stencil_dirty &&
        regs.depth_render_override.force_stencil_valid && regs.depth_buffer.StencilValid() &&
        regs.depth_buffer.StencilWriteValid() &&
        regs.depth_buffer.StencilAddress() != regs.depth_buffer.StencilWriteAddress();
    if (cb_disabled && (depth_copy || stencil_copy)) {
        // Games may disable color buffer and enable force depth/stencil dirty and valid to
        // do a copy from one depth-stencil surface to another, without a pixel shader.
        // We need to detect this case and perform the copy, otherwise it will have no effect.
        LOG_TRACE(Render_Vulkan, "Performing depth-stencil override copy");
        DepthStencilCopy(depth_copy, stencil_copy);
        return false;
    }

    return true;
}

void Rasterizer::PrepareRenderState(const GraphicsPipeline* pipeline) {
    // Prefetch render targets to handle overlaps with bound textures (e.g. mipgen)
    const auto& key = pipeline->GetGraphicsKey();
    const auto& regs = liverpool->regs;
    if (regs.color_control.degamma_enable) {
        LOG_WARNING(Render_Vulkan, "Color buffers require gamma correction");
    }

    const bool skip_cb_binding =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;
    for (s32 cb = 0; cb < std::bit_width(key.mrt_mask); ++cb) {
        auto& [image_id, desc] = cb_descs[cb];
        const auto& col_buf = regs.color_buffers[cb];
        const u32 target_mask = regs.color_target_mask.GetMask(cb);
        if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SOURCE_WEATHER_WRITERS">() &&
            (col_buf.Address() == 0x144fd18000 || col_buf.Address() == 0x144fd28000 ||
             col_buf.Address() == 0x144fd60000 || col_buf.Address() == 0x144fd70000)) {
            static std::unordered_set<u64> reported_source_weather_targets;
            const auto fs_hash = pipeline->GetStage(Shader::SwStage::Fragment).pgm_hash;
            const u64 writer_key = (u64(fs_hash) << 32) ^ col_buf.Address() ^
                                   (u64(cb) << 28) ^ target_mask;
            if (reported_source_weather_targets.insert(writer_key).second) {
                LOG_INFO(Render_Vulkan,
                         "GoT source weather render target fs={:#x} cb={} address={:#x} "
                         "mask={:#x} mrt_mask={:#x} blend={}",
                         fs_hash, cb, col_buf.Address(), target_mask, key.mrt_mask,
                         regs.blend_control[cb].enable);
            }
        }
        if (skip_cb_binding || !col_buf || !target_mask || (key.mrt_mask & (1 << cb)) == 0) {
            image_id = {};
            continue;
        }
        const auto& hint = liverpool->last_cb_extent[cb];
        std::construct_at(&desc, col_buf, hint);
        image_id = bound_images.emplace_back(texture_cache.FindImage(desc));
        auto& image = texture_cache.GetImage(image_id);
        image.binding.is_target = 1u;
    }

    if ((regs.depth_control.depth_enable && regs.depth_buffer.DepthValid()) ||
        (regs.depth_control.stencil_enable && regs.depth_buffer.StencilValid())) {
        const auto htile_address = regs.depth_htile_data_base.GetAddress();
        const auto& hint = liverpool->last_db_extent;
        auto& [image_id, desc] = db_desc;
        std::construct_at(&desc, regs.depth_buffer, regs.depth_view, regs.depth_control,
                          htile_address, hint);
        image_id = bound_images.emplace_back(texture_cache.FindImage(desc));
        auto& image = texture_cache.GetImage(image_id);
        image.binding.is_target = 1u;
    } else {
        db_desc.first = {};
    }
}

static std::pair<u32, u32> GetDrawOffsets(const AmdGpu::Regs& regs, const Shader::Info& info,
                                          const Shader::Gcn::FetchShaderData& fetch_shader) {
    u32 vertex_offset = regs.index_offset;
    u32 instance_offset = 0;
    if (!fetch_shader.Empty()) {
        if (vertex_offset == 0 && fetch_shader.vertex_offset_sgpr != -1) {
            vertex_offset = info.user_data[fetch_shader.vertex_offset_sgpr];
        }
        if (fetch_shader.instance_offset_sgpr != -1) {
            instance_offset = info.user_data[fetch_shader.instance_offset_sgpr];
        }
    }
    return {vertex_offset, instance_offset};
}

void Rasterizer::EliminateFastClear() {
    auto& col_buf = liverpool->regs.color_buffers[0];
    if (!col_buf || !col_buf.info.fast_clear) {
        return;
    }
    VideoCore::TextureCache::ImageDesc desc(col_buf, liverpool->last_cb_extent[0]);
    const auto image_id = texture_cache.FindImage(desc);
    const auto& image_view = texture_cache.FindRenderTarget(image_id, desc);
    if (!texture_cache.IsMetaCleared(col_buf.CmaskAddress(), col_buf.view.slice_start)) {
        return;
    }
    for (u32 slice = col_buf.view.slice_start; slice <= col_buf.view.slice_max; ++slice) {
        texture_cache.TouchMeta(col_buf.CmaskAddress(), slice, false);
    }
    auto& image = texture_cache.GetImage(image_id);
    const auto clear_value = LiverpoolToVK::ColorBufferClearValue(col_buf);

    ScopeMarkerBegin(fmt::format("EliminateFastClear:MRT={:#x}:M={:#x}", col_buf.Address(),
                                 col_buf.CmaskAddress()));
    runtime.ClearImage(&image, desc.view_info.range, clear_value);
    ScopeMarkerEnd();
}

void Rasterizer::Draw(bool is_indexed, u32 index_offset) {
    RENDERER_TRACE;

    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SWORD_PIXEL_TRACE">() &&
        diag_got_full_scene_submit != ~0u &&
        diag_draw_trace.submit_index == diag_got_full_scene_submit + 1 &&
        diag_draw_trace.attempted < 24 && diag_got_sword_rt0_id) {
        auto& image = texture_cache.GetImage(diag_got_sword_rt0_id);
        if (image.info.num_bits == 32 && image.info.size.width == 1920 &&
            image.info.size.height == 1080) {
            const auto download = runtime.GetStagingPool().Request(
                2 * sizeof(u32), VideoCore::MemoryType::HostCached, 4, true);
            const std::array<vk::BufferImageCopy, 2> copies = {{
                {.bufferOffset = download.offset,
                 .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                      .mipLevel = 0,
                                      .baseArrayLayer = 0,
                                      .layerCount = 1},
                 .imageOffset = {1250, 600, 0},
                 .imageExtent = {1, 1, 1}},
                {.bufferOffset = download.offset + sizeof(u32),
                 .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                      .mipLevel = 0,
                                      .baseArrayLayer = 0,
                                      .layerCount = 1},
                 .imageOffset = {1080, 230, 0},
                 .imageExtent = {1, 1, 1}},
            }};
            runtime.DownloadImage(&image, download.buffer, copies);
            scheduler.Finish();
            download.Invalidate();
            const auto* pixels = reinterpret_cast<const u32*>(download.mapped);
            LOG_INFO(Render_Vulkan,
                     "GoT sword pixels submit={} next_draw={} blade={:#x} handle={:#x}",
                     diag_draw_trace.submit_index, diag_draw_trace.attempted,
                     pixels[0], pixels[1]);
            runtime.GetStagingPool().FreeDeferred(download);
        }
    }

    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_FULL_END_COLOR0">() &&
        diag_got_full_scene_submit != ~0u &&
        diag_draw_trace.submit_index == diag_got_full_scene_submit + 1 &&
        diag_got_sword_rt0_id) {
        static bool captured_first_next_draw = false;
        if (!captured_first_next_draw) {
            auto& image = texture_cache.GetImage(diag_got_sword_rt0_id);
            if (image.info.num_bits == 32 && image.info.size.width == 1920 &&
                image.info.size.height == 1080) {
                const u64 words = u64(image.info.pitch) * image.info.size.height;
                const auto download = runtime.GetStagingPool().Request(
                    words * sizeof(u32), VideoCore::MemoryType::HostCached, 16, true);
                const vk::BufferImageCopy copy = {
                    .bufferOffset = download.offset,
                    .bufferRowLength = image.info.pitch,
                    .bufferImageHeight = image.info.size.height,
                    .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                         .mipLevel = 0,
                                         .baseArrayLayer = 0,
                                         .layerCount = 1},
                    .imageOffset = {0, 0, 0},
                    .imageExtent = {1920, 1080, 1},
                };
                runtime.DownloadImage(&image, download.buffer, std::span{&copy, 1});
                scheduler.Finish();
                download.Invalidate();
                const auto* pixels = reinterpret_cast<const u32*>(download.mapped);
                u64 nonzero = 0;
                for (u64 i = 0; i < words; ++i) {
                    nonzero += pixels[i] != 0;
                }
                LOG_INFO(Render_Vulkan,
                         "GoT first-next-draw color0 submit={} image_id={} nonzero={}",
                         diag_draw_trace.submit_index, diag_got_sword_rt0_id.index,
                         nonzero);
                runtime.GetStagingPool().FreeDeferred(download);
                captured_first_next_draw = true;
            }
        }
    }

    scheduler.PopPendingOperations();

    const bool count_draw_gates = Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_DRAW_GATE_COUNTS">() != nullptr ||
                                  Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_GROUND_DRAW_CENSUS">() != nullptr;
    if (count_draw_gates) {
        ++diag_draw_trace.attempted;
    }

    if (!FilterDraw()) {
        if (count_draw_gates) {
            ++diag_draw_trace.filtered;
        }
        return;
    }

    const auto& regs = liverpool->regs;
    const GraphicsPipeline* pipeline = pipeline_cache.GetGraphicsPipeline();
    if (!pipeline) {
        if (count_draw_gates) {
            ++diag_draw_trace.no_pipeline;
        }
        return;
    }

    static const u32 skip_fs_hash = [] -> u32 {
        const char* skip_fs = Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKIP_FS">();
        if (!skip_fs) {
            return 0;
        }
        char* end = nullptr;
        const unsigned long long parsed = std::strtoull(skip_fs, &end, 16);
        if (end == skip_fs || *end != '\0' || parsed == 0 || parsed > 0xffffffffull) {
            LOG_WARNING(Render_Vulkan,
                        "SHADPS4_DIAG_GOT_SKIP_FS malformed value '{}', skip disabled", skip_fs);
            return 0;
        }
        LOG_WARNING(Render_Vulkan,
                    "SHADPS4_DIAG_GOT_SKIP_FS active: skipping draws with fragment shader {:#x}",
                    static_cast<u32>(parsed));
        return static_cast<u32>(parsed);
    }();
    if (skip_fs_hash != 0) {
        for (const Shader::Info* stage : pipeline->GetStages()) {
            if (stage && stage->sw_stage == Shader::SwStage::Fragment &&
                stage->pgm_hash == skip_fs_hash) {
                static bool reported_skip_match = false;
                if (!reported_skip_match) {
                    LOG_WARNING(Render_Vulkan, "GoT diagnostic actually skipped fragment draw {:#x}",
                                skip_fs_hash);
                    reported_skip_match = true;
                }
                return;
            }
        }
    }

    PrepareRenderState(pipeline);

    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_PRE_SCENE_COLOR0">() &&
        diag_got_full_scene_submit != ~0u &&
        pipeline->GetStage(Shader::SwStage::Fragment).pgm_hash == 0xb0db526b &&
        diag_draw_trace.submit_index <= diag_got_full_scene_submit + 2) {
        LOG_INFO(Render_Vulkan,
                 "GoT pre-b0 probe submit={} full={} enabled={} address={:#x} image_id={}",
                 diag_draw_trace.submit_index, diag_got_full_scene_submit,
                 diag_draw_trace.enabled, regs.color_buffers[0].Address(),
                 cb_descs[0].first.index);
    }

    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_PRE_SCENE_COLOR0">() && diag_draw_trace.enabled &&
        diag_got_full_scene_submit != ~0u &&
        diag_draw_trace.submit_index > diag_got_full_scene_submit &&
        pipeline->GetStage(Shader::SwStage::Fragment).pgm_hash == 0xb0db526b &&
        regs.color_buffers[0].Address() == 0x14244d8000 && cb_descs[0].first) {
        static bool captured_pre_scene_color0 = false;
        if (!captured_pre_scene_color0) {
            captured_pre_scene_color0 = true;
            auto& image = texture_cache.GetImage(cb_descs[0].first);
            LOG_INFO(Render_Vulkan,
                     "GoT pre-b0 color0 state submit={} image_id={} cmask={:#x} "
                     "meta_cleared={} fast_clear={} color_mode={}",
                     diag_draw_trace.submit_index, cb_descs[0].first.index,
                     regs.color_buffers[0].CmaskAddress(),
                     texture_cache.IsMetaCleared(regs.color_buffers[0].CmaskAddress(),
                                                 regs.color_buffers[0].view.slice_start),
                     regs.color_buffers[0].info.fast_clear,
                     static_cast<u32>(regs.color_control.mode));
            if (image.info.num_bits == 32) {
                const u64 word_count = u64(image.info.pitch) * image.info.size.height;
                const auto download = runtime.GetStagingPool().Request(
                    word_count * sizeof(u32), VideoCore::MemoryType::HostCached, 16, true);
                const vk::BufferImageCopy copy = {
                    .bufferOffset = download.offset,
                    .bufferRowLength = image.info.pitch,
                    .bufferImageHeight = image.info.size.height,
                    .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                         .mipLevel = 0,
                                         .baseArrayLayer = 0,
                                         .layerCount = 1},
                    .imageOffset = {0, 0, 0},
                    .imageExtent = {image.info.size.width, image.info.size.height, 1},
                };
                runtime.DownloadImage(&image, download.buffer, std::span{&copy, 1});
                scheduler.Finish();
                download.Invalidate();
                const auto* words = reinterpret_cast<const u32*>(download.mapped);
                u64 nonzero = 0;
                for (u64 word = 0; word < word_count; ++word) {
                    nonzero += words[word] != 0;
                }
                LOG_INFO(Render_Vulkan,
                         "GoT pre-b0 color0 submit={} image_id={} words={} nonzero={}",
                         diag_draw_trace.submit_index, cb_descs[0].first.index, word_count,
                         nonzero);
                runtime.GetStagingPool().FreeDeferred(download);
            }
        }
    }

    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_GBUFFER_TARGETS">() && diag_draw_trace.enabled &&
        diag_draw_trace.submit_index >= 500 &&
        pipeline->GetStage(Shader::SwStage::Fragment).pgm_hash == 0x2a3cacd4) {
        static bool reported_gbuffer_targets = false;
        if (!reported_gbuffer_targets) {
            for (u32 cb = 0; cb < 5; ++cb) {
                LOG_INFO(Render_Vulkan,
                         "GoT G-buffer target {}: address={:#x} mask={:#x} bound={}", cb,
                         regs.color_buffers[cb].Address(), regs.color_target_mask.GetMask(cb),
                         cb_descs[cb].first.index);
            }
            LOG_INFO(Render_Vulkan, "GoT G-buffer depth: address={:#x} bound={}",
                     regs.depth_buffer.DepthAddress(), db_desc.first.index);
            reported_gbuffer_targets = true;
        }
    }

    if (diag_draw_trace.verbose && diag_draw_trace.submit_index < DiagDrawTraceMaxSubmits) {
        diag_draw_trace.draw_index++;
        diag_draw_trace.has_draws = true;
        const Shader::Info* fs_info{};
        const Shader::Info* vs_info{};
        for (const Shader::Info* stage : pipeline->GetStages()) {
            if (!stage) {
                continue;
            }
            if (stage->sw_stage == Shader::SwStage::Fragment) {
                fs_info = stage;
            } else if (stage->sw_stage == Shader::SwStage::Vertex) {
                vs_info = stage;
            }
        }
        const auto& key = pipeline->GetGraphicsKey();
        const u32 fs_hash = fs_info ? fs_info->pgm_hash : 0;
        const u32 vs_hash = vs_info ? vs_info->pgm_hash : 0;
        const u32 mrt_mask = key.mrt_mask;
        const u64 cb0_addr = regs.color_buffers[0].Address();
        const u32 cb0_target_mask = regs.color_target_mask.GetMask(0);
        if (diag_draw_trace.pending && fs_hash == diag_draw_trace.last_fs_hash &&
            mrt_mask == diag_draw_trace.last_mrt_mask &&
            cb0_addr == diag_draw_trace.last_cb0_addr &&
            cb0_target_mask == diag_draw_trace.last_cb0_target_mask) {
            ++diag_draw_trace.repeat_count;
        } else {
            DiagDrawTraceFlush("change");
            diag_draw_trace.pending = true;
            diag_draw_trace.first_draw_index = diag_draw_trace.draw_index - 1;
            diag_draw_trace.last_fs_hash = fs_hash;
            diag_draw_trace.last_vs_hash = vs_hash;
            diag_draw_trace.last_mrt_mask = mrt_mask;
            diag_draw_trace.last_cb0_addr = cb0_addr;
            diag_draw_trace.last_cb0_target_mask = cb0_target_mask;
            diag_draw_trace.last_num_indices = regs.num_indices;
            diag_draw_trace.last_num_instances = regs.num_instances.NumInstances();
            diag_draw_trace.repeat_count = 0;
        }
    }

    if (!BindResources(pipeline)) {
        if (count_draw_gates) {
            ++diag_draw_trace.bind_failed;
        }
        return;
    }
    static u32 scene_capture_submit = ~0u;
    static std::unordered_set<u32> captured_scene_stages;
    const u32 scene_stage_hash =
        pipeline->GetStage(Shader::SwStage::Fragment).pgm_hash;
    static u32 sword_probe_submit = ~0u;
    static u32 sword_c527_draws = 0;
    const bool sword_probe_candidate =
        Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SWORD_PRODUCER_PPM">() && diag_draw_trace.enabled &&
        diag_draw_trace.submit_index >= 700 && diag_draw_trace.submit_index < 900 &&
        regs.color_buffers[0].Address() == 0x14244d8000 && cb_descs[0].first;
    if (sword_probe_candidate && scene_stage_hash == 0xc5272859 &&
        sword_probe_submit == ~0u) {
        sword_probe_submit = diag_draw_trace.submit_index;
    }
    const bool sword_probe = sword_probe_candidate &&
                             diag_draw_trace.submit_index == sword_probe_submit &&
                             (scene_stage_hash == 0xc5272859 ||
                              scene_stage_hash == 0x75512df2);
    const auto capture_sword_probe = [&](const char* phase) {
        auto& image = texture_cache.GetImage(cb_descs[0].first);
        if (image.info.num_bits != 32 || image.info.size.width != 1920 ||
            image.info.size.height != 1080) {
            return;
        }
        const u64 words = u64(image.info.pitch) * image.info.size.height;
        const auto download = runtime.GetStagingPool().Request(
            words * sizeof(u32), VideoCore::MemoryType::HostCached, 16, true);
        const vk::BufferImageCopy copy = {
            .bufferOffset = download.offset,
            .bufferRowLength = image.info.pitch,
            .bufferImageHeight = image.info.size.height,
            .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                 .mipLevel = 0,
                                 .baseArrayLayer = 0,
                                 .layerCount = 1},
            .imageOffset = {0, 0, 0},
            .imageExtent = {1920, 1080, 1},
        };
        runtime.DownloadImage(&image, download.buffer, std::span{&copy, 1});
        scheduler.Finish();
        download.Invalidate();
        const auto* pixels = reinterpret_cast<const u32*>(download.mapped);
        const std::string path = fmt::format(
            "Build/got-ir-audit/sword-s{}-{}.ppm", diag_draw_trace.submit_index, phase);
        std::ofstream output(path, std::ios::binary);
        output << "P6\n1920 1080\n255\n";
        u64 nonzero = 0;
        for (u32 y = 0; y < 1080; ++y) {
            for (u32 x = 0; x < 1920; ++x) {
                const u32 pixel = pixels[u64(y) * image.info.pitch + x];
                nonzero += pixel != 0;
                const char rgb[3] = {static_cast<char>(pixel & 0xff),
                                     static_cast<char>((pixel >> 8) & 0xff),
                                     static_cast<char>((pixel >> 16) & 0xff)};
                output.write(rgb, sizeof(rgb));
            }
        }
        LOG_INFO(Render_Vulkan, "GoT sword producer submit={} phase={} nonzero={} capture={}",
                 diag_draw_trace.submit_index, phase, nonzero, path);
        runtime.GetStagingPool().FreeDeferred(download);
    };
    if (sword_probe && scene_stage_hash == 0xc5272859 && sword_c527_draws == 0) {
        capture_sword_probe("pre-c527");
    }
    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_FINAL_OVERLAY_STATE">() &&
        (scene_stage_hash == 0x24bcb6fe || scene_stage_hash == 0x167bdbe8) &&
        diag_draw_trace.enabled &&
        diag_draw_trace.submit_index >= 700) {
        static u32 reported_overlay_stages = 0;
        const u32 stage_bit = scene_stage_hash == 0x167bdbe8 ? 2 : 1;
        if (!(reported_overlay_stages & stage_bit)) {
            const auto& stage = pipeline->GetStage(Shader::SwStage::Fragment);
            const u64 srt_addr = static_cast<u64>(stage.user_data[0]) |
                                 (static_cast<u64>(stage.user_data[1]) << 32);
            u32 constants[4]{};
            if (memory->IsValidGpuMapping(srt_addr, sizeof(constants))) {
                std::memcpy(constants, reinterpret_cast<const void*>(srt_addr),
                            sizeof(constants));
            }
            const auto& blend = regs.blend_control[0];
            LOG_INFO(Render_Vulkan,
                     "GoT overlay state stage={:#x} submit={} target={:#x} srt={:#x} "
                     "color={:#x}/{:#x}/{:#x}/{:#x} blend={} bypass={} "
                     "src={} dst={} func={} alpha_src={} alpha_dst={} alpha_func={} "
                     "separate_alpha={} target_mask={:#x} rop={:#x}",
                     scene_stage_hash, diag_draw_trace.submit_index,
                     regs.color_buffers[0].Address(), srt_addr,
                     constants[0], constants[1], constants[2], constants[3], blend.enable,
                     regs.color_buffers[0].info.blend_bypass,
                     static_cast<u32>(blend.color_src_factor),
                     static_cast<u32>(blend.color_dst_factor),
                     static_cast<u32>(blend.color_func),
                     static_cast<u32>(blend.alpha_src_factor),
                     static_cast<u32>(blend.alpha_dst_factor),
                     static_cast<u32>(blend.alpha_func), blend.separate_alpha_blend,
                     regs.color_target_mask.GetMask(0),
                     static_cast<u32>(regs.color_control.rop3));
            reported_overlay_stages |= stage_bit;
        }
    }
    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_DRAW_STATE">() &&
        scene_stage_hash == 0x167bdbe8 && diag_draw_trace.submit_index >= 700) {
        static u32 sky_draw_logged = 0;
        if (sky_draw_logged++ < 32) {
            const auto& vs = pipeline->GetStage(Shader::SwStage::Vertex);
            LOG_INFO(Render_Vulkan,
                     "GoT sky draw submit={} draw={} vs={:#x} indexed={} indices={} "
                     "instances={} screen=({},{})-({},{}) window=({},{})-({},{}) "
                     "generic=({},{})-({},{}) vport_scissor={}",
                     diag_draw_trace.submit_index, sky_draw_logged, vs.pgm_hash, is_indexed,
                     regs.num_indices, regs.num_instances.NumInstances(),
                     regs.screen_scissor.top_left_x, regs.screen_scissor.top_left_y,
                     regs.screen_scissor.bottom_right_x, regs.screen_scissor.bottom_right_y,
                     regs.window_scissor.top_left_x, regs.window_scissor.top_left_y,
                     regs.window_scissor.bottom_right_x, regs.window_scissor.bottom_right_y,
                     regs.generic_scissor.top_left_x, regs.generic_scissor.top_left_y,
                     regs.generic_scissor.bottom_right_x, regs.generic_scissor.bottom_right_y,
                     regs.mode_control.vport_scissor_enable);
        }
    }
    const bool is_scene_capture_stage = scene_stage_hash == 0x167bdbe8 ||
                                        scene_stage_hash == 0x378b5341 ||
                                        scene_stage_hash == 0x0353be0a ||
                                        scene_stage_hash == 0x39d732d2 ||
                                        scene_stage_hash == 0xf0bbe72e;
    static const u32 scene_capture_min_submit = [] {
        const char* value = Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SCENE_MIN_SUBMIT">();
        return value ? static_cast<u32>(std::strtoul(value, nullptr, 10)) : 700u;
    }();
    const bool capture_scene = Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SCENE_BEFORE_AFTER">() &&
                               diag_draw_trace.enabled &&
                               diag_draw_trace.submit_index >= scene_capture_min_submit &&
                               (diag_draw_trace.scene_issued >= 19 ||
                                Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SCENE_STEADY">()) &&
                               is_scene_capture_stage &&
                               regs.color_buffers[0].Address() == 0x1426100000 &&
                               cb_descs[0].first &&
                               (scene_capture_submit == diag_draw_trace.submit_index ||
                                (scene_capture_submit == ~0u &&
                                 scene_stage_hash == 0x167bdbe8));
    auto capture_scene_image = [&](const char* phase) {
        auto& image = texture_cache.GetImage(cb_descs[0].first);
        if (image.info.num_bits != 64) {
            LOG_WARNING(Render_Vulkan, "GoT scene {} capture skipped: bits={}", phase,
                        image.info.num_bits);
            return;
        }
        const u32 width = image.info.size.width;
        const u32 height = image.info.size.height;
        const u64 byte_count = static_cast<u64>(image.info.pitch) * height * sizeof(u16) * 4;
        const auto download = runtime.GetStagingPool().Request(
            byte_count, VideoCore::MemoryType::HostCached, 16, true);
        const vk::BufferImageCopy copy = {
            .bufferOffset = download.offset,
            .bufferRowLength = image.info.pitch,
            .bufferImageHeight = height,
            .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                 .mipLevel = 0,
                                 .baseArrayLayer = 0,
                                 .layerCount = 1},
            .imageOffset = {0, 0, 0},
            .imageExtent = {width, height, 1},
        };
        runtime.DownloadImage(&image, download.buffer, std::span{&copy, 1});
        scheduler.Finish();
        download.Invalidate();
        const auto* halves = reinterpret_cast<const u16*>(download.mapped);
        if ((Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_UV_OUTPUT">() ||
             Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_SAMPLE_OUTPUT">()) &&
            scene_stage_hash == 0x167bdbe8 &&
            std::string_view{phase}.starts_with("after")) {
            const std::string raw_path =
                fmt::format("scene-s{}-sky-diagnostic.rgba16f", diag_draw_trace.submit_index);
            std::ofstream raw_output(raw_path, std::ios::binary);
            raw_output.write(reinterpret_cast<const char*>(halves), byte_count);
            LOG_INFO(Render_Vulkan, "GoT sky UV raw capture={} pitch={} size={}x{}",
                     raw_path, image.info.pitch, width, height);
        }
        u64 nonzero_pixels = 0;
        u64 bright_pixels = 0;
        float max_rgb = 0.f;
        u64 above_one = 0;
        u64 above_hundred = 0;
        for (float exposure : {1.f, 1.f / 1024.f}) {
            const std::string output_path = fmt::format(
                "scene-s{}-{}-exposure{}.ppm",
                diag_draw_trace.submit_index, phase,
                exposure == 1.f ? "1" : "0p001");
            std::ofstream output(output_path, std::ios::binary);
            if (!output) {
                LOG_WARNING(Render_Vulkan, "GoT scene image export failed: {}", output_path);
            }
            output << "P6\n" << width << ' ' << height << "\n255\n";
            for (u32 y = 0; y < height; ++y) {
                for (u32 x = 0; x < width; ++x) {
                    const u64 index = (u64(y) * image.info.pitch + x) * 4;
                    if (exposure == 1.f) {
                        nonzero_pixels += halves[index] || halves[index + 1] ||
                                          halves[index + 2];
                        bright_pixels += ((halves[index] >> 10) & 31) >= 26 ||
                                         ((halves[index + 1] >> 10) & 31) >= 26 ||
                                         ((halves[index + 2] >> 10) & 31) >= 26;
                        for (u32 channel = 0; channel < 3; ++channel) {
                            const float value = DiagHalfToFloat(halves[index + channel]);
                            if (std::isfinite(value)) {
                                max_rgb = std::max(max_rgb, value);
                                above_one += value > 1.f;
                                above_hundred += value > 100.f;
                            }
                        }
                    }
                    char rgb[3]{};
                    for (u32 channel = 0; channel < 3; ++channel) {
                        const float value = std::max(0.f, DiagHalfToFloat(halves[index + channel])) *
                                            exposure;
                        const float mapped = std::sqrt(value / (1.f + value));
                        rgb[channel] = static_cast<char>(
                            std::clamp(static_cast<int>(mapped * 255.f), 0, 255));
                    }
                    output.write(rgb, sizeof(rgb));
                }
            }
        }
        LOG_INFO(Render_Vulkan,
                 "GoT scene {} capture submit={} image_id={} size={}x{} nonzero={} bright={} max_rgb={} above_one={} above_hundred={}",
                 phase, diag_draw_trace.submit_index, cb_descs[0].first.index, width, height,
                 nonzero_pixels, bright_pixels, max_rgb, above_one, above_hundred);
        runtime.GetStagingPool().FreeDeferred(download);
    };
    if (capture_scene && scene_capture_submit == ~0u) {
        scene_capture_submit = diag_draw_trace.submit_index;
        capture_scene_image("before");
        if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SCENE_DEPTH_RAW">() && db_desc.first) {
            auto& depth = texture_cache.GetImage(db_desc.first);
            const u32 depth_width = depth.info.size.width;
            const u32 depth_height = depth.info.size.height;
            const u64 byte_count = u64(depth.info.pitch) * depth_height * 8;
            const auto download = runtime.GetStagingPool().Request(
                byte_count, VideoCore::MemoryType::HostCached, 16, true);
            const vk::BufferImageCopy copy = {
                .bufferOffset = download.offset,
                .bufferRowLength = depth.info.pitch,
                .bufferImageHeight = depth_height,
                .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eDepth,
                                     .mipLevel = 0,
                                     .baseArrayLayer = 0,
                                     .layerCount = 1},
                .imageOffset = {0, 0, 0},
                .imageExtent = {depth_width, depth_height, 1},
            };
            runtime.DownloadImage(&depth, download.buffer, std::span{&copy, 1});
            scheduler.Finish();
            download.Invalidate();
            const std::string path =
                fmt::format("scene-s{}-before-sky-depth.raw", diag_draw_trace.submit_index);
            std::ofstream output(path, std::ios::binary);
            output.write(reinterpret_cast<const char*>(download.mapped), byte_count);
            LOG_INFO(Render_Vulkan,
                     "GoT scene depth raw path={} image={} address={:#x} size={}x{} "
                     "pitch={} bits={} format={}",
                     path, db_desc.first.index, depth.info.guest_address, depth_width,
                     depth_height, depth.info.pitch, depth.info.num_bits,
                     static_cast<u32>(depth.info.pixel_format));
            runtime.GetStagingPool().FreeDeferred(download);
        }
    }
    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SCENE_PRE_FIRST_PPM">() &&
        scene_stage_hash == 0x2a3cacd4 && diag_draw_trace.enabled &&
        diag_draw_trace.scene_issued == 0 &&
        diag_draw_trace.submit_index >= 700 &&
        diag_draw_trace.submit_index < 900 && cb_descs[0].first) {
        static bool captured_pre_scene = false;
        if (!captured_pre_scene) {
            auto& image = texture_cache.GetImage(cb_descs[0].first);
            if (image.info.num_bits == 32 && image.info.size.width == 1920 &&
                image.info.size.height == 1080) {
                const u64 words = u64(image.info.pitch) * image.info.size.height;
                const auto download = runtime.GetStagingPool().Request(
                    words * sizeof(u32), VideoCore::MemoryType::HostCached, 16, true);
                const vk::BufferImageCopy copy = {
                    .bufferOffset = download.offset,
                    .bufferRowLength = image.info.pitch,
                    .bufferImageHeight = image.info.size.height,
                    .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                         .mipLevel = 0,
                                         .baseArrayLayer = 0,
                                         .layerCount = 1},
                    .imageOffset = {0, 0, 0},
                    .imageExtent = {1920, 1080, 1},
                };
                runtime.DownloadImage(&image, download.buffer, std::span{&copy, 1});
                scheduler.Finish();
                download.Invalidate();
                const auto* pixels = reinterpret_cast<const u32*>(download.mapped);
                const std::string path = fmt::format(
                    "Build/got-ir-audit/scene-pre-s{}.ppm", diag_draw_trace.submit_index);
                std::ofstream output(path, std::ios::binary);
                output << "P6\n1920 1080\n255\n";
                u64 nonzero = 0;
                for (u32 y = 0; y < 1080; ++y) {
                    for (u32 x = 0; x < 1920; ++x) {
                        const u32 pixel = pixels[u64(y) * image.info.pitch + x];
                        nonzero += pixel != 0;
                        const char rgb[3] = {
                            static_cast<char>(pixel & 0xff),
                            static_cast<char>((pixel >> 8) & 0xff),
                            static_cast<char>((pixel >> 16) & 0xff),
                        };
                        output.write(rgb, sizeof(rgb));
                    }
                }
                LOG_INFO(Render_Vulkan,
                         "GoT pre-first scene submit={} image_id={} nonzero={} capture={}",
                         diag_draw_trace.submit_index, cb_descs[0].first.index, nonzero, path);
                runtime.GetStagingPool().FreeDeferred(download);
                captured_pre_scene = true;
            }
        }
    }
    const bool trace_dynamic_draw = Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_VERBOSE_DYNAMIC_IMAGE">() &&
                                    pipeline->GetStage(Shader::SwStage::Fragment).pgm_hash ==
                                        0xff484786;
    if (trace_dynamic_draw) {
        LOG_INFO(Render_Vulkan, "GoT dynamic image diagnostic draw bound resources");
    }
    auto state = BeginRendering(pipeline);
    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_OMIT_DEPTH_ATTACHMENT">() &&
        pipeline->GetStage(Shader::SwStage::Fragment).pgm_hash == 0x167bdbe8) {
        state.depth_stencil_attachment = {};
        LOG_INFO(Render_Vulkan,
                 "GoT diagnostic sky draw without depth/stencil attachment submit={}",
                 diag_draw_trace.submit_index);
    }
    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_FEEDBACK_IDS">() &&
        pipeline->GetStage(Shader::SwStage::Fragment).pgm_hash == 0x6a242cff) {
        static bool reported_feedback_target = false;
        if (!reported_feedback_target) {
            LOG_INFO(Render_Vulkan,
                     "GoT feedback target image_id={} supported={} enabled={}",
                     cb_descs[0].first.index, instance.IsAttachmentFeedbackLoopLayoutSupported(),
                     attachment_feedback_loop);
            reported_feedback_target = true;
        }
    }
    if (trace_dynamic_draw) {
        LOG_INFO(Render_Vulkan, "GoT dynamic image diagnostic draw began rendering");
    }

    BindVertexBuffers(pipeline);
    if (is_indexed) {
        BindIndexBuffer(index_offset);
    }

    if (needs_barrier) {
        runtime.FlushBarriers();
    }

    if (trace_dynamic_draw) {
        LOG_INFO(Render_Vulkan, "GoT dynamic image diagnostic draw binding descriptor set");
    }
    pipeline->BindResources(set_writes, push_data);
    if (trace_dynamic_draw) {
        LOG_INFO(Render_Vulkan, "GoT dynamic image diagnostic draw bound descriptor set");
    }
    UpdateDynamicState(pipeline, is_indexed);
    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_NEXT_SCENE_NO_DEPTH_STENCIL">() &&
        pipeline->GetStage(Shader::SwStage::Fragment).pgm_hash == 0x2a3cacd4 &&
        diag_got_full_scene_submit != ~0u &&
        diag_draw_trace.submit_index == diag_got_full_scene_submit + 1) {
        auto& dynamic_state = scheduler.GetDynamicState();
        dynamic_state.SetDepthTestEnabled(false);
        dynamic_state.SetDepthWriteEnabled(false);
        dynamic_state.SetDepthBoundsTestEnabled(false);
        dynamic_state.SetStencilTestEnabled(false);
        dynamic_state.Commit(instance, scheduler.CommandBuffer());
    }
    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_NO_DEPTH_STENCIL">() &&
        pipeline->GetStage(Shader::SwStage::Fragment).pgm_hash == 0x167bdbe8) {
        auto& dynamic_state = scheduler.GetDynamicState();
        dynamic_state.SetDepthTestEnabled(false);
        dynamic_state.SetDepthWriteEnabled(false);
        dynamic_state.SetDepthBoundsTestEnabled(false);
        dynamic_state.SetStencilTestEnabled(false);
        dynamic_state.Commit(instance, scheduler.CommandBuffer());
    }
    if (trace_dynamic_draw) {
        LOG_INFO(Render_Vulkan, "GoT dynamic image diagnostic draw updated dynamic state");
    }
    scheduler.BeginRendering(state);
    if (trace_dynamic_draw) {
        LOG_INFO(Render_Vulkan, "GoT dynamic image diagnostic draw began command rendering");
    }

    const auto& vs_info = pipeline->GetStage(Shader::SwStage::Vertex);
    const auto& fetch_shader = pipeline->GetFetchShader();
    const auto [vertex_offset, instance_offset] = GetDrawOffsets(regs, vs_info, fetch_shader);
    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_DRAW_STATE">() &&
        pipeline->GetStage(Shader::SwStage::Fragment).pgm_hash == 0x167bdbe8 &&
        diag_draw_trace.submit_index >= 700) {
        static bool reported_sky_vertices = false;
        if (!reported_sky_vertices) {
            VertexInputs<vk::VertexInputAttributeDescription2EXT> attributes;
            VertexInputs<vk::VertexInputBindingDescription2EXT> bindings;
            VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT> divisors;
            VertexInputs<AmdGpu::Buffer> guest_buffers;
            pipeline->GetVertexInputs(attributes, bindings, divisors, guest_buffers,
                                      regs.vgt_instance_step_rate_0,
                                      regs.vgt_instance_step_rate_1);
            LOG_INFO(Render_Vulkan,
                     "GoT sky vertices submit={} vertex_offset={} instance_offset={} "
                     "buffers={} vp0=({},{},{},{}) clip_disabled={}",
                     diag_draw_trace.submit_index, vertex_offset, instance_offset,
                     guest_buffers.size(), regs.viewports[0].xscale, regs.viewports[0].yscale,
                     regs.viewports[0].xoffset, regs.viewports[0].yoffset,
                     regs.IsClipDisabled());
            if (!guest_buffers.empty()) {
                const auto& buffer = guest_buffers[0];
                const u64 address = buffer.base_address + u64(vertex_offset) * 16;
                LOG_INFO(Render_Vulkan,
                         "GoT sky position buffer={:#x} stride={} size={} read_address={:#x}",
                         buffer.base_address, buffer.GetStride(), buffer.GetSize(), address);
                if (memory->IsValidMapping(address, 3 * 16)) {
                    std::array<float, 12> positions{};
                    std::memcpy(positions.data(), reinterpret_cast<const void*>(address),
                                sizeof(positions));
                    for (u32 vertex = 0; vertex < 3; ++vertex) {
                        const u32 i = vertex * 4;
                        LOG_INFO(Render_Vulkan,
                                 "GoT sky position vertex={} ({},{},{},{})", vertex,
                                 positions[i], positions[i + 1], positions[i + 2],
                                 positions[i + 3]);
                    }
                }
            }
            reported_sky_vertices = true;
        }
    }

    const auto cmdbuf = scheduler.CommandBuffer();
    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_PRECLEAR_WHITE">() &&
        pipeline->GetStage(Shader::SwStage::Fragment).pgm_hash == 0x167bdbe8 &&
        diag_draw_trace.submit_index >= 700) {
        const vk::ClearAttachment attachment = {
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .colorAttachment = 0,
            .clearValue = vk::ClearValue{
                .color = {.float32 = std::array<float, 4>{1.f, 1.f, 1.f, 1.f}},
            },
        };
        const vk::ClearRect rect = {
            .rect = {.offset = {0, 0}, .extent = {state.width, state.height}},
            .baseArrayLayer = 0,
            .layerCount = 1,
        };
        cmdbuf.clearAttachments(attachment, rect);
        LOG_INFO(Render_Vulkan,
                 "GoT diagnostic sky target preclear submit={} extent={}x{} image={}",
                 diag_draw_trace.submit_index, state.width, state.height,
                 cb_descs[0].first.index);
    }
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->Handle());
    if (is_indexed) {
        cmdbuf.drawIndexed(regs.num_indices, regs.num_instances.NumInstances(), 0,
                           s32(vertex_offset), instance_offset);
    } else {
        cmdbuf.draw(regs.num_indices, regs.num_instances.NumInstances(), vertex_offset,
                    instance_offset);
    }
    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SWORD_PIXEL_TRACE">() &&
        diag_got_full_scene_submit != ~0u &&
        diag_draw_trace.submit_index == diag_got_full_scene_submit + 1 &&
        diag_draw_trace.attempted == 19 && diag_got_sword_rt0_id) {
        auto& image = texture_cache.GetImage(diag_got_sword_rt0_id);
        const auto download = runtime.GetStagingPool().Request(
            2 * sizeof(u32), VideoCore::MemoryType::HostCached, 4, true);
        const std::array<vk::BufferImageCopy, 2> copies = {{
            {.bufferOffset = download.offset,
             .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                  .mipLevel = 0,
                                  .baseArrayLayer = 0,
                                  .layerCount = 1},
             .imageOffset = {1250, 600, 0},
             .imageExtent = {1, 1, 1}},
            {.bufferOffset = download.offset + sizeof(u32),
             .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                  .mipLevel = 0,
                                  .baseArrayLayer = 0,
                                  .layerCount = 1},
             .imageOffset = {1080, 230, 0},
             .imageExtent = {1, 1, 1}},
        }};
        runtime.DownloadImage(&image, download.buffer, copies);
        scheduler.Finish();
        download.Invalidate();
        const auto* pixels = reinterpret_cast<const u32*>(download.mapped);
        LOG_INFO(Render_Vulkan,
                 "GoT sword pixels after-draw18 submit={} blade={:#x} handle={:#x}",
                 diag_draw_trace.submit_index, pixels[0], pixels[1]);
        runtime.GetStagingPool().FreeDeferred(download);
    }
    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_PM4_WINDOW">() &&
        diag_got_full_scene_submit != ~0u &&
        diag_draw_trace.submit_index == diag_got_full_scene_submit + 1 &&
        diag_draw_trace.attempted == 19) {
        liverpool->diag_got_pm4_window = true;
    }
    if (sword_probe && scene_stage_hash == 0xc5272859) {
        ++sword_c527_draws;
        const std::string phase = fmt::format("post-c527-{}", sword_c527_draws);
        capture_sword_probe(phase.c_str());
    } else if (sword_probe && scene_stage_hash == 0x75512df2) {
        capture_sword_probe("post-755");
    }
    if (count_draw_gates) {
        ++diag_draw_trace.issued;
        if (pipeline->GetStage(Shader::SwStage::Fragment).pgm_hash == 0x2a3cacd4) {
            if (diag_draw_trace.scene_issued < diag_draw_trace.scene_descriptors.size()) {
                const auto& fs = pipeline->GetStage(Shader::SwStage::Fragment);
                const auto table = fs.ReadUdReg<AmdGpu::Buffer>(0, 92);
                auto& snapshot = diag_draw_trace.scene_descriptors[diag_draw_trace.scene_issued];
                snapshot = {};
                snapshot.srt = static_cast<u64>(fs.user_data[0]) |
                               (static_cast<u64>(fs.user_data[1]) << 32);
                snapshot.table_base = table.base_address;
                snapshot.table_stride = table.GetStride();
                snapshot.table_records = table.num_records;
                const u64 first_image_addr = table.base_address + 64;
                if (table.base_address &&
                    memory->IsValidMapping(first_image_addr, sizeof(AmdGpu::Image))) {
                    AmdGpu::Image first_image{};
                    std::memcpy(&first_image, reinterpret_cast<const void*>(first_image_addr),
                                sizeof(first_image));
                    snapshot.first_image = first_image.Address();
                }
                if (diag_draw_trace.scene_issued == 0 && table.GetStride() == 340 &&
                    table.num_records <= 24) {
                    std::unordered_set<u64> sampled_images;
                    for (u32 record = 0; record < table.num_records; ++record) {
                        for (const u32 offset : {0u, 32u, 64u, 96u, 128u, 160u, 192u}) {
                            const u64 address = table.base_address +
                                                u64(record) * table.GetStride() + offset;
                            if (!memory->IsValidMapping(address, sizeof(AmdGpu::Image))) {
                                ++snapshot.descriptor_unmapped;
                                continue;
                            }
                            AmdGpu::Image image{};
                            std::memcpy(&image, reinterpret_cast<const void*>(address),
                                        sizeof(image));
                            ++snapshot.descriptor_valid;
                            snapshot.image_address_hash ^= image.Address();
                            snapshot.image_address_hash *= 1099511628211ull;
                            if (!image.Address()) {
                                ++snapshot.image_null;
                            } else if (!memory->IsValidMapping(image.Address(), 1)) {
                                ++snapshot.image_unmapped;
                            }
                            if (image.width == 0 && image.height == 0) {
                                ++snapshot.image_one_by_one;
                            }
                            if (image.Address() && (image.width + 1) >= 64 &&
                                (image.height + 1) >= 64 &&
                                sampled_images.insert(image.Address()).second &&
                                magic_enum::enum_contains(image.GetDataFmt()) &&
                                magic_enum::enum_contains(image.GetNumberFmt())) {
                                const VideoCore::ImageInfo info{image, Shader::ImageResource{}};
                                const u32 bytes = std::min<u32>(info.guest_size, 4096);
                                if (!bytes || !memory->IsValidMapping(image.Address(), bytes)) {
                                    ++snapshot.payload_unmapped;
                                    continue;
                                }
                                ++snapshot.payload_checked;
                                bool nonzero = false;
                                const auto* data = reinterpret_cast<const u8*>(image.Address());
                                for (u32 byte = 0; byte < bytes; ++byte) {
                                    nonzero |= data[byte] != 0;
                                    snapshot.payload_hash ^= data[byte];
                                    snapshot.payload_hash *= 1099511628211ull;
                                }
                                if (nonzero) {
                                    ++snapshot.payload_nonzero;
                                } else {
                                    ++snapshot.payload_zero;
                                }
                            }
                        }
                    }
                }
            }
            ++diag_draw_trace.scene_issued;
            static u32 logged_ground_draws = 0;
            if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_GROUND_DRAW_CENSUS">() &&
                diag_draw_trace.submit_index >= 700 && logged_ground_draws < 32) {
                const auto& fs = pipeline->GetStage(Shader::SwStage::Fragment);
                LOG_INFO(Render_Vulkan,
                         "GoT ground draw submit={} index={} indices={} instances={} "
                         "index_base={:#x} fs_srt={:#x} depth={} stencil={}",
                         diag_draw_trace.submit_index, diag_draw_trace.scene_issued - 1,
                         regs.num_indices, regs.num_instances.NumInstances(),
                         (u64(regs.index_base_address.base_addr_hi) << 32) |
                             regs.index_base_address.base_addr_lo,
                         u64(fs.user_data[0]) | (u64(fs.user_data[1]) << 32),
                         regs.depth_control.depth_enable,
                         regs.depth_control.stencil_enable);
                ++logged_ground_draws;
            }
            if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SCENE_INPUT_DIFF">() &&
                diag_draw_trace.submit_index >= 700 &&
                diag_draw_trace.submit_index < 900 &&
                diag_draw_trace.scene_issued <= 10) {
                static u32 first_scene_submit = ~0u;
                if (first_scene_submit == ~0u) {
                    first_scene_submit = diag_draw_trace.submit_index;
                }
                if (diag_draw_trace.submit_index == first_scene_submit ||
                    (diag_got_full_scene_submit != ~0u &&
                     diag_draw_trace.submit_index == diag_got_full_scene_submit + 1)) {
                    VertexInputs<vk::VertexInputAttributeDescription2EXT> attributes;
                    VertexInputs<vk::VertexInputBindingDescription2EXT> bindings;
                    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT> divisors;
                    VertexInputs<AmdGpu::Buffer> guest_buffers;
                    pipeline->GetVertexInputs(attributes, bindings, divisors, guest_buffers,
                                              regs.vgt_instance_step_rate_0,
                                              regs.vgt_instance_step_rate_1);
                    const auto hash_buffer = [&](u32 index) {
                        if (index >= guest_buffers.size()) {
                            return u64{0};
                        }
                        const auto& buffer = guest_buffers[index];
                        const u32 bytes = std::min<u32>(buffer.GetSize(), 256);
                        if (!buffer.base_address || !bytes ||
                            !memory->IsValidMapping(buffer.base_address, bytes)) {
                            return u64{0};
                        }
                        u64 hash = 14695981039346656037ull;
                        const auto* data = reinterpret_cast<const u8*>(buffer.base_address);
                        for (u32 i = 0; i < bytes; ++i) {
                            hash = (hash ^ data[i]) * 1099511628211ull;
                        }
                        return hash;
                    };
                    const auto& fs = pipeline->GetStage(Shader::SwStage::Fragment);
                    LOG_INFO(Render_Vulkan,
                             "GoT scene input submit={} draw={} indices={} instances={} "
                             "index_base={:#x} fs_srt={:#x} vs_ud0={:#x} vs_ud1={:#x} "
                             "vb0={:#x} vb0_hash={:#x} vb1={:#x} vb1_hash={:#x} "
                             "depth={} stencil={} depth_addr={:#x}",
                             diag_draw_trace.submit_index, diag_draw_trace.scene_issued,
                             regs.num_indices, regs.num_instances.NumInstances(),
                             (u64(regs.index_base_address.base_addr_hi) << 32) |
                                 regs.index_base_address.base_addr_lo,
                             u64(fs.user_data[0]) | (u64(fs.user_data[1]) << 32),
                             vs_info.user_data[0], vs_info.user_data[1],
                             guest_buffers.empty() ? 0 : guest_buffers[0].base_address,
                             hash_buffer(0), guest_buffers.size() < 2
                                                 ? 0
                                                 : guest_buffers[1].base_address,
                             hash_buffer(1), regs.depth_control.depth_enable,
                             regs.depth_control.stencil_enable,
                             regs.depth_buffer.DepthAddress());
                }
            }
            if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SCENE_DRAW_SPLIT_PPM">() &&
                diag_draw_trace.submit_index >= 700 &&
                diag_draw_trace.submit_index < 900 &&
                (diag_draw_trace.scene_issued == 10 ||
                 diag_draw_trace.scene_issued == 19) && cb_descs[0].first) {
                static bool captured[3]{};
                const u32 phase = diag_draw_trace.scene_issued == 19
                                      ? 1
                                      : (diag_got_full_scene_submit != ~0u &&
                                                 diag_draw_trace.submit_index ==
                                                     diag_got_full_scene_submit + 1
                                             ? 2
                                             : 0);
                if (!captured[phase]) {
                    auto& image = texture_cache.GetImage(cb_descs[0].first);
                    if (image.info.num_bits == 32 && image.info.size.width == 1920 &&
                        image.info.size.height == 1080) {
                        const u64 words = u64(image.info.pitch) * image.info.size.height;
                        const auto download = runtime.GetStagingPool().Request(
                            words * sizeof(u32), VideoCore::MemoryType::HostCached, 16, true);
                        const vk::BufferImageCopy copy = {
                            .bufferOffset = download.offset,
                            .bufferRowLength = image.info.pitch,
                            .bufferImageHeight = image.info.size.height,
                            .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                                 .mipLevel = 0,
                                                 .baseArrayLayer = 0,
                                                 .layerCount = 1},
                            .imageOffset = {0, 0, 0},
                            .imageExtent = {1920, 1080, 1},
                        };
                        runtime.DownloadImage(&image, download.buffer, std::span{&copy, 1});
                        scheduler.Finish();
                        download.Invalidate();
                        const auto* pixels = reinterpret_cast<const u32*>(download.mapped);
                        const std::string path = fmt::format(
                            "Build/got-ir-audit/scene-draw-s{}-d{}.ppm",
                            diag_draw_trace.submit_index, diag_draw_trace.scene_issued);
                        std::ofstream output(path, std::ios::binary);
                        output << "P6\n1920 1080\n255\n";
                        u64 nonzero = 0;
                        for (u32 y = 0; y < 1080; ++y) {
                            for (u32 x = 0; x < 1920; ++x) {
                                const u32 pixel = pixels[u64(y) * image.info.pitch + x];
                                nonzero += pixel != 0;
                                const char rgb[3] = {
                                    static_cast<char>(pixel & 0xff),
                                    static_cast<char>((pixel >> 8) & 0xff),
                                    static_cast<char>((pixel >> 16) & 0xff),
                                };
                                output.write(rgb, sizeof(rgb));
                            }
                        }
                        LOG_INFO(Render_Vulkan,
                                 "GoT scene draw split submit={} draw={} image_id={} "
                                 "nonzero={} capture={}",
                                 diag_draw_trace.submit_index, diag_draw_trace.scene_issued,
                                 cb_descs[0].first.index, nonzero, path);
                        runtime.GetStagingPool().FreeDeferred(download);
                        captured[phase] = true;
                    }
                }
            }
            if (diag_draw_trace.scene_issued == 19) {
                diag_got_full_scene_submit = diag_draw_trace.submit_index;
                diag_got_sword_rt0_id = cb_descs[0].first;
                if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_PRE_SCENE_COLOR0">()) {
                    LOG_INFO(Render_Vulkan, "GoT full scene marker submit={}",
                             diag_got_full_scene_submit);
                }
            }
        }
    }
    if (diag_draw_trace.enabled &&
        pipeline->GetStage(Shader::SwStage::Fragment).pgm_hash == 0x2a3cacd4) {
        diag_got_scene_draw_submit = diag_draw_trace.submit_index;
    }
    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_GBUFFER_ALL_READBACK">() &&
        scene_stage_hash == 0x6a242cff && cb_descs[0].first &&
        regs.color_buffers[0].Address() == 0x14244d8000 &&
        (diag_draw_trace.submit_index >= 1200 ||
         (diag_draw_trace.submit_index < 900 && diag_draw_trace.scene_issued >= 19))) {
        static bool captured_after_6a[2]{};
        const u32 phase = diag_draw_trace.submit_index >= 1200 ? 1 : 0;
        if (!captured_after_6a[phase]) {
            captured_after_6a[phase] = true;
            auto& image = texture_cache.GetImage(cb_descs[0].first);
            if (image.info.num_bits == 32) {
                const u64 word_count = u64(image.info.pitch) * image.info.size.height;
                const auto download = runtime.GetStagingPool().Request(
                    word_count * sizeof(u32), VideoCore::MemoryType::HostCached, 16, true);
                const vk::BufferImageCopy copy = {
                    .bufferOffset = download.offset,
                    .bufferRowLength = image.info.pitch,
                    .bufferImageHeight = image.info.size.height,
                    .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                         .mipLevel = 0,
                                         .baseArrayLayer = 0,
                                         .layerCount = 1},
                    .imageOffset = {0, 0, 0},
                    .imageExtent = {image.info.size.width, image.info.size.height, 1},
                };
                runtime.DownloadImage(&image, download.buffer, std::span{&copy, 1});
                scheduler.Finish();
                download.Invalidate();
                const auto* words = reinterpret_cast<const u32*>(download.mapped);
                u64 nonzero = 0;
                for (u64 word = 0; word < word_count; ++word) {
                    nonzero += words[word] != 0;
                }
                LOG_INFO(Render_Vulkan,
                         "GoT post-6a color0 readback phase={} submit={} words={} nonzero={}",
                         phase, diag_draw_trace.submit_index, word_count, nonzero);
                runtime.GetStagingPool().FreeDeferred(download);
            }
        }
    }
    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_GROUND_DEPTH_POINTS">() &&
        diag_draw_trace.submit_index >= 700 && diag_draw_trace.submit_index < 900 &&
        db_desc.first &&
        (scene_stage_hash == 0x141a6d03 || scene_stage_hash == 0xe4ba17c3 ||
         scene_stage_hash == 0x2a3cacd4)) {
        static u32 depth_probe_submit = ~0u;
        static u32 depth_probe_counts[3]{};
        static bool depth_probe_done[3]{};
        if (depth_probe_submit != diag_draw_trace.submit_index) {
            depth_probe_submit = diag_draw_trace.submit_index;
            std::fill(std::begin(depth_probe_counts), std::end(depth_probe_counts), 0);
        }
        const u32 slot = scene_stage_hash == 0x141a6d03 ? 0
                         : scene_stage_hash == 0xe4ba17c3 ? 1
                                                            : 2;
        const u32 draw_count = ++depth_probe_counts[slot];
        const u32 target_count = slot == 1 ? 35 : 19;
        if (!depth_probe_done[slot] && draw_count == target_count) {
            auto& depth = texture_cache.GetImage(db_desc.first);
            const auto download = runtime.GetStagingPool().Request(
                4 * sizeof(float), VideoCore::MemoryType::HostCached, 4, true);
            constexpr std::array<std::pair<u32, u32>, 4> points{{
                {300, 870}, {300, 900}, {900, 950}, {300, 500},
            }};
            std::array<vk::BufferImageCopy, 4> copies{};
            for (u32 i = 0; i < points.size(); ++i) {
                copies[i] = {
                    .bufferOffset = download.offset + i * sizeof(float),
                    .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eDepth,
                                         .mipLevel = 0,
                                         .baseArrayLayer = 0,
                                         .layerCount = 1},
                    .imageOffset = {static_cast<s32>(points[i].first),
                                    static_cast<s32>(points[i].second), 0},
                    .imageExtent = {1, 1, 1},
                };
            }
            runtime.DownloadImage(&depth, download.buffer, std::span{copies});
            scheduler.Finish();
            download.Invalidate();
            const auto* values = reinterpret_cast<const float*>(download.mapped);
            LOG_INFO(Render_Vulkan,
                     "GoT ground depth stage={:#x} submit={} draw={} image={} address={:#x} "
                     "gap={} grass={} center={} sky={}",
                     scene_stage_hash, diag_draw_trace.submit_index, draw_count,
                     db_desc.first.index, depth.info.guest_address, values[0], values[1],
                     values[2], values[3]);
            runtime.GetStagingPool().FreeDeferred(download);
            depth_probe_done[slot] = true;
        }
    }
    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_CLEAR_SKY_TARGET_AFTER_DRAW">() &&
        scene_stage_hash == 0x167bdbe8 && diag_draw_trace.submit_index >= 700 &&
        cb_descs[0].first) {
        static bool cleared_sky_target = false;
        if (!cleared_sky_target) {
            auto& image = texture_cache.GetImage(cb_descs[0].first);
            const VideoCore::SubresourceRange range = {
                .base = {.level = 0, .layer = 0},
                .extent = image.info.resources,
            };
            const vk::ClearValue clear = {
                .color = {.float32 = std::array<float, 4>{1.f, 1.f, 1.f, 1.f}},
            };
            runtime.ClearImage(&image, range, clear);
            LOG_INFO(Render_Vulkan,
                     "GoT diagnostic full sky target clear submit={} image={} address={:#x}",
                     diag_draw_trace.submit_index, cb_descs[0].first.index,
                     image.info.guest_address);
            cleared_sky_target = true;
        }
    }
    if (capture_scene && scene_capture_submit == diag_draw_trace.submit_index &&
        captured_scene_stages.insert(scene_stage_hash).second) {
        const std::string phase = fmt::format("after-{:08x}", scene_stage_hash);
        capture_scene_image(phase.c_str());
    }
    static const u32 post_capture_min_submit = [] {
        const char* value = Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_POST_PPM_MIN_SUBMIT">();
        return value ? static_cast<u32>(std::strtoul(value, nullptr, 10)) : 700u;
    }();
    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_POST_PPM">() && diag_draw_trace.enabled &&
        diag_draw_trace.submit_index >= post_capture_min_submit &&
        diag_got_scene_draw_submit == diag_draw_trace.submit_index &&
        (scene_stage_hash == 0xd9002625 || scene_stage_hash == 0x9f9aac14 ||
         scene_stage_hash == 0xc44aa69e || scene_stage_hash == 0x24bcb6fe ||
         scene_stage_hash == 0x8753cf85) &&
        cb_descs[0].first) {
        static std::unordered_set<u32> captured_post_stages;
        if (captured_post_stages.insert(scene_stage_hash).second) {
            auto& image = texture_cache.GetImage(cb_descs[0].first);
            if (image.info.num_bits == 32) {
                const u32 width = image.info.size.width;
                const u32 height = image.info.size.height;
                const u64 word_count = static_cast<u64>(image.info.pitch) * height;
                const auto download = runtime.GetStagingPool().Request(
                    word_count * sizeof(u32), VideoCore::MemoryType::HostCached, 16, true);
                const vk::BufferImageCopy copy = {
                    .bufferOffset = download.offset,
                    .bufferRowLength = image.info.pitch,
                    .bufferImageHeight = height,
                    .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                         .mipLevel = 0,
                                         .baseArrayLayer = 0,
                                         .layerCount = 1},
                    .imageOffset = {0, 0, 0},
                    .imageExtent = {width, height, 1},
                };
                runtime.DownloadImage(&image, download.buffer, std::span{&copy, 1});
                scheduler.Finish();
                download.Invalidate();
                const auto* words = reinterpret_cast<const u32*>(download.mapped);
                const std::string output_path = fmt::format(
                    "Build/got-ir-audit/post-s{}-{:08x}-rgba8.ppm",
                    diag_draw_trace.submit_index, scene_stage_hash);
                std::ofstream output(output_path, std::ios::binary);
                output << "P6\n" << width << ' ' << height << "\n255\n";
                u64 nonzero_pixels = 0;
                for (u32 y = 0; y < height; ++y) {
                    for (u32 x = 0; x < width; ++x) {
                        const u32 pixel = words[u64(y) * image.info.pitch + x];
                        nonzero_pixels += pixel != 0;
                        const char rgb[3] = {static_cast<char>(pixel & 0xff),
                                             static_cast<char>((pixel >> 8) & 0xff),
                                             static_cast<char>((pixel >> 16) & 0xff)};
                        output.write(rgb, sizeof(rgb));
                    }
                }
                LOG_INFO(Render_Vulkan,
                         "GoT post capture stage={:#x} submit={} image_id={} size={}x{} "
                         "nonzero={}",
                         scene_stage_hash, diag_draw_trace.submit_index,
                         cb_descs[0].first.index, width, height, nonzero_pixels);
                runtime.GetStagingPool().FreeDeferred(download);
            }
        }
    }
    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_AFTER_FEEDBACK_PPM">() && diag_draw_trace.enabled &&
        diag_draw_trace.submit_index >= 500 &&
        diag_got_scene_draw_submit == diag_draw_trace.submit_index &&
        pipeline->GetStage(Shader::SwStage::Fragment).pgm_hash == 0x6a242cff &&
        cb_descs[0].first) {
        static bool captured_after_feedback = false;
        if (!captured_after_feedback) {
            auto& image = texture_cache.GetImage(cb_descs[0].first);
            if (image.info.num_bits == 32) {
                const u32 width = image.info.size.width;
                const u32 height = image.info.size.height;
                const u64 word_count = static_cast<u64>(image.info.pitch) * height;
                const auto download = runtime.GetStagingPool().Request(
                    word_count * sizeof(u32), VideoCore::MemoryType::HostCached, 16, true);
                const vk::BufferImageCopy copy = {
                    .bufferOffset = download.offset,
                    .bufferRowLength = image.info.pitch,
                    .bufferImageHeight = height,
                    .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                         .mipLevel = 0,
                                         .baseArrayLayer = 0,
                                         .layerCount = 1},
                    .imageOffset = {0, 0, 0},
                    .imageExtent = {width, height, 1},
                };
                runtime.DownloadImage(&image, download.buffer, std::span{&copy, 1});
                scheduler.Finish();
                download.Invalidate();
                const auto* words = reinterpret_cast<const u32*>(download.mapped);
                u64 nonzero_words = 0;
                std::ofstream output("Build/got-ir-audit/after-feedback-rgba8.ppm", std::ios::binary);
                output << "P6\n" << width << ' ' << height << "\n255\n";
                for (u32 y = 0; y < height; ++y) {
                    for (u32 x = 0; x < width; ++x) {
                        const u32 pixel = words[u64(y) * image.info.pitch + x];
                        nonzero_words += pixel != 0;
                        const char rgb[3] = {static_cast<char>(pixel & 0xff),
                                             static_cast<char>((pixel >> 8) & 0xff),
                                             static_cast<char>((pixel >> 16) & 0xff)};
                        output.write(rgb, sizeof(rgb));
                    }
                }
                LOG_INFO(Render_Vulkan,
                         "GoT after feedback submit={} image_id={} size={}x{} nonzero={}",
                         diag_draw_trace.submit_index, cb_descs[0].first.index, width, height,
                         nonzero_words);
                runtime.GetStagingPool().FreeDeferred(download);
                captured_after_feedback = true;
            }
        }
    }
    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_GBUFFER_TARGETS">() && diag_draw_trace.enabled &&
        diag_draw_trace.submit_index >= 500 && vs_info.pgm_hash == 0x437ecdd3) {
        static bool reported_gbuffer_draw = false;
        if (!reported_gbuffer_draw) {
            LOG_INFO(Render_Vulkan,
                     "GoT G-buffer draw issued: submit={} indices={} indexed={} vertex_offset={}",
                     diag_draw_trace.submit_index, regs.num_indices, is_indexed, vertex_offset);
            reported_gbuffer_draw = true;
        }
    }
    if (trace_dynamic_draw) {
        LOG_INFO(Render_Vulkan, "GoT dynamic image diagnostic draw submitted draw command");
    }
    DebugState.IncDrawCall();

    ResetBindings(false);
    if (trace_dynamic_draw) {
        LOG_INFO(Render_Vulkan, "GoT dynamic image diagnostic draw fully returned");
    }
}

void Rasterizer::DrawIndirect(bool is_indexed, VAddr arg_address, u32 offset, u32 stride,
                              u32 max_count, VAddr count_address, u16 vertex_sgpr_offset,
                              u16 instance_sgpr_offset) {
    RENDERER_TRACE;

    static bool traced_first_next_indirect = false;
    const bool trace_first_next_indirect =
        Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SWORD_PIXEL_TRACE">() &&
        diag_got_full_scene_submit != ~0u &&
        diag_draw_trace.submit_index == diag_got_full_scene_submit + 1 &&
        !traced_first_next_indirect && diag_got_sword_rt0_id;
    const auto trace_indirect_pixels = [&](const char* phase) {
        auto& image = texture_cache.GetImage(diag_got_sword_rt0_id);
        const auto download = runtime.GetStagingPool().Request(
            2 * sizeof(u32), VideoCore::MemoryType::HostCached, 4, true);
        const std::array<vk::BufferImageCopy, 2> copies = {{
            {.bufferOffset = download.offset,
             .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                  .mipLevel = 0,
                                  .baseArrayLayer = 0,
                                  .layerCount = 1},
             .imageOffset = {1250, 600, 0},
             .imageExtent = {1, 1, 1}},
            {.bufferOffset = download.offset + sizeof(u32),
             .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                  .mipLevel = 0,
                                  .baseArrayLayer = 0,
                                  .layerCount = 1},
             .imageOffset = {1080, 230, 0},
             .imageExtent = {1, 1, 1}},
        }};
        runtime.DownloadImage(&image, download.buffer, copies);
        scheduler.Finish();
        download.Invalidate();
        const auto* pixels = reinterpret_cast<const u32*>(download.mapped);
        LOG_INFO(Render_Vulkan,
                 "GoT sword pixels indirect-{} submit={} arg={:#x} blade={:#x} "
                 "handle={:#x}",
                 phase, diag_draw_trace.submit_index, arg_address + offset,
                 pixels[0], pixels[1]);
        runtime.GetStagingPool().FreeDeferred(download);
    };
    if (trace_first_next_indirect) {
        trace_indirect_pixels("entry");
    }

    scheduler.PopPendingOperations();
    if (trace_first_next_indirect) {
        trace_indirect_pixels("post-pending");
        traced_first_next_indirect = true;
    }

    if (!FilterDraw()) {
        return;
    }

    const DrawIndirectParams params = {
        .vertex_sgpr_offset = vertex_sgpr_offset,
        .instance_sgpr_offset = instance_sgpr_offset,
    };
    const GraphicsPipeline* pipeline = pipeline_cache.GetGraphicsPipeline(params);
    if (!pipeline) {
        return;
    }

    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_INDIRECT_SCENE">()) {
        diag_got_indirect_draws.push_back(
            {pipeline->GetStage(Shader::SwStage::Fragment).pgm_hash,
             liverpool->regs.color_buffers[0].Address(),
             liverpool->regs.color_target_mask.GetMask(0), arg_address, offset, stride,
             max_count, count_address, is_indexed});
    }

    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_FULL_END_COLOR0">() &&
        diag_got_full_scene_submit != ~0u &&
        diag_draw_trace.submit_index == diag_got_full_scene_submit + 1 &&
        pipeline->GetStage(Shader::SwStage::Fragment).pgm_hash == 0xb0db526b &&
        diag_got_sword_rt0_id) {
        static bool captured_before_prepare = false;
        if (!captured_before_prepare) {
            auto& image = texture_cache.GetImage(diag_got_sword_rt0_id);
            if (image.info.num_bits == 32 && image.info.size.width == 1920 &&
                image.info.size.height == 1080) {
                const u64 words = u64(image.info.pitch) * image.info.size.height;
                const auto download = runtime.GetStagingPool().Request(
                    words * sizeof(u32), VideoCore::MemoryType::HostCached, 16, true);
                const vk::BufferImageCopy copy = {
                    .bufferOffset = download.offset,
                    .bufferRowLength = image.info.pitch,
                    .bufferImageHeight = image.info.size.height,
                    .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                         .mipLevel = 0,
                                         .baseArrayLayer = 0,
                                         .layerCount = 1},
                    .imageOffset = {0, 0, 0},
                    .imageExtent = {1920, 1080, 1},
                };
                runtime.DownloadImage(&image, download.buffer, std::span{&copy, 1});
                scheduler.Finish();
                download.Invalidate();
                const auto* pixels = reinterpret_cast<const u32*>(download.mapped);
                u64 nonzero = 0;
                for (u64 i = 0; i < words; ++i) {
                    nonzero += pixels[i] != 0;
                }
                LOG_INFO(Render_Vulkan,
                         "GoT before-b0-prepare color0 submit={} image_id={} nonzero={}",
                         diag_draw_trace.submit_index, diag_got_sword_rt0_id.index,
                         nonzero);
                runtime.GetStagingPool().FreeDeferred(download);
                captured_before_prepare = true;
            }
        }
    }

    PrepareRenderState(pipeline);
    const bool b0_split_candidate =
        Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_B0_SPLIT_PPM">() && diag_draw_trace.enabled &&
        diag_draw_trace.submit_index >= 700 && diag_draw_trace.submit_index < 900 &&
        pipeline->GetStage(Shader::SwStage::Fragment).pgm_hash == 0xb0db526b &&
        liverpool->regs.color_buffers[0].Address() == 0x14244d8000;
    static u32 b0_split_submit = ~0u;
    static u32 b0_split_count = 0;
    if (b0_split_candidate && b0_split_submit == ~0u) {
        b0_split_submit = diag_draw_trace.submit_index;
    }
    const bool b0_split = b0_split_candidate &&
                          diag_draw_trace.submit_index == b0_split_submit;
    const bool capture_b0 = Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_PRE_SCENE_COLOR0">() &&
                            diag_got_full_scene_submit != ~0u &&
                            diag_draw_trace.submit_index > diag_got_full_scene_submit &&
                            pipeline->GetStage(Shader::SwStage::Fragment).pgm_hash ==
                                0xb0db526b &&
                            liverpool->regs.color_buffers[0].Address() == 0x14244d8000 &&
                            cb_descs[0].first;
    static bool captured_b0 = false;
    const bool trace_b0 = capture_b0 && !captured_b0;
    auto read_b0_color0 = [&](const char* phase) {
        auto& image = texture_cache.GetImage(cb_descs[0].first);
        if (image.info.num_bits != 32) {
            return;
        }
        const u64 word_count = u64(image.info.pitch) * image.info.size.height;
        const auto download = runtime.GetStagingPool().Request(
            word_count * sizeof(u32), VideoCore::MemoryType::HostCached, 16, true);
        const vk::BufferImageCopy copy = {
            .bufferOffset = download.offset,
            .bufferRowLength = image.info.pitch,
            .bufferImageHeight = image.info.size.height,
            .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                 .mipLevel = 0,
                                 .baseArrayLayer = 0,
                                 .layerCount = 1},
            .imageOffset = {0, 0, 0},
            .imageExtent = {image.info.size.width, image.info.size.height, 1},
        };
        runtime.DownloadImage(&image, download.buffer, std::span{&copy, 1});
        scheduler.Finish();
        download.Invalidate();
        const auto* words = reinterpret_cast<const u32*>(download.mapped);
        u64 nonzero = 0;
        for (u64 i = 0; i < word_count; ++i) {
            nonzero += words[i] != 0;
        }
        if (phase[0] == 's') {
            const std::string path = fmt::format(
                "Build/got-ir-audit/b0-s{}-{}.ppm", diag_draw_trace.submit_index, phase);
            std::ofstream output(path, std::ios::binary);
            std::ofstream alpha_output(path + ".alpha.pgm", std::ios::binary);
            output << "P6\n" << image.info.size.width << ' ' << image.info.size.height
                   << "\n255\n";
            alpha_output << "P5\n" << image.info.size.width << ' '
                         << image.info.size.height << "\n255\n";
            for (u32 y = 0; y < image.info.size.height; ++y) {
                for (u32 x = 0; x < image.info.size.width; ++x) {
                    const u32 pixel = words[u64(y) * image.info.pitch + x];
                    const char rgb[3] = {static_cast<char>(pixel & 0xff),
                                         static_cast<char>((pixel >> 8) & 0xff),
                                         static_cast<char>((pixel >> 16) & 0xff)};
                    output.write(rgb, sizeof(rgb));
                    const char alpha = static_cast<char>((pixel >> 24) & 0xff);
                    alpha_output.write(&alpha, 1);
                }
            }
            LOG_INFO(Render_Vulkan, "GoT indirect-b0 split capture={}", path);
        }
        LOG_INFO(Render_Vulkan,
                 "GoT indirect-b0 color0 {} submit={} image_id={} words={} nonzero={}",
                 phase, diag_draw_trace.submit_index, cb_descs[0].first.index, word_count,
                 nonzero);
        runtime.GetStagingPool().FreeDeferred(download);
    };
    auto read_b0_other_mrts = [&](const char* phase, bool all = true) {
        for (u32 rt = 1; rt < 4; ++rt) {
            if (!all && rt != 2) {
                continue;
            }
            if (!cb_descs[rt].first) {
                LOG_INFO(Render_Vulkan, "GoT indirect-b0 {} RT{} unbound", phase, rt);
                continue;
            }
            auto& image = texture_cache.GetImage(cb_descs[rt].first);
            const u64 byte_count = u64(image.info.pitch) * image.info.size.height *
                                   image.info.num_bits / 8;
            if (!byte_count) {
                continue;
            }
            const auto download = runtime.GetStagingPool().Request(
                byte_count, VideoCore::MemoryType::HostCached, 16, true);
            const vk::BufferImageCopy copy = {
                .bufferOffset = download.offset,
                .bufferRowLength = image.info.pitch,
                .bufferImageHeight = image.info.size.height,
                .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                     .mipLevel = 0,
                                     .baseArrayLayer = 0,
                                     .layerCount = 1},
                .imageOffset = {0, 0, 0},
                .imageExtent = {image.info.size.width, image.info.size.height, 1},
            };
            runtime.DownloadImage(&image, download.buffer, std::span{&copy, 1});
            scheduler.Finish();
            download.Invalidate();
            const auto* bytes = reinterpret_cast<const u8*>(download.mapped);
            u64 nonzero_bytes = 0;
            for (u64 i = 0; i < byte_count; ++i) {
                nonzero_bytes += bytes[i] != 0;
            }
            const std::string path = fmt::format(
                "Build/got-ir-audit/b0-s{}-{}-rt{}.bin", diag_draw_trace.submit_index,
                phase, rt);
            std::ofstream output(path, std::ios::binary);
            output.write(reinterpret_cast<const char*>(bytes), byte_count);
            LOG_INFO(Render_Vulkan,
                     "GoT indirect-b0 {} RT{} image={} address={:#x} bits={} "
                     "nonzero_bytes={}/{} width={} height={} pitch={} format={} capture={}",
                     phase, rt, cb_descs[rt].first.index, image.info.guest_address,
                     image.info.num_bits, nonzero_bytes, byte_count, image.info.size.width,
                     image.info.size.height, image.info.pitch,
                     vk::to_string(image.info.pixel_format), path);
            runtime.GetStagingPool().FreeDeferred(download);
        }
    };
    if (trace_b0) {
        LOG_INFO(Render_Vulkan,
                 "GoT indirect-b0 args submit={} indexed={} arg={:#x} offset={} stride={} "
                 "max_count={} count={:#x}",
                 diag_draw_trace.submit_index, is_indexed, arg_address, offset, stride,
                 max_count, count_address);
        read_b0_color0("before");
    }
    if (b0_split && b0_split_count == 0) {
        read_b0_color0("split-before");
        read_b0_other_mrts("split-before");
    }
    if (!BindResources(pipeline)) {
        return;
    }
    const auto state = BeginRendering(pipeline);

    const auto [buffer, base] =
        buffer_cache.ObtainBuffer(arg_address + offset, stride * max_count, false);
    needs_barrier |= runtime.IsBufferAccessed(buffer, base, stride * max_count);

    const VideoCore::Buffer* count_buffer;
    u64 count_offset;
    if (count_address != 0) {
        std::tie(count_buffer, count_offset) = buffer_cache.ObtainBuffer(count_address, 4, false);
        needs_barrier |= runtime.IsBufferAccessed(count_buffer, count_offset, 4);
    }

    if (needs_barrier) {
        runtime.FlushBarriers();
    }

    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_INDIRECT_ARGS">() &&
        diag_draw_trace.submit_index >= 700 && diag_draw_trace.submit_index < 900 &&
        pipeline->GetStage(Shader::SwStage::Fragment).pgm_hash == 0xb0db526b &&
        liverpool->regs.color_buffers[0].Address() == 0x14244d8000 &&
        is_indexed && stride == sizeof(VkDrawIndexedIndirectCommand) && max_count == 1) {
        static u32 captured_args = 0;
        static u32 args_submit = ~0u;
        if (args_submit == ~0u) {
            args_submit = diag_draw_trace.submit_index;
        }
        // The matched console census has fourteen B0 draws; include the complete group.
        if (diag_draw_trace.submit_index == args_submit && captured_args < 14) {
            const auto download = runtime.GetStagingPool().Request(
                sizeof(VkDrawIndexedIndirectCommand), VideoCore::MemoryType::HostCached, 16,
                true);
            const vk::BufferCopy copy = {
                .srcOffset = base,
                .dstOffset = download.offset,
                .size = sizeof(VkDrawIndexedIndirectCommand),
            };
            runtime.CopyBuffer(buffer, download.buffer, std::span{&copy, 1});
            scheduler.Finish();
            download.Invalidate();
            VkDrawIndexedIndirectCommand args{};
            std::memcpy(&args, download.mapped, sizeof(args));
            LOG_INFO(Render_Vulkan,
                     "GoT indirect args submit={} entry={} guest={:#x} gpu_modified={} "
                     "index_count={} instance_count={} first_index={} vertex_offset={} "
                     "first_instance={}",
                     diag_draw_trace.submit_index, captured_args, arg_address + offset,
                     buffer_cache.IsRegionGpuModified(arg_address + offset, stride),
                     args.indexCount, args.instanceCount, args.firstIndex, args.vertexOffset,
                     args.firstInstance);
            runtime.GetStagingPool().FreeDeferred(download);
            const auto& regs = liverpool->regs;
            const u32 index_size = regs.index_buffer_type.index_type == AmdGpu::IndexType::Index16
                                       ? sizeof(u16)
                                       : sizeof(u32);
            const VAddr index_address = regs.index_base_address.Address<VAddr>() +
                                        u64(args.firstIndex) * index_size;
            const u64 index_bytes = memory->ClampRangeSize(
                index_address, std::min<u64>(u64(args.indexCount) * index_size, 1024 * 1024));
            LOG_INFO(Render_Vulkan,
                     "GoT B0 indices submit={} draw={} address={:#x} width={} capture_bytes={}",
                     diag_draw_trace.submit_index, captured_args, index_address, index_size,
                     index_bytes);
            if (index_bytes) {
                const auto [source, source_offset] =
                    buffer_cache.ObtainBuffer(index_address, index_bytes, false);
                const auto index_download = runtime.GetStagingPool().Request(
                    index_bytes, VideoCore::MemoryType::HostCached, 16, true);
                const vk::BufferCopy index_copy = {
                    .srcOffset = source_offset,
                    .dstOffset = index_download.offset,
                    .size = index_bytes,
                };
                runtime.CopyBuffer(source, index_download.buffer, std::span{&index_copy, 1});
                scheduler.Finish();
                index_download.Invalidate();
                std::ofstream index_output(
                    fmt::format("Build/got-ir-audit/b0-s{}-draw{}-indices.bin",
                                diag_draw_trace.submit_index, captured_args),
                    std::ios::binary);
                index_output.write(reinterpret_cast<const char*>(index_download.mapped),
                                   index_bytes);
                runtime.GetStagingPool().FreeDeferred(index_download);
            }
            const auto& vs = pipeline->GetStage(Shader::SwStage::Vertex);
            LOG_INFO(Render_Vulkan,
                     "GoT B0 mesh submit={} draw={} vs={:#x} fetch_shader={} buffers={}",
                     diag_draw_trace.submit_index, captured_args, vs.pgm_hash,
                     !pipeline->GetFetchShader().Empty(), vs.buffers.size());
            for (u32 i = 0; i < vs.buffers.size() && i < 16; ++i) {
                const auto& descriptor = vs.buffers[i];
                if (descriptor.IsSpecial()) {
                    continue;
                }
                const auto sharp = descriptor.GetSharp(vs);
                const auto size = memory->ClampRangeSize(
                    sharp.base_address, std::min<u64>(sharp.GetSize(), 4 * 1024 * 1024));
                LOG_INFO(Render_Vulkan,
                         "GoT B0 vertex storage submit={} draw={} buffer={} address={:#x} "
                         "stride={} records={} format={} number_format={} capture_bytes={}",
                         diag_draw_trace.submit_index, captured_args, i,
                         sharp.base_address, sharp.GetStride(), sharp.num_records,
                         u32(sharp.GetDataFmt()), u32(sharp.GetNumberFmt()), size);
                if (sharp.base_address == 0 || size == 0) {
                    continue;
                }
                const auto [source, source_offset] =
                    buffer_cache.ObtainBuffer(sharp.base_address, size, false);
                const auto mesh_download = runtime.GetStagingPool().Request(
                    size, VideoCore::MemoryType::HostCached, 16, true);
                const vk::BufferCopy mesh_copy = {
                    .srcOffset = source_offset,
                    .dstOffset = mesh_download.offset,
                    .size = size,
                };
                runtime.CopyBuffer(source, mesh_download.buffer, std::span{&mesh_copy, 1});
                scheduler.Finish();
                mesh_download.Invalidate();
                std::ofstream mesh_output(
                    fmt::format("Build/got-ir-audit/b0-s{}-draw{}-storage{}.bin",
                                diag_draw_trace.submit_index, captured_args, i),
                    std::ios::binary);
                mesh_output.write(reinterpret_cast<const char*>(mesh_download.mapped), size);
                runtime.GetStagingPool().FreeDeferred(mesh_download);
            }
            ++captured_args;
        }
    }

    // Diagnostic argument readbacks may finish the current command buffer. Bind
    // draw state afterward so the draw has vertex/index buffers in its command buffer.
    BindVertexBuffers(pipeline);
    if (is_indexed) {
        BindIndexBuffer();
    }
    if (needs_barrier) {
        runtime.FlushBarriers();
    }
    pipeline->BindResources(set_writes, push_data);
    UpdateDynamicState(pipeline, is_indexed);
    scheduler.BeginRendering(state);

    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->Handle());

    if (is_indexed) {
        ASSERT(sizeof(VkDrawIndexedIndirectCommand) == stride);

        if (count_address != 0) {
            cmdbuf.drawIndexedIndirectCount(buffer->Handle(), base, count_buffer->Handle(),
                                            count_offset, max_count, stride);
        } else {
            cmdbuf.drawIndexedIndirect(buffer->Handle(), base, max_count, stride);
        }
        DebugState.IncDrawCall();
    } else {
        ASSERT(sizeof(VkDrawIndirectCommand) == stride);

        if (count_address != 0) {
            cmdbuf.drawIndirectCount(buffer->Handle(), base, count_buffer->Handle(), count_offset,
                                     max_count, stride);
        } else {
            cmdbuf.drawIndirect(buffer->Handle(), base, max_count, stride);
        }
        DebugState.IncDrawCall();
    }

    if (trace_b0) {
        read_b0_color0("after");
        captured_b0 = true;
    }
    if (b0_split) {
        ++b0_split_count;
        if (b0_split_count <= 14) {
            const std::string phase = fmt::format("split-after{}", b0_split_count);
            read_b0_color0(phase.c_str());
            read_b0_other_mrts(phase.c_str(), b0_split_count == 14);
        }
    }
    ResetBindings(false);
}

void Rasterizer::DispatchDirect() {
    RENDERER_TRACE;

    scheduler.PopPendingOperations();

    const auto& cs_program = liverpool->GetCsRegs();
    const ComputePipeline* pipeline = pipeline_cache.GetComputePipeline();
    if (!pipeline) {
        return;
    }

    const auto& cs = pipeline->GetStage(Shader::SwStage::Compute);
    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_NOISE_USERS">() &&
        (cs.pgm_hash == 0x3c2e229a || cs.pgm_hash == 0x11d5a4b7)) {
        static std::unordered_set<u32> reported_noise_direct;
        if (reported_noise_direct.insert(cs.pgm_hash).second) {
            LOG_INFO(Render_Vulkan, "GoT sky noise DIRECT stage={:#x} groups={}/{}/{} local={}/{}/{}",
                     cs.pgm_hash, cs_program.dim_x, cs_program.dim_y, cs_program.dim_z,
                     cs_program.num_thread_x.full, cs_program.num_thread_y.full,
                     cs_program.num_thread_z.full);
        }
    }
    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_TILE_RESOURCES">() &&
        (cs.pgm_hash == 0xb7458b04 || cs.pgm_hash == 0xfabd68f2)) {
        static std::unordered_set<u64> reported_sky_dispatches;
        const u64 key = (static_cast<u64>(cs.pgm_hash) << 32) |
                        (static_cast<u64>(cs_program.dim_x) << 20) |
                        (static_cast<u64>(cs_program.dim_y) << 10) | cs_program.dim_z;
        if (reported_sky_dispatches.insert(key).second) {
            LOG_INFO(Render_Vulkan,
                     "GoT sky tile dispatch stage={:#x} groups={}/{}/{} local={}/{}/{}",
                     cs.pgm_hash, cs_program.dim_x, cs_program.dim_y, cs_program.dim_z,
                     cs_program.num_thread_x.full, cs_program.num_thread_y.full,
                     cs_program.num_thread_z.full);
        }
    }
    if (const char* skip_text = Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKIP_LUT_CS">()) {
        char* end = nullptr;
        const unsigned long skip_hash = std::strtoul(skip_text, &end, 0);
        if (end != skip_text && *end == '\0' && skip_hash <= UINT32_MAX &&
            (skip_hash == 0x0f16a579 || skip_hash == 0x9c3cb720) &&
            cs.pgm_hash == skip_hash) {
            static std::unordered_set<u32> reported_skips;
            if (reported_skips.insert(cs.pgm_hash).second) {
                LOG_WARNING(Render_Vulkan, "GoT diagnostic skipping 3D LUT compute {:#x}",
                            cs.pgm_hash);
            }
            return;
        }
    }
    if ((cs.pgm_hash == 0x0f16a579 || cs.pgm_hash == 0x9c3cb720 ||
         cs.pgm_hash == 0xdc800181 || cs.pgm_hash == 0x05b4953e ||
         cs.pgm_hash == 0x061a68f3) &&
        Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_3D_LUT_DISPATCH">()) {
        static std::unordered_set<u64> reported_lut_dispatches;
        const u64 key = (static_cast<u64>(cs.pgm_hash) << 32) |
                        (static_cast<u64>(cs_program.dim_x) << 20) |
                        (static_cast<u64>(cs_program.dim_y) << 10) | cs_program.dim_z;
        if (reported_lut_dispatches.insert(key).second) {
            LOG_INFO(Render_Vulkan,
                     "GoT 3D LUT dispatch: stage={:#x} groups={}/{}/{} local={}/{}/{} partial={}/{}/{}",
                     cs.pgm_hash, cs_program.dim_x, cs_program.dim_y, cs_program.dim_z,
                     cs_program.num_thread_x.full, cs_program.num_thread_y.full,
                     cs_program.num_thread_z.full, cs_program.num_thread_x.partial,
                     cs_program.num_thread_y.partial, cs_program.num_thread_z.partial);
        }
    }
    if (ExecuteShaderHLE(cs, liverpool->regs, cs_program, *this)) {
        return;
    }

    if (!BindResources(pipeline)) {
        return;
    }

    if (needs_barrier) {
        runtime.FlushBarriers();
    }

    scheduler.EndRendering();
    pipeline->BindResources(set_writes, push_data);

    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline->Handle());
    cmdbuf.dispatch(cs_program.dim_x, cs_program.dim_y, cs_program.dim_z);
    DebugState.IncDispatch();

    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_TARGET_GDS">() &&
        cs.pgm_hash == 0x8c5d11bd &&
        ((cs_program.user_data[0] == 60 && cs_program.user_data[1] == 84) ||
         (cs_program.user_data[0] == 60 && cs_program.user_data[1] == 72))) {
        const u32 gds_byte_offset = (cs_program.user_data[10] >> 16) & 0xffff;
        const auto download = runtime.GetStagingPool().Request(
            sizeof(u32) * 2, VideoCore::MemoryType::HostCached, 16, true);
        const vk::BufferCopy copy = {
            .srcOffset = gds_byte_offset,
            .dstOffset = download.offset,
            .size = sizeof(u32) * 2,
        };
        runtime.CopyBuffer(buffer_cache.GetGdsBuffer(), download.buffer,
                           std::span{&copy, 1});
        scheduler.Finish();
        download.Invalidate();
        const auto* counts = reinterpret_cast<const u32*>(download.mapped);
        LOG_INFO(Render_Vulkan,
                 "GoT target GDS after generator origin={}/{} offset={} detailed={} marker={} "
                 "groups={}/{}",
                 cs_program.user_data[0], cs_program.user_data[1], gds_byte_offset,
                 counts[0], counts[1], cs_program.dim_x, cs_program.dim_y);
        runtime.GetStagingPool().FreeDeferred(download);
    }

    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_NOISE_OUTPUT_READBACK">() &&
        (cs.pgm_hash == 0x3c2e229a || cs.pgm_hash == 0x11d5a4b7) &&
        diag_got_sky_noise_output_id) {
        static std::unordered_set<u64> captured_noise_outputs;
        auto& output_image = texture_cache.GetImage(diag_got_sky_noise_output_id);
        if (captured_noise_outputs.insert(output_image.info.guest_address).second &&
            output_image.info.num_bits == 8 && output_image.info.size.width == 64 &&
            output_image.info.size.height == 64 && output_image.info.size.depth == 64) {
            const u64 byte_count = u64(output_image.info.pitch) * 64 * 64;
            const auto download = runtime.GetStagingPool().Request(
                byte_count, VideoCore::MemoryType::HostCached, 16, true);
            const vk::BufferImageCopy copy = {
                .bufferOffset = download.offset,
                .bufferRowLength = output_image.info.pitch,
                .bufferImageHeight = 64,
                .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                     .mipLevel = 0,
                                     .baseArrayLayer = 0,
                                     .layerCount = 1},
                .imageOffset = {0, 0, 0},
                .imageExtent = {64, 64, 64},
            };
            runtime.DownloadImage(&output_image, download.buffer, std::span{&copy, 1});
            scheduler.Finish();
            download.Invalidate();
            const auto* bytes = reinterpret_cast<const u8*>(download.mapped);
            u64 nonzero = 0;
            u64 sum = 0;
            for (u64 j = 0; j < byte_count; ++j) {
                nonzero += bytes[j] != 0;
                sum += bytes[j];
            }
            LOG_INFO(Render_Vulkan,
                     "GoT sky noise immediately after writer stage={:#x} address={:#x} "
                     "nonzero={} sum={}",
                     cs.pgm_hash, output_image.info.guest_address, nonzero, sum);
            runtime.GetStagingPool().FreeDeferred(download);
        }
    }

    ResetBindings(true);
}

void Rasterizer::DispatchIndirect(VAddr address, u32 offset, u32 size) {
    RENDERER_TRACE;

    scheduler.PopPendingOperations();

    const auto& cs_program = liverpool->GetCsRegs();
    const ComputePipeline* pipeline = pipeline_cache.GetComputePipeline();
    if (!pipeline) {
        return;
    }

    const auto& cs = pipeline->GetStage(Shader::SwStage::Compute);
    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_NOISE_USERS">() &&
        (cs.pgm_hash == 0x3c2e229a || cs.pgm_hash == 0x11d5a4b7)) {
        static std::unordered_set<u32> reported_noise_indirect;
        if (reported_noise_indirect.insert(cs.pgm_hash).second) {
            LOG_INFO(Render_Vulkan,
                     "GoT sky noise INDIRECT stage={:#x} args={:#x}+{} size={} local={}/{}/{}",
                     cs.pgm_hash, address, offset, size, cs_program.num_thread_x.full,
                     cs_program.num_thread_y.full, cs_program.num_thread_z.full);
        }
    }
    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_TILE_RESOURCES">() &&
        (cs.pgm_hash == 0xb7458b04 || cs.pgm_hash == 0xfabd68f2)) {
        static std::unordered_set<u32> reported_sky_indirect;
        if (reported_sky_indirect.insert(cs.pgm_hash).second) {
            LOG_INFO(Render_Vulkan,
                     "GoT sky tile INDIRECT dispatch stage={:#x} args={:#x}+{} size={} local={}/{}/{}",
                     cs.pgm_hash, address, offset, size, cs_program.num_thread_x.full,
                     cs_program.num_thread_y.full, cs_program.num_thread_z.full);
        }
    }

    if (!BindResources(pipeline)) {
        return;
    }

    const auto [buffer, base] = buffer_cache.ObtainBuffer(address + offset, size, false);
    needs_barrier |= runtime.IsBufferAccessed(buffer, base, size);

    if (needs_barrier) {
        runtime.FlushBarriers();
    }

    u32 diag_sky_args_x = 0;
    const int sky_args_phase = diag_draw_trace.submit_index >= 1200
                                   ? 1
                                   : (diag_draw_trace.submit_index >= 700 &&
                                              diag_draw_trace.submit_index < 800
                                          ? 0
                                          : -1);
    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_INDIRECT_ARGS">() && sky_args_phase >= 0 &&
        (cs.pgm_hash == 0xb7458b04 || cs.pgm_hash == 0xfabd68f2)) {
        static std::array<u32, 4> captured_sky_args{};
        static std::array<u32, 4> captured_nonzero_sky_args{};
        const u32 capture_index = u32(sky_args_phase) * 2 +
                                  (cs.pgm_hash == 0xb7458b04 ? 0u : 1u);
        VkDispatchIndirectCommand guest_peek{};
        if (memory->IsValidMapping(address + offset, sizeof(guest_peek))) {
            std::memcpy(&guest_peek, reinterpret_cast<const void*>(address + offset),
                        sizeof(guest_peek));
        }
        diag_sky_args_x = guest_peek.x;
        const bool args_gpu_modified =
            buffer_cache.IsRegionGpuModified(address + offset, sizeof(guest_peek));
        if (captured_sky_args[capture_index] < 3 ||
            (guest_peek.x > 0 && captured_nonzero_sky_args[capture_index] < 4) ||
            (args_gpu_modified && captured_sky_args[capture_index] < 12)) {
            const auto download = runtime.GetStagingPool().Request(
                sizeof(VkDispatchIndirectCommand), VideoCore::MemoryType::HostCached, 16,
                true);
            const vk::BufferCopy copy = {
                .srcOffset = base,
                .dstOffset = download.offset,
                .size = sizeof(VkDispatchIndirectCommand),
            };
            runtime.CopyBuffer(buffer, download.buffer, std::span{&copy, 1});
            scheduler.Finish();
            download.Invalidate();
            VkDispatchIndirectCommand args{};
            std::memcpy(&args, download.mapped, sizeof(args));
            diag_sky_args_x = args.x;
            VkDispatchIndirectCommand guest_args{};
            if (memory->IsValidMapping(address + offset, sizeof(guest_args))) {
                std::memcpy(&guest_args, reinterpret_cast<const void*>(address + offset),
                            sizeof(guest_args));
            }
            LOG_INFO(Render_Vulkan,
                     "GoT sky indirect args submit={} stage={:#x} guest={:#x} "
                     "gpu_modified={} groups={}/{}/{} guest_groups={}/{}/{}",
                     diag_draw_trace.submit_index, cs.pgm_hash, address + offset,
                     args_gpu_modified,
                     args.x, args.y, args.z, guest_args.x, guest_args.y,
                     guest_args.z);
            runtime.GetStagingPool().FreeDeferred(download);
            ++captured_sky_args[capture_index];
            captured_nonzero_sky_args[capture_index] += args.x > 0;
        }
    }

    scheduler.EndRendering();
    pipeline->BindResources(set_writes, push_data);

    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline->Handle());
    cmdbuf.dispatchIndirect(buffer->Handle(), base);
    DebugState.IncDispatch();

    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_ALL_INDIRECT">() &&
        (cs.pgm_hash == 0xb7458b04 || cs.pgm_hash == 0xfabd68f2) &&
        memory->IsValidMapping(address + offset, sizeof(VkDispatchIndirectCommand))) {
        VkDispatchIndirectCommand guest_args{};
        std::memcpy(&guest_args, reinterpret_cast<const void*>(address + offset),
                    sizeof(guest_args));
        if (guest_args.x > 0) {
            LOG_INFO(Render_Vulkan,
                     "GoT sky all indirect submit={} stage={:#x} groups={} image={} address={:#x}",
                     diag_draw_trace.submit_index, cs.pgm_hash, guest_args.x,
                     diag_got_sky_tile_id.index,
                     diag_got_sky_tile_id
                         ? texture_cache.GetImage(diag_got_sky_tile_id).info.guest_address
                         : 0);
        }
    }
    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_TARGET_CHUNKS">() &&
        (cs.pgm_hash == 0xb7458b04 || cs.pgm_hash == 0xfabd68f2) &&
        ((diag_got_last_origin_x == 60 && diag_got_last_origin_y == 84) ||
         (diag_got_last_origin_x == 60 && diag_got_last_origin_y == 72) ||
         (diag_got_last_origin_x == 48 && diag_got_last_origin_y == 72))) {
        const auto download = runtime.GetStagingPool().Request(
            sizeof(VkDispatchIndirectCommand), VideoCore::MemoryType::HostCached, 16, true);
        const vk::BufferCopy copy = {
            .srcOffset = base,
            .dstOffset = download.offset,
            .size = sizeof(VkDispatchIndirectCommand),
        };
        runtime.CopyBuffer(buffer, download.buffer, std::span{&copy, 1});
        scheduler.Finish();
        download.Invalidate();
        VkDispatchIndirectCommand args{};
        std::memcpy(&args, download.mapped, sizeof(args));
        LOG_INFO(Render_Vulkan,
                 "GoT target chunk submit={} origin={}/{} stage={:#x} gpu_groups={} image={} "
                 "address={:#x}",
                 diag_draw_trace.submit_index, diag_got_last_origin_x,
                 diag_got_last_origin_y, cs.pgm_hash, args.x, diag_got_sky_tile_id.index,
                 diag_got_sky_tile_id
                     ? texture_cache.GetImage(diag_got_sky_tile_id).info.guest_address
                     : 0);
        if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_TARGET_TILE_IMAGE">() &&
            cs.pgm_hash == 0xb7458b04 && args.x > 0 &&
            diag_got_last_origin_x == 60 && diag_got_last_origin_y == 84 &&
            diag_got_sky_tile_id) {
            static bool captured_target_tile = false;
            if (!captured_target_tile) {
                captured_target_tile = true;
                auto& image = texture_cache.GetImage(diag_got_sky_tile_id);
                const u64 word_count = u64(image.info.pitch) * image.info.size.height;
                const auto pixels = runtime.GetStagingPool().Request(
                    word_count * sizeof(u32), VideoCore::MemoryType::HostCached, 16, true);
                const vk::BufferImageCopy image_copy = {
                    .bufferOffset = pixels.offset,
                    .bufferRowLength = image.info.pitch,
                    .bufferImageHeight = image.info.size.height,
                    .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                         .mipLevel = 0,
                                         .baseArrayLayer = 0,
                                         .layerCount = 1},
                    .imageOffset = {0, 0, 0},
                    .imageExtent = {768, 768, 1},
                };
                runtime.DownloadImage(&image, pixels.buffer, std::span{&image_copy, 1});
                scheduler.Finish();
                pixels.Invalidate();
                const auto* words = reinterpret_cast<const u32*>(pixels.mapped);
                u32 patch_nonzero = 0;
                for (u32 y = 84 * 8; y < 96 * 8; ++y) {
                    for (u32 x = 60 * 8; x < 72 * 8; ++x) {
                        patch_nonzero += words[u64(y) * image.info.pitch + x] != 0;
                    }
                }
                const std::string path = fmt::format("sky-target-tile-s{}.raw",
                                                     diag_draw_trace.submit_index);
                std::ofstream output(path, std::ios::binary);
                output.write(reinterpret_cast<const char*>(words),
                             word_count * sizeof(u32));
                LOG_INFO(Render_Vulkan,
                         "GoT target tile after producer image={} address={:#x} patch_nonzero={} "
                         "path={}",
                         diag_got_sky_tile_id.index, image.info.guest_address,
                         patch_nonzero, path);
                runtime.GetStagingPool().FreeDeferred(pixels);
            }
        }
        runtime.GetStagingPool().FreeDeferred(download);
    }

    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_ATLAS_SEQUENCE">() &&
        diag_draw_trace.submit_index >= 700 && diag_draw_trace.submit_index < 1200 &&
        diag_sky_args_x >= 32 &&
        (cs.pgm_hash == 0xb7458b04 || cs.pgm_hash == 0xfabd68f2)) {
        LOG_INFO(Render_Vulkan, "GoT atlas producer submit={} stage={:#x} groups={} image={} address={:#x}",
                 diag_draw_trace.submit_index, cs.pgm_hash, diag_sky_args_x,
                 diag_got_sky_tile_id.index,
                 diag_got_sky_tile_id ? texture_cache.GetImage(diag_got_sky_tile_id).info.guest_address : 0);
    }

    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_TILE_READBACK">() &&
        diag_draw_trace.submit_index >= 700 && diag_sky_args_x >= 32 &&
        diag_got_sky_tile_id &&
        (cs.pgm_hash == 0xb7458b04 || cs.pgm_hash == 0xfabd68f2)) {
        static std::unordered_set<u32> captured_stages;
        if (captured_stages.insert(cs.pgm_hash).second) {
            auto& image = texture_cache.GetImage(diag_got_sky_tile_id);
            if (image.info.num_bits == 32 && image.info.size.width == 768 &&
                image.info.size.height == 768) {
                const u64 words_count = u64(image.info.pitch) * image.info.size.height;
                const auto download = runtime.GetStagingPool().Request(
                    words_count * sizeof(u32), VideoCore::MemoryType::HostCached, 16, true);
                const vk::BufferImageCopy copy = {
                    .bufferOffset = download.offset,
                    .bufferRowLength = image.info.pitch,
                    .bufferImageHeight = image.info.size.height,
                    .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                         .mipLevel = 0,
                                         .baseArrayLayer = 0,
                                         .layerCount = 1},
                    .imageOffset = {0, 0, 0},
                    .imageExtent = {768, 768, 1},
                };
                runtime.DownloadImage(&image, download.buffer, std::span{&copy, 1});
                scheduler.Finish();
                download.Invalidate();
                const auto* words = reinterpret_cast<const u32*>(download.mapped);
                if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_TILE_RAW">()) {
                    const std::string path = fmt::format(
                        "sky-tile-s{}-stage{:08x}-image{}.bin", diag_draw_trace.submit_index,
                        cs.pgm_hash, diag_got_sky_tile_id.index);
                    std::ofstream output(path, std::ios::binary);
                    output.write(reinterpret_cast<const char*>(words),
                                 words_count * sizeof(u32));
                    LOG_INFO(Render_Vulkan, "GoT sky tile raw capture={} pitch={}", path,
                             image.info.pitch);
                }
                if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_TILE_ORIGINS">() &&
                    !cs.buffers.empty() && !cs.buffers[0].IsSpecial()) {
                    const auto origin_sharp = cs.buffers[0].GetSharp(cs);
                    const u32 origin_bytes =
                        std::min<u32>(diag_sky_args_x * sizeof(u32), origin_sharp.GetSize());
                    const auto [origin_buffer, origin_offset] = buffer_cache.ObtainBuffer(
                        origin_sharp.base_address, origin_bytes, false);
                    const auto origins = runtime.GetStagingPool().Request(
                        origin_bytes, VideoCore::MemoryType::HostCached, 16, true);
                    const vk::BufferCopy origin_copy = {
                        .srcOffset = origin_offset,
                        .dstOffset = origins.offset,
                        .size = origin_bytes,
                    };
                    runtime.CopyBuffer(origin_buffer, origins.buffer,
                                       std::span{&origin_copy, 1});
                    scheduler.Finish();
                    origins.Invalidate();
                    const std::string path = fmt::format(
                        "sky-tile-origins-s{}-stage{:08x}.bin", diag_draw_trace.submit_index,
                        cs.pgm_hash);
                    std::ofstream output(path, std::ios::binary);
                    output.write(reinterpret_cast<const char*>(origins.mapped), origin_bytes);
                    LOG_INFO(Render_Vulkan,
                             "GoT sky tile origins capture={} groups={} address={:#x}",
                             path, diag_sky_args_x, origin_sharp.base_address);
                    runtime.GetStagingPool().FreeDeferred(origins);
                }
                u64 nonzero = 0;
                u64 sentinel = 0;
                u64 blue_zero = 0;
                u64 blue_below_one = 0;
                u64 blue_one = 0;
                u64 blue_above_one = 0;
                u64 hash = 14695981039346656037ull;
                for (u64 i = 0; i < words_count; ++i) {
                    nonzero += words[i] != 0;
                    sentinel += words[i] == 0x3c0;
                    if (words[i] != 0) {
                        const u32 blue = (words[i] >> 22) & 0x3ff;
                        blue_zero += blue == 0;
                        blue_below_one += blue < 0x1e0;
                        blue_one += blue == 0x1e0;
                        blue_above_one += blue > 0x1e0;
                    }
                    hash ^= words[i];
                    hash *= 1099511628211ull;
                }
                LOG_INFO(Render_Vulkan,
                         "GoT sky tile after indirect submit={} stage={:#x} image={} address={:#x} "
                         "nonzero={} sentinel={} blue_zero={} blue_below_one={} "
                         "blue_one={} blue_above_one={} hash={:#x}",
                         diag_draw_trace.submit_index, cs.pgm_hash, diag_got_sky_tile_id.index,
                         image.info.guest_address, nonzero, sentinel, blue_zero,
                         blue_below_one, blue_one, blue_above_one, hash);
                runtime.GetStagingPool().FreeDeferred(download);
            }
        }
    }

    ResetBindings(true);
}

u64 Rasterizer::Flush() {
    const u64 current_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return current_tick;
}

void Rasterizer::Finish() {
    scheduler.Finish();
}

void Rasterizer::OnSubmit() {
    if (diag_draw_trace.enabled) {
        if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_FULL_END_COLOR0">() &&
            diag_draw_trace.submit_index == diag_got_full_scene_submit &&
            diag_got_sword_rt0_id) {
            auto& image = texture_cache.GetImage(diag_got_sword_rt0_id);
            if (image.info.num_bits == 32 && image.info.size.width == 1920 &&
                image.info.size.height == 1080) {
                const u64 words = u64(image.info.pitch) * image.info.size.height;
                const auto download = runtime.GetStagingPool().Request(
                    words * sizeof(u32), VideoCore::MemoryType::HostCached, 16, true);
                const vk::BufferImageCopy copy = {
                    .bufferOffset = download.offset,
                    .bufferRowLength = image.info.pitch,
                    .bufferImageHeight = image.info.size.height,
                    .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                         .mipLevel = 0,
                                         .baseArrayLayer = 0,
                                         .layerCount = 1},
                    .imageOffset = {0, 0, 0},
                    .imageExtent = {1920, 1080, 1},
                };
                runtime.DownloadImage(&image, download.buffer, std::span{&copy, 1});
                scheduler.Finish();
                download.Invalidate();
                const auto* pixels = reinterpret_cast<const u32*>(download.mapped);
                u64 nonzero = 0;
                for (u64 i = 0; i < words; ++i) {
                    nonzero += pixels[i] != 0;
                }
                LOG_INFO(Render_Vulkan,
                         "GoT full-end color0 submit={} image_id={} nonzero={}",
                         diag_draw_trace.submit_index, diag_got_sword_rt0_id.index,
                         nonzero);
                runtime.GetStagingPool().FreeDeferred(download);
            }
        }
        if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_INDIRECT_SCENE">()) {
            const bool selected = diag_draw_trace.scene_issued >= 19 ||
                                  (diag_got_full_scene_submit != ~0u &&
                                   diag_draw_trace.submit_index ==
                                       diag_got_full_scene_submit + 1);
            if (selected) {
                for (u32 i = 0; i < diag_got_indirect_draws.size(); ++i) {
                    const auto& draw = diag_got_indirect_draws[i];
                    LOG_INFO(Render_Vulkan,
                             "GoT indirect scene submit={} entry={} stage={:#x} target={:#x} "
                             "mask={:#x} args={:#x}+{} stride={} max_count={} count={:#x} "
                             "indexed={}",
                             diag_draw_trace.submit_index, i, draw.stage, draw.target,
                             draw.mask, draw.args, draw.offset, draw.stride, draw.max_count,
                             draw.count, draw.indexed);
                }
            }
            diag_got_indirect_draws.clear();
        }
        if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_DRAW_GATE_COUNTS">()) {
            static bool reported_late = false;
            static bool reported_full = false;
            const bool early_scene = diag_draw_trace.submit_index >= 700 &&
                                     diag_draw_trace.submit_index < 800 &&
                                     diag_draw_trace.scene_issued > 0;
            const bool late_scene = diag_draw_trace.submit_index >= 1200 &&
                                    diag_draw_trace.scene_issued > 0 && !reported_late;
            const bool full_scene = diag_draw_trace.scene_issued >= 19 && !reported_full;
            if (early_scene || late_scene || full_scene) {
                LOG_INFO(Render_Vulkan,
                         "GoT draw gates submit={} attempted={} filtered={} no_pipeline={} "
                         "bind_failed={} issued={} scene_issued={}",
                         diag_draw_trace.submit_index, diag_draw_trace.attempted,
                         diag_draw_trace.filtered, diag_draw_trace.no_pipeline,
                         diag_draw_trace.bind_failed, diag_draw_trace.issued,
                         diag_draw_trace.scene_issued);
                reported_late |= late_scene;
            }
            if (full_scene || late_scene) {
                for (u32 i = 0; i < std::min<u32>(diag_draw_trace.scene_issued,
                                                   diag_draw_trace.scene_descriptors.size()); ++i) {
                    const auto& snapshot = diag_draw_trace.scene_descriptors[i];
                    LOG_INFO(Render_Vulkan,
                             "GoT scene descriptor submit={} draw={} srt={:#x} table={:#x} "
                             "stride={} records={} first_image={:#x}",
                             diag_draw_trace.submit_index, i, snapshot.srt,
                             snapshot.table_base, snapshot.table_stride,
                             snapshot.table_records, snapshot.first_image);
                    if (i == 0) {
                        LOG_INFO(Render_Vulkan,
                                 "GoT scene table coverage submit={} descriptors={} "
                                 "unmapped_descriptors={} null_images={} unmapped_images={} "
                                 "one_by_one={} address_hash={:#x} payload_checked={} "
                                 "payload_nonzero={} payload_zero={} payload_unmapped={} "
                                 "payload_hash={:#x}",
                                 diag_draw_trace.submit_index, snapshot.descriptor_valid,
                                 snapshot.descriptor_unmapped, snapshot.image_null,
                                 snapshot.image_unmapped, snapshot.image_one_by_one,
                                 snapshot.image_address_hash, snapshot.payload_checked,
                                 snapshot.payload_nonzero, snapshot.payload_zero,
                                 snapshot.payload_unmapped, snapshot.payload_hash);
                    }
                }
                reported_full |= full_scene;
            }
            diag_draw_trace.attempted = 0;
            diag_draw_trace.filtered = 0;
            diag_draw_trace.no_pipeline = 0;
            diag_draw_trace.bind_failed = 0;
            diag_draw_trace.issued = 0;
            diag_draw_trace.scene_issued = 0;
        }
        if (diag_draw_trace.verbose) {
            DiagDrawTraceFlush("submit");
            if (diag_draw_trace.has_draws) {
                LOG_INFO(Render_Vulkan, "DiagDraw submit={} draws={}", diag_draw_trace.submit_index,
                         diag_draw_trace.draw_index);
                diag_draw_trace.has_draws = false;
            }
        }
        diag_draw_trace.draw_index = 0;
        diag_draw_trace.repeat_count = 0;
        diag_draw_trace.pending = false;
        if (diag_draw_trace.submit_index < DiagDrawTraceMaxSubmits) {
            ++diag_draw_trace.submit_index;
        }
    }
    buffer_cache.TickFrame();
    texture_cache.ProcessDownloadImages();
    texture_cache.RunGarbageCollector();
    runtime.TickFrame();
    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_FULL_END_COLOR0">() &&
        diag_got_full_scene_submit != ~0u &&
        diag_draw_trace.submit_index == diag_got_full_scene_submit + 1 &&
        diag_got_sword_rt0_id) {
        auto& image = texture_cache.GetImage(diag_got_sword_rt0_id);
        if (image.info.num_bits == 32 && image.info.size.width == 1920 &&
            image.info.size.height == 1080) {
            const u64 words = u64(image.info.pitch) * image.info.size.height;
            const auto download = runtime.GetStagingPool().Request(
                words * sizeof(u32), VideoCore::MemoryType::HostCached, 16, true);
            const vk::BufferImageCopy copy = {
                .bufferOffset = download.offset,
                .bufferRowLength = image.info.pitch,
                .bufferImageHeight = image.info.size.height,
                .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                     .mipLevel = 0,
                                     .baseArrayLayer = 0,
                                     .layerCount = 1},
                .imageOffset = {0, 0, 0},
                .imageExtent = {1920, 1080, 1},
            };
            runtime.DownloadImage(&image, download.buffer, std::span{&copy, 1});
            scheduler.Finish();
            download.Invalidate();
            const auto* pixels = reinterpret_cast<const u32*>(download.mapped);
            u64 nonzero = 0;
            for (u64 i = 0; i < words; ++i) {
                nonzero += pixels[i] != 0;
            }
            LOG_INFO(Render_Vulkan,
                     "GoT post-housekeeping color0 full_submit={} image_id={} nonzero={}",
                     diag_got_full_scene_submit, diag_got_sword_rt0_id.index, nonzero);
            runtime.GetStagingPool().FreeDeferred(download);
        }
    }
}

void Rasterizer::SignalGpuCompletion(Common::UniqueFunction<void>&& callback) {
    scheduler.DeferPriorityOperation(std::move(callback));
    scheduler.Flush();
}

void Rasterizer::OnFence() {
    const bool trace_sword_fence = Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_PM4_WINDOW">() &&
                                   liverpool->diag_got_pm4_window &&
                                   diag_got_sword_rt0_id;
    const auto trace_pixels = [&](const char* phase) {
        auto& image = texture_cache.GetImage(diag_got_sword_rt0_id);
        const auto download = runtime.GetStagingPool().Request(
            2 * sizeof(u32), VideoCore::MemoryType::HostCached, 4, true);
        const std::array<vk::BufferImageCopy, 2> copies = {{
            {.bufferOffset = download.offset,
             .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                  .mipLevel = 0,
                                  .baseArrayLayer = 0,
                                  .layerCount = 1},
             .imageOffset = {1250, 600, 0},
             .imageExtent = {1, 1, 1}},
            {.bufferOffset = download.offset + sizeof(u32),
             .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                  .mipLevel = 0,
                                  .baseArrayLayer = 0,
                                  .layerCount = 1},
             .imageOffset = {1080, 230, 0},
             .imageExtent = {1, 1, 1}},
        }};
        runtime.DownloadImage(&image, download.buffer, copies);
        scheduler.Finish();
        download.Invalidate();
        const auto* pixels = reinterpret_cast<const u32*>(download.mapped);
        LOG_INFO(Render_Vulkan, "GoT sword fence {} blade={:#x} handle={:#x}",
                 phase, pixels[0], pixels[1]);
        runtime.GetStagingPool().FreeDeferred(download);
    };
    if (trace_sword_fence) {
        trace_pixels("before");
    }
    texture_cache.ProcessDownloadImages();
    if (trace_sword_fence) {
        trace_pixels("after");
    }
}

bool Rasterizer::BindResources(const Pipeline* pipeline) {
    if (IsComputeImageCopy(pipeline) || IsComputeMetaClear(pipeline) ||
        IsComputeImageClear(pipeline)) {
        return false;
    }

    set_write_index = 0;
    set_writes.clear();
    buffer_infos.clear();
    image_infos.clear();

    bool uses_dma = false;

    // Bind resource buffers and textures.
    Shader::Backend::Bindings binding{};
    push_data = MakeUserData(liverpool->regs);
    for (const auto* stage : pipeline->GetStages()) {
        if (!stage) {
            continue;
        }
        set_writes.resize(set_writes.size() + stage->buffers.size() + stage->images.size() +
                          stage->samplers.size());
        BindBuffers(*stage, binding, push_data);
        BindTextures(*stage, binding);
        uses_dma |= stage->uses_dma;
    }

    if (uses_dma) {
        buffer_cache.SynchronizeDmaBuffers();
    }

    return true;
}

void Rasterizer::BindVertexBuffers(const GraphicsPipeline* pipeline) {
    const auto& regs = liverpool->regs;
    VertexInputs<vk::VertexInputAttributeDescription2EXT> attributes;
    VertexInputs<vk::VertexInputBindingDescription2EXT> bindings;
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT> divisors;
    VertexInputs<AmdGpu::Buffer> guest_buffers;
    pipeline->GetVertexInputs(attributes, bindings, divisors, guest_buffers,
                              regs.vgt_instance_step_rate_0, regs.vgt_instance_step_rate_1);

    if (instance.IsVertexInputDynamicState()) {
        // Update current vertex inputs.
        const auto cmdbuf = scheduler.CommandBuffer();
        cmdbuf.setVertexInputEXT(bindings, attributes);
    }

    if (bindings.empty()) {
        // If there are no bindings, there is nothing further to do.
        return;
    }

    struct BufferRange {
        VAddr base_address;
        VAddr end_address;
        const VideoCore::Buffer* buffer;
        u64 offset;

        [[nodiscard]] size_t GetSize() const {
            return end_address - base_address;
        }
    };

    // Build list of ranges covering the requested buffers
    VertexInputs<BufferRange> ranges{};
    for (const auto& buffer : guest_buffers) {
        if (buffer.base_address != 0 && buffer.GetSize() > 0) {
            ranges.emplace_back(buffer.base_address, buffer.base_address + buffer.GetSize());
        }
    }

    // Merge connecting ranges together
    VertexInputs<BufferRange> ranges_merged{};
    if (!ranges.empty()) {
        std::ranges::sort(ranges, [](const BufferRange& lhv, const BufferRange& rhv) {
            return lhv.base_address < rhv.base_address;
        });
        ranges_merged.emplace_back(ranges[0]);
        for (auto range : ranges) {
            auto& prev_range = ranges_merged.back();
            if (prev_range.end_address < range.base_address) {
                ranges_merged.emplace_back(range);
            } else {
                prev_range.end_address = std::max(prev_range.end_address, range.end_address);
            }
        }
    }

    // Map buffers for merged ranges
    for (auto& range : ranges_merged) {
        const u64 size = memory->ClampRangeSize(range.base_address, range.GetSize());
        std::tie(range.buffer, range.offset) =
            buffer_cache.ObtainBuffer(range.base_address, size, false);
        needs_barrier |= runtime.IsBufferAccessed(range.buffer, range.offset, size);
    }

    // Bind vertex buffers
    VertexInputs<vk::Buffer> host_buffers;
    VertexInputs<vk::DeviceSize> host_offsets;
    VertexInputs<vk::DeviceSize> host_sizes;
    VertexInputs<vk::DeviceSize> host_strides;
    for (const auto& buffer : guest_buffers) {
        if (buffer.base_address != 0 && buffer.GetSize() > 0) {
            const auto host_buffer_info =
                std::ranges::find_if(ranges_merged, [&](const BufferRange& range) {
                    return buffer.base_address >= range.base_address &&
                           buffer.base_address < range.end_address;
                });
            ASSERT(host_buffer_info != ranges_merged.cend());
            host_buffers.emplace_back(host_buffer_info->buffer->Handle());
            host_offsets.push_back(host_buffer_info->offset + buffer.base_address -
                                   host_buffer_info->base_address);
        } else {
            host_buffers.emplace_back(VK_NULL_HANDLE);
            host_offsets.push_back(0);
        }
        host_sizes.push_back(buffer.GetSize());
        host_strides.push_back(buffer.GetStride());
    }

    const auto cmdbuf = scheduler.CommandBuffer();
    const auto num_buffers = guest_buffers.size();
    if (instance.IsVertexInputDynamicState()) {
        cmdbuf.bindVertexBuffers(0, num_buffers, host_buffers.data(), host_offsets.data());
    } else {
        cmdbuf.bindVertexBuffers2(0, num_buffers, host_buffers.data(), host_offsets.data(),
                                  host_sizes.data(), host_strides.data());
    }
}

void Rasterizer::BindIndexBuffer(u32 index_offset) {
    const auto& regs = liverpool->regs;

    // Figure out index type and size.
    const bool is_index16 = regs.index_buffer_type.index_type == AmdGpu::IndexType::Index16;
    const vk::IndexType index_type = is_index16 ? vk::IndexType::eUint16 : vk::IndexType::eUint32;
    const u32 index_size = is_index16 ? sizeof(u16) : sizeof(u32);
    const VAddr index_address =
        regs.index_base_address.Address<VAddr>() + index_offset * index_size;

    // Bind index buffer.
    const u32 index_buffer_size = regs.num_indices * index_size;
    const auto [buffer, offset] =
        buffer_cache.ObtainBuffer(index_address, index_buffer_size, false);
    needs_barrier |= runtime.IsBufferAccessed(buffer, offset, index_buffer_size);
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindIndexBuffer(buffer->Handle(), offset, index_type);
}

void Rasterizer::ResetBindings(bool is_compute) {
    for (auto& image_id : bound_images) {
        texture_cache.GetImage(image_id).binding = {};
    }
    for (const auto [buffer, offset, size, is_written] : bound_buffers) {
        const auto dst_stage = is_compute ? vk::PipelineStageFlagBits2::eComputeShader
                                          : vk::PipelineStageFlagBits2::eAllGraphics;
        const auto write_flag =
            is_written ? vk::AccessFlagBits2::eShaderWrite : vk::AccessFlagBits2::eNone;
        runtime.AccessBuffer(buffer, offset, size, dst_stage,
                             vk::AccessFlagBits2::eShaderRead | write_flag);
    }
    bound_images.clear();
    bound_buffers.clear();
    needs_barrier = false;
}

bool Rasterizer::IsComputeMetaClear(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Most of the time when a metadata is updated with a shader it gets cleared. It means
    // we can skip the whole dispatch and update the tracked state instead. Also, it is not
    // intended to be consumed and in such rare cases (e.g. HTile introspection, CRAA) we
    // will need its full emulation anyways.
    const auto& info = pipeline->GetStage(Shader::SwStage::Compute);

    // Assume if a shader reads metadata, it is a copy shader.
    for (const auto& desc : info.buffers) {
        const VAddr address = desc.GetSharp(info).base_address;
        if (!desc.IsSpecial() && !desc.is_written && texture_cache.IsMeta(address)) {
            return false;
        }
    }

    // Metadata surfaces are tiled and thus need address calculation to be written properly.
    // If a shader wants to encode HTILE, for example, from a depth image it will have to compute
    // proper tile address from dispatch invocation id. This address calculation contains an xor
    // operation so use it as a heuristic for metadata writes that are probably not clears.
    if (!info.has_bitwise_xor) {
        // Assume if a shader writes metadata without address calculation, it is a clear shader.
        for (const auto& desc : info.buffers) {
            const VAddr address = desc.GetSharp(info).base_address;
            if (!desc.IsSpecial() && desc.is_written && texture_cache.ClearMeta(address)) {
                // Assume all slices were updates
                LOG_TRACE(Render_Vulkan, "Metadata update skipped");
                return true;
            }
        }
    }
    return false;
}

bool Rasterizer::IsComputeImageCopy(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Ensure shader only has 2 bound buffers
    const auto& cs_pgm = liverpool->GetCsRegs();
    const auto& info = pipeline->GetStage(Shader::SwStage::Compute);
    if (cs_pgm.num_thread_x.full != 64 || info.buffers.size() != 2 || !info.images.empty()) {
        return false;
    }

    // Those 2 buffers must both be formatted. One must be source and another destination.
    const auto& desc0 = info.buffers[0];
    const auto& desc1 = info.buffers[1];
    if (!desc0.is_formatted || !desc1.is_formatted || desc0.is_written == desc1.is_written) {
        return false;
    }

    // Buffers must have the same size and each thread of the dispatch must copy 1 dword of data
    const AmdGpu::Buffer buf0 = desc0.GetSharp(info);
    const AmdGpu::Buffer buf1 = desc1.GetSharp(info);
    if (buf0.GetSize() != buf1.GetSize() || cs_pgm.dim_x != (buf0.GetSize() / 256)) {
        return false;
    }

    // Find images the buffer alias
    const auto image0_id = texture_cache.FindImageFromRange(buf0.base_address, buf0.GetSize());
    if (!image0_id) {
        return false;
    }
    const auto image1_id =
        texture_cache.FindImageFromRange(buf1.base_address, buf1.GetSize(), false);
    if (!image1_id) {
        return false;
    }

    // Image copy must be valid
    VideoCore::Image& image0 = texture_cache.GetImage(image0_id);
    VideoCore::Image& image1 = texture_cache.GetImage(image1_id);
    if (image0.info.guest_size != image1.info.guest_size ||
        image0.info.pitch != image1.info.pitch || image0.info.guest_size != buf0.GetSize() ||
        image0.info.num_bits != image1.info.num_bits) {
        return false;
    }

    // Perform image copy
    VideoCore::Image& src_image = desc0.is_written ? image1 : image0;
    VideoCore::Image& dst_image = desc0.is_written ? image0 : image1;
    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_PRE_SCENE_COLOR0">() &&
        dst_image.info.guest_address == 0x14244d8000) {
        LOG_INFO(Render_Vulkan, "GoT color0 compute copy submit={} src={:#x} dst={:#x}",
                 diag_draw_trace.submit_index, src_image.info.guest_address,
                 dst_image.info.guest_address);
    }
    runtime.CopyColorAndDepth(&src_image, &dst_image);
    return true;
}

bool Rasterizer::IsComputeImageClear(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Ensure shader only has 2 bound buffers
    const auto& cs_pgm = liverpool->GetCsRegs();
    const auto& info = pipeline->GetStage(Shader::SwStage::Compute);
    if (cs_pgm.num_thread_x.full != 64 || info.buffers.size() != 2 || !info.images.empty()) {
        return false;
    }

    // From those 2 buffers, first must hold the clear vector and second the image being cleared
    const auto& desc0 = info.buffers[0];
    const auto& desc1 = info.buffers[1];
    if (desc0.is_formatted || !desc1.is_formatted || desc0.is_written || !desc1.is_written) {
        return false;
    }

    // First buffer must have size of vec4 and second the size of a single layer
    const AmdGpu::Buffer buf0 = desc0.GetSharp(info);
    const AmdGpu::Buffer buf1 = desc1.GetSharp(info);
    const u32 buf1_bpp = AmdGpu::NumBitsPerBlock(buf1.GetDataFmt());
    if (buf0.GetSize() != 16 || (cs_pgm.dim_x * 128ULL * (buf1_bpp / 8)) != buf1.GetSize()) {
        return false;
    }

    // Find image the buffer alias
    const auto image1_id =
        texture_cache.FindImageFromRange(buf1.base_address, buf1.GetSize(), false);
    if (!image1_id) {
        return false;
    }

    // Image clear must be valid
    VideoCore::Image& image1 = texture_cache.GetImage(image1_id);
    if (image1.info.guest_size != buf1.GetSize() || image1.info.num_bits != buf1_bpp ||
        image1.info.props.is_depth) {
        return false;
    }

    // Perform image clear
    const float* values = reinterpret_cast<float*>(buf0.base_address);
    const vk::ClearValue clear = {
        .color = {.float32 = std::array<float, 4>{values[0], values[1], values[2], values[3]}},
    };
    const VideoCore::SubresourceRange range = {
        .base =
            {
                .level = 0,
                .layer = 0,
            },
        .extent = image1.info.resources,
    };
    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_PRE_SCENE_COLOR0">() &&
        image1.info.guest_address == 0x14244d8000) {
        LOG_INFO(Render_Vulkan,
                 "GoT color0 compute clear submit={} image_id={} values={}/{}/{}/{}",
                 diag_draw_trace.submit_index, image1_id.index, values[0], values[1], values[2],
                 values[3]);
    }
    runtime.ClearImage(&image1, range, clear);
    return true;
}

void Rasterizer::BindBuffers(const Shader::Info& stage, Shader::Backend::Bindings& binding,
                             Shader::PushData& push_data) {
    const u64 alignment = instance.StorageMinAlignment();
    static bool reported_scene_buffers[2]{};
    const int scene_phase = stage.pgm_hash == 0x167bdbe8 && diag_draw_trace.enabled
                                ? (diag_draw_trace.submit_index < 900
                                       ? 0
                                       : (diag_draw_trace.submit_index >= 1200 ? 1 : -1))
                                : -1;
    const bool trace_scene_buffers = Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_TEMPORAL_SCENE_INPUT">() &&
                                     scene_phase >= 0 && !reported_scene_buffers[scene_phase];
    u32 scene_buffer_index = 0;
    for (const auto& desc : stage.buffers) {
        if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_DRAW_STATE">() &&
            stage.pgm_hash == 0xbc086a4d && diag_draw_trace.submit_index >= 700) {
            static std::unordered_set<u32> reported_sky_vs_buffers;
            if (reported_sky_vs_buffers.insert(scene_buffer_index).second) {
                if (desc.IsSpecial()) {
                    LOG_INFO(Render_Vulkan, "GoT sky VS buffer index={} special={}",
                             scene_buffer_index, static_cast<u32>(desc.buffer_type));
                } else {
                    const auto sharp = desc.GetSharp(stage);
                    LOG_INFO(Render_Vulkan,
                             "GoT sky VS buffer index={} address={:#x} stride={} size={} "
                             "gpu_modified={}",
                             scene_buffer_index, sharp.base_address, sharp.GetStride(),
                             sharp.GetSize(), buffer_cache.IsRegionGpuModified(
                                                  sharp.base_address, sharp.GetSize()));
                    if (sharp.GetSize() >= 3 * 16 &&
                        memory->IsValidMapping(sharp.base_address, 3 * 16)) {
                        std::array<float, 12> positions{};
                        std::memcpy(positions.data(),
                                    reinterpret_cast<const void*>(sharp.base_address),
                                    sizeof(positions));
                        for (u32 vertex = 0; vertex < 3; ++vertex) {
                            const u32 i = vertex * 4;
                            LOG_INFO(Render_Vulkan,
                                     "GoT sky VS buffer vertex={} ({},{},{},{})", vertex,
                                     positions[i], positions[i + 1], positions[i + 2],
                                     positions[i + 3]);
                        }
                    }
                }
            }
        }
        if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_ORIGIN_WRITERS">() &&
            stage.pgm_hash == 0x8c5d11bd && !desc.IsSpecial()) {
            static std::unordered_set<u32> reported_origin_descriptors;
            if (reported_origin_descriptors.insert(scene_buffer_index).second) {
                const auto sharp = desc.GetSharp(stage);
                LOG_INFO(Render_Vulkan,
                         "GoT origin descriptor index={} address={:#x} size={} written={} "
                         "formatted={}",
                         scene_buffer_index, sharp.base_address, sharp.GetSize(),
                         desc.is_written, desc.is_formatted);
            }
        }
        if (trace_scene_buffers) {
            if (desc.IsSpecial()) {
                LOG_INFO(Render_Vulkan, "GoT scene phase={} submit={} buffer={} special={}",
                         scene_phase, diag_draw_trace.submit_index, scene_buffer_index,
                         static_cast<u32>(desc.buffer_type));
            } else {
                const auto sharp = desc.GetSharp(stage);
                LOG_INFO(Render_Vulkan,
                         "GoT scene phase={} submit={} buffer={} address={:#x} size={} written={}",
                         scene_phase, diag_draw_trace.submit_index, scene_buffer_index,
                         sharp.base_address, sharp.GetSize(), desc.is_written);
            }
        }
        if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_TILE_BUFFERS">() &&
            (stage.pgm_hash == 0xb7458b04 || stage.pgm_hash == 0xfabd68f2) &&
            !desc.IsSpecial()) {
            const auto sharp = desc.GetSharp(stage);
            static std::unordered_set<u64> reported_tile_buffers;
            const u64 key = (u64(stage.pgm_hash) << 32) ^ sharp.base_address ^
                            scene_buffer_index;
            if (reported_tile_buffers.insert(key).second) {
                LOG_INFO(Render_Vulkan,
                         "GoT sky tile buffer stage={:#x} index={} address={:#x} "
                         "size={} written={} formatted={} gpu_modified={}",
                         stage.pgm_hash, scene_buffer_index, sharp.base_address,
                         sharp.GetSize(), desc.is_written, desc.is_formatted,
                         buffer_cache.IsRegionGpuModified(sharp.base_address, sharp.GetSize()));
            }
        }
        if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_ORIGIN_WRITERS">() &&
            !desc.IsSpecial() && desc.is_written) {
            const auto sharp = desc.GetSharp(stage);
            for (u64 target : {0x1406a1bf00ull, 0x1406a24f00ull}) {
                if (sharp.base_address < target + 36864 &&
                    target < sharp.base_address + sharp.GetSize()) {
                    static std::unordered_set<u64> reported_origin_writers;
                    const u64 key = (u64(stage.pgm_hash) << 32) ^ sharp.base_address ^ target;
                    if (reported_origin_writers.insert(key).second) {
                        LOG_INFO(Render_Vulkan,
                                 "GoT sky origin writer submit={} shader={:#x} address={:#x} "
                                 "size={} target={:#x} index={} formatted={}",
                                 diag_draw_trace.submit_index, stage.pgm_hash,
                                 sharp.base_address, sharp.GetSize(), target,
                                 scene_buffer_index, desc.is_formatted);
                    }
                }
            }
        }
        if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_ORIGIN_WRITERS">() &&
            stage.pgm_hash == 0x8c5d11bd && !desc.IsSpecial() &&
            scene_buffer_index == 0) {
            const auto& regs = liverpool->GetCsRegs();
            diag_got_last_origin_x = regs.user_data[0];
            diag_got_last_origin_y = regs.user_data[1];
            static std::unordered_set<u64> reported_origin_offsets;
            const u64 key = (u64(regs.user_data[0]) << 32) | regs.user_data[1];
            if (reported_origin_offsets.insert(key).second) {
                LOG_INFO(Render_Vulkan,
                         "GoT origin generator submit={} offset={}/{} dims={}/{}/{} "
                         "sgpr4={} sgpr8={} sgpr10={:#x}",
                         diag_draw_trace.submit_index, regs.user_data[0], regs.user_data[1],
                         regs.dim_x, regs.dim_y, regs.dim_z, regs.user_data[4],
                         regs.user_data[8], regs.user_data[10]);
            }
        }
        ++scene_buffer_index;
        if (desc.IsSpecial()) {
            if (desc.buffer_type == Shader::BufferType::GdsBuffer) {
                const auto* gds_buf = buffer_cache.GetGdsBuffer();
                buffer_infos.emplace_back(gds_buf->Handle(), 0, gds_buf->SizeBytes());
                needs_barrier |=
                    runtime.IsBufferAccessed(gds_buf, 0, gds_buf->SizeBytes(), desc.is_written);
                bound_buffers.emplace_back(gds_buf, 0, gds_buf->SizeBytes(), desc.is_written);
            } else if (desc.buffer_type == Shader::BufferType::Flatbuf) {
                auto& vk_buffer = buffer_cache.GetStreamBuffer();
                const u32 ubo_size = stage.flattened_ud_buf.size() * sizeof(u32);
                const u64 offset =
                    vk_buffer.Copy(stage.flattened_ud_buf.data(), ubo_size, alignment);
                buffer_infos.emplace_back(vk_buffer.Handle(), offset, ubo_size);
            } else if (desc.buffer_type == Shader::BufferType::ClipPlanes) {
                // Permutations compiled without enabled planes never read the buffer, so the
                // declared binding is satisfied with a null descriptor instead of a copy.
                if (liverpool->regs.clipper_control.user_clip_plane_enable == 0) {
                    buffer_infos.emplace_back(VK_NULL_HANDLE, 0, VK_WHOLE_SIZE);
                } else {
                    auto& vk_buffer = buffer_cache.GetStreamBuffer();
                    std::array<float, AmdGpu::NUM_CLIP_PLANES * 4> planes{};
                    for (u32 i = 0; i < AmdGpu::NUM_CLIP_PLANES; ++i) {
                        const auto& plane = liverpool->regs.clip_user_data[i];
                        planes[i * 4 + 0] = std::bit_cast<float>(plane.data_x);
                        planes[i * 4 + 1] = std::bit_cast<float>(plane.data_y);
                        planes[i * 4 + 2] = std::bit_cast<float>(plane.data_z);
                        planes[i * 4 + 3] = std::bit_cast<float>(plane.data_w);
                    }
                    const u32 ubo_size = static_cast<u32>(sizeof(planes));
                    const u64 offset = vk_buffer.Copy(planes.data(), ubo_size, alignment);
                    buffer_infos.emplace_back(vk_buffer.Handle(), offset, ubo_size);
                }
            } else if (desc.buffer_type == Shader::BufferType::BdaPagetable) {
                const auto* bda_buffer = buffer_cache.GetBdaPageTableBuffer();
                buffer_infos.emplace_back(bda_buffer->Handle(), 0, bda_buffer->SizeBytes());
            } else if (desc.buffer_type == Shader::BufferType::FaultBuffer) {
                const auto* fault_buffer = buffer_cache.GetFaultBuffer();
                buffer_infos.emplace_back(fault_buffer->Handle(), 0, fault_buffer->SizeBytes());
            } else if (desc.buffer_type == Shader::BufferType::SharedMemory) {
                auto& lds_buffer = buffer_cache.GetStreamBuffer();
                const auto& cs_program = liverpool->GetCsRegs();
                const auto lds_size = cs_program.SharedMemSize() * cs_program.NumWorkgroups();
                if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SHARED_STORAGE_TRACE">()) {
                    static std::unordered_set<u32> reported;
                    if (reported.size() < 32 && reported.insert(stage.pgm_hash).second) {
                        LOG_INFO(Render_Vulkan,
                                 "LDS storage fallback shader={:#x} bytes_per_group={} "
                                 "groups={} total_bytes={}", stage.pgm_hash,
                                 cs_program.SharedMemSize(), cs_program.NumWorkgroups(), lds_size);
                    }
                }
                const auto [data, offset] = lds_buffer.Map(lds_size, alignment);
                std::memset(data, 0, lds_size);
                lds_buffer.Commit();
                buffer_infos.emplace_back(lds_buffer.Handle(), offset, lds_size);
            } else {
                UNREACHABLE_MSG("Unexpected buffer type {}", u32(desc.buffer_type));
            }
        } else {
            const auto vsharp = desc.GetSharp(stage);
            if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_INDIRECT_WRITERS">() && desc.is_written &&
                vsharp.base_address < 0x1403a0e000ull &&
                vsharp.GetSize() > 0x1403a0d000ull -
                                       std::min<u64>(vsharp.base_address, 0x1403a0d000ull)) {
                static std::unordered_set<u64> reported_writers;
                const u32 phase = diag_got_full_scene_submit != ~0u &&
                                          diag_draw_trace.submit_index > diag_got_full_scene_submit
                                      ? 1
                                      : 0;
                const u64 key = (static_cast<u64>(stage.pgm_hash) << 32) |
                                (static_cast<u64>(scene_buffer_index - 1) << 1) | phase;
                if (reported_writers.insert(key).second) {
                    LOG_INFO(Render_Vulkan,
                             "GoT indirect writer submit={} phase={} stage={:#x} buffer={} "
                             "address={:#x} size={} written={}",
                             diag_draw_trace.submit_index, phase, stage.pgm_hash,
                             scene_buffer_index - 1, vsharp.base_address, vsharp.GetSize(),
                             desc.is_written);
                }
            }
            if (vsharp.base_address == 0 || vsharp.GetSize() == 0) {
                buffer_infos.emplace_back(VK_NULL_HANDLE, 0, VK_WHOLE_SIZE);
            } else {
                const u64 size = memory->ClampRangeSize(vsharp.base_address, vsharp.GetSize());
                if (size != vsharp.GetSize()) {
                    // Report once per descriptor rather than per draw: this fires on every draw of
                    // the affected stage and used to bury the rest of the log.
                    static std::unordered_set<u64> reported_clamped;
                    const u64 key = (static_cast<u64>(stage.pgm_hash) << 32) ^
                                    (scene_buffer_index & 0xffffffff) ^
                                    (static_cast<u64>(vsharp.base_address) >> 8);
                    if (reported_clamped.insert(key).second) {
                        const auto* words = reinterpret_cast<const u32*>(&vsharp);
                        LOG_ERROR(Render,
                                  "Buffer descriptor exceeds its mapping: stage={:#x} buffer={} "
                                  "address={:#x} size={} clamped_to={} num_records={} stride={} "
                                  "raw=[{:#010x} {:#010x} {:#010x} {:#010x}]",
                                  stage.pgm_hash, scene_buffer_index - 1, vsharp.base_address,
                                  vsharp.GetSize(), size, vsharp.num_records,
                                  static_cast<u32>(vsharp.stride), words[0], words[1], words[2],
                                  words[3]);
                    }
                }
                if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_WEATHER_COPY_BUFFERS">() &&
                    stage.pgm_hash == 0xa415aaca) {
                    static std::unordered_set<u64> reported_weather_copy_buffers;
                    const u64 weather_copy_key =
                        (u64(scene_buffer_index - 1) << 48) ^ vsharp.base_address;
                    if (reported_weather_copy_buffers.insert(weather_copy_key).second) {
                        const auto alias = texture_cache.FindImageFromRange(
                            vsharp.base_address, size);
                        LOG_INFO(Render_Vulkan,
                                 "GoT weather copy buffer index={} address={:#x} size={} "
                                 "written={} formatted={} gpu_modified={} image_alias={} "
                                 "image_flags={:#x}",
                                 scene_buffer_index - 1, vsharp.base_address, size,
                                 desc.is_written, desc.is_formatted,
                                 buffer_cache.IsRegionGpuModified(vsharp.base_address, size),
                                 alias.index,
                                 alias ? static_cast<u32>(texture_cache.GetImage(alias).flags) : 0);
                    }
                }
                const auto [buffer, offset] = buffer_cache.ObtainBuffer(
                    vsharp.base_address, size, desc.is_written, desc.is_formatted);
                const u64 offset_aligned = Common::AlignDown(offset, alignment);
                const u64 adjust = offset - offset_aligned;
                if (adjust % 4 != 0) {
                    LOG_WARNING(Render_Vulkan, "Buffer binding in shader {:#x} isn't dword aligned",
                                stage.pgm_hash);
                }
                push_data.AddOffset(binding.buffer, adjust);
                buffer_infos.emplace_back(buffer->Handle(), offset_aligned, size + adjust);
                bound_buffers.emplace_back(buffer, offset, size, desc.is_written);
                if (desc.is_written) {
                    // Raw storage-buffer writes can also make an aliased cached image stale.
                    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_WEATHER_INVALIDATE">() &&
                        (vsharp.base_address == 0x144fd30000 ||
                         vsharp.base_address == 0x144fd78000 ||
                         vsharp.base_address == 0x144fd40000 ||
                         vsharp.base_address == 0x144fd88000)) {
                        LOG_INFO(Render_Vulkan,
                                 "GoT weather storage-buffer writer shader={:#x} "
                                 "address={:#x} size={}",
                                 stage.pgm_hash, vsharp.base_address, size);
                    }
                    texture_cache.InvalidateMemoryFromGPU(vsharp.base_address, size);
                }
                needs_barrier |= runtime.IsBufferAccessed(buffer, offset, size, desc.is_written);
            }
        }

        auto& set_write = set_writes[set_write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified++;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = 1;
        set_write.descriptorType = vk::DescriptorType::eStorageBuffer;
        set_write.pBufferInfo = &buffer_infos.back();
        ++binding.buffer;
    }
    if (trace_scene_buffers) {
        reported_scene_buffers[scene_phase] = true;
    }
}

void Rasterizer::BindTextures(const Shader::Info& stage, Shader::Backend::Bindings& binding) {
    image_bindings.clear();
    const u32 first_image_idx = image_infos.size();
    // To emulate storing to explicit mip levels, build a descriptor array with each mip level.
    boost::container::small_vector<u32, 8> image_descriptor_array_sizes;
    static const bool verbose_dynamic_diag =
        Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_VERBOSE_DYNAMIC_IMAGE">() != nullptr;
    static bool reported_scene_images[2]{};
    const int scene_phase = stage.pgm_hash == 0x167bdbe8 && diag_draw_trace.enabled
                                ? (diag_draw_trace.submit_index < 900
                                       ? 0
                                       : (diag_draw_trace.submit_index >= 1200 ? 1 : -1))
                                : -1;
    const bool trace_scene_images = Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_TEMPORAL_SCENE_INPUT">() &&
                                    scene_phase >= 0 && !reported_scene_images[scene_phase];
    if (trace_scene_images) {
        LOG_INFO(Render_Vulkan, "GoT scene phase={} submit={} srt={:#x}/{:#x}", scene_phase,
                 diag_draw_trace.submit_index, stage.user_data[0], stage.user_data[1]);
    }

    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_LERP_CONSTANTS">() &&
        stage.pgm_hash == 0x167bdbe8 && diag_draw_trace.submit_index >= 1200) {
        static bool reported_sky_lerp_constants = false;
        if (!reported_sky_lerp_constants) {
            reported_sky_lerp_constants = true;
            const u64 first_srt = u64(stage.user_data[0]) |
                                  (u64(stage.user_data[1]) << 32);
            u64 second_srt = 0;
            if (memory->IsValidMapping(first_srt + 98 * sizeof(u32), sizeof(second_srt))) {
                std::memcpy(&second_srt,
                            reinterpret_cast<const void*>(first_srt + 98 * sizeof(u32)),
                            sizeof(second_srt));
            }
            LOG_INFO(Render_Vulkan,
                     "GoT sky lerp SRT first={:#x} second={:#x} second_mapped={}",
                     first_srt, second_srt,
                     second_srt && memory->IsValidMapping(second_srt, 249 * sizeof(u32)));
            if (second_srt && memory->IsValidMapping(second_srt, 249 * sizeof(u32))) {
                const auto* words = reinterpret_cast<const u32*>(second_srt);
                for (u32 offset : {138u, 139u, 140u, 141u, 168u, 169u, 170u,
                                   225u, 226u, 231u, 243u, 244u, 245u, 246u,
                                   247u, 248u}) {
                    LOG_INFO(Render_Vulkan,
                             "GoT sky second-SRT dw={} bits={:#x} float={}", offset,
                             words[offset], std::bit_cast<float>(words[offset]));
                }
            }
        }
    }

    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_POST_CONSTANTS">() && stage.pgm_hash == 0xd9002625 &&
        diag_draw_trace.enabled && diag_draw_trace.submit_index >= 700) {
        static bool reported_post_constants = false;
        if (!reported_post_constants) {
            reported_post_constants = true;
            for (u32 offset : {95u, 107u, 108u, 109u, 118u, 119u, 120u, 121u,
                               123u, 124u, 125u, 126u, 127u, 128u}) {
                const u32 word = stage.ReadUdReg<u32>(0, offset);
                LOG_INFO(Render_Vulkan, "GoT post constant dw={} bits={:#x} float={}", offset,
                         word, std::bit_cast<float>(word));
            }
        }
    }
    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_PRODUCER_CONSTANTS">() &&
        stage.pgm_hash == 0x9c3cb720 && diag_draw_trace.enabled) {
        static bool reported_producer_phase[2]{};
        const u32 phase = diag_draw_trace.submit_index >= 1200 ? 1 : 0;
        if (!reported_producer_phase[phase]) {
            reported_producer_phase[phase] = true;
            for (u32 offset : {281u, 282u, 283u, 284u, 292u, 293u, 295u}) {
                const u32 word = stage.ReadUdReg<u32>(0, offset);
                LOG_INFO(Render_Vulkan,
                         "GoT sky producer constant phase={} submit={} dw={} bits={:#x} float={}",
                         phase, diag_draw_trace.submit_index, offset, word,
                         std::bit_cast<float>(word));
            }
        }
    }

    if (stage.pgm_hash == 0x2a3cacd4 &&
        Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SECONDARY_TABLE">()) {
        static bool reported_secondary_table = false;
        if (!reported_secondary_table) {
            const auto table = stage.ReadUdReg<AmdGpu::Buffer>(0, 141);
            LOG_INFO(Render_Vulkan,
                     "GoT secondary image table: base={:#x} stride={} records={} bytes={}",
                     table.base_address, table.GetStride(), table.num_records, table.GetSize());
            if (table.base_address && table.GetStride() == 60 && table.num_records <= 64) {
                for (u32 entry = 0; entry < table.num_records; ++entry) {
                    AmdGpu::Image element{};
                    std::memcpy(&element,
                                reinterpret_cast<const void*>(table.base_address + u64(entry) * 60),
                                sizeof(element));
                    LOG_INFO(Render_Vulkan,
                             "GoT secondary descriptor entry={} address={:#x} type={} format={} "
                             "num_format={} size={}x{} pitch={} levels={}",
                             entry, element.Address(), static_cast<u32>(element.GetType()),
                             static_cast<u32>(element.GetDataFmt()),
                             static_cast<u32>(element.GetNumberFmt()),
                             static_cast<u32>(element.width + 1),
                             static_cast<u32>(element.height + 1), element.Pitch(),
                             element.NumLevels());
                    u32 upper_image_words[4]{};
                    std::memcpy(upper_image_words,
                                reinterpret_cast<const void*>(table.base_address + u64(entry) * 60 + 16),
                                sizeof(upper_image_words));
                    LOG_INFO(Render_Vulkan,
                             "GoT secondary image T# dwords4-7 entry={} words={:#x}/{:#x}/{:#x}/{:#x}",
                             entry, upper_image_words[0], upper_image_words[1],
                             upper_image_words[2], upper_image_words[3]);
                }
            }
            reported_secondary_table = true;
        }
    }

    u32 image_index = 0;
    for (const auto& image_desc : stage.images) {
        const u32 current_image_index = image_index++;
        if (stage.pgm_hash == 0x2a3cacd4 && image_desc.dynamic_image_array &&
            Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_MATERIAL_TYPES">()) {
            static std::unordered_set<u32> reported_material_arrays;
            if (reported_material_arrays.insert(current_image_index).second) {
                for (u32 entry = 0; entry < image_desc.dynamic_image_count; ++entry) {
                    const auto element = image_desc.GetSharpAt(stage, entry);
                    LOG_INFO(Render_Vulkan,
                             "GoT material descriptor array={} entry={} address={:#x} "
                             "format={} num_format={} type={} size={}x{} pitch={} levels={} r128={}",
                             current_image_index, entry, element.Address(),
                             static_cast<u32>(element.GetDataFmt()),
                             static_cast<u32>(element.GetNumberFmt()),
                             static_cast<u32>(element.GetType()),
                             static_cast<u32>(element.width + 1),
                             static_cast<u32>(element.height + 1), element.Pitch(),
                             element.NumLevels(), image_desc.is_r128);
                }
            }
        }
        if (image_desc.dynamic_image_array) {
            static std::unordered_set<u64> reported_tables;
            const u64 table_key = (static_cast<u64>(image_desc.dynamic_table_ud_reg) << 48) ^
                                  (static_cast<u64>(image_desc.dynamic_table_dw_offset) << 32) ^
                                  (static_cast<u64>(image_desc.dynamic_image_byte_offset) << 16) ^
                                  image_desc.dynamic_image_stride;
            if (reported_tables.insert(table_key).second) {
                const auto table = stage.ReadUdReg<AmdGpu::Buffer>(
                    image_desc.dynamic_table_ud_reg, image_desc.dynamic_table_dw_offset);
                LOG_INFO(Render_Vulkan,
                         "GoT dynamic image table geometry: base={:#x} stride={} records={} "
                         "count={} byte_offset={} record_stride={}",
                         table.base_address, table.GetStride(), table.num_records,
                         image_desc.dynamic_image_count, image_desc.dynamic_image_byte_offset,
                         image_desc.dynamic_image_stride);
            }
        }
        if (stage.pgm_hash == 0x9716bed2 &&
            Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_RESOURCE_USE_GUARD">() &&
            image_desc.use_guard.offset != Shader::UNKNOWN_LOCATION) {
            const auto& guard = image_desc.use_guard;
            const u32 count = guard.offset < stage.flattened_ud_buf.size()
                                  ? stage.flattened_ud_buf[guard.offset] : UINT32_MAX;
            static std::unordered_set<u64> reported_guards;
            const u64 key = (u64(current_image_index) << 32) | count;
            if (reported_guards.insert(key).second) {
                LOG_INFO(Render_Vulkan,
                         "GoT image use guard: stage={:#x} image={} count_offset={} count={} "
                         "limit={} less_than={} active={} descriptor_offset={}",
                         stage.pgm_hash, current_image_index, guard.offset, count, guard.limit,
                         guard.less_than, guard.Active(stage.flattened_ud_buf),
                         image_desc.sharp_fetch.offsets[0]);
            }
        }
        auto tsharp = image_desc.dynamic_image_array ? image_desc.GetSharpAt(stage, 0)
                                                     : image_desc.GetSharp(stage);
        if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_LUT_WRITER_RESOURCES">() &&
            (stage.pgm_hash == 0x0f16a579 || stage.pgm_hash == 0x9c3cb720)) {
            static std::unordered_set<u64> reported_lut_writer_images;
            const u64 key = (static_cast<u64>(stage.pgm_hash) << 32) |
                            (static_cast<u64>(current_image_index) << 1) |
                            image_desc.is_written;
            if (reported_lut_writer_images.insert(key).second) {
                LOG_INFO(Render_Vulkan,
                         "GoT sky LUT writer resource shader={:#x} image={} address={:#x} "
                         "written={} format={}/{} size={}x{}x{} type={} tile={} dynamic={}",
                         stage.pgm_hash, current_image_index, tsharp.Address(),
                         image_desc.is_written, static_cast<u32>(tsharp.GetDataFmt()),
                         static_cast<u32>(tsharp.GetNumberFmt()),
                         static_cast<u32>(tsharp.width + 1),
                         static_cast<u32>(tsharp.height + 1),
                         static_cast<u32>(tsharp.depth + 1),
                         static_cast<u32>(tsharp.GetType()),
                         static_cast<u32>(tsharp.tiling_index),
                         image_desc.dynamic_image_array);
            }
        }
        if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_WEATHER_USERS">() &&
            (tsharp.Address() == 0x144fd30000 || tsharp.Address() == 0x144fd78000 ||
             tsharp.Address() == 0x144fd40000 || tsharp.Address() == 0x144fd88000)) {
            static std::unordered_set<u64> reported_weather_users;
            const u64 key = (static_cast<u64>(stage.pgm_hash) << 32) ^
                            (static_cast<u64>(current_image_index) << 48) ^ tsharp.Address() ^
                            (static_cast<u64>(image_desc.is_written) << 63);
            if (reported_weather_users.insert(key).second) {
                LOG_INFO(Render_Vulkan,
                         "GoT sky weather user stage={:#x} index={} address={:#x} written={} "
                         "size={}x{} format={}/{}",
                         stage.pgm_hash, current_image_index, tsharp.Address(),
                         image_desc.is_written, static_cast<u32>(tsharp.width + 1),
                         static_cast<u32>(tsharp.height + 1),
                         static_cast<u32>(tsharp.GetDataFmt()),
                         static_cast<u32>(tsharp.GetNumberFmt()));
            }
        }
        if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_NOISE_USERS">() && current_image_index == 0 &&
            (stage.pgm_hash == 0x3c2e229a || stage.pgm_hash == 0x11d5a4b7)) {
            static std::unordered_set<u32> reported_noise_tables;
            if (reported_noise_tables.insert(stage.pgm_hash).second) {
                const u64 srt = static_cast<u64>(stage.user_data[0]) |
                                (static_cast<u64>(stage.user_data[1]) << 32);
                LOG_INFO(Render_Vulkan,
                         "GoT sky noise SRT stage={:#x} srt={:#x} loop_count={} "
                         "offset16_mapped={} offset96_mapped={} weights={:#x},{:#x},{:#x},"
                         "{:#x},{:#x},{:#x} base={:#x}",
                         stage.pgm_hash, srt, stage.ReadUdReg<u32>(0, 66),
                         memory->IsValidMapping(srt + 16, 32),
                         memory->IsValidMapping(srt + 96, 32),
                         stage.ReadUdReg<u32>(0, 67), stage.ReadUdReg<u32>(0, 68),
                         stage.ReadUdReg<u32>(0, 69), stage.ReadUdReg<u32>(0, 70),
                         stage.ReadUdReg<u32>(0, 71), stage.ReadUdReg<u32>(0, 72),
                         stage.ReadUdReg<u32>(0, 73));
            }
        }
        if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_NOISE_USERS">() &&
            (tsharp.Address() == 0x1405f02200 || tsharp.Address() == 0x140699bf00 ||
             tsharp.Address() == 0x140695bf00 || tsharp.Address() == 0x14069dbf00)) {
            static std::unordered_set<u64> reported_noise_users;
            const u64 key = (static_cast<u64>(stage.pgm_hash) << 32) ^
                            (static_cast<u64>(current_image_index) << 48) ^ tsharp.Address() ^
                            (static_cast<u64>(image_desc.is_written) << 63);
            if (reported_noise_users.insert(key).second) {
                LOG_INFO(Render_Vulkan,
                         "GoT sky noise user stage={:#x} index={} address={:#x} written={} "
                         "size={}x{}x{} format={} num_format={} type={}",
                         stage.pgm_hash, current_image_index, tsharp.Address(),
                         image_desc.is_written, static_cast<u32>(tsharp.width + 1),
                         static_cast<u32>(tsharp.height + 1), static_cast<u32>(tsharp.depth + 1),
                         static_cast<u32>(tsharp.GetDataFmt()),
                         static_cast<u32>(tsharp.GetNumberFmt()),
                         static_cast<u32>(tsharp.GetType()));
            }
        }
        if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_TILE_RESOURCES">() &&
            (stage.pgm_hash == 0xb7458b04 || stage.pgm_hash == 0xfabd68f2 ||
             stage.pgm_hash == 0x3c2e229a || stage.pgm_hash == 0x11d5a4b7)) {
            static std::unordered_set<u64> reported_sky_images;
            const u64 key = (static_cast<u64>(stage.pgm_hash) << 32) | current_image_index;
            if (reported_sky_images.insert(key).second) {
                LOG_INFO(Render_Vulkan,
                         "GoT sky tile image stage={:#x} index={} address={:#x} format={} "
                         "num_format={} type={} size={}x{}x{} levels={} written={} "
                         "dst_sel={}/{}/{}/{}",
                         stage.pgm_hash, current_image_index, tsharp.Address(),
                         static_cast<u32>(tsharp.GetDataFmt()),
                         static_cast<u32>(tsharp.GetNumberFmt()),
                         static_cast<u32>(tsharp.GetType()), static_cast<u32>(tsharp.width + 1),
                         static_cast<u32>(tsharp.height + 1),
                         static_cast<u32>(tsharp.depth + 1), tsharp.NumLevels(),
                         image_desc.is_written, static_cast<u32>(tsharp.dst_sel_x),
                         static_cast<u32>(tsharp.dst_sel_y),
                         static_cast<u32>(tsharp.dst_sel_z),
                         static_cast<u32>(tsharp.dst_sel_w));
            }
        }
        if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SOURCE_WEATHER_WRITERS">() &&
            image_desc.is_written &&
            (tsharp.Address() == 0x144fd18000 || tsharp.Address() == 0x144fd28000 ||
             tsharp.Address() == 0x144fd60000 || tsharp.Address() == 0x144fd70000)) {
            static std::unordered_set<u64> reported_source_weather_storage;
            const u64 key = (u64(stage.pgm_hash) << 32) ^ tsharp.Address() ^
                            current_image_index;
            if (reported_source_weather_storage.insert(key).second) {
                LOG_INFO(Render_Vulkan,
                         "GoT source weather storage writer shader={:#x} index={} "
                         "address={:#x} format={} swizzle={}/{}/{}/{}",
                         stage.pgm_hash, current_image_index, tsharp.Address(),
                         static_cast<u32>(tsharp.GetDataFmt()), static_cast<u32>(tsharp.dst_sel_x),
                         static_cast<u32>(tsharp.dst_sel_y), static_cast<u32>(tsharp.dst_sel_z),
                         static_cast<u32>(tsharp.dst_sel_w));
            }
        }
        if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_TEMPORAL_SCENE_INPUT">() && diag_draw_trace.enabled &&
            image_desc.is_written &&
            (tsharp.Address() == 0x1406a30000 || tsharp.Address() == 0x1406c70000 ||
             tsharp.Address() == 0x1406eb0000)) {
            const u32 phase = diag_draw_trace.submit_index < 900 ? 0 :
                              diag_draw_trace.submit_index >= 1200 ? 1 : 2;
            static std::unordered_set<u64> reported_scene_writers;
            const u64 key = (static_cast<u64>(phase) << 60) ^
                            (static_cast<u64>(stage.pgm_hash) << 24) ^ tsharp.Address();
            if (phase != 2 && reported_scene_writers.insert(key).second) {
                LOG_INFO(Render_Vulkan,
                         "GoT scene rotating-image writer phase={} submit={} stage={:#x} image={} address={:#x}",
                         phase, diag_draw_trace.submit_index, stage.pgm_hash, current_image_index,
                         tsharp.Address());
            }
        }
        if (trace_scene_images) {
            LOG_INFO(Render_Vulkan,
                     "GoT scene phase={} submit={} image={} address={:#x} format={} size={}x{}x{} written={}",
                     scene_phase, diag_draw_trace.submit_index, current_image_index,
                     tsharp.Address(), static_cast<u32>(tsharp.GetDataFmt()),
                     static_cast<u32>(tsharp.width + 1), static_cast<u32>(tsharp.height + 1),
                     static_cast<u32>(tsharp.depth + 1), image_desc.is_written);
        }
        if (tsharp.Address() >= 0x144fa00000 && tsharp.Address() < 0x144fd80000 &&
            Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SMALL_LUT_PRODUCERS">()) {
            static std::unordered_set<u64> reported_small_lut_uses;
            const u64 key = (static_cast<u64>(stage.pgm_hash) << 32) ^
                            (static_cast<u64>(current_image_index) << 48) ^ tsharp.Address() ^
                            (static_cast<u64>(image_desc.is_written) << 63);
            if (reported_small_lut_uses.insert(key).second) {
                LOG_INFO(Render_Vulkan,
                         "GoT small LUT use: stage={:#x} image_index={} address={:#x} "
                         "written={} format={} size={}x{} type={}",
                         stage.pgm_hash, current_image_index, tsharp.Address(),
                         image_desc.is_written, static_cast<u32>(tsharp.GetDataFmt()),
                         static_cast<u32>(tsharp.width + 1), static_cast<u32>(tsharp.height + 1),
                         static_cast<u32>(tsharp.type));
            }
        }
        if (tsharp.Address() == 0x143dd50000 &&
            Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_3D_LUT_PRODUCER">()) {
            static std::unordered_set<u64> reported_lut_uses;
            const u64 key = (static_cast<u64>(stage.pgm_hash) << 32) |
                            (static_cast<u64>(current_image_index) << 1) | image_desc.is_written;
            if (reported_lut_uses.insert(key).second) {
                LOG_INFO(Render_Vulkan,
                         "GoT 3D LUT use: stage={:#x} image_index={} written={} "
                         "type={} depth={} tile={} srt={:#x}",
                         stage.pgm_hash, current_image_index, image_desc.is_written,
                         static_cast<u32>(tsharp.type), static_cast<u32>(tsharp.depth + 1),
                         static_cast<u32>(tsharp.tiling_index),
                         static_cast<u64>(stage.user_data[0]) |
                             (static_cast<u64>(stage.user_data[1]) << 32));
            }
        }
        if ((tsharp.Address() == 0x1448a40000 || tsharp.Address() == 0x14489c0000) &&
            Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_POST_LUT_USERS">()) {
            static std::unordered_set<u64> reported_post_lut_users;
            const u64 key = (u64(stage.pgm_hash) << 2) |
                            ((tsharp.Address() == 0x1448a40000) << 1) | image_desc.is_written;
            if (reported_post_lut_users.insert(key).second) {
                LOG_INFO(Render_Vulkan,
                         "GoT post LUT use: stage={:#x} image_index={} address={:#x} written={} type={} format={} depth={} tile={}",
                         stage.pgm_hash, current_image_index, tsharp.Address(), image_desc.is_written,
                         static_cast<u32>(tsharp.GetType()),
                         static_cast<u32>(tsharp.GetDataFmt()),
                         static_cast<u32>(tsharp.depth + 1),
                         static_cast<u32>(tsharp.tiling_index));
            }
        }
        if (stage.pgm_hash == 0xd9002625) {
            if (const char* input_text = Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_NULL_POST_INPUT">()) {
                char* end = nullptr;
                const long input_index = std::strtol(input_text, &end, 10);
                if (end != input_text && *end == '\0' && input_index >= 0 && input_index <= 6 &&
                    static_cast<u32>(input_index) == current_image_index) {
                    tsharp = AmdGpu::Image::Null(false);
                    static std::unordered_set<long> reported_null_inputs;
                    if (reported_null_inputs.insert(input_index).second) {
                        LOG_WARNING(Render_Vulkan,
                                    "GoT diagnostic nulled post-process input {}", input_index);
                    }
                }
            }
        }
        if (stage.pgm_hash == 0x167bdbe8 && current_image_index < 12) {
            if (const char* mask_text = Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_NULL_SKY_INPUT_MASK">()) {
                char* end = nullptr;
                const unsigned long mask = std::strtoul(mask_text, &end, 0);
                if (end != mask_text && *end == '\0' && mask <= 0xfff &&
                    (mask & (1ul << current_image_index))) {
                    tsharp = AmdGpu::Image::Null(false);
                    static std::unordered_set<u32> reported_sky_nulls;
                    if (reported_sky_nulls.insert(current_image_index).second) {
                        LOG_WARNING(Render_Vulkan,
                                    "GoT diagnostic nulled scene-pass input {}",
                                    current_image_index);
                    }
                }
            }
        }
        if (stage.pgm_hash == 0x6a242cff && current_image_index == 8 &&
            Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_NULL_LIGHT_COLOR">()) {
            tsharp = AmdGpu::Image::Null(false);
        }
        static AmdGpu::Image got_scene_input{};
        static bool got_scene_input_valid = false;
        static AmdGpu::Image got_gbuffer_color{};
        static bool got_gbuffer_color_valid = false;
        if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SHOW_GBUFFER0">()) {
            if (stage.pgm_hash == 0x6a242cff && current_image_index == 8) {
                got_gbuffer_color = tsharp;
                got_gbuffer_color_valid = true;
            } else if (stage.pgm_hash == 0x9f9aac14 && current_image_index == 0 &&
                       got_gbuffer_color_valid) {
                tsharp = got_gbuffer_color;
            }
        }
        if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SHOW_SCENE_INPUT">()) {
            if (stage.pgm_hash == 0xd9002625 && current_image_index == 1) {
                got_scene_input = tsharp;
                got_scene_input_valid = true;
            } else if (stage.pgm_hash == 0x9f9aac14 && current_image_index == 0 &&
                       got_scene_input_valid) {
                tsharp = got_scene_input;
                static bool reported_scene_substitution = false;
                if (!reported_scene_substitution) {
                    LOG_WARNING(Render_Vulkan,
                                "GoT diagnostic substituted scene image {:#x} into final pass",
                                tsharp.Address());
                    reported_scene_substitution = true;
                }
            }
        }
        if ((stage.pgm_hash == 0xdc800181 || stage.pgm_hash == 0x05b4953e ||
             stage.pgm_hash == 0x061a68f3 ||
             stage.pgm_hash == 0x9f9aac14 || stage.pgm_hash == 0x8753cf85 ||
             stage.pgm_hash == 0xd9002625 ||
             stage.pgm_hash == 0xf0bbe72e || stage.pgm_hash == 0x167bdbe8 ||
             stage.pgm_hash == 0x378b5341 || stage.pgm_hash == 0x0353be0a ||
             stage.pgm_hash == 0x39d732d2 || stage.pgm_hash == 0x2ecc4202 ||
             stage.pgm_hash == 0x9c3cb720 || stage.pgm_hash == 0x0f16a579 ||
             stage.pgm_hash == 0x6a5e57c7 || stage.pgm_hash == 0x0ceb269d ||
             stage.pgm_hash == 0x6a242cff) &&
            Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_FINAL_INPUT">()) {
            static std::unordered_set<u64> reported_inputs;
            const u64 key = (static_cast<u64>(stage.pgm_hash) << 32) ^
                            (static_cast<u64>(current_image_index) << 48) ^ tsharp.Address();
            if (reported_inputs.insert(key).second) {
                LOG_INFO(Render_Vulkan,
                         "GoT background chain input: fs={:#x} image_index={} address={:#x} "
                         "format={} num_format={} width={} height={} pitch={} tile={} type={} "
                         "levels={} layers={} swizzle={}/{}/{}/{} srt={:#x}",
                         stage.pgm_hash, current_image_index, tsharp.Address(),
                         static_cast<u32>(tsharp.GetDataFmt()),
                         static_cast<u32>(tsharp.GetNumberFmt()), static_cast<u32>(tsharp.width + 1),
                         static_cast<u32>(tsharp.height + 1), tsharp.Pitch(),
                         static_cast<u32>(tsharp.tiling_index), static_cast<u32>(tsharp.type),
                         tsharp.NumLevels(), tsharp.NumLayers(), static_cast<u32>(tsharp.dst_sel_x),
                         static_cast<u32>(tsharp.dst_sel_y), static_cast<u32>(tsharp.dst_sel_z),
                         static_cast<u32>(tsharp.dst_sel_w),
                         static_cast<u64>(stage.user_data[0]) |
                             (static_cast<u64>(stage.user_data[1]) << 32));
            }
        }
        if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_RT0_CONSUMERS">() && diag_draw_trace.enabled &&
            diag_draw_trace.submit_index >= 500 && tsharp.Address() == 0x14244d8000) {
            static std::unordered_set<u64> reported_consumers;
            const u64 key = (static_cast<u64>(stage.pgm_hash) << 32) | current_image_index;
            if (reported_consumers.insert(key).second) {
                LOG_INFO(Render_Vulkan,
                         "GoT RT0 consumer submit={} draw={} stage={:#x} image_index={} "
                         "format={} num_format={} size={}x{}",
                         diag_draw_trace.submit_index, diag_draw_trace.draw_index,
                         stage.pgm_hash, current_image_index,
                         static_cast<u32>(tsharp.GetDataFmt()),
                         static_cast<u32>(tsharp.GetNumberFmt()),
                         static_cast<u32>(tsharp.width + 1),
                         static_cast<u32>(tsharp.height + 1));
            }
        }
        if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SCENE_STORAGE_WRITERS">() &&
            diag_draw_trace.enabled && diag_draw_trace.submit_index >= 500 &&
            image_desc.is_written && tsharp.Address() == 0x1426100000) {
            static std::unordered_set<u64> reported_scene_writers;
            const u64 key = (static_cast<u64>(stage.pgm_hash) << 32) | current_image_index;
            if (reported_scene_writers.insert(key).second) {
                LOG_INFO(Render_Vulkan,
                         "GoT scene storage writer submit={} draw={} stage={:#x} "
                         "image_index={} format={} num_format={} size={}x{}",
                         diag_draw_trace.submit_index, diag_draw_trace.draw_index,
                         stage.pgm_hash, current_image_index,
                         static_cast<u32>(tsharp.GetDataFmt()),
                         static_cast<u32>(tsharp.GetNumberFmt()),
                         static_cast<u32>(tsharp.width + 1),
                         static_cast<u32>(tsharp.height + 1));
            }
        }
        if (verbose_dynamic_diag && image_desc.dynamic_image_array) {
            LOG_INFO(Render_Vulkan, "GoT dynamic image diagnostic entering BindTextures index={}",
                     current_image_index);
            LOG_INFO(Render_Vulkan,
                     "GoT dynamic image diagnostic resolved first image address={:#x}",
                     tsharp.Address());
        }
        if (stage.pgm_hash == 0xff484786 && current_image_index == 2) {
            if (const char* forced_index = Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_IMAGE_INDEX">()) {
                const auto table = stage.ReadUdReg<AmdGpu::Buffer>(0, 92);
                char* end = nullptr;
                const long index = std::strtol(forced_index, &end, 10);
                if (*forced_index && !*end && index >= 0 &&
                    static_cast<u64>(index) < table.num_records) {
                    const u64 addr = table.base_address +
                                     static_cast<u64>(index) * table.GetStride() + 64;
                    if (memory->IsValidGpuMapping(addr, sizeof(tsharp))) {
                        std::memcpy(&tsharp, reinterpret_cast<const void*>(addr), sizeof(tsharp));
                        static std::unordered_set<long> reported_forced_images;
                        if (reported_forced_images.insert(index).second) {
                            LOG_WARNING(Render_Vulkan,
                                        "GoT forced image index={} descriptor={:#x} format={}",
                                        index, tsharp.Address(),
                                        static_cast<u32>(tsharp.GetDataFmt()));
                        }
                    }
                }
            }
        }
        if (AmdGpu::IsFmask(tsharp.GetDataFmt())) {
            LOG_WARNING(Render_Vulkan,
                        "FMask descriptor reached Vulkan binding: shader={:#x} address={:#x} data_format={} num_format={} {}x{} written={}",
                        stage.pgm_hash, tsharp.Address(),
                        static_cast<u32>(tsharp.GetDataFmt()),
                        static_cast<u32>(tsharp.GetNumberFmt()),
                        static_cast<u32>(tsharp.width + 1),
                        static_cast<u32>(tsharp.height + 1), image_desc.is_written);
            // FMask reads are specialized to identity in the recompiler. If a runtime
            // descriptor becomes FMask for a storage binding, leave it unbound as well.
            const u32 null_count = image_desc.NumBindings(stage);
            for (u32 i = 0; i < null_count; ++i) {
                image_bindings.emplace_back(std::piecewise_construct, std::tuple{}, std::tuple{});
            }
            image_descriptor_array_sizes.push_back(null_count);
            continue;
        }
        if (const auto meta_type = texture_cache.IsMeta(tsharp.Address())) {
            if (Common::DiagnosticEnv<"SHADPS4_DIAG_LOG_META_IMAGES">() != nullptr) {
                static std::unordered_set<u64> reported_meta_images;
                const u64 key = static_cast<u64>(stage.pgm_hash) ^ tsharp.Address();
                if (reported_meta_images.insert(key).second) {
                    LOG_WARNING(Render_Vulkan,
                                "Metadata image read: shader={:#x} stage={} image_index={} address={:#x} meta_type={} data_format={} num_format={}",
                                stage.pgm_hash, static_cast<u32>(stage.hw_stage),
                                current_image_index, tsharp.Address(),
                                static_cast<u32>(*meta_type),
                                static_cast<u32>(tsharp.GetDataFmt()),
                                static_cast<u32>(tsharp.GetNumberFmt()));
                }
            } else {
                LOG_WARNING(Render_Vulkan, "Unexpected metadata read by a shader (texture)");
            }
        }

        const auto data_fmt = tsharp.GetDataFmt();
        const auto num_fmt = tsharp.GetNumberFmt();
        if (tsharp.Address() == 0 || data_fmt == AmdGpu::DataFormat::FormatInvalid) {
            if (Common::DiagnosticEnv<"SHADPS4_DIAG_LOG_NULL_IMAGES">() != nullptr) {
                bool has_unknown_location = false;
                for (const auto offset : image_desc.sharp_fetch.offsets) {
                    has_unknown_location |= offset == Shader::UNKNOWN_LOCATION;
                }
                if (has_unknown_location) {
                    static std::unordered_set<u64> reported_images;
                    const u64 key = (static_cast<u64>(stage.pgm_hash) << 32) | current_image_index;
                    if (reported_images.insert(key).second) {
                        LOG_WARNING(Render_Vulkan,
                                    "Live runtime-selected image is null: shader={:#x} stage={} image_index={} written={}",
                                    stage.pgm_hash, static_cast<u32>(stage.hw_stage),
                                    current_image_index, image_desc.is_written);
                        if (stage.pgm_hash == 0xff484786 && current_image_index == 2) {
                            const auto table = stage.ReadUdReg<AmdGpu::Buffer>(0, 92);
                            LOG_WARNING(Render_Vulkan,
                                        "GoT dynamic image table: srt={:#x} base={:#x} stride={} records={} bytes={}",
                                        static_cast<u64>(stage.user_data[0]) |
                                            (static_cast<u64>(stage.user_data[1]) << 32),
                                        table.base_address, table.GetStride(),
                                        table.num_records, table.GetSize());
                        }
                    }
                }
            }
            const u32 null_count = image_desc.NumBindings(stage);
            for (u32 i = 0; i < null_count; ++i) {
                image_bindings.emplace_back(std::piecewise_construct, std::tuple{}, std::tuple{});
            }
            image_descriptor_array_sizes.push_back(null_count);
            continue;
        }

        // Inactive uniform branches are resolved by ImageResource::GetSharp; an unsupported
        // Vulkan format alone must not cause an active guest resource to be discarded.
        auto effective_num_fmt = num_fmt;
        if (image_desc.is_atomic_u32) {
            effective_num_fmt = AmdGpu::NumberFormat::Uint;
        } else if (image_desc.is_written && effective_num_fmt == AmdGpu::NumberFormat::Srgb) {
            effective_num_fmt = AmdGpu::NumberFormat::Unorm;
        }
        if (!memory->IsValidGpuMapping(tsharp.Address(), 0) ||
            !magic_enum::enum_contains(data_fmt) || !magic_enum::enum_contains(num_fmt)) {
            static std::unordered_set<u64> reported_bad_descriptors;
            const u64 key =
                (static_cast<u64>(stage.pgm_hash) << 32) | (current_image_index & 0xffffffff) |
                (static_cast<u64>(static_cast<u32>(data_fmt)) << 8) ^
                (static_cast<u64>(static_cast<u32>(effective_num_fmt)) << 4);
            if (reported_bad_descriptors.insert(key).second) {
                const auto* words = reinterpret_cast<const u32*>(&tsharp);
                LOG_ERROR(Render_Vulkan,
                          "Rejected unusable image descriptor: shader={:#x} stage={} image_index={} "
                          "written={} srt={:#x} raw=[{:#010x} {:#010x} {:#010x} {:#010x} "
                          "{:#010x} {:#010x} {:#010x} {:#010x}] data_format={} num_format={} "
                          "effective_num_format={} type={} tiling={} downsample_count={}",
                          stage.pgm_hash, static_cast<u32>(stage.hw_stage), current_image_index,
                          image_desc.is_written,
                          static_cast<u64>(stage.user_data[0]) |
                              (static_cast<u64>(stage.user_data[1]) << 32),
                          words[0], words[1], words[2], words[3], words[4], words[5], words[6],
                          words[7], static_cast<u32>(data_fmt), static_cast<u32>(num_fmt),
                          static_cast<u32>(effective_num_fmt), static_cast<u32>(tsharp.type),
                          static_cast<u32>(tsharp.tiling_index),
                          stage.pgm_hash == 0x9716bed2 ? stage.ReadUdReg<u32>(0, 0) : 0);
            }
            const u32 null_count = image_desc.NumBindings(stage);
            for (u32 i = 0; i < null_count; ++i) {
                image_bindings.emplace_back(std::piecewise_construct, std::tuple{}, std::tuple{});
            }
            image_descriptor_array_sizes.push_back(null_count);
            continue;
        }

        const Shader::MipStorageFallbackMode mip_fallback_mode = image_desc.mip_fallback_mode;
        const u32 num_bindings = image_desc.NumBindings(stage);

        for (auto i = 0; i < num_bindings; i++) {
            const auto element_sharp = image_desc.dynamic_image_array
                                           ? image_desc.GetSharpAt(
                                                 stage,
                                                 Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_DUPLICATE_IMAGE">()
                                                     ? 0
                                                     : i)
                                           : tsharp;
            if (verbose_dynamic_diag && image_desc.dynamic_image_array) {
                LOG_INFO(Render_Vulkan,
                         "GoT dynamic image diagnostic candidate={} address={:#x} format={} type={}",
                         i, element_sharp.Address(),
                         static_cast<u32>(element_sharp.GetDataFmt()),
                         static_cast<u32>(element_sharp.GetType()));
            }
            if (image_desc.dynamic_image_array &&
                (element_sharp.Address() == 0 ||
                 !memory->IsValidGpuMapping(element_sharp.Address(), 0))) {
                image_bindings.emplace_back(std::piecewise_construct, std::tuple{}, std::tuple{});
                continue;
            }
            auto& [image_id, desc] = image_bindings.emplace_back(
                std::piecewise_construct, std::tuple{}, std::tuple{element_sharp, image_desc});

            if (mip_fallback_mode == Shader::MipStorageFallbackMode::ConstantIndex) {
                ASSERT(num_bindings == 1);
                desc.view_info.range.base.level += image_desc.constant_mip_index;
                desc.view_info.range.extent.levels = 1;
            } else if (mip_fallback_mode == Shader::MipStorageFallbackMode::DynamicIndex) {
                desc.view_info.range.base.level += i;
                desc.view_info.range.extent.levels = 1;
            }

            image_id = texture_cache.FindImage(desc);
            if (verbose_dynamic_diag && image_desc.dynamic_image_array) {
                LOG_INFO(Render_Vulkan, "GoT dynamic image diagnostic cached candidate={}", i);
            }
            auto* image = &texture_cache.GetImage(image_id);
            if (auto depth_image_id = texture_cache.GetAssociatedDepth(*image)) {
                // The association is keyed on address and may outlive a stencil surface.
                // Only stencil-compatible views can access the associated depth image.
                if (LiverpoolToVK::IsFormatStencilCompatible(desc.view_info.format)) {
                    image_id = depth_image_id;
                    image = &texture_cache.GetImage(image_id);
                }
            }
            if (image->binding.is_bound) {
                // The image is already bound. In case if it is about to be used as storage we
                // need to force general layout on it.
                image->binding.force_general |= image_desc.is_written;
            }
            image->binding.is_bound = 1u;
        }

        image_descriptor_array_sizes.push_back(num_bindings);
    }

    // Second pass to re-bind images that were updated after binding
    u32 diagnostic_pass_index = 0;
    for (auto& [image_id, desc] : image_bindings) {
        if (stage.pgm_hash == 0xff484786 && verbose_dynamic_diag) {
            LOG_INFO(Render_Vulkan,
                     "GoT dynamic image diagnostic second pass={} image_id={}",
                     diagnostic_pass_index, image_id.index);
        }
        ++diagnostic_pass_index;
        bool is_storage = desc.type == VideoCore::TextureCache::BindingType::Storage;
        if (!image_id) {
            image_infos.emplace_back(VK_NULL_HANDLE, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
        } else {
            if (auto& old_image = texture_cache.GetImage(image_id);
                old_image.binding.needs_rebind) {
                old_image.binding = {};
                image_id = texture_cache.FindImage(desc);
            }

            bound_images.emplace_back(image_id);

            if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_NOISE_OUTPUT_READBACK">() && is_storage &&
                (stage.pgm_hash == 0x3c2e229a || stage.pgm_hash == 0x11d5a4b7)) {
                diag_got_sky_noise_output_id = image_id;
            }

            if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_NOISE_SOURCE_READBACK">() &&
                stage.pgm_hash == 0x3c2e229a && diagnostic_pass_index <= 6 &&
                diag_draw_trace.submit_index >= 1200) {
                static std::unordered_set<u32> captured_source_indices;
                if (captured_source_indices.insert(diagnostic_pass_index).second) {
                    auto& source_image = texture_cache.GetImage(image_id);
                    if (source_image.info.num_bits == 8 && source_image.info.size.width == 64 &&
                        source_image.info.size.height == 64 &&
                        source_image.info.size.depth == 64) {
                        const u64 byte_count = u64(source_image.info.pitch) * 64 * 64;
                        const auto download = runtime.GetStagingPool().Request(
                            byte_count, VideoCore::MemoryType::HostCached, 16, true);
                        const vk::BufferImageCopy copy = {
                            .bufferOffset = download.offset,
                            .bufferRowLength = source_image.info.pitch,
                            .bufferImageHeight = 64,
                            .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                                 .mipLevel = 0,
                                                 .baseArrayLayer = 0,
                                                 .layerCount = 1},
                            .imageOffset = {0, 0, 0},
                            .imageExtent = {64, 64, 64},
                        };
                        runtime.DownloadImage(&source_image, download.buffer,
                                              std::span{&copy, 1});
                        scheduler.Finish();
                        download.Invalidate();
                        const auto* bytes = reinterpret_cast<const u8*>(download.mapped);
                        u64 nonzero = 0;
                        u64 sum = 0;
                        for (u64 j = 0; j < byte_count; ++j) {
                            nonzero += bytes[j] != 0;
                            sum += bytes[j];
                        }
                        LOG_INFO(Render_Vulkan,
                                 "GoT sky source index={} address={:#x} nonzero={} sum={}",
                                 diagnostic_pass_index - 1, source_image.info.guest_address,
                                 nonzero, sum);
                        runtime.GetStagingPool().FreeDeferred(download);
                    }
                }
            }

            if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_LUT_SOURCE_READBACK">() &&
                (stage.pgm_hash == 0x0f16a579 || stage.pgm_hash == 0x9c3cb720) &&
                desc.type != VideoCore::TextureCache::BindingType::Storage &&
                diag_draw_trace.submit_index >= 700) {
                auto& source_image = texture_cache.GetImage(image_id);
                static std::unordered_set<u64> captured_lut_sources;
                if (captured_lut_sources.insert(desc.info.guest_address).second &&
                    ((source_image.info.size.width == 256 && source_image.info.size.height == 32) ||
                     (source_image.info.size.width == 384 && source_image.info.size.height == 384)) &&
                    source_image.info.num_bits % 8 == 0 && !source_image.info.props.is_block) {
                    const u32 bpp = source_image.info.num_bits / 8;
                    const u64 size = u64(source_image.info.pitch) * source_image.info.size.height * bpp;
                    const auto download = runtime.GetStagingPool().Request(
                        size, VideoCore::MemoryType::HostCached, 16, true);
                    const vk::BufferImageCopy copy = {
                        .bufferOffset = download.offset,
                        .bufferRowLength = source_image.info.pitch,
                        .bufferImageHeight = source_image.info.size.height,
                        .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                             .mipLevel = 0,
                                             .baseArrayLayer = 0,
                                             .layerCount = 1},
                        .imageOffset = {0, 0, 0},
                        .imageExtent = {source_image.info.size.width, source_image.info.size.height, 1},
                    };
                    runtime.DownloadImage(&source_image, download.buffer, std::span{&copy, 1});
                    scheduler.Finish();
                    download.Invalidate();
                    const auto* bytes = reinterpret_cast<const u8*>(download.mapped);
                    u64 gpu_nonzero = 0;
                    for (u64 j = 0; j < size; ++j) {
                        gpu_nonzero += bytes[j] != 0;
                    }
                    u64 alpha_nonzero = 0;
                    if (bpp == 8) {
                        for (u64 j = 0; j < size / 8; ++j) {
                            alpha_nonzero += bytes[j * 8 + 6] != 0 || bytes[j * 8 + 7] != 0;
                        }
                    }
                    u64 guest_nonzero = 0;
                    const bool guest_valid = memory->IsValidMapping(desc.info.guest_address, size);
                    if (guest_valid) {
                        const auto* guest = reinterpret_cast<const u8*>(desc.info.guest_address);
                        for (u64 j = 0; j < size; ++j) {
                            guest_nonzero += guest[j] != 0;
                        }
                    }
                    LOG_INFO(Render_Vulkan,
                             "GoT sky LUT source shader={:#x} address={:#x} size={}x{} "
                             "bpp={} gpu_nonzero_bytes={} alpha_nonzero_pixels={} "
                             "guest_valid={} guest_nonzero_bytes={}",
                             stage.pgm_hash, desc.info.guest_address, source_image.info.size.width,
                             source_image.info.size.height, bpp, gpu_nonzero, alpha_nonzero, guest_valid,
                             guest_nonzero);
                    runtime.GetStagingPool().FreeDeferred(download);
                }
            }
            if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_WEATHER_READBACK">() &&
                stage.pgm_hash == 0xb7458b04 &&
                (diagnostic_pass_index == 2 || diagnostic_pass_index == 3 ||
                 diagnostic_pass_index == 4 || diagnostic_pass_index == 6) &&
                diag_draw_trace.submit_index >= [] {
                    const char* value = Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_WEATHER_MIN_SUBMIT">();
                    return value ? static_cast<u32>(std::strtoul(value, nullptr, 10)) : 1200u;
                }()) {
                static std::unordered_set<u64> captured_weather_inputs;
                const u32 weather_phase = diag_draw_trace.submit_index < 710 ? 0 :
                                          diag_draw_trace.submit_index < 750 ? 1 :
                                          diag_draw_trace.submit_index < 900 ? 2 :
                                          diag_draw_trace.submit_index < 1200 ? 3 : 4;
                const u64 weather_key = (u64(weather_phase) << 32) | diagnostic_pass_index;
                if (captured_weather_inputs.insert(weather_key).second) {
                    auto& weather_image = texture_cache.GetImage(image_id);
                    const u32 bytes_per_pixel = weather_image.info.num_bits / 8;
                    if (weather_image.info.size.width == 256 &&
                        weather_image.info.size.height == 32 && bytes_per_pixel >= 4) {
                        const u64 byte_count = u64(weather_image.info.pitch) * 32 * bytes_per_pixel;
                        const auto download = runtime.GetStagingPool().Request(
                            byte_count, VideoCore::MemoryType::HostCached, 16, true);
                        const vk::BufferImageCopy copy = {
                            .bufferOffset = download.offset,
                            .bufferRowLength = weather_image.info.pitch,
                            .bufferImageHeight = 32,
                            .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                                 .mipLevel = 0,
                                                 .baseArrayLayer = 0,
                                                 .layerCount = 1},
                            .imageOffset = {0, 0, 0},
                            .imageExtent = {256, 32, 1},
                        };
                        runtime.DownloadImage(&weather_image, download.buffer,
                                              std::span{&copy, 1});
                        scheduler.Finish();
                        download.Invalidate();
                        const auto* bytes = reinterpret_cast<const u8*>(download.mapped);
                        u64 nonzero_pixels = 0;
                        u64 nonzero_alpha = 0;
                        u16 min_alpha = 0xffff;
                        u16 max_alpha = 0;
                        for (u32 y = 0; y < 32; ++y) {
                            for (u32 x = 0; x < 256; ++x) {
                                const auto* pixel = bytes +
                                    (u64(y) * weather_image.info.pitch + x) * bytes_per_pixel;
                                bool nonzero = false;
                                for (u32 c = 0; c < bytes_per_pixel; ++c) {
                                    nonzero |= pixel[c] != 0;
                                }
                                nonzero_pixels += nonzero;
                                if (bytes_per_pixel == 8) {
                                    const u16 alpha = u16(pixel[6]) | (u16(pixel[7]) << 8);
                                    nonzero_alpha += alpha != 0;
                                    min_alpha = std::min(min_alpha, alpha);
                                    max_alpha = std::max(max_alpha, alpha);
                                }
                            }
                        }
                        LOG_INFO(Render_Vulkan,
                                 "GoT sky weather input phase={} submit={} index={} address={:#x} "
                                 "image_id={} flags={:#x} bpp={} "
                                 "nonzero_pixels={} nonzero_alpha={} alpha_bits_min={:#x} "
                                 "alpha_bits_max={:#x}",
                                 weather_phase, diag_draw_trace.submit_index,
                                 diagnostic_pass_index - 1, weather_image.info.guest_address,
                                 image_id.index, static_cast<u32>(weather_image.flags),
                                 bytes_per_pixel, nonzero_pixels, nonzero_alpha, min_alpha,
                                 max_alpha);
                        const u64 guest_bytes = u64(weather_image.info.pitch) * 32 * bytes_per_pixel;
                        if (memory->IsValidMapping(weather_image.info.guest_address,
                                                   guest_bytes)) {
                            const auto* guest = reinterpret_cast<const u8*>(
                                weather_image.info.guest_address);
                            u64 guest_nonzero = 0;
                            for (u64 j = 0; j < guest_bytes; ++j) {
                                guest_nonzero += guest[j] != 0;
                            }
                            LOG_INFO(Render_Vulkan,
                                     "GoT sky weather guest address={:#x} nonzero_bytes={} "
                                     "bytes={}",
                                     weather_image.info.guest_address, guest_nonzero,
                                     guest_bytes);
                        }
                        runtime.GetStagingPool().FreeDeferred(download);
                    }
                }
            }

            if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_NOISE_READBACK">() &&
                stage.pgm_hash == 0xb7458b04 &&
                (diagnostic_pass_index == 5 ||
                 (diagnostic_pass_index >= 7 && diagnostic_pass_index <= 9)) &&
                diag_draw_trace.submit_index >= 1200) {
                static std::unordered_set<u32> captured_noise_inputs;
                if (captured_noise_inputs.insert(diagnostic_pass_index).second) {
                    auto& noise_image = texture_cache.GetImage(image_id);
                    if (noise_image.info.num_bits == 8 && noise_image.info.size.width == 64 &&
                        noise_image.info.size.height == 64 &&
                        noise_image.info.size.depth == 64) {
                        const u64 byte_count = u64(noise_image.info.pitch) * 64 * 64;
                        const auto download = runtime.GetStagingPool().Request(
                            byte_count, VideoCore::MemoryType::HostCached, 16, true);
                        const vk::BufferImageCopy copy = {
                            .bufferOffset = download.offset,
                            .bufferRowLength = noise_image.info.pitch,
                            .bufferImageHeight = 64,
                            .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                                 .mipLevel = 0,
                                                 .baseArrayLayer = 0,
                                                 .layerCount = 1},
                            .imageOffset = {0, 0, 0},
                            .imageExtent = {64, 64, 64},
                        };
                        runtime.DownloadImage(&noise_image, download.buffer,
                                              std::span{&copy, 1});
                        scheduler.Finish();
                        download.Invalidate();
                        const auto* bytes = reinterpret_cast<const u8*>(download.mapped);
                        u64 nonzero = 0;
                        u64 sum = 0;
                        u8 min_value = 255;
                        u8 max_value = 0;
                        for (u64 j = 0; j < byte_count; ++j) {
                            nonzero += bytes[j] != 0;
                            sum += bytes[j];
                            min_value = std::min(min_value, bytes[j]);
                            max_value = std::max(max_value, bytes[j]);
                        }
                        LOG_INFO(Render_Vulkan,
                                 "GoT sky noise input index={} address={:#x} bytes={} nonzero={} "
                                 "sum={} min={} max={}",
                                 diagnostic_pass_index - 1, noise_image.info.guest_address,
                                 byte_count, nonzero, sum, min_value, max_value);
                        runtime.GetStagingPool().FreeDeferred(download);
                    }
                }
            }

            if ((Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_TILE_READBACK">() ||
                 Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_ATLAS_SEQUENCE">()) &&
                (stage.pgm_hash == 0xb7458b04 || stage.pgm_hash == 0xfabd68f2) &&
                diagnostic_pass_index == 1) {
                diag_got_sky_tile_id = image_id;
            }

            if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_FEEDBACK_IDS">() &&
                stage.pgm_hash == 0x6a242cff && diagnostic_pass_index == 9) {
                static bool reported_feedback_sample = false;
                if (!reported_feedback_sample) {
                    LOG_INFO(Render_Vulkan, "GoT feedback sampled image_id={}", image_id.index);
                    reported_feedback_sample = true;
                }
            }

            auto& image = texture_cache.GetImage(image_id);
            const auto flags_before_find_texture = image.flags;
            if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_ATLAS_SEQUENCE">() &&
                stage.pgm_hash == 0x167bdbe8 && diagnostic_pass_index <= 2 &&
                diag_draw_trace.submit_index >= 700 && diag_draw_trace.submit_index < 1200) {
                static u64 last_consumer_addresses[2]{};
                const u32 input = diagnostic_pass_index - 1;
                if (last_consumer_addresses[input] != desc.info.guest_address) {
                    last_consumer_addresses[input] = desc.info.guest_address;
                    LOG_INFO(Render_Vulkan,
                             "GoT atlas consumer submit={} input={} image={} address={:#x} flags={:#x}",
                             diag_draw_trace.submit_index, input, image_id.index,
                             desc.info.guest_address, static_cast<u32>(image.flags));
                }
            }
            if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_TILE_REFRESH">() &&
                stage.pgm_hash == 0x167bdbe8 && diagnostic_pass_index <= 2 &&
                (diag_draw_trace.submit_index < 750 ||
                 diag_draw_trace.submit_index >= 1200)) {
                static std::unordered_set<u64> reported_tile_bindings;
                const u32 bucket = diag_draw_trace.submit_index < 750 ? 0u : 1u;
                const u64 key = (u64(bucket) << 48) ^ desc.info.guest_address;
                if (reported_tile_bindings.insert(key).second) {
                    LOG_INFO(Render_Vulkan,
                             "GoT tile atlas consumer submit={} index={} address={:#x} "
                             "image_id={} flags={:#x}",
                             diag_draw_trace.submit_index, diagnostic_pass_index - 1,
                             desc.info.guest_address, image_id.index,
                             static_cast<u32>(image.flags));
                }
            }
            auto& image_view = texture_cache.FindTexture(image_id, desc);
            if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_WEATHER_POST_BIND_READBACK">() &&
                stage.pgm_hash == 0xb7458b04 && diag_draw_trace.submit_index >= 700 &&
                diag_draw_trace.submit_index < 750 &&
                True(flags_before_find_texture & VideoCore::ImageFlagBits::Dirty) &&
                (desc.info.guest_address == 0x144fd30000 ||
                 desc.info.guest_address == 0x144fd78000 ||
                 desc.info.guest_address == 0x144fd40000 ||
                 desc.info.guest_address == 0x144fd88000)) {
                static std::unordered_set<u64> captured_post_bind_weather;
                if (captured_post_bind_weather.insert(desc.info.guest_address).second) {
                    const u32 bpp = image.info.num_bits / 8;
                    const u64 size = u64(image.info.pitch) * image.info.size.height * bpp;
                    const auto download = runtime.GetStagingPool().Request(
                        size, VideoCore::MemoryType::HostCached, 16, true);
                    const vk::BufferImageCopy copy = {
                        .bufferOffset = download.offset,
                        .bufferRowLength = image.info.pitch,
                        .bufferImageHeight = image.info.size.height,
                        .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                             .mipLevel = 0,
                                             .baseArrayLayer = 0,
                                             .layerCount = 1},
                        .imageOffset = {0, 0, 0},
                        .imageExtent = {image.info.size.width, image.info.size.height, 1},
                    };
                    runtime.DownloadImage(&image, download.buffer, std::span{&copy, 1});
                    scheduler.Finish();
                    download.Invalidate();
                    const auto* bytes = reinterpret_cast<const u8*>(download.mapped);
                    u64 nonzero_bytes = 0;
                    for (u64 j = 0; j < size; ++j) {
                        nonzero_bytes += bytes[j] != 0;
                    }
                    u64 alpha_nonzero_pixels = 0;
                    if (bpp == 8) {
                        for (u64 j = 0; j < size / 8; ++j) {
                            alpha_nonzero_pixels +=
                                bytes[j * 8 + 6] != 0 || bytes[j * 8 + 7] != 0;
                        }
                    }
                    LOG_INFO(Render_Vulkan,
                             "GoT weather post-bind submit={} address={:#x} image_id={} "
                             "flags_before={:#x} flags_after={:#x} nonzero_bytes={} "
                             "alpha_nonzero_pixels={} size={}",
                             diag_draw_trace.submit_index, desc.info.guest_address,
                             image_id.index, static_cast<u32>(flags_before_find_texture),
                             static_cast<u32>(image.flags), nonzero_bytes,
                             alpha_nonzero_pixels, size);
                    runtime.GetStagingPool().FreeDeferred(download);
                }
            }
            if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_MATERIAL_HOST_IMAGE">() &&
                stage.pgm_hash == 0x2a3cacd4 &&
                (desc.info.guest_address == 0x122f82e700 ||
                 desc.info.guest_address == 0x122f338f00)) {
                static bool reported_material_host[2]{};
                const u32 phase = desc.info.guest_address == 0x122f82e700 ? 0 : 1;
                if (!reported_material_host[phase]) {
                    reported_material_host[phase] = true;
                    LOG_INFO(Render_Vulkan,
                             "GoT material host phase={} submit={} guest={:#x} image_id={} "
                             "image_uid={} format={} bits={} extent={}x{} pitch={} "
                             "guest_size={} host_size={} flags={:#x} hash={:#x}",
                             phase, diag_draw_trace.submit_index, desc.info.guest_address,
                             image_id.index, image.image_uid,
                             static_cast<u32>(image.info.pixel_format), image.info.num_bits,
                             image.info.size.width, image.info.size.height, image.info.pitch,
                             image.info.guest_size, image.GetHostImageSize(),
                             static_cast<u32>(image.flags), image.hash);
                    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_MATERIAL_HOST_READBACK">() &&
                        image.info.props.is_block && image.info.num_bits == 64 &&
                        image.info.size.depth == 1) {
                        const u32 width = image.info.size.width;
                        const u32 height = image.info.size.height;
                        const u64 byte_count = u64((width + 3) / 4) * ((height + 3) / 4) * 8;
                        const auto download = runtime.GetStagingPool().Request(
                            byte_count, VideoCore::MemoryType::HostCached, 16, true);
                        const vk::BufferImageCopy copy = {
                            .bufferOffset = download.offset,
                            .bufferRowLength = 0,
                            .bufferImageHeight = 0,
                            .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                                 .mipLevel = 0,
                                                 .baseArrayLayer = 0,
                                                 .layerCount = 1},
                            .imageOffset = {0, 0, 0},
                            .imageExtent = {width, height, 1},
                        };
                        runtime.DownloadImage(&image, download.buffer, std::span{&copy, 1});
                        scheduler.Finish();
                        download.Invalidate();
                        const auto* gpu_bytes = reinterpret_cast<const u8*>(download.mapped);
                        u64 gpu_nonzero = 0;
                        u64 gpu_hash = 14695981039346656037ull;
                        for (u64 byte = 0; byte < byte_count; ++byte) {
                            gpu_nonzero += gpu_bytes[byte] != 0;
                            gpu_hash ^= gpu_bytes[byte];
                            gpu_hash *= 1099511628211ull;
                        }
                        u64 guest_nonzero = 0;
                        const u64 guest_bytes = std::min<u64>(image.info.guest_size, 4096);
                        if (memory->IsValidMapping(desc.info.guest_address, guest_bytes)) {
                            const auto* guest_data =
                                reinterpret_cast<const u8*>(desc.info.guest_address);
                            for (u64 byte = 0; byte < guest_bytes; ++byte) {
                                guest_nonzero += guest_data[byte] != 0;
                            }
                        }
                        LOG_INFO(Render_Vulkan,
                                 "GoT material host readback phase={} submit={} guest={:#x} "
                                 "gpu_bytes={} gpu_nonzero={} gpu_hash={:#x} "
                                 "guest_head_bytes={} guest_head_nonzero={}",
                                 phase, diag_draw_trace.submit_index, desc.info.guest_address,
                                 byte_count, gpu_nonzero, gpu_hash, guest_bytes, guest_nonzero);
                        runtime.GetStagingPool().FreeDeferred(download);
                    }
                }
            }
            if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_TEMPORAL_SCENE_READBACK">() &&
                stage.pgm_hash == 0x167bdbe8 && diagnostic_pass_index <= 2 &&
                image.info.num_bits == 32 && image.info.size.width == 768 &&
                image.info.size.height == 768 && diag_draw_trace.enabled) {
                const u32 submit = diag_draw_trace.submit_index;
                const u32 target_submit = [] {
                    const char* value = Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_TEMPORAL_TARGET_SUBMIT">();
                    return value ? static_cast<u32>(std::strtoul(value, nullptr, 10)) : 700u;
                }();
                const int phase = submit >= 1200 ? 1
                                  : (submit >= target_submit && submit < 900 ? 0 : -1);
                static u32 captured_scene_inputs[2]{};
                const u32 bit = 1u << (diagnostic_pass_index - 1);
                if (phase >= 0 && !(captured_scene_inputs[phase] & bit)) {
                    const u64 word_count = u64(image.info.pitch) * image.info.size.height;
                    const auto download = runtime.GetStagingPool().Request(
                        word_count * sizeof(u32), VideoCore::MemoryType::HostCached, 16, true);
                    const vk::BufferImageCopy copy = {
                        .bufferOffset = download.offset,
                        .bufferRowLength = image.info.pitch,
                        .bufferImageHeight = image.info.size.height,
                        .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                             .mipLevel = 0,
                                             .baseArrayLayer = 0,
                                             .layerCount = 1},
                        .imageOffset = {0, 0, 0},
                        .imageExtent = {image.info.size.width, image.info.size.height, 1},
                    };
                    runtime.DownloadImage(&image, download.buffer, std::span{&copy, 1});
                    scheduler.Finish();
                    download.Invalidate();
                    const auto* words = reinterpret_cast<const u32*>(download.mapped);
                    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_TEMPORAL_SCENE_RAW">()) {
                        const std::string raw_path = fmt::format(
                            "scene-input-s{}-index{}.r11g11b10.raw", submit,
                            diagnostic_pass_index - 1);
                        std::ofstream raw_output(raw_path, std::ios::binary);
                        raw_output.write(reinterpret_cast<const char*>(words),
                                         word_count * sizeof(u32));
                        LOG_INFO(Render_Vulkan, "GoT scene input raw capture={} pitch={}",
                                 raw_path, image.info.pitch);
                    }
                    u64 nonzero = 0;
                    u64 blue_zero = 0;
                    u64 blue_below_one = 0;
                    u64 blue_one = 0;
                    u64 blue_above_one = 0;
                    u64 sum = 0;
                    u32 xor_words = 0;
                    for (u64 i = 0; i < word_count; ++i) {
                        nonzero += words[i] != 0;
                        if (words[i] != 0) {
                            const u32 blue = (words[i] >> 22) & 0x3ff;
                            blue_zero += blue == 0;
                            blue_below_one += blue < 0x1e0;
                            blue_one += blue == 0x1e0;
                            blue_above_one += blue > 0x1e0;
                        }
                        sum += words[i];
                        xor_words ^= words[i];
                    }
                    LOG_INFO(Render_Vulkan,
                             "GoT scene input readback phase={} submit={} index={} address={:#x} image_id={} words={} nonzero={} sum={} xor={:#x} first={:#x} middle={:#x} last={:#x}",
                             phase, submit, diagnostic_pass_index - 1, desc.info.guest_address,
                             image_id.index, word_count, nonzero, sum, xor_words, words[0],
                             words[word_count / 2], words[word_count - 1]);
                    LOG_INFO(Render_Vulkan,
                             "GoT scene input blue phase={} submit={} index={} zero={} "
                             "below_one={} one={} above_one={}",
                             phase, submit, diagnostic_pass_index - 1, blue_zero,
                             blue_below_one, blue_one, blue_above_one);
                    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_TEMPORAL_SCENE_PPM">()) {
                        const auto unpack = [](u32 bits, u32 mantissa_bits) {
                            const u32 mantissa_mask = (1u << mantissa_bits) - 1;
                            const u32 mantissa = bits & mantissa_mask;
                            const u32 exponent = bits >> mantissa_bits;
                            if (exponent == 31) {
                                return 0.f;
                            }
                            if (exponent == 0) {
                                return std::ldexp(static_cast<float>(mantissa),
                                                  -14 - static_cast<int>(mantissa_bits));
                            }
                            return std::ldexp(static_cast<float>((1u << mantissa_bits) + mantissa),
                                              static_cast<int>(exponent) - 15 -
                                                  static_cast<int>(mantissa_bits));
                        };
                        const std::string path = fmt::format(
                            "temporal-phase{}-input{}-submit{}.ppm", phase,
                            diagnostic_pass_index - 1, submit);
                        std::ofstream ppm(path, std::ios::binary);
                        if (!ppm) {
                            LOG_WARNING(Render_Vulkan, "GoT scene input image export failed: {}", path);
                        }
                        ppm << "P6\n" << image.info.size.width << ' ' << image.info.size.height
                            << "\n255\n";
                        for (u32 y = 0; y < image.info.size.height; ++y) {
                            for (u32 x = 0; x < image.info.size.width; ++x) {
                                const u32 pixel = words[u64(y) * image.info.pitch + x];
                                const float rgb[3] = {unpack(pixel & 0x7ffu, 6),
                                                      unpack((pixel >> 11) & 0x7ffu, 6),
                                                      unpack(pixel >> 22, 5)};
                                for (const float channel : rgb) {
                                    const u8 value = static_cast<u8>(
                                        std::clamp(channel / 8.f, 0.f, 1.f) * 255.f);
                                    ppm.write(reinterpret_cast<const char*>(&value), 1);
                                }
                            }
                        }
                    }
                    runtime.GetStagingPool().FreeDeferred(download);
                    captured_scene_inputs[phase] |= bit;
                }
            }
            if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_ADAPTIVE_VOLUME_READBACK">() &&
                (stage.pgm_hash == 0x05b4953e || stage.pgm_hash == 0x061a68f3) &&
                desc.info.guest_address == 0x14489c0000 && image.info.num_bits == 32 &&
                diag_draw_trace.enabled && diag_draw_trace.submit_index >= 1000) {
                static u32 captured_volume_stages = 0;
                const u32 stage_bit = stage.pgm_hash == 0x05b4953e ? 1u : 2u;
                if (!(captured_volume_stages & stage_bit)) {
                    captured_volume_stages |= stage_bit;
                    const u32 width = image.info.size.width;
                    const u32 height = image.info.size.height;
                    const u32 depth = image.info.size.depth;
                    const u64 pixel_count = u64(image.info.pitch) * height * depth;
                    const auto download = runtime.GetStagingPool().Request(
                        pixel_count * sizeof(u16) * 2, VideoCore::MemoryType::HostCached, 16,
                        true);
                    const vk::BufferImageCopy copy = {
                        .bufferOffset = download.offset,
                        .bufferRowLength = image.info.pitch,
                        .bufferImageHeight = height,
                        .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                             .mipLevel = 0,
                                             .baseArrayLayer = 0,
                                             .layerCount = 1},
                        .imageOffset = {0, 0, 0},
                        .imageExtent = {width, height, depth},
                    };
                    runtime.DownloadImage(&image, download.buffer, std::span{&copy, 1});
                    scheduler.Finish();
                    download.Invalidate();
                    const auto* halves = reinterpret_cast<const u16*>(download.mapped);
                    std::array<u64, 2> positive{};
                    std::array<u64, 2> above_one{};
                    std::array<u64, 2> exponent_31{};
                    std::array<u64, 2> nonfinite{};
                    std::array<float, 2> maxima{};
                    std::array<u16, 2> max_bits{};
                    std::array<u64, 2> max_index{};
                    for (u64 pixel = 0; pixel < pixel_count; ++pixel) {
                        for (u32 channel = 0; channel < 2; ++channel) {
                            const u16 bits = halves[pixel * 2 + channel];
                            const float value = DiagHalfToFloat(bits);
                            exponent_31[channel] += ((bits >> 10) & 31) == 31;
                            if (std::isfinite(value)) {
                                positive[channel] += value > 0.f;
                                above_one[channel] += value > 1.f;
                                if (value > maxima[channel]) {
                                    maxima[channel] = value;
                                    max_bits[channel] = bits;
                                    max_index[channel] = pixel;
                                }
                            } else {
                                ++nonfinite[channel];
                            }
                        }
                    }
                    LOG_INFO(Render_Vulkan,
                             "GoT adaptive volume submit={} before stage={:#x} image_id={} format={} extent={}x{}x{} positive={}/{} above_one={}/{} max={}/{} max_bits={:#x}/{:#x} max_index={}/{} exponent31={}/{} nonfinite={}/{}",
                             diag_draw_trace.submit_index, stage.pgm_hash, image_id.index,
                             vk::to_string(image.info.pixel_format), width, height, depth,
                             positive[0], positive[1], above_one[0], above_one[1], maxima[0],
                             maxima[1], max_bits[0], max_bits[1], max_index[0], max_index[1],
                             exponent_31[0], exponent_31[1], nonfinite[0], nonfinite[1]);
                    runtime.GetStagingPool().FreeDeferred(download);
                }
            }
            if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_POST_LUT_READBACK">() &&
                stage.pgm_hash == 0xd9002625 &&
                desc.info.guest_address == 0x1448a40000 && image.info.num_bits == 16 &&
                diag_draw_trace.enabled && diag_draw_trace.submit_index >= 1000) {
                static bool captured_post_lut = false;
                if (!captured_post_lut) {
                    captured_post_lut = true;
                    const u32 width = image.info.size.width;
                    const u32 height = image.info.size.height;
                    const u32 depth = image.info.size.depth;
                    const u64 count = u64(image.info.pitch) * height * depth;
                    const auto download = runtime.GetStagingPool().Request(
                        count * sizeof(u16), VideoCore::MemoryType::HostCached, 16, true);
                    const vk::BufferImageCopy copy = {
                        .bufferOffset = download.offset,
                        .bufferRowLength = image.info.pitch,
                        .bufferImageHeight = height,
                        .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                             .mipLevel = 0,
                                             .baseArrayLayer = 0,
                                             .layerCount = 1},
                        .imageOffset = {0, 0, 0},
                        .imageExtent = {width, height, depth},
                    };
                    runtime.DownloadImage(&image, download.buffer, std::span{&copy, 1});
                    scheduler.Finish();
                    download.Invalidate();
                    const auto* values = reinterpret_cast<const u16*>(download.mapped);
                    float min_value = std::numeric_limits<float>::max();
                    float max_value = -std::numeric_limits<float>::max();
                    double sum = 0;
                    u64 nonfinite = 0;
                    u64 above_one = 0;
                    std::ofstream atlas("Build/got-ir-audit/post-lut-slices.pgm", std::ios::binary);
                    std::ofstream slice_values("Build/got-ir-audit/post-lut-slices.txt");
                    atlas << "P5\n" << width << ' ' << height * depth << "\n255\n";
                    for (u32 z = 0; z < depth; ++z) {
                        float slice_min = std::numeric_limits<float>::max();
                        float slice_max = -std::numeric_limits<float>::max();
                        double slice_sum = 0;
                        for (u32 y = 0; y < height; ++y) {
                            for (u32 x = 0; x < width; ++x) {
                                const float value = DiagHalfToFloat(
                                    values[(u64(z) * height + y) * image.info.pitch + x]);
                                if (std::isfinite(value)) {
                                    min_value = std::min(min_value, value);
                                    max_value = std::max(max_value, value);
                                    sum += value;
                                    above_one += value > 1.f;
                                    slice_min = std::min(slice_min, value);
                                    slice_max = std::max(slice_max, value);
                                    slice_sum += value;
                                } else {
                                    ++nonfinite;
                                }
                                const u8 pixel = std::isfinite(value)
                                                     ? static_cast<u8>(std::clamp(value, 0.f, 1.f) * 255.f)
                                                     : 255;
                                atlas.write(reinterpret_cast<const char*>(&pixel), 1);
                            }
                        }
                        slice_values << z << ' ' << slice_min << ' ' << slice_max << ' '
                                     << slice_sum / (u64(width) * height) << '\n';
                    }
                    LOG_INFO(Render_Vulkan,
                             "GoT post LUT submit={} image id={} extent={}x{}x{} pitch={} min={} max={} mean={} above_one={} nonfinite={}",
                             diag_draw_trace.submit_index, image_id.index, width, height, depth, image.info.pitch, min_value,
                             max_value, sum / (u64(width) * height * depth), above_one, nonfinite);
                }
            }
            static const u32 color0_capture_min_submit = [] {
                const char* value = Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_COLOR0_MIN_SUBMIT">();
                return value ? static_cast<u32>(std::strtoul(value, nullptr, 10)) : 500u;
            }();
            if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_GBUFFER_ALL_READBACK">() &&
                stage.pgm_hash == 0x6a242cff && diag_draw_trace.enabled &&
                (diag_draw_trace.submit_index >= 1200 ||
                 (diag_draw_trace.submit_index < 900 &&
                  diag_draw_trace.scene_issued >= 19)) &&
                diag_got_scene_draw_submit == diag_draw_trace.submit_index &&
                image.info.size.width == 1920 && image.info.size.height == 1080 &&
                !image.info.props.is_block && image.info.num_bits % 8 == 0) {
                constexpr std::array<u64, 4> targets{
                    0x14244d8000, 0x1424cd0000, 0x1403a10000, 0x1425cc0000};
                const u32 phase = diag_draw_trace.submit_index >= 1200 ? 1 : 0;
                static u32 captured_targets[2]{};
                const auto it = std::ranges::find(targets, desc.info.guest_address);
                if (it != targets.end()) {
                    const u32 target = static_cast<u32>(it - targets.begin());
                    const u32 bit = 1u << target;
                    if (!(captured_targets[phase] & bit)) {
                        captured_targets[phase] |= bit;
                        const u32 bytes_per_pixel = image.info.num_bits / 8;
                        const u64 pixel_count = u64(image.info.pitch) * image.info.size.height;
                        const u64 byte_count = pixel_count * bytes_per_pixel;
                        const auto download = runtime.GetStagingPool().Request(
                            byte_count, VideoCore::MemoryType::HostCached, 16, true);
                        const vk::BufferImageCopy copy = {
                            .bufferOffset = download.offset,
                            .bufferRowLength = image.info.pitch,
                            .bufferImageHeight = image.info.size.height,
                            .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                                 .mipLevel = 0,
                                                 .baseArrayLayer = 0,
                                                 .layerCount = 1},
                            .imageOffset = {0, 0, 0},
                            .imageExtent = {image.info.size.width, image.info.size.height, 1},
                        };
                        runtime.DownloadImage(&image, download.buffer, std::span{&copy, 1});
                        scheduler.Finish();
                        download.Invalidate();
                        const auto* bytes = reinterpret_cast<const u8*>(download.mapped);
                        u64 nonzero_pixels = 0;
                        u64 nonzero_bytes = 0;
                        std::array<std::array<u32, 256>, 4> channel_histogram{};
                        for (u64 pixel = 0; pixel < pixel_count; ++pixel) {
                            bool nonzero_pixel = false;
                            for (u32 channel = 0; channel < bytes_per_pixel; ++channel) {
                                const bool nonzero =
                                    bytes[pixel * bytes_per_pixel + channel] != 0;
                                nonzero_bytes += nonzero;
                                nonzero_pixel |= nonzero;
                            }
                            nonzero_pixels += nonzero_pixel;
                            if (target == 2 && nonzero_pixel && bytes_per_pixel == 4) {
                                for (u32 channel = 0; channel < 4; ++channel) {
                                    ++channel_histogram[channel]
                                                       [bytes[pixel * bytes_per_pixel + channel]];
                                }
                            }
                        }
                        LOG_INFO(Render_Vulkan,
                                 "GoT G-buffer readback phase={} submit={} target={} guest={:#x} "
                                 "format={} bits={} image_id={} pixels={} nonzero_pixels={} "
                                 "nonzero_bytes={}",
                                 phase, diag_draw_trace.submit_index, target,
                                 desc.info.guest_address,
                                 static_cast<u32>(image.info.pixel_format),
                                 image.info.num_bits, image_id.index, pixel_count,
                                 nonzero_pixels, nonzero_bytes);
                        if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_GBUFFER_MASK_PGM">()) {
                            const std::string path = fmt::format(
                                "Build/got-ir-audit/gbuffer-s{}-target{}-mask.pgm",
                                diag_draw_trace.submit_index, target);
                            std::ofstream mask(path, std::ios::binary);
                            mask << "P5\n" << image.info.size.width << ' '
                                 << image.info.size.height << "\n255\n";
                            for (u32 y = 0; y < image.info.size.height; ++y) {
                                for (u32 x = 0; x < image.info.size.width; ++x) {
                                    const u64 offset =
                                        (u64(y) * image.info.pitch + x) * bytes_per_pixel;
                                    bool nonzero = false;
                                    for (u32 channel = 0; channel < bytes_per_pixel; ++channel) {
                                        nonzero |= bytes[offset + channel] != 0;
                                    }
                                    const u8 value = nonzero ? 255 : 0;
                                    mask.write(reinterpret_cast<const char*>(&value), 1);
                                }
                            }
                        }
                        if (target == 0 && bytes_per_pixel == 4 &&
                            Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_GBUFFER_COLOR_PPM">()) {
                            const std::string path = fmt::format(
                                "Build/got-ir-audit/gbuffer-s{}-target0-color.ppm",
                                diag_draw_trace.submit_index);
                            std::ofstream color(path, std::ios::binary);
                            color << "P6\n" << image.info.size.width << ' '
                                  << image.info.size.height << "\n255\n";
                            for (u32 y = 0; y < image.info.size.height; ++y) {
                                for (u32 x = 0; x < image.info.size.width; ++x) {
                                    const u64 offset =
                                        (u64(y) * image.info.pitch + x) * bytes_per_pixel;
                                    color.write(reinterpret_cast<const char*>(bytes + offset), 3);
                                }
                            }
                        }
                        if (target == 2 && bytes_per_pixel == 4) {
                            for (u32 channel = 0; channel < 4; ++channel) {
                                const auto& histogram = channel_histogram[channel];
                                const auto dominant = std::ranges::max_element(histogram);
                                LOG_INFO(Render_Vulkan,
                                         "GoT G-buffer target2 phase={} channel={} dominant_byte={:#x} "
                                         "count={} among_nonzero={}",
                                         phase, channel,
                                         static_cast<u32>(dominant - histogram.begin()),
                                         *dominant, nonzero_pixels);
                            }
                        }
                        runtime.GetStagingPool().FreeDeferred(download);
                    }
                }
            }
            if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_COLOR0_READBACK">() &&
                stage.pgm_hash == 0x6a242cff &&
                desc.info.guest_address == 0x14244d8000 && image.info.num_bits == 32 &&
                diag_draw_trace.enabled &&
                diag_draw_trace.submit_index >= color0_capture_min_submit &&
                diag_got_scene_draw_submit == diag_draw_trace.submit_index) {
                static bool reported_color0 = false;
                if (!reported_color0) {
                    const u64 word_count = static_cast<u64>(image.info.pitch) *
                                           image.info.size.height * image.info.size.depth;
                    const auto download = runtime.GetStagingPool().Request(
                        word_count * sizeof(u32), VideoCore::MemoryType::HostCached, 16, true);
                    const vk::BufferImageCopy copy = {
                        .bufferOffset = download.offset,
                        .bufferRowLength = image.info.pitch,
                        .bufferImageHeight = image.info.size.height,
                        .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                             .mipLevel = 0,
                                             .baseArrayLayer = 0,
                                             .layerCount = 1},
                        .imageOffset = {0, 0, 0},
                        .imageExtent = {image.info.size.width, image.info.size.height,
                                        image.info.size.depth},
                    };
                    runtime.DownloadImage(&image, download.buffer, std::span{&copy, 1});
                    scheduler.Finish();
                    download.Invalidate();
                    const auto* words = reinterpret_cast<const u32*>(download.mapped);
                    u64 nonzero_words = 0;
                    u32 xor_words = 0;
                    for (u64 i = 0; i < word_count; ++i) {
                        nonzero_words += words[i] != 0;
                        xor_words ^= words[i];
                    }
                    LOG_INFO(Render_Vulkan,
                             "GoT color0 readback submit={} words={} nonzero={} xor={:#x}",
                             diag_draw_trace.submit_index, word_count, nonzero_words, xor_words);
                    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_COLOR0_PPM">()) {
                        const u32 width = image.info.size.width;
                        const u32 height = image.info.size.height;
                        const std::string output_path = fmt::format(
                            "Build/got-ir-audit/color0-s{}-rgba8.ppm",
                            diag_draw_trace.submit_index);
                        std::ofstream output(output_path, std::ios::binary);
                        output << "P6\n" << width << ' ' << height << "\n255\n";
                        for (u32 y = 0; y < height; ++y) {
                            for (u32 x = 0; x < width; ++x) {
                                const u32 pixel = words[u64(y) * image.info.pitch + x];
                                const char rgb[3] = {
                                    static_cast<char>(pixel & 0xff),
                                    static_cast<char>((pixel >> 8) & 0xff),
                                    static_cast<char>((pixel >> 16) & 0xff),
                                };
                                output.write(rgb, sizeof(rgb));
                            }
                        }
                        LOG_INFO(Render_Vulkan, "GoT color0 RGBA8 capture={}", output_path);
                    }
                    runtime.GetStagingPool().FreeDeferred(download);
                    reported_color0 = true;
                }
            }
            const bool head_volume_mode =
                Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_HEAD_VOLUME_CAPTURE">();
            const bool is_head_volume =
                (stage.pgm_hash == 0x6a5e57c7 &&
                 (desc.info.guest_address == 0x144fa00000 ||
                  desc.info.guest_address == 0x144fb00000)) ||
                (stage.pgm_hash == 0x0ceb269d &&
                 (desc.info.guest_address == 0x144fb80000 ||
                  desc.info.guest_address == 0x144fc80000));
            if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_FP16_CHAIN_READBACK">() &&
                desc.type != VideoCore::TextureCache::BindingType::Storage &&
                ((head_volume_mode && is_head_volume) ||
                 (!head_volume_mode &&
                  (((stage.pgm_hash == 0x6a5e57c7 || stage.pgm_hash == 0x0ceb269d ||
                     stage.pgm_hash == 0x167bdbe8) &&
                    desc.info.guest_address >= 0x144fa00000 &&
                    desc.info.guest_address < 0x144fd80000) ||
                   (stage.pgm_hash == 0xd9002625 &&
                    desc.info.guest_address == 0x1426100000)))) &&
                (image.info.num_bits == 32 || image.info.num_bits == 64)) {
                static std::unordered_set<u64> reported_fp16_images;
                static u32 scene_readbacks = 0;
                const bool is_scene = desc.info.guest_address == 0x1426100000;
                const bool head_mode = head_volume_mode;
                const bool capture_head = head_mode && !is_scene;
                const u32 readback_index = is_scene ? ++scene_readbacks : 1;
                if ((is_scene && !head_mode && diag_draw_trace.enabled &&
                     diag_draw_trace.submit_index >= 500 && readback_index <= 2000 &&
                     (readback_index % 60 == 0)) ||
                    (!is_scene && (!capture_head || (diag_draw_trace.enabled &&
                                                    diag_draw_trace.submit_index >= 700)) &&
                     reported_fp16_images.insert(desc.info.guest_address).second)) {
                    const u64 byte_count = static_cast<u64>(image.info.pitch) *
                                           image.info.size.height * image.info.size.depth *
                                           (image.info.num_bits / 8);
                    const auto download = runtime.GetStagingPool().Request(
                        byte_count, VideoCore::MemoryType::HostCached, 16, true);
                    const vk::BufferImageCopy copy = {
                        .bufferOffset = download.offset,
                        .bufferRowLength = image.info.pitch,
                        .bufferImageHeight = image.info.size.height,
                        .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                             .mipLevel = 0,
                                             .baseArrayLayer = 0,
                                             .layerCount = 1},
                        .imageOffset = {0, 0, 0},
                        .imageExtent = {image.info.size.width, image.info.size.height,
                                        image.info.size.depth},
                    };
                    runtime.DownloadImage(&image, download.buffer, std::span{&copy, 1});
                    scheduler.Finish();
                    download.Invalidate();
                    const auto* halves = reinterpret_cast<const u16*>(download.mapped);
                    const auto* words = reinterpret_cast<const u32*>(download.mapped);
                    u64 zero_halves = 0;
                    u64 exp31 = 0;
                    u64 exp26plus = 0;
                    u32 xor_words = 0;
                    for (u64 i = 0; i < byte_count / sizeof(u16); ++i) {
                        const u16 half = halves[i];
                        const u16 exponent = (half >> 10) & 31;
                        zero_halves += half == 0;
                        exp31 += exponent == 31;
                        exp26plus += exponent >= 26;
                    }
                    for (u64 i = 0; i < byte_count / sizeof(u32); ++i) {
                        xor_words ^= words[i];
                    }
                    LOG_INFO(Render_Vulkan,
                             "GoT FP16 chain readback: stage={:#x} address={:#x} capture={} "
                             "size={}x{}x{} "
                             "bits={} halves={} zero={} exp31={} exp26plus={} xor={:#x} "
                             "first={:#x}/{:#x}",
                             stage.pgm_hash, desc.info.guest_address, readback_index,
                             image.info.size.width,
                             image.info.size.height, image.info.size.depth, image.info.num_bits,
                             byte_count / sizeof(u16), zero_halves, exp31, exp26plus, xor_words,
                             words[0], words[1]);
                    if (capture_head) {
                        const u32 channels = image.info.num_bits / 16;
                        const u32 width = image.info.size.width;
                        const u32 height = image.info.size.height;
                        const u32 depth = image.info.size.depth;
                        const u64 pixels_per_slice = u64(image.info.pitch) * height;
                        const std::string base = fmt::format(
                            "Build/got-ir-audit/fp16-head-{:#x}", desc.info.guest_address);
                        std::ofstream raw(base + ".bin", std::ios::binary);
                        raw.write(reinterpret_cast<const char*>(halves), byte_count);
                        std::ofstream atlas(base + ".pgm", std::ios::binary);
                        std::ofstream stats(base + ".txt");
                        atlas << "P5\n" << width << ' ' << height * depth << "\n255\n";
                        stats << "address=" << fmt::format("{:#x}", desc.info.guest_address)
                              << " extent=" << width << 'x' << height << 'x' << depth
                              << " pitch=" << image.info.pitch << " channels=" << channels
                              << " submit=" << diag_draw_trace.submit_index << '\n';
                        for (u32 z = 0; z < depth; ++z) {
                            std::array<float, 4> minima;
                            std::array<float, 4> maxima;
                            minima.fill(std::numeric_limits<float>::max());
                            maxima.fill(-std::numeric_limits<float>::max());
                            std::array<double, 4> sums{};
                            std::array<u64, 4> invalid{};
                            for (u32 y = 0; y < height; ++y) {
                                for (u32 x = 0; x < width; ++x) {
                                    const u64 index = ((u64(z) * pixels_per_slice) +
                                                       u64(y) * image.info.pitch + x) * channels;
                                    for (u32 channel = 0; channel < channels; ++channel) {
                                        const u16 bits = halves[index + channel];
                                        if ((bits & 0x7c00) == 0x7c00) {
                                            ++invalid[channel];
                                            if (channel == 0) {
                                                atlas.put(0);
                                            }
                                            continue;
                                        }
                                        const float value = DiagHalfToFloat(bits);
                                        minima[channel] = std::min(minima[channel], value);
                                        maxima[channel] = std::max(maxima[channel], value);
                                        sums[channel] += value;
                                        if (channel == 0) {
                                            const float scaled = std::clamp(
                                                std::log2(1.f + std::abs(value)) / 16.f,
                                                0.f, 1.f);
                                            atlas.put(static_cast<char>(scaled * 255.f));
                                        }
                                    }
                                }
                            }
                            stats << "z=" << z;
                            for (u32 channel = 0; channel < channels; ++channel) {
                                const u64 count = u64(width) * height - invalid[channel];
                                stats << " ch" << channel << " min="
                                      << (count ? minima[channel] : 0.f) << " max="
                                      << (count ? maxima[channel] : 0.f) << " mean="
                                      << (count ? sums[channel] / count : 0.0) << " invalid="
                                      << invalid[channel];
                            }
                            stats << '\n';
                        }
                        LOG_INFO(Render_Vulkan,
                                 "GoT head volume captured address={:#x} submit={} base={} (raw FP16 and log-scale channel-0 atlas)",
                                 desc.info.guest_address, diag_draw_trace.submit_index, base);
                    }
                    runtime.GetStagingPool().FreeDeferred(download);
                }
            }
            if (stage.pgm_hash == 0x167bdbe8 &&
                desc.info.guest_address == 0x143dd50000 &&
                (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_3D_LUT_READBACK">() ||
                 Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_LUT_RAW">()) &&
                (!Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_LUT_RAW">() ||
                 diag_draw_trace.submit_index >= 1200)) {
                static u32 lut_readbacks = 0;
                if (lut_readbacks++ <
                    (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_LUT_RAW">() ? 1u : 3u)) {
                    const u64 byte_count = static_cast<u64>(image.info.pitch) *
                                           image.info.size.height * image.info.size.depth * 4;
                    const auto download = runtime.GetStagingPool().Request(
                        byte_count, VideoCore::MemoryType::HostCached, 16, true);
                    const vk::BufferImageCopy copy = {
                        .bufferOffset = download.offset,
                        .bufferRowLength = image.info.pitch,
                        .bufferImageHeight = image.info.size.height,
                        .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                             .mipLevel = 0,
                                             .baseArrayLayer = 0,
                                             .layerCount = 1},
                        .imageOffset = {0, 0, 0},
                        .imageExtent = {image.info.size.width, image.info.size.height,
                                        image.info.size.depth},
                    };
                    runtime.DownloadImage(&image, download.buffer, std::span{&copy, 1});
                    scheduler.Finish();
                    download.Invalidate();
                    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_LUT_RAW">()) {
                        const std::string path = fmt::format(
                            "sky-lut6-submit{}.r11g11b10.raw", diag_draw_trace.submit_index);
                        std::ofstream raw(path, std::ios::binary);
                        raw.write(reinterpret_cast<const char*>(download.mapped), byte_count);
                        LOG_INFO(Render_Vulkan, "GoT sky LUT raw path={} bytes={}", path,
                                 byte_count);
                    }
                    const auto* words = reinterpret_cast<const u32*>(download.mapped);
                    u64 zero_words = 0;
                    u64 invalid_exponents = 0;
                    u64 bright_exponents = 0;
                    u64 near_zero_all_channels = 0;
                    u64 near_zero_any_channel = 0;
                    u64 zero_exponent_any_channel = 0;
                    u64 exponent_15_all_channels = 0;
                    u64 partial_quad_x_edge = 0;
                    u64 partial_quad_y_edge = 0;
                    u64 partial_quad_z_edge = 0;
                    std::array<u64, 8> partial_quad_z_octants{};
                    u32 xor_words = 0;
                    for (u64 i = 0; i < byte_count / sizeof(u32); ++i) {
                        const u32 word = words[i];
                        if (word != 0 && Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_PARTIAL_QUAD_DUMP">()) {
                            const u64 x = i % image.info.pitch;
                            const u64 y = (i / image.info.pitch) % image.info.size.height;
                            const u64 z = i / (u64(image.info.pitch) * image.info.size.height);
                            partial_quad_x_edge +=
                                x < 4 || x + 4 >= image.info.size.width;
                            partial_quad_y_edge +=
                                y < 4 || y + 4 >= image.info.size.height;
                            partial_quad_z_edge +=
                                z == 0 || z + 1 == image.info.size.depth;
                            ++partial_quad_z_octants[std::min<u64>(
                                z * partial_quad_z_octants.size() / image.info.size.depth,
                                partial_quad_z_octants.size() - 1)];
                        }
                        zero_words += word == 0;
                        const u32 r_exp = (word >> 6) & 31;
                        const u32 g_exp = (word >> 17) & 31;
                        const u32 b_exp = (word >> 27) & 31;
                        invalid_exponents += r_exp == 31 || g_exp == 31 || b_exp == 31;
                        bright_exponents += r_exp >= 26 || g_exp >= 26 || b_exp >= 26;
                        if (word != 0) {
                            near_zero_all_channels += r_exp <= 5 && g_exp <= 5 && b_exp <= 5;
                            near_zero_any_channel += r_exp <= 5 || g_exp <= 5 || b_exp <= 5;
                        }
                        zero_exponent_any_channel += r_exp == 0 || g_exp == 0 || b_exp == 0;
                        exponent_15_all_channels +=
                            r_exp >= 15 && g_exp >= 15 && b_exp >= 15;
                        xor_words ^= word;
                    }
                    LOG_INFO(Render_Vulkan,
                             "GoT 3D LUT readback {}: words={} zero={} exp31={} exp26plus={} "
                             "xor={:#x} first={:#x}/{:#x}/{:#x}/{:#x}",
                             lut_readbacks, byte_count / sizeof(u32), zero_words,
                             invalid_exponents, bright_exponents, xor_words, words[0], words[1],
                             words[2], words[3]);
                    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_DENOM_DUMP">()) {
                        LOG_INFO(Render_Vulkan,
                                 "GoT sky denominators readback={} nonzero_words={} "
                                 "all_channels_exp_le_5={} any_channel_exp_le_5={}",
                                 lut_readbacks, byte_count / sizeof(u32) - zero_words,
                                 near_zero_all_channels, near_zero_any_channel);
                    }
                    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_PREMAX_DUMP">()) {
                        LOG_INFO(Render_Vulkan,
                                 "GoT sky pre-max readback={} zero_words={} any_exp_zero={} "
                                 "any_exp_le_5_nonzero={} all_exp_ge_15={}",
                                 lut_readbacks, zero_words, zero_exponent_any_channel,
                                 near_zero_any_channel, exponent_15_all_channels);
                    }
                    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_PARTIAL_QUAD_DUMP">()) {
                        LOG_INFO(Render_Vulkan,
                                 "GoT sky partial quad readback={} flagged={} x_edge={} y_edge={} "
                                 "z_edge={} z_octants={}/{}/{}/{}/{}/{}/{}/{}",
                                 lut_readbacks, byte_count / sizeof(u32) - zero_words,
                                 partial_quad_x_edge, partial_quad_y_edge, partial_quad_z_edge,
                                 partial_quad_z_octants[0], partial_quad_z_octants[1],
                                 partial_quad_z_octants[2], partial_quad_z_octants[3],
                                 partial_quad_z_octants[4], partial_quad_z_octants[5],
                                 partial_quad_z_octants[6], partial_quad_z_octants[7]);
                    }
                    runtime.GetStagingPool().FreeDeferred(download);
                }
            }
            if (stage.pgm_hash == 0xff484786 && verbose_dynamic_diag) {
                LOG_INFO(Render_Vulkan,
                         "GoT dynamic image diagnostic found view second pass={}",
                         diagnostic_pass_index - 1);
            }
            const auto binding = image.binding;

            // The image is either bound as storage in a separate descriptor or bound as render
            // target in feedback loop. Depth images are excluded because they can't be bound as
            // storage and feedback loop doesn't make sense for them
            if ((binding.force_general || binding.is_target) && !image.info.props.is_depth) {
                if (instance.IsAttachmentFeedbackLoopLayoutSupported() && image.binding.is_target) {
                    needs_barrier |= runtime.Transit(
                        &image, vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT,
                        vk::PipelineStageFlagBits2::eAllGraphics, vk::AccessFlagBits2::eShaderRead);
                } else {
                    needs_barrier |= runtime.Transit(
                        &image, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eAllCommands,
                        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
                        desc.view_info.range);
                }
            } else {
                if (is_storage) {
                    needs_barrier |= runtime.Transit(
                        &image, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eAllCommands,
                        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
                        desc.view_info.range);
                } else {
                    const auto new_layout = image.info.props.is_depth
                                                ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                                : vk::ImageLayout::eShaderReadOnlyOptimal;
                    needs_barrier |= runtime.Transit(
                        &image, new_layout, vk::PipelineStageFlagBits2::eAllCommands,
                        vk::AccessFlagBits2::eShaderRead, desc.view_info.range);
                }
            }
            image.usage.storage |= is_storage;
            image.usage.texture |= !is_storage;

            image_infos.emplace_back(VK_NULL_HANDLE, *image_view.image_view,
                                     image.backing->state.layout);
        }
    }
    if (stage.pgm_hash == 0xff484786 && verbose_dynamic_diag) {
        LOG_INFO(Render_Vulkan, "GoT dynamic image diagnostic completed image second pass");
    }

    u32 image_info_idx = first_image_idx;
    u32 image_binding_idx = 0;
    for (u32 array_size : image_descriptor_array_sizes) {
        const auto& [_, desc] = image_bindings[image_binding_idx];
        const bool is_storage = desc.type == VideoCore::TextureCache::BindingType::Storage;
        auto& set_write = set_writes[set_write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = array_size;
        set_write.descriptorType =
            is_storage ? vk::DescriptorType::eStorageImage : vk::DescriptorType::eSampledImage;
        set_write.pImageInfo = &image_infos[image_info_idx];

        image_info_idx += array_size;
        image_binding_idx += array_size;
        binding.unified += array_size;
    }
    if (stage.pgm_hash == 0xff484786 && verbose_dynamic_diag) {
        LOG_INFO(Render_Vulkan, "GoT dynamic image diagnostic completed image descriptor writes");
    }

    u32 sampler_index = 0;
    for (const auto& sampler : stage.samplers) {
        auto ssharp = sampler.GetSharp(stage);
        if (!ssharp.Valid() || (ssharp.border_color_type.Value() == AmdGpu::BorderColor::Custom &&
                                liverpool->regs.ta_bc_base.Address() == 0)) {
            LOG_WARNING(Render_Vulkan,
                        "Rejecting invalid S# max_aniso={}, filter_mode={}, mip_filter={}, "
                        "border_color_type={}, border_color_base={:#x}",
                        static_cast<u32>(ssharp.max_aniso.Value()),
                        static_cast<u32>(ssharp.filter_mode.Value()),
                        static_cast<u32>(ssharp.mip_filter.Value()),
                        static_cast<u32>(ssharp.border_color_type.Value()),
                        liverpool->regs.ta_bc_base.Address());
            ssharp = AmdGpu::Sampler{};
        }
        if (stage.pgm_hash == 0xb0db526b) {
            static std::unordered_set<u32> reported_b0_samplers;
            if (reported_b0_samplers.insert(sampler_index).second) {
                LOG_INFO(Render_Vulkan,
                         "Grass bound sampler index={} raw={:#x}/{:#x} offsets={}/{}/{}/{} "
                         "mask={:#x} clamp={}/{}/{} aniso={} unnorm={} degamma={} filters="
                         "{}/{}/{}/{} lods={}/{}",
                         sampler_index, ssharp.raw0, ssharp.raw1,
                         sampler.sharp_fetch.offsets[0], sampler.sharp_fetch.offsets[1],
                         sampler.sharp_fetch.offsets[2], sampler.sharp_fetch.offsets[3],
                         sampler.sharp_fetch.load_mask, u32(ssharp.clamp_x.Value()),
                         u32(ssharp.clamp_y.Value()), u32(ssharp.clamp_z.Value()),
                         u32(ssharp.max_aniso.Value()), u32(ssharp.force_unnormalized.Value()),
                         u32(ssharp.force_degamma.Value()), u32(ssharp.xy_mag_filter.Value()),
                         u32(ssharp.xy_min_filter.Value()), u32(ssharp.z_filter.Value()),
                         u32(ssharp.mip_filter.Value()), u32(ssharp.min_lod.Value()),
                         u32(ssharp.max_lod.Value()));
            }
        }
        if (stage.pgm_hash == 0x167bdbe8 && sampler_index < 2 &&
            Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_TILE_SAMPLERS">()) {
            static std::unordered_set<u32> reported_tile_samplers;
            if (reported_tile_samplers.insert(sampler_index).second) {
                LOG_INFO(Render_Vulkan,
                         "GoT sky tile sampler index={} raw={:#x}/{:#x} xy_mag={} xy_min={} "
                         "z_filter={} mip_filter={} unnormalized={}",
                         sampler_index, ssharp.raw0, ssharp.raw1,
                         static_cast<u64>(ssharp.xy_mag_filter.Value()),
                         static_cast<u64>(ssharp.xy_min_filter.Value()), ssharp.z_filter.Value(),
                         static_cast<u64>(ssharp.mip_filter.Value()),
                         ssharp.force_unnormalized.Value());
            }
        }
        if (stage.pgm_hash == 0x167bdbe8 && sampler_index == 2 &&
            Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_SAMPLER">()) {
            static bool reported_sky_sampler = false;
            if (!reported_sky_sampler) {
                LOG_INFO(Render_Vulkan,
                         "GoT sky LUT sampler 2 raw={:#x}/{:#x} xy_mag={} xy_min={} z_filter={} "
                         "mip_filter={} unnormalized={}",
                         ssharp.raw0, ssharp.raw1, static_cast<u64>(ssharp.xy_mag_filter.Value()),
                         static_cast<u64>(ssharp.xy_min_filter.Value()), ssharp.z_filter.Value(),
                         static_cast<u64>(ssharp.mip_filter.Value()),
                         ssharp.force_unnormalized.Value());
                reported_sky_sampler = true;
            }
        }
        if (stage.pgm_hash == 0x2a3cacd4 &&
            Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SECONDARY_SAMPLER_BINDINGS">()) {
            static std::unordered_set<u32> reported_samplers;
            if (reported_samplers.insert(sampler_index).second) {
                const auto* words = reinterpret_cast<const u32*>(&ssharp);
                LOG_INFO(Render_Vulkan,
                         "GoT bound sampler index={} words={:#x}/{:#x}/{:#x}/{:#x} "
                         "offsets={}/{}/{}/{}",
                         sampler_index, words[0], words[1], words[2], words[3],
                         sampler.sharp_fetch.offsets[0], sampler.sharp_fetch.offsets[1],
                         sampler.sharp_fetch.offsets[2], sampler.sharp_fetch.offsets[3]);
            }
        }
        ++sampler_index;
        const auto vk_sampler =
            texture_cache.GetSampler(ssharp, liverpool->regs.ta_bc_base, sampler.is_depth);
        image_infos.emplace_back(vk_sampler, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
        auto& set_write = set_writes[set_write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified++;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = 1;
        set_write.descriptorType = vk::DescriptorType::eSampler;
        set_write.pImageInfo = &image_infos.back();
    }
    if (stage.pgm_hash == 0xff484786 && verbose_dynamic_diag) {
        LOG_INFO(Render_Vulkan, "GoT dynamic image diagnostic completed BindTextures");
    }
    if (trace_scene_images) {
        reported_scene_images[scene_phase] = true;
    }
}

RenderState Rasterizer::BeginRendering(const GraphicsPipeline* pipeline) {
    attachment_feedback_loop = false;
    const auto& regs = liverpool->regs;
    const auto& key = pipeline->GetGraphicsKey();
    RenderState state;
    state.width = instance.GetMaxFramebufferWidth();
    state.height = instance.GetMaxFramebufferHeight();
    state.num_layers = std::numeric_limits<u16>::max();
    state.num_color_attachments = std::bit_width(key.mrt_mask);
    for (auto cb = 0u; cb < state.num_color_attachments; ++cb) {
        auto& [image_id, desc] = cb_descs[cb];
        if (!image_id) {
            state.color_attachments[cb] = {};
            continue;
        }
        auto* image = &texture_cache.GetImage(image_id);
        if (image->binding.needs_rebind) {
            image_id = bound_images.emplace_back(texture_cache.FindImage(desc));
            image = &texture_cache.GetImage(image_id);
        }
        texture_cache.UpdateImage(image_id);
        runtime.SetBackingSamples(image, key.color_samples[cb]);
        const auto& image_view = texture_cache.FindRenderTarget(image_id, desc);
        const auto slice = image_view.info.range.base.layer;
        const auto mip = image_view.info.range.base.level;

        const auto& col_buf = regs.color_buffers[cb];
        const bool is_clear = texture_cache.IsMetaCleared(col_buf.CmaskAddress(), slice);
        if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_PRE_SCENE_COLOR0">() &&
            diag_got_full_scene_submit != ~0u &&
            diag_draw_trace.submit_index <= diag_got_full_scene_submit + 2 &&
            col_buf.Address() == 0x14244d8000) {
            LOG_INFO(Render_Vulkan,
                     "GoT color0 begin rendering submit={} stage={:#x} cb={} image_id={} "
                     "is_clear={} cmask={:#x} mask={:#x}",
                     diag_draw_trace.submit_index,
                     pipeline->GetStage(Shader::SwStage::Fragment).pgm_hash, cb, image_id.index,
                     is_clear, col_buf.CmaskAddress(), regs.color_target_mask.GetMask(cb));
        }
        texture_cache.TouchMeta(col_buf.CmaskAddress(), slice, false);

        if (image->binding.is_bound) {
            ASSERT_MSG(!image->binding.force_general,
                       "Having image both as storage and render target is unsupported");
            runtime.FlushBarriers();
            needs_barrier |=
                runtime.Transit(image,
                                instance.IsAttachmentFeedbackLoopLayoutSupported()
                                    ? vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT
                                    : vk::ImageLayout::eGeneral,
                                vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                vk::AccessFlagBits2::eColorAttachmentWrite |
                                    vk::AccessFlagBits2::eColorAttachmentRead);
            attachment_feedback_loop = true;
        } else {
            needs_barrier |= runtime.Transit(image, vk::ImageLayout::eColorAttachmentOptimal,
                                             vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                             vk::AccessFlagBits2::eColorAttachmentWrite |
                                                 vk::AccessFlagBits2::eColorAttachmentRead,
                                             desc.view_info.range);
        }

        state.width = std::min<u32>(state.width, std::max(image->info.size.width >> mip, 1u));
        state.height = std::min<u32>(state.height, std::max(image->info.size.height >> mip, 1u));
        state.num_layers = std::min<u32>(state.num_layers, image_view.info.range.extent.layers);

        const auto clear_value =
            is_clear ? LiverpoolToVK::ColorBufferClearValue(col_buf) : vk::ClearValue{};
        auto& attachment = state.color_attachments[cb];
        attachment.image_view = *image_view.image_view;
        attachment.image_layout = image->backing->state.layout;
        attachment.clear_value = clear_value.color.uint32;
        attachment.is_clear = is_clear;

        image->usage.render_target = 1u;
    }
    for (u32 cb = state.num_color_attachments; cb < state.color_attachments.size(); ++cb) {
        state.color_attachments[cb] = {};
    }

    if (auto image_id = db_desc.first; image_id) {
        auto& desc = db_desc.second;
        const auto htile_address = regs.depth_htile_data_base.GetAddress();
        const auto& image_view = texture_cache.FindDepthTarget(image_id, desc);
        auto& image = texture_cache.GetImage(image_id);

        const auto slice = image_view.info.range.base.layer;
        const bool is_depth_clear =
            (regs.depth_render_control.depth_clear_enable && regs.depth_control.depth_enable &&
             regs.depth_control.depth_write_enable) ||
            texture_cache.IsMetaCleared(htile_address, slice);
        const bool is_stencil_clear = regs.depth_render_control.stencil_clear_enable;
        texture_cache.TouchMeta(htile_address, slice, false);
        ASSERT(desc.view_info.range.extent.levels == 1 && !image.binding.needs_rebind);

        const bool has_stencil = image.info.props.has_stencil;
        // Stencil writes can be enabled while depth writes are off.
        const bool stencil_write =
            has_stencil && regs.depth_control.stencil_enable && !desc.view_info.is_storage;
        const auto new_layout = desc.view_info.is_storage
                                    ? has_stencil ? vk::ImageLayout::eDepthStencilAttachmentOptimal
                                                  : vk::ImageLayout::eDepthAttachmentOptimal
                                : stencil_write
                                    ? vk::ImageLayout::eDepthReadOnlyStencilAttachmentOptimal
                                : has_stencil ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                              : vk::ImageLayout::eDepthReadOnlyOptimal;
        needs_barrier |= runtime.Transit(&image, new_layout,
                                         vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                                             vk::PipelineStageFlagBits2::eLateFragmentTests,
                                         vk::AccessFlagBits2::eDepthStencilAttachmentWrite |
                                             vk::AccessFlagBits2::eDepthStencilAttachmentRead,
                                         desc.view_info.range);

        state.width = std::min<u32>(state.width, image.info.size.width);
        state.height = std::min<u32>(state.height, image.info.size.height);
        state.num_layers = std::min<u32>(state.num_layers, image_view.info.range.extent.layers);

        auto& attachment = state.depth_stencil_attachment;
        attachment.image_view = *image_view.image_view;
        attachment.image_layout = image.backing->state.layout;
        attachment.clear_value = {};
        attachment.is_clear = 0;

        if (regs.depth_buffer.DepthValid()) {
            attachment.clear_value[0] = is_depth_clear ? std::bit_cast<u32>(regs.depth_clear) : 0u;
            attachment.has_depth = true;
            attachment.depth_clear = is_depth_clear;
        }
        if (regs.depth_buffer.StencilValid()) {
            attachment.clear_value[1] = is_stencil_clear ? regs.stencil_clear : 0u;
            attachment.has_stencil = true;
            attachment.stencil_clear = is_stencil_clear;
        }

        image.usage.depth_target = true;
    } else {
        state.depth_stencil_attachment = {};
    }

    if (state.num_layers == std::numeric_limits<u16>::max()) {
        state.num_layers = 1;
    }

    return state;
}

void Rasterizer::Resolve() {
    const auto& mrt0_hint = liverpool->last_cb_extent[0];
    const auto& mrt1_hint = liverpool->last_cb_extent[1];
    VideoCore::TextureCache::ImageDesc mrt0_desc{liverpool->regs.color_buffers[0], mrt0_hint};
    VideoCore::TextureCache::ImageDesc mrt1_desc{liverpool->regs.color_buffers[1], mrt1_hint};
    auto& mrt0_image = texture_cache.GetImage(texture_cache.FindImage(mrt0_desc, true));
    auto& mrt1_image = texture_cache.GetImage(texture_cache.FindImage(mrt1_desc, true));

    ScopeMarkerBegin(fmt::format("Resolve:MRT0={:#x}:MRT1={:#x}",
                                 liverpool->regs.color_buffers[0].Address(),
                                 liverpool->regs.color_buffers[1].Address()));
    runtime.ResolveImage(&mrt0_image, &mrt1_image, mrt0_desc.view_info.range,
                         mrt1_desc.view_info.range);
    ScopeMarkerEnd();
}

void Rasterizer::DepthStencilCopy(bool is_depth, bool is_stencil) {
    auto& regs = liverpool->regs;

    auto read_desc = VideoCore::TextureCache::ImageDesc(
        regs.depth_buffer, regs.depth_view, regs.depth_control,
        regs.depth_htile_data_base.GetAddress(), liverpool->last_db_extent, false);
    auto write_desc = VideoCore::TextureCache::ImageDesc(
        regs.depth_buffer, regs.depth_view, regs.depth_control,
        regs.depth_htile_data_base.GetAddress(), liverpool->last_db_extent, true);

    auto& read_image = texture_cache.GetImage(texture_cache.FindImage(read_desc));
    auto& write_image = texture_cache.GetImage(texture_cache.FindImage(write_desc));

    VideoCore::SubresourceRange sub_range;
    sub_range.base.layer = liverpool->regs.depth_view.slice_start;
    sub_range.extent.layers = liverpool->regs.depth_view.NumSlices() - sub_range.base.layer;

    ScopeMarkerBegin(fmt::format(
        "DepthStencilCopy:DR={:#x}:SR={:#x}:DW={:#x}:SW={:#x}", regs.depth_buffer.DepthAddress(),
        regs.depth_buffer.StencilAddress(), regs.depth_buffer.DepthWriteAddress(),
        regs.depth_buffer.StencilWriteAddress()));

    runtime.CopyDepthStencil(&read_image, &write_image, sub_range);

    ScopeMarkerEnd();
}

void Rasterizer::FillBuffer(VAddr address, u32 num_bytes, u32 value, bool is_gds) {
    ASSERT_MSG(address % 4 == 0 && num_bytes % 4 == 0,
               "FillBuffer address and size must be a multiple of 4 bytes");
    if (!is_gds) {
        texture_cache.ClearMeta(address);
        if (!buffer_cache.IsRegionGpuModified(address, num_bytes)) {
            u32* buffer = std::bit_cast<u32*>(address);
            std::fill(buffer, buffer + (num_bytes / sizeof(u32)), value);
            return;
        }
    }
    const auto [buffer, offset] = [&] -> std::pair<const VideoCore::Buffer*, u64> {
        if (is_gds) {
            return {buffer_cache.GetGdsBuffer(), address};
        }
        return buffer_cache.ObtainBuffer(address, num_bytes, true);
    }();
    runtime.FillBuffer(buffer, offset, num_bytes, value);
}

void Rasterizer::CopyBuffer(VAddr dst, VAddr src, u32 num_bytes, bool dst_gds, bool src_gds) {
    if (Common::DiagnosticEnv<"SHADPS4_DIAG_GOT_SKY_WEATHER_INVALIDATE">() &&
        (dst == 0x144fd30000 || dst == 0x144fd78000 ||
         dst == 0x144fd40000 || dst == 0x144fd88000)) {
        LOG_INFO(Render_Vulkan, "GoT weather copy dst={:#x} src={:#x} size={} gds={}/{}",
                 dst, src, num_bytes, dst_gds, src_gds);
    }
    if (!dst_gds && !buffer_cache.IsRegionGpuModified(dst, num_bytes)) {
        if (!src_gds && !buffer_cache.IsRegionGpuModified(src, num_bytes) &&
            !texture_cache.FindImageFromRange(src, num_bytes)) {
            // Both buffers were not transferred to GPU yet. Can safely copy in host memory.
            std::memcpy(std::bit_cast<void*>(dst), std::bit_cast<void*>(src), num_bytes);
            return;
        }
    }
    texture_cache.InvalidateMemoryFromGPU(dst, num_bytes);
    const auto* gds_buffer = buffer_cache.GetGdsBuffer();
    const auto [src_buffer, src_offset] = [&] -> std::pair<const VideoCore::Buffer*, u64> {
        if (src_gds) {
            return {gds_buffer, src};
        }
        return buffer_cache.ObtainBuffer(src, num_bytes, false, true);
    }();
    const auto [dst_buffer, dst_offset] = [&] -> std::pair<const VideoCore::Buffer*, u64> {
        if (dst_gds) {
            return {gds_buffer, dst};
        }
        return buffer_cache.ObtainBuffer(dst, num_bytes, true, true);
    }();
    const vk::BufferCopy copy = {
        .srcOffset = src_offset,
        .dstOffset = dst_offset,
        .size = num_bytes,
    };
    runtime.CopyBuffer(src_buffer, dst_buffer, std::span{&copy, 1});
}

u32 Rasterizer::ReadDataFromGds(u32 gds_offset) {
    auto* gds_buf = buffer_cache.GetGdsBuffer();
    u32 value;
    std::memcpy(&value, gds_buf->mapped_data.data() + gds_offset, sizeof(u32));
    return value;
}

bool Rasterizer::InvalidateMemory(VAddr addr, u64 size, bool assume_locks) {
    if (!IsMapped(addr, size)) {
        // Not GPU mapped memory, can skip invalidation logic entirely.
        return false;
    }
    buffer_cache.InvalidateMemory(addr, size, assume_locks);
    texture_cache.InvalidateMemory(addr, size);
    return true;
}

bool Rasterizer::ReadMemory(VAddr addr, u64 size, bool assume_locks) {
    if (!IsMapped(addr, size)) {
        // Not GPU mapped memory, can skip invalidation logic entirely.
        return false;
    }
    buffer_cache.ReadMemory(addr, size, false, assume_locks);
    return true;
}

bool Rasterizer::IsMapped(VAddr addr, u64 size) {
    if (size == 0) {
        // There is no memory, so not mapped.
        return false;
    }
    if (static_cast<u64>(addr) > std::numeric_limits<u64>::max() - size) {
        // Memory range wrapped the address space, cannot be mapped.
        return false;
    }
    const auto range = decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);

    Common::RecursiveSharedLock lock{mapped_ranges_mutex};
    return boost::icl::contains(mapped_ranges, range);
}

void Rasterizer::MapMemory(VAddr addr, u64 size) {
    {
        std::scoped_lock lock{mapped_ranges_mutex};
        mapped_ranges += decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
    }
}

void Rasterizer::RegisterMemory(VAddr addr, u64 size) {
    page_manager.OnGpuMap(addr, size);
}

void Rasterizer::UnmapMemory(VAddr addr, u64 size) {
    buffer_cache.InvalidateMemory(addr, size);
    texture_cache.UnmapMemory(addr, size);
    {
        std::scoped_lock lock{mapped_ranges_mutex};
        mapped_ranges -= decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
    }
}

void Rasterizer::UpdateDynamicState(const GraphicsPipeline* pipeline, const bool is_indexed) const {
    UpdateViewportScissorState();
    UpdateDepthStencilState();
    UpdatePrimitiveState(is_indexed);
    UpdateRasterizationState();
    UpdateColorBlendingState(pipeline);

    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.Commit(instance, scheduler.CommandBuffer());
}

void Rasterizer::UpdateViewportScissorState() const {
    const auto& regs = liverpool->regs;

    const auto combined_scissor_value_tl = [](s16 scr, s16 win, s16 gen, s16 win_offset) {
        return std::max({scr, s16(win + win_offset), s16(gen + win_offset)});
    };
    const auto combined_scissor_value_br = [](s16 scr, s16 win, s16 gen, s16 win_offset) {
        return std::min({scr, s16(win + win_offset), s16(gen + win_offset)});
    };
    const bool enable_offset = !regs.window_scissor.window_offset_disable;

    AmdGpu::Scissor scsr{};
    scsr.top_left_x = combined_scissor_value_tl(
        regs.screen_scissor.top_left_x, s16(regs.window_scissor.top_left_x),
        s16(regs.generic_scissor.top_left_x),
        enable_offset ? regs.window_offset.window_x_offset : 0);
    scsr.top_left_y = combined_scissor_value_tl(
        regs.screen_scissor.top_left_y, s16(regs.window_scissor.top_left_y),
        s16(regs.generic_scissor.top_left_y),
        enable_offset ? regs.window_offset.window_y_offset : 0);
    scsr.bottom_right_x = combined_scissor_value_br(
        regs.screen_scissor.bottom_right_x, regs.window_scissor.bottom_right_x,
        regs.generic_scissor.bottom_right_x,
        enable_offset ? regs.window_offset.window_x_offset : 0);
    scsr.bottom_right_y = combined_scissor_value_br(
        regs.screen_scissor.bottom_right_y, regs.window_scissor.bottom_right_y,
        regs.generic_scissor.bottom_right_y,
        enable_offset ? regs.window_offset.window_y_offset : 0);

    boost::container::static_vector<vk::Viewport, AmdGpu::NUM_VIEWPORTS> viewports;
    boost::container::static_vector<vk::Rect2D, AmdGpu::NUM_VIEWPORTS> scissors;

    if (regs.polygon_control.enable_window_offset &&
        (regs.window_offset.window_x_offset != 0 || regs.window_offset.window_y_offset != 0)) {
        LOG_ERROR(Render_Vulkan,
                  "PA_SU_SC_MODE_CNTL.VTX_WINDOW_OFFSET_ENABLE support is not yet implemented.");
    }

    const auto& vp_ctl = regs.viewport_control;
    for (u32 i = 0; i < AmdGpu::NUM_VIEWPORTS; i++) {
        const auto& vp = regs.viewports[i];
        const auto& vp_d = regs.viewport_depths[i];
        if (vp.xscale == 0) {
            continue;
        }

        const auto zoffset = vp_ctl.zoffset_enable ? vp.zoffset : 0.f;
        const auto zscale = vp_ctl.zscale_enable ? vp.zscale : 1.f;

        vk::Viewport viewport{};

        // https://gitlab.freedesktop.org/mesa/mesa/-/blob/209a0ed/src/amd/vulkan/radv_pipeline_graphics.c#L688-689
        // https://gitlab.freedesktop.org/mesa/mesa/-/blob/209a0ed/src/amd/vulkan/radv_cmd_buffer.c#L3103-3109
        // When the clip space is ranged [-1...1], the zoffset is centered.
        // By reversing the above viewport calculations, we get the following:
        if (regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW) {
            viewport.minDepth = zoffset - zscale;
            viewport.maxDepth = zoffset + zscale;
        } else {
            viewport.minDepth = zoffset;
            viewport.maxDepth = zoffset + zscale;
        }

        if (!instance.IsDepthRangeUnrestrictedSupported()) {
            // Unrestricted depth range not supported by device. Restrict to valid range.
            viewport.minDepth = std::max(viewport.minDepth, 0.f);
            viewport.maxDepth = std::min(viewport.maxDepth, 1.f);
        }

        if (regs.IsClipDisabled()) {
            // In case if clipping is disabled we patch the shader to convert vertex position
            // from screen space coordinates to NDC by defining a render space as full hardware
            // window range [0..16383, 0..16383] and setting the viewport to its size.
            viewport.x = 0.f;
            viewport.y = 0.f;
            viewport.width = float(std::min<u32>(instance.GetMaxViewportWidth(), 16_KB));
            viewport.height = float(std::min<u32>(instance.GetMaxViewportHeight(), 16_KB));
        } else {
            const auto xoffset = vp_ctl.xoffset_enable ? vp.xoffset : 0.f;
            const auto xscale = vp_ctl.xscale_enable ? vp.xscale : 1.f;
            const auto yoffset = vp_ctl.yoffset_enable ? vp.yoffset : 0.f;
            const auto yscale = vp_ctl.yscale_enable ? vp.yscale : 1.f;

            viewport.x = xoffset - xscale;
            viewport.y = yoffset - yscale;
            viewport.width = xscale * 2.0f;
            viewport.height = yscale * 2.0f;
        }

        viewports.push_back(viewport);

        auto vp_scsr = scsr;
        if (regs.mode_control.vport_scissor_enable) {
            vp_scsr.top_left_x =
                std::max(vp_scsr.top_left_x, s16(regs.viewport_scissors[i].top_left_x));
            vp_scsr.top_left_y =
                std::max(vp_scsr.top_left_y, s16(regs.viewport_scissors[i].top_left_y));
            vp_scsr.bottom_right_x = std::min(AmdGpu::Scissor::Clamp(vp_scsr.bottom_right_x),
                                              regs.viewport_scissors[i].bottom_right_x);
            vp_scsr.bottom_right_y = std::min(AmdGpu::Scissor::Clamp(vp_scsr.bottom_right_y),
                                              regs.viewport_scissors[i].bottom_right_y);
        }
        scissors.push_back({
            .offset = {vp_scsr.top_left_x, vp_scsr.top_left_y},
            .extent = {vp_scsr.GetWidth(), vp_scsr.GetHeight()},
        });
    }

    if (viewports.empty()) {
        // Vulkan requires providing at least one viewport.
        constexpr vk::Viewport empty_viewport = {
            .x = -1.0f,
            .y = -1.0f,
            .width = 1.0f,
            .height = 1.0f,
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };
        constexpr vk::Rect2D empty_scissor = {
            .offset = {0, 0},
            .extent = {1, 1},
        };
        viewports.push_back(empty_viewport);
        scissors.push_back(empty_scissor);
    }

    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetViewports(viewports);
    dynamic_state.SetScissors(scissors);
}

void Rasterizer::UpdateDepthStencilState() const {
    const auto& regs = liverpool->regs;
    auto& dynamic_state = scheduler.GetDynamicState();

    const auto depth_test_enabled =
        regs.depth_control.depth_enable && regs.depth_buffer.DepthValid();
    dynamic_state.SetDepthTestEnabled(depth_test_enabled);
    if (depth_test_enabled) {
        dynamic_state.SetDepthWriteEnabled(regs.depth_control.depth_write_enable &&
                                           !regs.depth_render_control.depth_clear_enable);
        dynamic_state.SetDepthCompareOp(LiverpoolToVK::CompareOp(regs.depth_control.depth_func));
    }

    const auto depth_bounds_test_enabled = regs.depth_control.depth_bounds_enable;
    dynamic_state.SetDepthBoundsTestEnabled(depth_bounds_test_enabled);
    if (depth_bounds_test_enabled) {
        dynamic_state.SetDepthBounds(regs.depth_bounds_min, regs.depth_bounds_max);
    }

    const auto depth_bias_enabled = regs.polygon_control.NeedsBias();
    dynamic_state.SetDepthBiasEnabled(depth_bias_enabled);
    if (depth_bias_enabled) {
        const bool front = regs.polygon_control.enable_polygon_offset_front;
        dynamic_state.SetDepthBias(
            front ? regs.poly_offset.front_offset : regs.poly_offset.back_offset,
            regs.poly_offset.depth_bias,
            (front ? regs.poly_offset.front_scale : regs.poly_offset.back_scale) / 16.f);
    }

    const auto stencil_test_enabled =
        regs.depth_control.stencil_enable && regs.depth_buffer.StencilValid();
    dynamic_state.SetStencilTestEnabled(stencil_test_enabled);
    if (stencil_test_enabled) {
        const StencilOps front_ops{
            .fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_fail_front),
            .pass_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zpass_front),
            .depth_fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zfail_front),
            .compare_op = LiverpoolToVK::CompareOp(regs.depth_control.stencil_ref_func),
        };
        const StencilOps back_ops = regs.depth_control.backface_enable ? StencilOps{
            .fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_fail_back),
            .pass_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zpass_back),
            .depth_fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zfail_back),
            .compare_op = LiverpoolToVK::CompareOp(regs.depth_control.stencil_bf_func),
        } : front_ops;
        dynamic_state.SetStencilOps(front_ops, back_ops);

        const bool stencil_clear = regs.depth_render_control.stencil_clear_enable;
        const auto front = regs.stencil_ref_front;
        const auto back =
            regs.depth_control.backface_enable ? regs.stencil_ref_back : regs.stencil_ref_front;
        // GCN REPLACE_OP writes DB_STENCILREFMASK.STENCILOPVAL, so a face whose stencil ops
        // include ReplaceOp takes its Vulkan reference from op_val.
        const auto& sc = regs.stencil_control;
        const auto uses_op_val = [](AmdGpu::StencilFunc fail, AmdGpu::StencilFunc zpass,
                                    AmdGpu::StencilFunc zfail) {
            return fail == AmdGpu::StencilFunc::ReplaceOp ||
                   zpass == AmdGpu::StencilFunc::ReplaceOp ||
                   zfail == AmdGpu::StencilFunc::ReplaceOp;
        };
        const bool front_op =
            uses_op_val(sc.stencil_fail_front, sc.stencil_zpass_front, sc.stencil_zfail_front);
        const bool back_op =
            regs.depth_control.backface_enable
                ? uses_op_val(sc.stencil_fail_back, sc.stencil_zpass_back, sc.stencil_zfail_back)
                : front_op;
        const auto ref_conflict = [](AmdGpu::CompareFunc func, const AmdGpu::StencilRefMask& ref) {
            return func != AmdGpu::CompareFunc::Always && func != AmdGpu::CompareFunc::Never &&
                   ref.stencil_test_val != ref.stencil_op_val;
        };
        if ((front_op && ref_conflict(regs.depth_control.stencil_ref_func, front)) ||
            (back_op && regs.depth_control.backface_enable &&
             ref_conflict(regs.depth_control.stencil_bf_func, back))) {
            LOG_WARNING(Render_Vulkan, "Stencil test requires test_val while ReplaceOp requires "
                                       "op_val; the stencil test will use op_val");
        }
        dynamic_state.SetStencilReferences(front_op ? front.stencil_op_val : front.stencil_test_val,
                                           back_op ? back.stencil_op_val : back.stencil_test_val);
        dynamic_state.SetStencilWriteMasks(!stencil_clear ? front.stencil_write_mask : 0U,
                                           !stencil_clear ? back.stencil_write_mask : 0U);
        dynamic_state.SetStencilCompareMasks(front.stencil_mask, back.stencil_mask);
    }
}

void Rasterizer::UpdatePrimitiveState(const bool is_indexed) const {
    const auto& regs = liverpool->regs;
    auto& dynamic_state = scheduler.GetDynamicState();

    const auto is_list_topology = [](const AmdGpu::PrimitiveType type) {
        const auto topology = LiverpoolToVK::PrimitiveType(type);
        return topology == vk::PrimitiveTopology::ePointList ||
               topology == vk::PrimitiveTopology::eLineList ||
               topology == vk::PrimitiveTopology::eTriangleList ||
               topology == vk::PrimitiveTopology::eLineListWithAdjacency ||
               topology == vk::PrimitiveTopology::eTriangleListWithAdjacency;
    };
    const auto is_patch_list_topology = [](const AmdGpu::PrimitiveType type) {
        // Quad and rect lists are emulated using tessellation.
        return type == AmdGpu::PrimitiveType::PatchPrimitive ||
               type == AmdGpu::PrimitiveType::QuadList || type == AmdGpu::PrimitiveType::RectList;
    };

    const auto prim_restart =
        (regs.enable_primitive_restart & 1) != 0 &&
        (instance.IsListRestartSupported() || !is_list_topology(regs.primitive_type)) &&
        (instance.IsPatchListRestartSupported() || !is_patch_list_topology(regs.primitive_type));
    ASSERT_MSG(!is_indexed || !prim_restart || regs.primitive_restart_index == 0xFFFF ||
                   regs.primitive_restart_index == 0xFFFFFFFF,
               "Primitive restart index other than -1 is not supported yet");

    const auto cull_mode = LiverpoolToVK::IsPrimitiveCulled(regs.primitive_type)
                               ? LiverpoolToVK::CullMode(regs.polygon_control.CullingMode())
                               : vk::CullModeFlagBits::eNone;
    const auto front_face = LiverpoolToVK::FrontFace(regs.polygon_control.front_face);

    dynamic_state.SetPrimitiveRestartEnabled(prim_restart);
    dynamic_state.SetRasterizerDiscardEnabled(regs.clipper_control.dx_rasterization_kill);
    dynamic_state.SetCullMode(cull_mode);
    dynamic_state.SetFrontFace(front_face);
}

void Rasterizer::UpdateRasterizationState() const {
    const auto& regs = liverpool->regs;
    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetLineWidth(regs.line_control.Width());
}

void Rasterizer::UpdateColorBlendingState(const GraphicsPipeline* pipeline) const {
    const auto& regs = liverpool->regs;
    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetBlendConstants(regs.blend_constants);
    dynamic_state.SetColorWriteMasks(pipeline->GetGraphicsKey().write_masks);
    dynamic_state.SetAttachmentFeedbackLoopEnabled(attachment_feedback_loop);
}

void Rasterizer::ScopeMarkerBegin(const std::string_view& str, bool from_guest) {
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
    });
}

void Rasterizer::ScopeMarkerEnd(bool from_guest) {
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.endDebugUtilsLabelEXT();
}

void Rasterizer::ScopedMarkerInsert(const std::string_view& str, bool from_guest) {
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.insertDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
    });
}

void Rasterizer::ScopedMarkerInsertColor(const std::string_view& str, const u32 color,
                                         bool from_guest) {
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.insertDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
        .color = std::array<f32, 4>(
            {(f32)((color >> 16) & 0xff) / 255.0f, (f32)((color >> 8) & 0xff) / 255.0f,
             (f32)(color & 0xff) / 255.0f, (f32)((color >> 24) & 0xff) / 255.0f})});
}

std::thread::id Rasterizer::GetGpuCommandProcessorThread() {
    return liverpool->GetGpuCommandProcessorThread();
}

#ifdef __linux__
u32 Rasterizer::GetGpuCommandProcessorThreadId() {
    return liverpool->GetGpuCommandProcessorThreadId();
}
#endif

} // namespace Vulkan
