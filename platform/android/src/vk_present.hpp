// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Vulkan for the Android host: an instance and device that can present to the SDL window, and
// a swapchain the GE's Vulkan output image (VkGpu::output_image) is blitted into, letterboxed.
// The device is handed to the GE renderer as a VkContext it does not own.

#include "vk_gpu.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

struct SDL_Window;

namespace anypsp::android {

class VkPresenter {
public:
    // Throws psprecomp::Error when there is no usable Vulkan 1.1 device that can present to `window`.
    explicit VkPresenter(SDL_Window *window);
    ~VkPresenter();
    VkPresenter(const VkPresenter &) = delete;
    VkPresenter &operator=(const VkPresenter &) = delete;

    [[nodiscard]] const pspweb::VkContext &context() const noexcept { return context_; }
    // Shows `output` (width x height, RGBA8, colour-attachment layout, all GPU work on it finished)
    // and leaves it in the colour-attachment layout again. Recreates the swapchain when the window
    // changed; returns false when nothing could be shown this time (window hidden).
    bool present(VkImage output, int width, int height);

private:
    void create_swapchain();
    void destroy_swapchain();
    void release();

    SDL_Window *window_;
    pspweb::VkContext context_;
    VkSurfaceKHR surface_{};
    VkSwapchainKHR swapchain_{};
    VkFormat format_{};
    VkExtent2D extent_{};
    std::vector<VkImage> images_;
    VkCommandPool pool_{};
    VkCommandBuffer cmd_{};
    VkSemaphore acquired_{}, rendered_{};
    VkFence done_{};
};

} // namespace anypsp::android
