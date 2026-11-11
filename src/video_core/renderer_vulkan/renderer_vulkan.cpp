// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <fmt/ranges.h>

#include "common/logging.h"
#include <ranges>
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core_timing.h"
#include "core/frontend/graphics_context.h"
#include "video_core/capture.h"
#include "video_core/gpu.h"
#include "video_core/present.h"
#include "video_core/render_trace.h"
#include "video_core/renderer_vulkan/present/util.h"
#ifdef HAS_RESHADE
#include "video_core/post_processing/fx_chain.h"
#endif
#include "video_core/renderer_vulkan/renderer_vulkan.h"
#include "video_core/renderer_vulkan/vk_blit_screen.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/textures/decoders.h"
#include "video_core/vulkan_common/vulkan_debug_callback.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_instance.h"
#include "video_core/vulkan_common/vulkan_library.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_surface.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#ifdef EGLANTINE_STREAMLINE
#include "video_core/vulkan_common/streamline.h"
#endif
#ifdef __ANDROID__
#include <jni.h>
#endif
namespace Vulkan {
namespace {

constexpr VkExtent2D CaptureImageSize{
    .width = VideoCore::Capture::LinearWidth,
    .height = VideoCore::Capture::LinearHeight,
};

#ifdef HAS_LSFG
[[nodiscard]] VkExtent2D GuestExtent(std::span<const Tegra::FramebufferConfig> framebuffers) {
    if (framebuffers.empty()) {
        return VkExtent2D{};
    }

    const auto& framebuffer = framebuffers.front();
    if (framebuffer.crop_rect.IsEmpty()) {
        return VkExtent2D{.width = framebuffer.width, .height = framebuffer.height};
    }
    return VkExtent2D{
        .width = static_cast<u32>(framebuffer.crop_rect.GetWidth()),
        .height = static_cast<u32>(framebuffer.crop_rect.GetHeight()),
    };
}
#endif

constexpr VkExtent3D CaptureImageExtent{
    .width = VideoCore::Capture::LinearWidth,
    .height = VideoCore::Capture::LinearHeight,
    .depth = VideoCore::Capture::LinearDepth,
};

constexpr VkFormat CaptureFormat = VK_FORMAT_A8B8G8R8_UNORM_PACK32;

std::string GetReadableVersion(u32 version) {
    return fmt::format("{}.{}.{}", VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version),
                       VK_VERSION_PATCH(version));
}

std::string GetDriverVersion(const Device& device) {
    // Extracted from
    // https://github.com/SaschaWillems/vulkan.gpuinfo.org/blob/5dddea46ea1120b0df14eef8f15ff8e318e35462/functions.php#L308-L314
    const u32 version = device.GetDriverVersion();

    if (device.GetDriverID() == VK_DRIVER_ID_NVIDIA_PROPRIETARY) {
        const u32 major = (version >> 22) & 0x3ff;
        const u32 minor = (version >> 14) & 0x0ff;
        const u32 secondary = (version >> 6) & 0x0ff;
        const u32 tertiary = version & 0x003f;
        return fmt::format("{}.{}.{}.{}", major, minor, secondary, tertiary);
    }
    if (device.GetDriverID() == VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS) {
        const u32 major = version >> 14;
        const u32 minor = version & 0x3fff;
        return fmt::format("{}.{}", major, minor);
    }
    return GetReadableVersion(version);
}

std::string BuildCommaSeparatedExtensions(
    const std::set<std::string, std::less<>>& available_extensions) {
    return fmt::format("{}", fmt::join(available_extensions, ","));
}

/// A screenshot of a game's HDR10 output holds PQ values: tone map them to SDR the way
/// vulkan_present_hdr.frag does for an SDR swapchain, so the saved image isn't washed out.
void ToneMapPqScreenshot(std::span<u8> bgra, f32 paper_white) {
    constexpr f32 m1 = 0.1593017578125f, m2 = 78.84375f;
    constexpr f32 c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    std::array<f32, 256> pq_to_relative{}; // Linear light, 1.0 = paper white
    for (size_t i = 0; i < pq_to_relative.size(); ++i) {
        const f32 p = std::pow(static_cast<f32>(i) / 255.0f, 1.0f / m2);
        pq_to_relative[i] =
            10000.0f * std::pow((std::max)(p - c1, 0.0f) / (c2 - c3 * p), 1.0f / m1) / paper_white;
    }
    std::array<u8, 1024> encode{}; // Gamma 2.2 of [0, 1]
    for (size_t i = 0; i < encode.size(); ++i) {
        const f32 x = static_cast<f32>(i) / static_cast<f32>(encode.size() - 1);
        encode[i] = static_cast<u8>(std::pow(x, 1.0f / 2.2f) * 255.0f + 0.5f);
    }
    for (size_t i = 0; i + 3 < bgra.size(); i += 4) {
        const f32 r = pq_to_relative[bgra[i + 2]];
        const f32 g = pq_to_relative[bgra[i + 1]];
        const f32 b = pq_to_relative[bgra[i]];
        // BT.2020 to BT.709.
        std::array<f32, 3> rgb{
            (std::max)(1.6604910f * r - 0.5876411f * g - 0.0728499f * b, 0.0f),
            (std::max)(-0.1245505f * r + 1.1328999f * g - 0.0083494f * b, 0.0f),
            (std::max)(-0.0181508f * r - 0.1005789f * g + 1.1187297f * b, 0.0f),
        };
        constexpr f32 knee = 0.8f;
        if (const f32 m = (std::max)({rgb[0], rgb[1], rgb[2]}); m > knee) {
            const f32 t = (m - knee) / (1.0f - knee);
            const f32 scale = (knee + (1.0f - knee) * t / (1.0f + t)) / m;
            for (f32& c : rgb) {
                c *= scale;
            }
        }
        const auto to_byte = [&](f32 c) {
            return encode[static_cast<size_t>((std::min)(c, 1.0f) * (encode.size() - 1) + 0.5f)];
        };
        bgra[i + 2] = to_byte(rgb[0]);
        bgra[i + 1] = to_byte(rgb[1]);
        bgra[i] = to_byte(rgb[2]);
    }
}

} // Anonymous namespace

