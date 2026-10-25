// SPDX-License-Identifier: GPL-2.0-or-later
#include "vk_present.hpp"

#include "psprecomp/common.hpp"

#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <string>

namespace anypsp::android {
namespace {

void check(VkResult result, const char *what) {
    if (result != VK_SUCCESS) throw psprecomp::Error(std::string("Vulkan: ") + what + " failed (" + std::to_string(result) + ")");
}

void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src_access,
             VkAccessFlags dst_access, VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

} // namespace

VkPresenter::VkPresenter(SDL_Window *window) : window_(window) {
    Uint32 count = 0;
    const char *const *sdl_extensions = SDL_Vulkan_GetInstanceExtensions(&count);
    if (sdl_extensions == nullptr) throw psprecomp::Error(std::string("Vulkan: no surface extensions: ") + SDL_GetError());
    std::vector<const char *> extensions(sdl_extensions, sdl_extensions + count);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "AnyPSP";
    app.pEngineName = "AnyPSP GE";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    check(vkCreateInstance(&info, nullptr, &context_.instance), "vkCreateInstance");
    context_.owned = true;  // destroyed here, in ~VkPresenter
    try {
        if (!SDL_Vulkan_CreateSurface(window_, context_.instance, nullptr, &surface_))
            throw psprecomp::Error(std::string("Vulkan: no window surface: ") + SDL_GetError());
        std::uint32_t devices = 0;
        vkEnumeratePhysicalDevices(context_.instance, &devices, nullptr);
        std::vector<VkPhysicalDevice> list(devices);
        vkEnumeratePhysicalDevices(context_.instance, &devices, list.data());
        for (VkPhysicalDevice physical : list) {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(physical, &props);
            if (props.apiVersion < VK_API_VERSION_1_1) continue;
            std::uint32_t families = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, nullptr);
            std::vector<VkQueueFamilyProperties> family(families);
            vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, family.data());
            for (std::uint32_t i = 0; i < families; ++i) {
                VkBool32 present = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(physical, i, surface_, &present);
                if ((family[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0u && present) {
                    context_.physical = physical;
                    context_.queue_family = i;
                    context_.device_name = props.deviceName;
                    break;
                }
            }
            if (context_.physical != VK_NULL_HANDLE) break;
        }
        if (context_.physical == VK_NULL_HANDLE) throw psprecomp::Error("Vulkan: no 1.1 device can draw and present to the window");
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue.queueFamilyIndex = context_.queue_family;
        queue.queueCount = 1;
        queue.pQueuePriorities = &priority;
        const char *swapchain_extension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        device.queueCreateInfoCount = 1;
        device.pQueueCreateInfos = &queue;
        device.enabledExtensionCount = 1;
        device.ppEnabledExtensionNames = &swapchain_extension;
        check(vkCreateDevice(context_.physical, &device, nullptr, &context_.device), "vkCreateDevice");
        vkGetDeviceQueue(context_.device, context_.queue_family, 0, &context_.queue);

        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex = context_.queue_family;
        check(vkCreateCommandPool(context_.device, &pool, nullptr, &pool_), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool = pool_;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(context_.device, &alloc, &cmd_), "vkAllocateCommandBuffers");
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        check(vkCreateSemaphore(context_.device, &semaphore, nullptr, &acquired_), "vkCreateSemaphore");
        check(vkCreateSemaphore(context_.device, &semaphore, nullptr, &rendered_), "vkCreateSemaphore");
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        check(vkCreateFence(context_.device, &fence, nullptr, &done_), "vkCreateFence");
        create_swapchain();
    } catch (...) {
        release();
        throw;
    }
}

VkPresenter::~VkPresenter() { release(); }

void VkPresenter::release() {
    if (context_.device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(context_.device);
        destroy_swapchain();
        if (done_) vkDestroyFence(context_.device, done_, nullptr);
        if (rendered_) vkDestroySemaphore(context_.device, rendered_, nullptr);
        if (acquired_) vkDestroySemaphore(context_.device, acquired_, nullptr);
        if (pool_) vkDestroyCommandPool(context_.device, pool_, nullptr);
        vkDestroyDevice(context_.device, nullptr);
        context_.device = VK_NULL_HANDLE;
        done_ = VK_NULL_HANDLE;
        rendered_ = acquired_ = VK_NULL_HANDLE;
        pool_ = VK_NULL_HANDLE;
    }
    if (surface_) SDL_Vulkan_DestroySurface(context_.instance, surface_, nullptr);
    surface_ = VK_NULL_HANDLE;
    if (context_.instance) vkDestroyInstance(context_.instance, nullptr);
    context_.instance = VK_NULL_HANDLE;
}

void VkPresenter::create_swapchain() {
    VkSurfaceCapabilitiesKHR caps;
    check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(context_.physical, surface_, &caps), "surface capabilities");
    if ((caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0u)
        throw psprecomp::Error("Vulkan: the window's images cannot be copied into");
    std::uint32_t formats = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(context_.physical, surface_, &formats, nullptr);
    std::vector<VkSurfaceFormatKHR> list(formats);
    vkGetPhysicalDeviceSurfaceFormatsKHR(context_.physical, surface_, &formats, list.data());
    if (list.empty()) throw psprecomp::Error("Vulkan: the window has no surface formats");
    VkSurfaceFormatKHR chosen = list.front();
    for (const auto &f : list)
        if (f.format == VK_FORMAT_R8G8B8A8_UNORM || f.format == VK_FORMAT_B8G8R8A8_UNORM) {
            chosen = f;
            break;
        }
    format_ = chosen.format;
    extent_ = caps.currentExtent;
    if (extent_.width == 0xFFFFFFFFu) {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window_, &w, &h);
        extent_ = {static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h)};
    }
    if (extent_.width == 0 || extent_.height == 0) return;  // minimised; try again later
    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = surface_;
    info.minImageCount = std::max(caps.minImageCount, 2u);
    if (caps.maxImageCount != 0) info.minImageCount = std::min(info.minImageCount, caps.maxImageCount);
    info.imageFormat = format_;
    info.imageColorSpace = chosen.colorSpace;
    info.imageExtent = extent_;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) != 0u
                              ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                              : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;  // always available; paces the loop at the display rate
    info.clipped = VK_TRUE;
    check(vkCreateSwapchainKHR(context_.device, &info, nullptr, &swapchain_), "vkCreateSwapchainKHR");
    std::uint32_t images = 0;
    vkGetSwapchainImagesKHR(context_.device, swapchain_, &images, nullptr);
    images_.resize(images);
    vkGetSwapchainImagesKHR(context_.device, swapchain_, &images, images_.data());
}

