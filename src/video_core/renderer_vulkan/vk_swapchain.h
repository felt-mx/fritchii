// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Layout {
struct FramebufferLayout;
}

namespace Vulkan {

class Device;
class Scheduler;

/// What the swapchain images hold. The values are vulkan_present_hdr.frag's target codes.
enum class SwapchainOutput : u32 {
    Sdr = 0,   ///< sRGB non-linear colour space
    Hdr10 = 1, ///< BT.2020 primaries, ST 2084 (PQ) transfer
    ScRgb = 2, ///< Linear BT.709 primaries, 1.0 = 80 nits, values outside [0, 1] allowed
};

/// Format of SDR present frames, and of every frame while frame generation runs (its shaders
/// store rgba8).
#ifdef __ANDROID__
inline constexpr VkFormat SdrFrameFormat = VK_FORMAT_R8G8B8A8_UNORM;
#else
inline constexpr VkFormat SdrFrameFormat = VK_FORMAT_B8G8R8A8_UNORM;
#endif
/// Format of present frames on an HDR swapchain: 10 bits keep a game's PQ output free of banding.
inline constexpr VkFormat HdrFrameFormat = VK_FORMAT_A2B10G10R10_UNORM_PACK32;

class Swapchain {
public:
    explicit Swapchain(
        VkSurfaceKHR_T* surface,
        const Device& device,
        Scheduler& scheduler,
        u32 width,
        u32 height);
    ~Swapchain();

    /// Creates (or recreates) the swapchain with a given size.
    void Create(
        VkSurfaceKHR_T* surface,
        u32 width,
        u32 height);

    /// Acquires the next image in the swapchain, waits as needed. Returns true when no image was
    /// acquired (out of date) and the swapchain has to be recreated first.
    /// `may_flush`: false on the async present thread (see Scheduler::Wait).
    bool AcquireNextImage(bool may_flush = true);

    /// Presents the rendered image to the swapchain. Returns its present id for
    /// vkWaitForPresentKHR, or 0 (no present wait support, or the present failed).
    u64 Present(VkSemaphore render_semaphore);

    VkSwapchainKHR Handle() const {
        return *swapchain;
    }

    VkPresentModeKHR PresentMode() const {
        return present_mode;
    }

    /// Returns true when the swapchain needs to be recreated.
    bool NeedsRecreation() const {
        return !swapchain || IsSubOptimal() || NeedsPresentModeUpdate() || NeedsOutputUpdate();
    }

    /// Re-sends the HDR10 metadata when the HDR luminance settings changed.
    void RefreshHdrMetadata();

    bool IsCreated() const {
        return static_cast<bool>(swapchain);
    }

    /// Returns true when the swapchain is outdated.
    bool IsOutDated() const {
        return is_outdated;
    }

    /// Returns true when the swapchain is suboptimal.
    bool IsSubOptimal() const {
        return is_suboptimal;
    }

    VkExtent2D GetSize() const {
        return extent;
    }

    std::size_t GetImageCount() const {
        return image_count;
    }

    std::size_t GetImageIndex() const {
        return image_index;
    }

    std::size_t GetFrameIndex() const {
        return frame_index;
    }

    VkImage GetImageIndex(std::size_t index) const {
        return images[index];
    }

    VkImage CurrentImage() const {
        return images[image_index];
    }

    /// Format of the present frames copied to this swapchain (SdrFrameFormat or HdrFrameFormat).
    VkFormat GetImageViewFormat() const {
        return image_view_format;
    }

    VkFormat GetImageFormat() const {
        return surface_format.format;
    }

    SwapchainOutput GetOutput() const {
        return output;
    }

    /// Changes with every swapchain this object creates.
    u64 GetGeneration() const {
        return generation;
    }

    VkSemaphore CurrentPresentSemaphore() const {
        return *present_semaphores[frame_index];
    }

    VkSemaphore CurrentRenderSemaphore() const {
        return *render_semaphores[image_index];
    }

    u32 GetWidth() const {
        return width;
    }

    u32 GetHeight() const {
        return height;
    }

    VkExtent2D GetExtent() const {
        return extent;
    }

private:
    void CreateSwapchain(const VkSurfaceCapabilitiesKHR& capabilities);
    void CreateSemaphores();
    void CreateImageViews();

    void Destroy();

    bool NeedsPresentModeUpdate() const;

    /// The HDR output or frame generation setting changed since the swapchain was created.
    bool NeedsOutputUpdate() const;

    VkSurfaceKHR_T* surface;

    const Device& device;
    Scheduler& scheduler;

    vk::SwapchainKHR swapchain;

    std::size_t image_count{};
    std::vector<VkImage> images;
    std::vector<u64> resource_ticks;
    std::vector<vk::Semaphore> present_semaphores;
    std::vector<vk::Semaphore> render_semaphores;

    u32 width;
    u32 height;

    u32 image_index{};
    u32 frame_index{};
    u64 next_present_id{};

    VkFormat image_view_format{};
    VkExtent2D extent{};
    VkPresentModeKHR present_mode{};
    VkSurfaceFormatKHR surface_format{};
    SwapchainOutput output{SwapchainOutput::Sdr};
    bool hdr_requested{};       ///< hdr_output when the swapchain was created
    bool hdr_unavailable_logged{};
    u64 generation{};
    u32 metadata_peak{};        ///< Luminance last sent with vkSetHdrMetadataEXT
    u32 metadata_paper_white{};
    bool has_imm{false};
    bool has_mailbox{false};
    bool has_fifo_relaxed{false};

    bool is_outdated{};
    bool is_suboptimal{};
};

} // namespace Vulkan