Device CreateDevice(const vk::Instance& instance, const vk::InstanceDispatch& dld, VkSurfaceKHR surface) {
    const std::vector<VkPhysicalDevice> devices = instance.EnumeratePhysicalDevices();
    const u32 device_index = Settings::values.vulkan_device.GetValue();
    if (device_index >= u32(devices.size())) {
        LOG_ERROR(Render_Vulkan, "Invalid device index {}!", device_index);
        throw vk::Exception(VK_ERROR_INITIALIZATION_FAILED);
    }
    const vk::PhysicalDevice physical_device(devices[device_index], dld);
    return Device(*instance, physical_device, surface, dld);
}

RendererVulkan::RendererVulkan(Core::Frontend::EmuWindow& emu_window,
                               Tegra::MaxwellDeviceMemoryManager& device_memory_,
                               Tegra::GPU& gpu_,
                               std::unique_ptr<Core::Frontend::GraphicsContext> context_)
try
    : RendererBase(emu_window, std::move(context_))
    , device_memory(device_memory_)
    , gpu(gpu_)
    , library(OpenLibrary(context.get()))
    , dld()
#ifdef EGLANTINE_STREAMLINE
    , streamline_started(Streamline::Start())
#endif
    // Create raw Vulkan instance first
    , instance(CreateInstance(*library,
                            dld,
                            VK_API_VERSION_1_1,
                            render_window.GetWindowInfo().type,
                            Settings::values.renderer_debug.GetValue()))
    // Create debug messenger if debug is enabled
    , debug_messenger(Settings::values.renderer_debug ? CreateDebugUtilsCallback(instance)
                                                    : vk::DebugUtilsMessenger{})
    // Create surface
    , surface(CreateSurface(instance, render_window.GetWindowInfo()))
    , device(CreateDevice(instance, dld, *surface))
    , memory_allocator(device)
    , state_tracker()
    , scheduler(device, state_tracker)
    , swapchain(*surface,
                device,
                scheduler,
               render_window.GetFramebufferLayout().width,
               render_window.GetFramebufferLayout().height)
    , present_manager(instance,
                      render_window,
                      device,
                      memory_allocator,
                      scheduler,
                      swapchain,
                      surface)
    , blit_swapchain(device_memory,
                   device,
                   memory_allocator,
                   present_manager,
                   scheduler,
                   PresentFiltersForDisplay)
    , blit_capture(device_memory,
                   device,
                   memory_allocator,
                   present_manager,
                   scheduler,
                   PresentFiltersForDisplay)
    , blit_applet(device_memory,
                  device,
                  memory_allocator,
                  present_manager,
                  scheduler,
                  PresentFiltersForAppletCapture)
    , rasterizer(render_window, gpu, device_memory, device, memory_allocator, state_tracker, scheduler)