void VkPresenter::destroy_swapchain() {
    if (swapchain_) vkDestroySwapchainKHR(context_.device, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
    images_.clear();
}

bool VkPresenter::present(VkImage output, int width, int height) {
    if (swapchain_ == VK_NULL_HANDLE) {
        create_swapchain();
        if (swapchain_ == VK_NULL_HANDLE) return false;
    }
    std::uint32_t index = 0;
    VkResult result = vkAcquireNextImageKHR(context_.device, swapchain_, UINT64_MAX, acquired_, VK_NULL_HANDLE, &index);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        vkDeviceWaitIdle(context_.device);
        destroy_swapchain();
        return false;
    }
    if (result != VK_SUBOPTIMAL_KHR) check(result, "vkAcquireNextImageKHR");

    // The largest 480:272 rectangle that fits, centred.
    const int w = static_cast<int>(extent_.width), h = static_cast<int>(extent_.height);
    int pw = w, ph = w * 272 / 480;
    if (ph > h) {
        ph = h;
        pw = h * 480 / 272;
    }
    const int x = (w - pw) / 2, y = (h - ph) / 2;
    VkImage target = images_[index];
    vkResetCommandBuffer(cmd_, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd_, &begin);
    barrier(cmd_, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    barrier(cmd_, output, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
    const VkClearColorValue black{{0.f, 0.f, 0.f, 1.f}};
    const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd_, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {width, height, 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[0] = {x, y, 0};
    blit.dstOffsets[1] = {x + pw, y + ph, 1};
    vkCmdBlitImage(cmd_, output, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                   VK_FILTER_LINEAR);
    barrier(cmd_, output, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    barrier(cmd_, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    check(vkEndCommandBuffer(cmd_), "vkEndCommandBuffer");

    const VkPipelineStageFlags wait = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &acquired_;
    submit.pWaitDstStageMask = &wait;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd_;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &rendered_;
    check(vkQueueSubmit(context_.queue, 1, &submit, done_), "vkQueueSubmit");
    VkPresentInfoKHR info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &rendered_;
    info.swapchainCount = 1;
    info.pSwapchains = &swapchain_;
    info.pImageIndices = &index;
    result = vkQueuePresentKHR(context_.queue, &info);
    // The GE renderer records into the same queue next; wait so the output image is free again.
    vkWaitForFences(context_.device, 1, &done_, VK_TRUE, UINT64_MAX);
    vkResetFences(context_.device, 1, &done_);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        vkQueueWaitIdle(context_.queue);
        destroy_swapchain();
    } else {
        check(result, "vkQueuePresentKHR");
    }
    return true;
}

} // namespace anypsp::android