#ifdef HAS_LSFG
    , frame_gen(memory_allocator, scheduler)
#endif
{

    if (Settings::values.renderer_force_max_clock.GetValue() && device.ShouldBoostClocks()) {
        turbo_mode.emplace(instance, dld);
        scheduler.RegisterOnSubmit([this] { turbo_mode->QueueSubmitted(); });
    }

#ifdef HAS_RESHADE
    VideoCore::FxChain::Instance().LoadFromSettings();
#endif

    Report();
} catch (const vk::Exception& exception) {
    LOG_ERROR(Render_Vulkan, "Vulkan initialization failed with error: {}", exception.what());
    throw std::runtime_error{fmt::format("Vulkan initialization error {}", exception.what())};
}

RendererVulkan::~RendererVulkan() {
    scheduler.RegisterOnSubmit([] {});
    void(device.GetLogical().WaitIdle());
}

void RendererVulkan::Composite(std::span<const Tegra::FramebufferConfig> framebuffers) {
    const bool trace = Common::DkbFrameTrace::Enabled();
    const auto entered = trace ? std::chrono::steady_clock::now()
                               : std::chrono::steady_clock::time_point{};
    SCOPE_EXIT {
        render_window.OnFrameDisplayed();
    };

    RenderAppletCaptureLayer(framebuffers);

    if (!render_window.IsShown()) {
        return;
    }

    RenderScreenshot(framebuffers);
    const auto acquiring = trace ? std::chrono::steady_clock::now()
                                 : std::chrono::steady_clock::time_point{};
    Frame* frame = present_manager.GetRenderFrame();
    const auto acquired = trace ? std::chrono::steady_clock::now()
                                : std::chrono::steady_clock::time_point{};

    frame->pq_content =
        std::ranges::any_of(framebuffers, &Tegra::FramebufferConfig::pq_encoded);

    scheduler.RequestOutsideRenderPassOperationContext();
    blit_swapchain.DrawToFrame(device, rasterizer, frame, framebuffers,
                               render_window.GetFramebufferLayout(),
                               present_manager.SwapchainImageCount(),
                               present_manager.FrameFormat());

#ifdef HAS_LSFG
    void(frame_gen.WantedGenerations(present_manager.MaxExtraFrames()));

    frame_gen.Process(device, frame, present_manager.FrameImageFormat(*frame),
                      GuestExtent(framebuffers));

    const size_t generated_frames = frame_gen.GeneratedFrameCount();
    // Each generated frame keeps the screen for an equal share of the real frame interval. Sent
    // back to back, a high refresh rate display shows it for one refresh only.
    const auto spacing = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::duration<f32>(std::min(frame_gen.FrameInterval(), 0.1f) /
                                   static_cast<f32>(generated_frames + 1)));
    for (size_t generation = 0; generation < generated_frames; ++generation) {
        Frame* generated = present_manager.GetRenderFrame();
        generated->present_spacing = spacing;
        generated->pq_content = frame->pq_content;
        blit_swapchain.PrepareFrame(device, generated, render_window.GetFramebufferLayout());
        frame_gen.GenerateInto(device, generated, generation);
        generated->render_tick = scheduler.Flush(*generated->render_ready);
        present_manager.Present(generated);
    }
#endif

    if (trace) {
        scheduler.MarkFrameEnd(++trace_frame);
    }
    frame->render_tick = scheduler.Flush(*frame->render_ready);

    const auto presenting = trace ? std::chrono::steady_clock::now()
                                  : std::chrono::steady_clock::time_point{};
    present_manager.Present(frame);
    const auto presented = trace ? std::chrono::steady_clock::now()
                                 : std::chrono::steady_clock::time_point{};
#ifdef HAS_LSFG
    scheduler.DispatchWork();
#endif

    gpu.RendererFrameEndNotify();
    rasterizer.TickFrame();
    if (trace) {
        const auto end = std::chrono::steady_clock::now();
        const auto ms = [](auto duration) {
            return std::chrono::duration<double, std::milli>(duration).count();
        };
        if (trace_cpu_rate.Ready(end)) {
            LOG_INFO(Render_Vulkan,
                     "DKB renderer renderer_frame={} host_pacing_mode={} vsync_mode={} "
                     "cpu_frame_wall_ms={:.3f} "
                     "cpu_composite_wall_ms={:.3f} capture_wall_ms={:.3f} "
                     "acquire_frame_wall_ms={:.3f} record_submit_wall_ms={:.3f} "
                     "present_call_wall_ms={:.3f} frame_end_wall_ms={:.3f}",
                     trace_frame, static_cast<int>(Settings::values.frame_pacing_mode.GetValue()),
                     static_cast<int>(Settings::values.vsync_mode.GetValue()),
                     trace_last_frame == std::chrono::steady_clock::time_point{}
                         ? -1.0 : ms(entered - trace_last_frame),
                     ms(end - entered), ms(acquiring - entered), ms(acquired - acquiring),
                     ms(presenting - acquired), ms(presented - presenting), ms(end - presented));
        }
        trace_last_frame = entered;
    }
}

void RendererVulkan::Report() const {
    using namespace Common::Literals;
    const std::string vendor_name{device.GetVendorName()};
    const std::string model_name{device.GetModelName()};
    const std::string driver_version = GetDriverVersion(device);
    const std::string driver_name = fmt::format("{} {}", vendor_name, driver_version);

    const std::string api_version = GetReadableVersion(device.ApiVersion());

    const std::string extensions = BuildCommaSeparatedExtensions(device.GetAvailableExtensions());

    const auto available_vram = static_cast<f64>(device.GetDeviceLocalMemory()) / f64{1_GiB};

    LOG_INFO(Render_Vulkan, "Driver: {}", driver_name);
    LOG_INFO(Render_Vulkan, "Device: {}", model_name);
    LOG_INFO(Render_Vulkan, "Vulkan: {}", api_version);
    LOG_INFO(Render_Vulkan, "Available VRAM: {:.2f} GiB", available_vram);
    if (VideoCommon::RenderTrace::Enabled()) {
        LOG_INFO(Render_Vulkan, "Render trace enabled: interval=600 frames, log_filter={}",
                 Settings::values.log_filter.GetValue());
    }
    LOG_DEBUG(Render_Vulkan,
              "Render trace capabilities: geometry_shader={} transform_feedback={} "
              "geometry_streams={} mesh_shader={}",
              device.GetPhysical().GetFeatures().geometryShader, device.IsExtTransformFeedbackSupported(),
              device.AreTransformFeedbackGeometryStreamsSupported(), device.IsExtMeshShaderSupported());
}

vk::Buffer RendererVulkan::RenderToBuffer(std::span<const Tegra::FramebufferConfig> framebuffers,
                                          const Layout::FramebufferLayout& layout, VkFormat format,
                                          VkDeviceSize buffer_size) {
    auto frame = [&]() {
        Frame f{};
        f.image =
            CreateWrappedImage(memory_allocator, VkExtent2D{layout.width, layout.height}, format);
        f.image_view = CreateWrappedImageView(device, f.image, format);
        f.framebuffer = blit_capture.CreateFramebuffer(device, layout, *f.image_view, format);
        return f;
    }();

    auto dst_buffer = CreateWrappedBuffer(memory_allocator, buffer_size, MemoryUsage::Download);
    blit_capture.DrawToFrame(device, rasterizer, &frame, framebuffers, layout, 1, format);

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([&](vk::CommandBuffer cmdbuf) {
        DownloadColorImage(cmdbuf, *frame.image, *dst_buffer,
                           VkExtent3D{layout.width, layout.height, 1});
    });

    // Ensure the copy is fully completed before saving the capture
    scheduler.Finish();

    // Copy backing image data to the capture buffer
    dst_buffer.Invalidate();
    return dst_buffer;
}

void RendererVulkan::RenderScreenshot(std::span<const Tegra::FramebufferConfig> framebuffers) {
    if (!renderer_settings.screenshot_requested) {
        return;
    }

    const auto screenshot_layers = Tegra::FilterLayerStack(
        framebuffers, renderer_settings.screenshot_layer_stack, screenshot_layer_scratch);

    const auto& layout{renderer_settings.screenshot_framebuffer_layout};
    const auto dst_buffer = RenderToBuffer(screenshot_layers, layout, VK_FORMAT_B8G8R8A8_UNORM,
                                           layout.width * layout.height * 4);

    std::memcpy(renderer_settings.screenshot_bits, dst_buffer.Mapped().data(),
                dst_buffer.Mapped().size());
    if (std::ranges::any_of(screenshot_layers, &Tegra::FramebufferConfig::pq_encoded)) {
        ToneMapPqScreenshot(
            {static_cast<u8*>(renderer_settings.screenshot_bits), dst_buffer.Mapped().size()},
            static_cast<f32>(Settings::values.hdr_paper_white.GetValue()));
    }
    renderer_settings.screenshot_complete_callback(false);
    renderer_settings.screenshot_requested = false;
}

std::vector<u8> RendererVulkan::GetAppletCaptureBuffer() {
    using namespace VideoCore::Capture;

    std::vector<u8> out(VideoCore::Capture::TiledSize);

    if (!applet_frame.image) {
        return out;
    }

    const auto dst_buffer =
        CreateWrappedBuffer(memory_allocator, VideoCore::Capture::TiledSize, MemoryUsage::Download);

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([&](vk::CommandBuffer cmdbuf) {
        DownloadColorImage(cmdbuf, *applet_frame.image, *dst_buffer, CaptureImageExtent);
    });

    // Ensure the copy is fully completed before writing the capture
    scheduler.Finish();

    // Swizzle image data to the capture buffer
    dst_buffer.Invalidate();
    Tegra::Texture::SwizzleTexture(out, dst_buffer.Mapped(), BytesPerPixel, LinearWidth,
                                   LinearHeight, LinearDepth, BlockHeight, BlockDepth);

    return out;
}

void RendererVulkan::RenderAppletCaptureLayer(
    std::span<const Tegra::FramebufferConfig> framebuffers) {
    const auto capture_layers = Tegra::FilterLayerStack(
        framebuffers, Service::Nvnflinger::LayerStackId::LastFrame, applet_capture_layers);

    if (capture_layers.empty())
        return;

    if (!applet_frame.image) {
        applet_frame.image = CreateWrappedImage(memory_allocator, CaptureImageSize, CaptureFormat);
        applet_frame.image_view = CreateWrappedImageView(device, applet_frame.image, CaptureFormat);
        applet_frame.framebuffer = blit_applet.CreateFramebuffer(device,
            VideoCore::Capture::Layout, *applet_frame.image_view, CaptureFormat);
    }

    scheduler.RequestOutsideRenderPassOperationContext();
    blit_applet.DrawToFrame(device, rasterizer, &applet_frame, capture_layers, VideoCore::Capture::Layout, 1,
                            CaptureFormat);
}

} // namespace Vulkan
