// SPDX-License-Identifier: GPL-2.0-or-later

#include "vk_gpu.hpp"

#include "psprecomp/common.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>

// SPIR-V of host/shaders/*.inc, compiled and embedded at build time (vulkan.cmake).
#define ANYPSP_VK_SHADER(name)                                                                                         \
    extern const unsigned char anypsp_vk_##name[];                                                                     \
    extern const std::size_t anypsp_vk_##name##_size;
ANYPSP_VK_SHADER(ge_vert)
ANYPSP_VK_SHADER(ge_frag)
ANYPSP_VK_SHADER(fullscreen_vert)
ANYPSP_VK_SHADER(reinterpret_frag)
ANYPSP_VK_SHADER(fill_frag)
ANYPSP_VK_SHADER(alpha_to_stencil_frag)
ANYPSP_VK_SHADER(copy_frag)
ANYPSP_VK_SHADER(present_vert)
ANYPSP_VK_SHADER(present_frag)
#undef ANYPSP_VK_SHADER

namespace pspweb {
namespace {

constexpr VkDeviceSize kBlockSize = 64ull << 20u;
constexpr VkDeviceSize kStageChunk = 16ull << 20u;
constexpr VkDeviceSize kStageLimit = 256ull << 20u; // staged bytes that force a submission
constexpr std::uint32_t kDescriptorsPerPool = 1024u;
constexpr std::size_t kMaxTextures = 4096u;
constexpr std::uint32_t kStagingWidth = 2048u, kStagingHeight = 512u;
constexpr std::uint32_t kMaxSurfaceStride = 1024u;
constexpr std::uint32_t kPushBytes = sizeof(GpuShaderParams::v);
constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;

const char *result_name(VkResult r) {
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    default: return "VkResult error";
    }
}

void require(VkResult r, const char *what) {
    if (r != VK_SUCCESS) throw psprecomp::Error(std::string("Vulkan: ") + what + " failed (" + result_name(r) + ")");
}

VKAPI_ATTR VkBool32 VKAPI_CALL debug_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                            const VkDebugUtilsMessengerCallbackDataEXT *data, void *) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        std::cerr << "[vulkan] " << (data->pMessage != nullptr ? data->pMessage : "?") << "\n";
    return VK_FALSE;
}

bool has_layer(const char *name) {
    std::uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    return std::any_of(layers.begin(), layers.end(), [&](const VkLayerProperties &l) { return std::strcmp(l.layerName, name) == 0; });
}

bool has_instance_extension(const char *name) {
    std::uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data());
    return std::any_of(extensions.begin(), extensions.end(),
                       [&](const VkExtensionProperties &e) { return std::strcmp(e.extensionName, name) == 0; });
}

VkCompareOp compare_op(GpuCompare f) {
    static constexpr VkCompareOp table[8] = {VK_COMPARE_OP_NEVER, VK_COMPARE_OP_ALWAYS, VK_COMPARE_OP_EQUAL,
                                             VK_COMPARE_OP_NOT_EQUAL, VK_COMPARE_OP_LESS, VK_COMPARE_OP_LESS_OR_EQUAL,
                                             VK_COMPARE_OP_GREATER, VK_COMPARE_OP_GREATER_OR_EQUAL};
    return table[static_cast<int>(f) & 7];
}

VkStencilOp stencil_op(GpuStencilOp op) {
    static constexpr VkStencilOp table[6] = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_ZERO, VK_STENCIL_OP_REPLACE,
                                             VK_STENCIL_OP_INVERT, VK_STENCIL_OP_INCREMENT_AND_CLAMP,
                                             VK_STENCIL_OP_DECREMENT_AND_CLAMP};
    return table[static_cast<int>(op)];
}

VkBlendFactor blend_factor(GpuBlendFactor f) {
    static constexpr VkBlendFactor table[11] = {
        VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
        VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_SRC_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
        VK_BLEND_FACTOR_CONSTANT_COLOR};
    return table[static_cast<int>(f)];
}

VkBlendOp blend_op(GpuBlendOp op) {
    static constexpr VkBlendOp table[5] = {VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_REVERSE_SUBTRACT,
                                           VK_BLEND_OP_MIN, VK_BLEND_OP_MAX};
    return table[static_cast<int>(op)];
}

VkDeviceSize align_up(VkDeviceSize v, VkDeviceSize a) { return a <= 1u ? v : (v + a - 1u) / a * a; }

// The pipeline cache header Vulkan defines (VkPipelineCacheHeaderVersionOne).
struct CacheHeader {
    std::uint32_t length, version, vendor, device;
    std::uint8_t uuid[VK_UUID_SIZE];
};

} // namespace

// ---------------------------------------------------------------------------
// Device memory: 64 MiB blocks per memory type, first fit with coalescing; larger
// requests get a block of their own.

class VkGpu::MemoryPool {
public:
    MemoryPool(VkDevice device, const VkPhysicalDeviceMemoryProperties &properties) : device_(device), properties_(properties) {}
    ~MemoryPool() {
        for (auto &b : blocks_)
            if (b.memory != VK_NULL_HANDLE) vkFreeMemory(device_, b.memory, nullptr);
    }
    MemoryPool(const MemoryPool &) = delete;
    MemoryPool &operator=(const MemoryPool &) = delete;

    Allocation allocate(const VkMemoryRequirements &req, VkMemoryPropertyFlags flags) {
        const std::uint32_t type = memory_type(req.memoryTypeBits, flags);
        if (req.size > kBlockSize / 2u) {
            const std::uint32_t index = new_block(type, req.size, true);
            return Allocation{blocks_[index].memory, 0u, req.size, index};
        }
        for (std::uint32_t i = 0; i < blocks_.size(); ++i) {
            Block &b = blocks_[i];
            if (b.memory == VK_NULL_HANDLE || b.dedicated || b.type != type) continue;
            if (auto offset = carve(b, req.size, req.alignment)) return Allocation{b.memory, *offset, req.size, i};
        }
        const std::uint32_t index = new_block(type, kBlockSize, false);
        const auto offset = carve(blocks_[index], req.size, req.alignment);
        return Allocation{blocks_[index].memory, *offset, req.size, index};
    }

    void release(const Allocation &a) {
        if (a.block >= blocks_.size()) return;
        Block &b = blocks_[a.block];
        if (b.dedicated) {
            vkFreeMemory(device_, b.memory, nullptr);
            b.memory = VK_NULL_HANDLE;
            return;
        }
        auto [it, inserted] = b.free.emplace(a.offset, a.size);
        if (!inserted) return;
        if (auto next = std::next(it); next != b.free.end() && it->first + it->second == next->first) {
            it->second += next->second;
            b.free.erase(next);
        }
        if (it != b.free.begin()) {
            auto prev = std::prev(it);
            if (prev->first + prev->second == it->first) {
                prev->second += it->second;
                b.free.erase(it);
            }
        }
    }

private:
    struct Block {
        VkDeviceMemory memory{};
        VkDeviceSize size{};
        std::uint32_t type{};
        bool dedicated{};
        std::map<VkDeviceSize, VkDeviceSize> free; // offset -> size
    };

    std::uint32_t memory_type(std::uint32_t bits, VkMemoryPropertyFlags flags) const {
        for (std::uint32_t i = 0; i < properties_.memoryTypeCount; ++i)
            if ((bits & (1u << i)) != 0u && (properties_.memoryTypes[i].propertyFlags & flags) == flags) return i;
        for (std::uint32_t i = 0; i < properties_.memoryTypeCount; ++i)
            if ((bits & (1u << i)) != 0u) return i; // device-local preferred, any will do
        throw psprecomp::Error("Vulkan: no memory type for an image");
    }

    std::uint32_t new_block(std::uint32_t type, VkDeviceSize size, bool dedicated) {
        VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        info.allocationSize = size;
        info.memoryTypeIndex = type;
        Block b;
        require(vkAllocateMemory(device_, &info, nullptr, &b.memory), "vkAllocateMemory");
        b.size = size;
        b.type = type;
        b.dedicated = dedicated;
        if (!dedicated) b.free.emplace(0u, size);
        for (std::uint32_t i = 0; i < blocks_.size(); ++i) {
            if (blocks_[i].memory == VK_NULL_HANDLE) {
                blocks_[i] = std::move(b);
                return i;
            }
        }
        blocks_.push_back(std::move(b));
        return static_cast<std::uint32_t>(blocks_.size() - 1u);
    }

    static std::optional<VkDeviceSize> carve(Block &b, VkDeviceSize size, VkDeviceSize alignment) {
        for (auto it = b.free.begin(); it != b.free.end(); ++it) {
            const VkDeviceSize start = it->first, end = it->first + it->second;
            const VkDeviceSize at = align_up(start, alignment);
            if (at + size > end) continue;
            b.free.erase(it);
            if (at > start) b.free.emplace(start, at - start);
            if (at + size < end) b.free.emplace(at + size, end - at - size);
            return at;
        }
        return std::nullopt;
    }

    VkDevice device_;
    VkPhysicalDeviceMemoryProperties properties_;
    std::vector<Block> blocks_;
};

// ---------------------------------------------------------------------------
// Context

VkContext VkContext::create_headless(bool validation) {
    VkContext c;
    c.owned = true;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "AnyPSP";
    app.pEngineName = "AnyPSP GE";
    app.apiVersion = VK_API_VERSION_1_1;
    std::vector<const char *> layers, extensions;
    const bool debug = validation && has_layer("VK_LAYER_KHRONOS_validation");
    if (validation && !debug) std::cerr << "[vulkan] validation layer not installed; running without it\n";
    if (debug) layers.push_back("VK_LAYER_KHRONOS_validation");
    const bool messenger = debug && has_instance_extension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (messenger) extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
    info.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    info.ppEnabledLayerNames = layers.data();
    info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    require(vkCreateInstance(&info, nullptr, &c.instance), "vkCreateInstance (is a Vulkan driver installed?)");
    try {
        if (messenger) {
            auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(c.instance, "vkCreateDebugUtilsMessengerEXT"));
            VkDebugUtilsMessengerCreateInfoEXT m{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            m.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            m.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            m.pfnUserCallback = debug_message;
            if (create != nullptr) require(create(c.instance, &m, nullptr, &c.messenger), "vkCreateDebugUtilsMessengerEXT");
        }
        std::uint32_t count = 0;
        vkEnumeratePhysicalDevices(c.instance, &count, nullptr);
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(c.instance, &count, devices.data());
        if (devices.empty()) throw psprecomp::Error("Vulkan: no physical device");
        // Discrete over integrated over virtual over CPU, unless ANYPSP_VK_DEVICE names one.
        auto rank = [](VkPhysicalDeviceType type) {
            switch (type) {
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 4;
            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
            case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 2;
            case VK_PHYSICAL_DEVICE_TYPE_CPU: return 1;
            default: return 0;
            }
        };
        int best = -1;
        for (std::size_t i = 0; i < devices.size(); ++i) {
            VkPhysicalDeviceProperties p;
            vkGetPhysicalDeviceProperties(devices[i], &p);
            if (best < 0 || rank(p.deviceType) > best) {
                best = rank(p.deviceType);
                c.physical = devices[i];
            }
        }
        if (const char *pick = std::getenv("ANYPSP_VK_DEVICE")) {
            const unsigned long index = std::strtoul(pick, nullptr, 10);
            if (index >= devices.size()) throw psprecomp::Error("ANYPSP_VK_DEVICE=" + std::string(pick) + ": no such device");
            c.physical = devices[index];
        }
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(c.physical, &props);
        c.device_name = props.deviceName;
        vkEnumeratePhysicalDevices(c.instance, &count, nullptr);
        std::uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(c.physical, &families, nullptr);
        std::vector<VkQueueFamilyProperties> family(families);
        vkGetPhysicalDeviceQueueFamilyProperties(c.physical, &families, family.data());
        c.queue_family = families;
        for (std::uint32_t i = 0; i < families; ++i) {
            if ((family[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0u) {
                c.queue_family = i;
                break;
            }
        }
        if (c.queue_family == families) throw psprecomp::Error("Vulkan: " + c.device_name + " has no graphics queue");
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue.queueFamilyIndex = c.queue_family;
        queue.queueCount = 1;
        queue.pQueuePriorities = &priority;
        VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        device.queueCreateInfoCount = 1;
        device.pQueueCreateInfos = &queue;
        require(vkCreateDevice(c.physical, &device, nullptr, &c.device), "vkCreateDevice");
        vkGetDeviceQueue(c.device, c.queue_family, 0, &c.queue);
        const std::uint32_t v = props.apiVersion;
        c.device_name += ", Vulkan " + std::to_string(VK_API_VERSION_MAJOR(v)) + "." + std::to_string(VK_API_VERSION_MINOR(v)) +
                         "." + std::to_string(VK_API_VERSION_PATCH(v));
    } catch (...) {
        c.destroy();
        throw;
    }
    return c;
}

void VkContext::destroy() {
    if (!owned) return;
    if (device != VK_NULL_HANDLE) vkDestroyDevice(device, nullptr);
    if (messenger != VK_NULL_HANDLE) {
        auto destroy_messenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy_messenger != nullptr) destroy_messenger(instance, messenger, nullptr);
    }
    if (instance != VK_NULL_HANDLE) vkDestroyInstance(instance, nullptr);
    *this = VkContext{};
}

// ---------------------------------------------------------------------------
// Device setup

std::size_t VkGpu::PipelineKeyHash::operator()(const PipelineKey &k) const noexcept {
    const GpuPipeline &s = k.state;
    std::uint64_t h = static_cast<std::uint64_t>(k.program);
    auto mix = [&h](std::uint64_t v) { h = (h ^ v) * 0x100000001B3ull; };
    mix(s.color_mask);
    mix(static_cast<std::uint64_t>(s.depth_test) | (static_cast<std::uint64_t>(s.depth_write) << 1u) |
        (static_cast<std::uint64_t>(s.depth_func) << 2u));
    mix(static_cast<std::uint64_t>(s.blend) | (static_cast<std::uint64_t>(s.blend_src) << 1u) |
        (static_cast<std::uint64_t>(s.blend_dst) << 5u) | (static_cast<std::uint64_t>(s.blend_op) << 9u));
    mix(static_cast<std::uint64_t>(s.stencil_test) | (static_cast<std::uint64_t>(s.stencil_func) << 1u) |
        (static_cast<std::uint64_t>(s.stencil_fail) << 4u) | (static_cast<std::uint64_t>(s.stencil_zfail) << 7u) |
        (static_cast<std::uint64_t>(s.stencil_zpass) << 10u));
    return static_cast<std::size_t>(h ^ (h >> 29u));
}

void VkGpu::check(VkResult result, const char *what) const { require(result, what); }

VkShaderModule VkGpu::shader(const unsigned char *code, std::size_t size) {
    std::vector<std::uint32_t> words((size + 3u) / 4u);
    std::memcpy(words.data(), code, size);
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = size;
    info.pCode = words.data();
    VkShaderModule module{};
    check(vkCreateShaderModule(device_, &info, nullptr, &module), "vkCreateShaderModule");
    return module;
}

VkGpu::VkGpu(const VkContext &context, const VkOptions &options) : context_(context), device_(context.device) {
    vkGetPhysicalDeviceProperties(context.physical, &properties_);
    vkGetPhysicalDeviceMemoryProperties(context.physical, &memory_properties_);
    pool_ = std::make_unique<MemoryPool>(device_, memory_properties_);

    auto supports = [&](VkFormat format, VkFormatFeatureFlags features) {
        VkFormatProperties p;
        vkGetPhysicalDeviceFormatProperties(context.physical, format, &p);
        return (p.optimalTilingFeatures & features) == features;
    };
    const VkFormatFeatureFlags color_features = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
                                                VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT |
                                                VK_FORMAT_FEATURE_BLIT_DST_BIT;
    if (!supports(kColorFormat, color_features)) throw psprecomp::Error("Vulkan: RGBA8 render targets are not supported");
    for (VkFormat f : {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT}) {
        if (supports(f, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
            depth_format_ = f;
            break;
        }
    }
    if (depth_format_ == VK_FORMAT_UNDEFINED) throw psprecomp::Error("Vulkan: no depth/stencil format");
    const VkSampleCountFlags counts = properties_.limits.framebufferColorSampleCounts &
                                      properties_.limits.framebufferDepthSampleCounts &
                                      properties_.limits.framebufferStencilSampleCounts;
    for (int s = std::clamp(options.msaa, 1, 8); s >= 1; s /= 2) {
        if ((counts & static_cast<VkSampleCountFlags>(s)) != 0u && (s & (s - 1)) == 0) {
            samples_ = static_cast<VkSampleCountFlagBits>(s);
            break;
        }
    }
    if (static_cast<int>(samples_) != options.msaa)
        std::cerr << "[vulkan] MSAA " << options.msaa << "x not available, using " << samples_ << "x\n";

    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = context.queue_family;
    check(vkCreateCommandPool(device_, &pool, nullptr, &command_pool_), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = command_pool_;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    check(vkAllocateCommandBuffers(device_, &alloc, &cmd_), "vkAllocateCommandBuffers");
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    check(vkCreateFence(device_, &fence, nullptr, &fence_), "vkCreateFence");

    // Render passes. Every pass keeps what its attachments held; the dependencies order it
    // after, and before, any other work.
    const bool msaa = samples_ != VK_SAMPLE_COUNT_1_BIT;
    std::array<VkSubpassDependency, 2> deps{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
    deps[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    deps[0].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    deps[1].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    auto attachment = [](VkFormat format, VkSampleCountFlagBits samples, VkImageLayout layout, VkAttachmentLoadOp load) {
        VkAttachmentDescription a{};
        a.format = format;
        a.samples = samples;
        a.loadOp = load;
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp = load;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.initialLayout = layout;
        a.finalLayout = layout;
        return a;
    };
    auto make_pass = [&](const std::vector<VkAttachmentDescription> &attachments, const VkAttachmentReference *color,
                         const VkAttachmentReference *depth, const VkAttachmentReference *resolve) {
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = color != nullptr ? 1u : 0u;
        sub.pColorAttachments = color;
        sub.pDepthStencilAttachment = depth;
        sub.pResolveAttachments = resolve;
        VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        info.attachmentCount = static_cast<std::uint32_t>(attachments.size());
        info.pAttachments = attachments.data();
        info.subpassCount = 1;
        info.pSubpasses = &sub;
        info.dependencyCount = static_cast<std::uint32_t>(deps.size());
        info.pDependencies = deps.data();
        VkRenderPass pass{};
        check(vkCreateRenderPass(device_, &info, nullptr, &pass), "vkCreateRenderPass");
        return pass;
    };
    const VkImageLayout color_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    const VkImageLayout depth_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    {
        std::vector<VkAttachmentDescription> a = {attachment(kColorFormat, samples_, color_layout, VK_ATTACHMENT_LOAD_OP_LOAD),
                                                  attachment(depth_format_, samples_, depth_layout, VK_ATTACHMENT_LOAD_OP_LOAD)};
        if (msaa) a.push_back(attachment(kColorFormat, VK_SAMPLE_COUNT_1_BIT, color_layout, VK_ATTACHMENT_LOAD_OP_DONT_CARE));
        const VkAttachmentReference color{0, color_layout}, depth{1, depth_layout}, resolve{2, color_layout};
        main_pass_ = make_pass(a, &color, &depth, msaa ? &resolve : nullptr);
    }
    {
        const VkAttachmentReference depth{0, depth_layout};
        stencil_pass_ = make_pass({attachment(depth_format_, samples_, depth_layout, VK_ATTACHMENT_LOAD_OP_LOAD)}, nullptr, &depth, nullptr);
    }
    if (msaa) {
        const VkAttachmentReference color{0, color_layout};
        unresolve_pass_ = make_pass({attachment(kColorFormat, samples_, color_layout, VK_ATTACHMENT_LOAD_OP_LOAD)}, &color, nullptr, nullptr);
    }
    {
        const VkAttachmentReference color{0, color_layout};
        output_pass_ = make_pass({attachment(kColorFormat, VK_SAMPLE_COUNT_1_BIT, color_layout, VK_ATTACHMENT_LOAD_OP_CLEAR)}, &color,
                                 nullptr, nullptr);
    }

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set.bindingCount = 1;
    set.pBindings = &binding;
    check(vkCreateDescriptorSetLayout(device_, &set, nullptr, &set_layout_), "vkCreateDescriptorSetLayout");
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, kPushBytes};
    if (properties_.limits.maxPushConstantsSize < kPushBytes) throw psprecomp::Error("Vulkan: too little push constant space");
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &set_layout_;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    check(vkCreatePipelineLayout(device_, &layout, nullptr, &pipeline_layout_), "vkCreatePipelineLayout");

    ge_vert_ = shader(anypsp_vk_ge_vert, anypsp_vk_ge_vert_size);
    ge_frag_ = shader(anypsp_vk_ge_frag, anypsp_vk_ge_frag_size);
    fullscreen_vert_ = shader(anypsp_vk_fullscreen_vert, anypsp_vk_fullscreen_vert_size);
    reinterpret_frag_ = shader(anypsp_vk_reinterpret_frag, anypsp_vk_reinterpret_frag_size);
    fill_frag_ = shader(anypsp_vk_fill_frag, anypsp_vk_fill_frag_size);
    stencil_frag_ = shader(anypsp_vk_alpha_to_stencil_frag, anypsp_vk_alpha_to_stencil_frag_size);
    copy_frag_ = shader(anypsp_vk_copy_frag, anypsp_vk_copy_frag_size);
    present_vert_ = shader(anypsp_vk_present_vert, anypsp_vk_present_vert_size);
    present_frag_ = shader(anypsp_vk_present_frag, anypsp_vk_present_frag_size);

    for (int i = 0; i < 8; ++i) {
        VkSamplerCreateInfo s{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        s.magFilter = s.minFilter = (i & 1) != 0 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        s.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        s.addressModeU = (i & 2) != 0 ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
        s.addressModeV = (i & 4) != 0 ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
        s.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        s.maxLod = 0.0f;
        check(vkCreateSampler(device_, &s, nullptr, &samplers_[i]), "vkCreateSampler");
    }

    pipeline_cache_path_ = options.pipeline_cache;
    load_pipeline_cache(pipeline_cache_path_);

    white_ = make_image(1, 1, kColorFormat, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SAMPLE_COUNT_1_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT);
    const std::uint32_t white = 0xFFFFFFFFu;
    upload(white_, 0, 0, 1, 1, reinterpret_cast<const std::uint8_t *>(&white), 1u);
    staging_ = make_image(kStagingWidth, kStagingHeight, kColorFormat,
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SAMPLE_COUNT_1_BIT,
                          VK_IMAGE_ASPECT_COLOR_BIT);
    present_image_ = make_image(GeSurfaces::kDisplayWidth, GeSurfaces::kDisplayHeight, kColorFormat,
                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SAMPLE_COUNT_1_BIT,
                                VK_IMAGE_ASPECT_COLOR_BIT);
    set_scale(1);
    finish();
}

VkGpu::~VkGpu() {
    try {
        finish();
    } catch (const std::exception &e) {
        std::cerr << "[vulkan] " << e.what() << "\n";
    }
    vkDeviceWaitIdle(device_);
    save_pipeline_cache();
    for (auto &t : targets_) {
        if (!t.alive) continue;
        for (VkFramebuffer fb : {t.main, t.stencil, t.unresolve})
            if (fb != VK_NULL_HANDLE) vkDestroyFramebuffer(device_, fb, nullptr);
        for (Image *img : {&t.color, &t.depth, &t.msaa}) destroy_image(*img);
    }
    for (auto &[key, texture] : textures_) destroy_image(texture.image);
    for (Image *img : {&white_, &scratch_, &staging_, &present_image_, &output_}) destroy_image(*img);
    if (output_framebuffer_ != VK_NULL_HANDLE) vkDestroyFramebuffer(device_, output_framebuffer_, nullptr);
    for (auto &release : garbage_) release();
    garbage_.clear();
    for (auto &[key, p] : pipelines_) vkDestroyPipeline(device_, p, nullptr);
    if (pipeline_cache_ != VK_NULL_HANDLE) vkDestroyPipelineCache(device_, pipeline_cache_, nullptr);
    for (VkShaderModule m : {ge_vert_, ge_frag_, fullscreen_vert_, reinterpret_frag_, fill_frag_, stencil_frag_, copy_frag_,
                             present_vert_, present_frag_})
        vkDestroyShaderModule(device_, m, nullptr);
    for (VkSampler s : samplers_) vkDestroySampler(device_, s, nullptr);
    for (VkDescriptorPool p : descriptor_pools_) vkDestroyDescriptorPool(device_, p, nullptr);
    vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
    for (VkRenderPass rp : {main_pass_, stencil_pass_, unresolve_pass_, output_pass_})
        if (rp != VK_NULL_HANDLE) vkDestroyRenderPass(device_, rp, nullptr);
    for (auto &chunk : stage_) {
        vkDestroyBuffer(device_, chunk.buffer, nullptr);
        vkFreeMemory(device_, chunk.memory, nullptr);
    }
    vkDestroyFence(device_, fence_, nullptr);
    vkDestroyCommandPool(device_, command_pool_, nullptr);
    pool_.reset();
}

void VkGpu::load_pipeline_cache(const std::string &path) {
    std::vector<char> data;
    if (!path.empty()) {
        std::ifstream in(path, std::ios::binary);
        if (in) data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        CacheHeader header{};
        if (data.size() >= sizeof header) std::memcpy(&header, data.data(), sizeof header);
        // Data from another driver or GPU is dropped rather than handed to this one.
        if (data.size() < sizeof header || header.version != VK_PIPELINE_CACHE_HEADER_VERSION_ONE ||
            header.vendor != properties_.vendorID || header.device != properties_.deviceID ||
            std::memcmp(header.uuid, properties_.pipelineCacheUUID, VK_UUID_SIZE) != 0)
            data.clear();
    }
    VkPipelineCacheCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    info.initialDataSize = data.size();
    info.pInitialData = data.empty() ? nullptr : data.data();
    if (vkCreatePipelineCache(device_, &info, nullptr, &pipeline_cache_) != VK_SUCCESS) {
        info.initialDataSize = 0;
        info.pInitialData = nullptr;
        check(vkCreatePipelineCache(device_, &info, nullptr, &pipeline_cache_), "vkCreatePipelineCache");
    }
}

void VkGpu::save_pipeline_cache() const {
    if (pipeline_cache_path_.empty() || pipeline_cache_ == VK_NULL_HANDLE) return;
    std::size_t size = 0;
    if (vkGetPipelineCacheData(device_, pipeline_cache_, &size, nullptr) != VK_SUCCESS || size == 0u) return;
    std::vector<char> data(size);
    if (vkGetPipelineCacheData(device_, pipeline_cache_, &size, data.data()) != VK_SUCCESS) return;
    try {
        const std::filesystem::path path(pipeline_cache_path_);
        if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
        // Runs sharing the file each write their own temporary file and rename it over the cache.
        const std::filesystem::path temp =
            path.string() + "." + std::to_string(reinterpret_cast<std::uintptr_t>(this) ^
                                                 static_cast<std::uintptr_t>(std::chrono::steady_clock::now().time_since_epoch().count())) + ".tmp";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            out.write(data.data(), static_cast<std::streamsize>(size));
            if (!out) throw std::runtime_error("cannot write " + temp.string());
        }
        std::filesystem::rename(temp, path);
    } catch (const std::exception &e) {
        std::cerr << "[vulkan] pipeline cache not saved: " << e.what() << "\n";
    }
}

int VkGpu::max_scale() const {
    const std::uint32_t limit = std::min(properties_.limits.maxImageDimension2D, properties_.limits.maxFramebufferWidth);
    int scale = 8;
    while (scale > 1 && kMaxSurfaceStride * static_cast<std::uint32_t>(scale) > limit) --scale;
    return scale;
}

void VkGpu::set_scale(int scale) {
    end_pass();
    scale_ = scale;
    destroy_image(output_);
    if (output_framebuffer_ != VK_NULL_HANDLE) {
        later([device = device_, fb = output_framebuffer_] { vkDestroyFramebuffer(device, fb, nullptr); });
        output_framebuffer_ = VK_NULL_HANDLE;
    }
    const auto width = GeSurfaces::kDisplayWidth * static_cast<std::uint32_t>(scale);
    const auto height = GeSurfaces::kDisplayHeight * static_cast<std::uint32_t>(scale);
    output_ = make_image(width, height, kColorFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                         VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fb.renderPass = output_pass_;
    fb.attachmentCount = 1;
    fb.pAttachments = &output_.view;
    fb.width = width;
    fb.height = height;
    fb.layers = 1;
    check(vkCreateFramebuffer(device_, &fb, nullptr, &output_framebuffer_), "vkCreateFramebuffer");
}

// ---------------------------------------------------------------------------
// Resources and recording

VkGpu::Image VkGpu::make_image(std::uint32_t width, std::uint32_t height, VkFormat format, VkImageUsageFlags usage,
                               VkSampleCountFlagBits samples, VkImageAspectFlags aspect) {
    Image img;
    img.width = width;
    img.height = height;
    img.format = format;
    img.aspect = aspect;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {width, height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = samples;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    check(vkCreateImage(device_, &info, nullptr, &img.image), "vkCreateImage");
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device_, img.image, &req);
    img.memory = pool_->allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkBindImageMemory(device_, img.image, img.memory.memory, img.memory.offset), "vkBindImageMemory");
    if ((usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) == 0u)
        return img; // transfers only
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = img.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = {aspect, 0, 1, 0, 1};
    check(vkCreateImageView(device_, &view, nullptr, &img.view), "vkCreateImageView");
    return img;
}

void VkGpu::later(std::function<void()> release) {
    if (recording_) garbage_.push_back(std::move(release));
    else release(); // every submission is waited for, so the GPU is idle
}

void VkGpu::destroy_image(Image &image) {
    if (image.image == VK_NULL_HANDLE) return;
    later([this, img = image] {
        if (img.view != VK_NULL_HANDLE) vkDestroyImageView(device_, img.view, nullptr);
        vkDestroyImage(device_, img.image, nullptr);
        pool_->release(img.memory);
    });
    image = Image{};
}

VkCommandBuffer VkGpu::commands() {
    if (recording_) return cmd_;
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(cmd_, &begin), "vkBeginCommandBuffer");
    recording_ = true;
    bound_pipeline_ = VK_NULL_HANDLE;
    bound_set_ = VK_NULL_HANDLE;
    bound_push_.clear();
    bound_scissor_ = VkRect2D{{-1, -1}, {0, 0}};
    bound_ref_ = bound_compare_ = bound_write_ = ~0u;
    std::fill(std::begin(bound_blend_), std::end(bound_blend_), -1.0f);
    return cmd_;
}

void VkGpu::submit_and_wait() {
    if (!recording_) return;
    end_pass();
    check(vkEndCommandBuffer(cmd_), "vkEndCommandBuffer");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd_;
    check(vkQueueSubmit(context_.queue, 1, &submit, fence_), "vkQueueSubmit");
    check(vkWaitForFences(device_, 1, &fence_, VK_TRUE, ~0ull), "vkWaitForFences");
    check(vkResetFences(device_, 1, &fence_), "vkResetFences");
    check(vkResetCommandBuffer(cmd_, 0), "vkResetCommandBuffer");
    recording_ = false;
    // Staged memory keeps its contents until the next stage() call: readbacks copy out of it
    // right after this returns.
    for (auto &chunk : stage_) chunk.used = 0u;
    staged_bytes_ = 0u;
    for (VkDescriptorPool p : descriptor_pools_) vkResetDescriptorPool(device_, p, 0);
    descriptor_pool_ = 0;
    descriptors_left_ = descriptor_pools_.empty() ? 0u : kDescriptorsPerPool;
    last_view_ = VK_NULL_HANDLE;
    last_sampler_ = VK_NULL_HANDLE;
    last_set_ = VK_NULL_HANDLE;
    std::vector<std::function<void()>> garbage;
    garbage.swap(garbage_);
    for (auto &release : garbage) release();
}

void VkGpu::finish() { submit_and_wait(); }

VkGpu::Slice VkGpu::stage(VkDeviceSize bytes, VkDeviceSize alignment) {
    staged_bytes_ += bytes;
    for (auto &chunk : stage_) {
        const VkDeviceSize at = align_up(chunk.used, alignment);
        if (at + bytes <= chunk.size) {
            chunk.used = at + bytes;
            return Slice{chunk.buffer, at, chunk.data + at};
        }
    }
    StageChunk chunk;
    chunk.size = std::max(kStageChunk, align_up(bytes, 4096u));
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = chunk.size;
    info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateBuffer(device_, &info, nullptr, &chunk.buffer), "vkCreateBuffer");
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device_, chunk.buffer, &req);
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    std::uint32_t type = memory_properties_.memoryTypeCount;
    for (std::uint32_t i = 0; i < memory_properties_.memoryTypeCount && type == memory_properties_.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i)) != 0u && (memory_properties_.memoryTypes[i].propertyFlags & want) == want) type = i;
    if (type == memory_properties_.memoryTypeCount) throw psprecomp::Error("Vulkan: no host-visible coherent memory");
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type;
    check(vkAllocateMemory(device_, &alloc, nullptr, &chunk.memory), "vkAllocateMemory");
    check(vkBindBufferMemory(device_, chunk.buffer, chunk.memory, 0), "vkBindBufferMemory");
    void *mapped = nullptr;
    check(vkMapMemory(device_, chunk.memory, 0, VK_WHOLE_SIZE, 0, &mapped), "vkMapMemory");
    chunk.data = static_cast<std::uint8_t *>(mapped);
    chunk.used = bytes;
    stage_.push_back(chunk);
    return Slice{chunk.buffer, 0u, chunk.data};
}

void VkGpu::use(Image &image, VkImageLayout layout) {
    // Sampling and attachment use are ordered by layout changes and the render passes'
    // dependencies; transfers into or out of the same layout still need a barrier.
    if (image.layout == layout && layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
        return;
    end_pass();
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = image.layout;
    b.newLayout = layout;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image.image;
    b.subresourceRange = {image.aspect, 0, 1, 0, 1};
    vkCmdPipelineBarrier(commands(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
    image.layout = layout;
}

void VkGpu::end_pass() {
    if (pass_ == Pass::None) return;
    vkCmdEndRenderPass(cmd_);
    pass_ = Pass::None;
    pass_target_ = nullptr;
    bound_pipeline_ = VK_NULL_HANDLE;
}

void VkGpu::begin_pass(Pass pass, Target *target) {
    if (pass_ == pass && pass_target_ == target) return;
    end_pass();
    VkRenderPassBeginInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    std::uint32_t width = 0, height = 0;
    VkClearValue clear{};
    switch (pass) {
    case Pass::Main:
        if (samples_ != VK_SAMPLE_COUNT_1_BIT) {
            unresolve(*target);
            use(target->msaa, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        }
        use(target->color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        use(target->depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        info.renderPass = main_pass_;
        info.framebuffer = target->main;
        width = target->color.width;
        height = target->color.height;
        break;
    case Pass::Stencil:
        use(target->depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        info.renderPass = stencil_pass_;
        info.framebuffer = target->stencil;
        width = target->depth.width;
        height = target->depth.height;
        break;
    case Pass::Unresolve:
        use(target->msaa, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        info.renderPass = unresolve_pass_;
        info.framebuffer = target->unresolve;
        width = target->msaa.width;
        height = target->msaa.height;
        break;
    case Pass::Output:
        use(output_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        info.renderPass = output_pass_;
        info.framebuffer = output_framebuffer_;
        width = output_.width;
        height = output_.height;
        clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        info.clearValueCount = 1;
        info.pClearValues = &clear;
        break;
    case Pass::None: return;
    }
    info.renderArea = {{0, 0}, {width, height}};
    VkCommandBuffer cb = commands();
    vkCmdBeginRenderPass(cb, &info, VK_SUBPASS_CONTENTS_INLINE);
    pass_ = pass;
    pass_target_ = target;
    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    vkCmdSetViewport(cb, 0, 1, &viewport);
    bound_scissor_ = VkRect2D{{-1, -1}, {0, 0}};
}

VkPipeline VkGpu::pipeline(Program program, const GpuPipeline &state) {
    const PipelineKey key{program, state};
    if (const auto found = pipelines_.find(key); found != pipelines_.end()) return found->second;

    VkShaderModule vert = fullscreen_vert_, frag = fill_frag_;
    VkRenderPass pass = main_pass_;
    VkSampleCountFlagBits samples = samples_;
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    switch (program) {
    case Program::Ge: vert = ge_vert_; frag = ge_frag_; break;
    case Program::Reinterpret: frag = reinterpret_frag_; break;
    case Program::Fill: break;
    case Program::AlphaToStencil: frag = stencil_frag_; pass = stencil_pass_; break;
    case Program::Unresolve: frag = copy_frag_; pass = unresolve_pass_; break;
    case Program::Present:
        vert = present_vert_;
        frag = present_frag_;
        pass = output_pass_;
        samples = VK_SAMPLE_COUNT_1_BIT;
        topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        break;
    }
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    const VkVertexInputBindingDescription binding{0, sizeof(GlVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attributes[5] = {
        {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, static_cast<std::uint32_t>(offsetof(GlVertex, x))},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<std::uint32_t>(offsetof(GlVertex, u))},
        {2, 0, VK_FORMAT_R8G8B8A8_UNORM, static_cast<std::uint32_t>(offsetof(GlVertex, r))},
        {3, 0, VK_FORMAT_R32_SFLOAT, static_cast<std::uint32_t>(offsetof(GlVertex, q))},
        {4, 0, VK_FORMAT_R32_SFLOAT, static_cast<std::uint32_t>(offsetof(GlVertex, fog))},
    };
    VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    if (program == Program::Ge) {
        input.vertexBindingDescriptionCount = 1;
        input.pVertexBindingDescriptions = &binding;
        input.vertexAttributeDescriptionCount = 5;
        input.pVertexAttributeDescriptions = attributes;
    }
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = topology;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = samples;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = state.depth_test ? VK_TRUE : VK_FALSE;
    depth.depthWriteEnable = state.depth_test && state.depth_write ? VK_TRUE : VK_FALSE;
    depth.depthCompareOp = compare_op(state.depth_func);
    depth.stencilTestEnable = state.stencil_test ? VK_TRUE : VK_FALSE;
    depth.front.failOp = stencil_op(state.stencil_fail);
    depth.front.depthFailOp = stencil_op(state.stencil_zfail);
    depth.front.passOp = stencil_op(state.stencil_zpass);
    depth.front.compareOp = compare_op(state.stencil_func);
    depth.back = depth.front;
    depth.maxDepthBounds = 1.0f;
    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = state.blend ? VK_TRUE : VK_FALSE;
    blend.srcColorBlendFactor = blend.srcAlphaBlendFactor = blend_factor(state.blend_src);
    blend.dstColorBlendFactor = blend.dstAlphaBlendFactor = blend_factor(state.blend_dst);
    blend.colorBlendOp = blend.alphaBlendOp = blend_op(state.blend_op);
    blend.colorWriteMask = state.color_mask;
    VkPipelineColorBlendStateCreateInfo blending{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blending.attachmentCount = program == Program::AlphaToStencil ? 0u : 1u;
    blending.pAttachments = &blend;
    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                             VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                                             VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = static_cast<std::uint32_t>(std::size(dynamic_states));
    dynamic.pDynamicStates = dynamic_states;

    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blending;
    info.pDynamicState = &dynamic;
    info.layout = pipeline_layout_;
    info.renderPass = pass;
    VkPipeline created{};
    check(vkCreateGraphicsPipelines(device_, pipeline_cache_, 1, &info, nullptr, &created), "vkCreateGraphicsPipelines");
    ++pipelines_created_;
    pipelines_.emplace(key, created);
    return created;
}

VkDescriptorSet VkGpu::descriptor(VkImageView view, VkSampler sampler) {
    if (view == last_view_ && sampler == last_sampler_ && last_set_ != VK_NULL_HANDLE) return last_set_;
    if (descriptors_left_ == 0u) {
        if (descriptor_pools_.empty() || descriptor_pool_ + 1u >= descriptor_pools_.size()) {
            const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kDescriptorsPerPool};
            VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            info.maxSets = kDescriptorsPerPool;
            info.poolSizeCount = 1;
            info.pPoolSizes = &size;
            VkDescriptorPool p{};
            check(vkCreateDescriptorPool(device_, &info, nullptr, &p), "vkCreateDescriptorPool");
            descriptor_pools_.push_back(p);
            descriptor_pool_ = descriptor_pools_.size() - 1u;
        } else {
            ++descriptor_pool_;
        }
        descriptors_left_ = kDescriptorsPerPool;
    }
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = descriptor_pools_[descriptor_pool_];
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &set_layout_;
    VkDescriptorSet set{};
    check(vkAllocateDescriptorSets(device_, &alloc, &set), "vkAllocateDescriptorSets");
    --descriptors_left_;
    const VkDescriptorImageInfo image{sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    last_view_ = view;
    last_sampler_ = sampler;
    last_set_ = set;
    return set;
}

void VkGpu::bind(VkPipeline pipeline, VkDescriptorSet set, const void *push, std::uint32_t push_bytes) {
    VkCommandBuffer cb = commands();
    if (bound_pipeline_ != pipeline) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        bound_pipeline_ = pipeline;
    }
    if (set != VK_NULL_HANDLE && set != bound_set_) {
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &set, 0, nullptr);
        bound_set_ = set;
    }
    if (push_bytes != 0u) {
        const auto *bytes = static_cast<const std::uint8_t *>(push);
        if (bound_push_.size() != push_bytes || !std::equal(bound_push_.begin(), bound_push_.end(), bytes)) {
            vkCmdPushConstants(cb, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, push_bytes, push);
            bound_push_.assign(bytes, bytes + push_bytes);
        }
    }
}

void VkGpu::set_scissor(const GeRect &r, int width, int height) {
    const GeRect c = r.clip(GeRect{0, 0, width - 1, height - 1});
    VkRect2D s{{0, 0}, {0, 0}};
    if (!c.empty()) {
        s.offset = {c.x0 * scale_, c.y0 * scale_};
        s.extent = {static_cast<std::uint32_t>(c.width() * scale_), static_cast<std::uint32_t>(c.height() * scale_)};
    }
    if (s.offset.x == bound_scissor_.offset.x && s.offset.y == bound_scissor_.offset.y &&
        s.extent.width == bound_scissor_.extent.width && s.extent.height == bound_scissor_.extent.height)
        return;
    vkCmdSetScissor(commands(), 0, 1, &s);
    bound_scissor_ = s;
}

void VkGpu::upload(Image &image, int x, int y, int w, int h, const std::uint8_t *rgba, std::uint32_t row_pixels) {
    const auto row = static_cast<std::size_t>(w) * 4u;
    const Slice s = stage(row * static_cast<std::size_t>(h), 16u);
    for (int r = 0; r < h; ++r)
        std::memcpy(s.data + row * static_cast<std::size_t>(r), rgba + static_cast<std::size_t>(r) * row_pixels * 4u, row);
    use(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy copy{};
    copy.bufferOffset = s.offset;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageOffset = {x, y, 0};
    copy.imageExtent = {static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), 1};
    vkCmdCopyBufferToImage(commands(), s.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
}

// ---------------------------------------------------------------------------
// GeGpu

GpuTarget VkGpu::create_target(int width, int height) {
    GpuTarget handle;
    if (!free_targets_.empty()) {
        handle = free_targets_.back();
        free_targets_.pop_back();
    } else {
        targets_.emplace_back();
        handle = static_cast<GpuTarget>(targets_.size());
    }
    Target &t = at(handle);
    t = Target{};
    t.alive = true;
    t.width = width;
    t.height = height;
    const auto w = static_cast<std::uint32_t>(width * scale_), h = static_cast<std::uint32_t>(height * scale_);
    const bool msaa = samples_ != VK_SAMPLE_COUNT_1_BIT;
    t.color = make_image(w, h, kColorFormat,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                         VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    t.depth = make_image(w, h, depth_format_,
                         VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                         samples_, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    if (msaa)
        t.msaa = make_image(w, h, kColorFormat,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, samples_,
                            VK_IMAGE_ASPECT_COLOR_BIT);
    auto framebuffer = [&](VkRenderPass pass, std::initializer_list<VkImageView> views) {
        VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        info.renderPass = pass;
        info.attachmentCount = static_cast<std::uint32_t>(views.size());
        info.pAttachments = views.begin();
        info.width = w;
        info.height = h;
        info.layers = 1;
        VkFramebuffer fb{};
        check(vkCreateFramebuffer(device_, &info, nullptr, &fb), "vkCreateFramebuffer");
        return fb;
    };
    if (msaa) {
        t.main = framebuffer(main_pass_, {t.msaa.view, t.depth.view, t.color.view});
        t.unresolve = framebuffer(unresolve_pass_, {t.msaa.view});
    } else {
        t.main = framebuffer(main_pass_, {t.color.view, t.depth.view});
    }
    t.stencil = framebuffer(stencil_pass_, {t.depth.view});
    // Depth and stencil start at 0; colour too (the surface manager uploads VRAM over it).
    VkCommandBuffer cb = commands();
    use(t.depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkClearDepthStencilValue zero_depth{0.0f, 0u};
    const VkImageSubresourceRange depth_range{VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
    vkCmdClearDepthStencilImage(cb, t.depth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero_depth, 1, &depth_range);
    const VkClearColorValue zero_color{};
    const VkImageSubresourceRange color_range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    for (Image *img : {&t.color, &t.msaa}) {
        if (img->image == VK_NULL_HANDLE) continue;
        use(*img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdClearColorImage(cb, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero_color, 1, &color_range);
    }
    return handle;
}

void VkGpu::destroy_target(GpuTarget target) {
    Target &t = at(target);
    if (pass_target_ == &t) end_pass();
    for (VkFramebuffer fb : {t.main, t.stencil, t.unresolve})
        if (fb != VK_NULL_HANDLE) later([device = device_, fb] { vkDestroyFramebuffer(device, fb, nullptr); });
    for (Image *img : {&t.color, &t.depth, &t.msaa}) destroy_image(*img);
    t = Target{};
    free_targets_.push_back(target);
}

void VkGpu::copy_target(GpuTarget from, int fx, int fy, GpuTarget to, int tx, int ty, int w, int h) {
    Target &a = at(from), &b = at(to);
    end_pass();
    VkImageCopy region{};
    region.srcOffset = {fx * scale_, fy * scale_, 0};
    region.dstOffset = {tx * scale_, ty * scale_, 0};
    region.extent = {static_cast<std::uint32_t>(w * scale_), static_cast<std::uint32_t>(h * scale_), 1};
    region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    use(a.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    use(b.color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyImage(commands(), a.color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b.color.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    use(a.depth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    use(b.depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyImage(commands(), a.depth.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b.depth.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    if (samples_ != VK_SAMPLE_COUNT_1_BIT) b.msaa_stale.add(tx, ty, tx + w - 1, ty + h - 1);
}

void VkGpu::read_pixels(GpuTarget target, const GeRect &r, std::uint8_t *rgba) {
    Target &t = at(target);
    const auto w = static_cast<std::uint32_t>(std::min(r.width(), static_cast<int>(kStagingWidth)));
    const auto h = static_cast<std::uint32_t>(std::min(r.height(), static_cast<int>(kStagingHeight)));
    if (w == 0u || h == 0u) return;
    Image *source = &t.color;
    VkOffset3D at{r.x0, r.y0, 0};
    use(t.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    if (scale_ != 1) {
        // Shrink to native size first (one sample per PSP pixel, like a nearest blit).
        use(staging_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageBlit blit{};
        blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[0] = {r.x0 * scale_, r.y0 * scale_, 0};
        blit.srcOffsets[1] = {(r.x0 + static_cast<int>(w)) * scale_, (r.y0 + static_cast<int>(h)) * scale_, 1};
        blit.dstOffsets[1] = {static_cast<int>(w), static_cast<int>(h), 1};
        vkCmdBlitImage(commands(), t.color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging_.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
        use(staging_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        source = &staging_;
        at = {0, 0, 0};
    }
    const Slice s = stage(static_cast<VkDeviceSize>(w) * h * 4u, 16u);
    VkBufferImageCopy copy{};
    copy.bufferOffset = s.offset;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageOffset = at;
    copy.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(commands(), source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.buffer, 1, &copy);
    submit_and_wait();
    // Rows of r.width() pixels; only the first w are filled when it is wider than staging allows.
    for (std::uint32_t y = 0; y < h; ++y)
        std::memcpy(rgba + static_cast<std::size_t>(y) * static_cast<std::size_t>(r.width()) * 4u,
                    s.data + static_cast<std::size_t>(y) * w * 4u, static_cast<std::size_t>(w) * 4u);
}

void VkGpu::write_pixels(GpuTarget target, const GeRect &r, const std::uint8_t *rgba) {
    Target &t = at(target);
    const int w = r.width(), h = r.height();
    if (w <= 0 || h <= 0) return;
    if (scale_ == 1) {
        upload(t.color, r.x0, r.y0, w, h, rgba, static_cast<std::uint32_t>(w));
    } else {
        // Upload at native size, then stretch into the surface.
        const int sw = std::min(w, static_cast<int>(kStagingWidth)), sh = std::min(h, static_cast<int>(kStagingHeight));
        upload(staging_, 0, 0, sw, sh, rgba, static_cast<std::uint32_t>(w));
        use(staging_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        use(t.color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageBlit blit{};
        blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {sw, sh, 1};
        blit.dstOffsets[0] = {r.x0 * scale_, r.y0 * scale_, 0};
        blit.dstOffsets[1] = {(r.x0 + sw) * scale_, (r.y0 + sh) * scale_, 1};
        vkCmdBlitImage(commands(), staging_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.color.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    }
    if (samples_ != VK_SAMPLE_COUNT_1_BIT) t.msaa_stale.add(r);
}

void VkGpu::unresolve(Target &t) {
    if (t.msaa_stale.empty()) return;
    const GeRect r = t.msaa_stale;
    t.msaa_stale = GeRect{};
    use(t.color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    begin_pass(Pass::Unresolve, &t);
    bind(pipeline(Program::Unresolve, GpuPipeline{}), descriptor(t.color.view, samplers_[0]), nullptr, 0u);
    set_scissor(r, t.width, t.height);
    vkCmdDraw(cmd_, 3, 1, 0, 0);
    end_pass();
}

void VkGpu::reinterpret(GpuTarget into, GpuTarget from, const GeRect &hit, const GpuReinterpret &params) {
    Target &a = at(into), &b = at(from);
    use(b.color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    begin_pass(Pass::Main, &a);
    const std::int32_t push[8] = {params.mode, params.format, params.delta, params.row_bytes,
                                  params.src_width, params.src_height, scale_, 0};
    bind(pipeline(Program::Reinterpret, GpuPipeline{}), descriptor(b.color.view, samplers_[0]), push, sizeof push);
    set_scissor(hit, a.width, a.height);
    vkCmdDraw(cmd_, 3, 1, 0, 0);
}

void VkGpu::fill_rect(Target &t, const GeRect &r, const GpuPipeline &state, const float color[4], std::uint8_t ref,
                      std::uint8_t compare_mask) {
    begin_pass(Pass::Main, &t);
    bind(pipeline(Program::Fill, state), VK_NULL_HANDLE, color, 4u * sizeof(float));
    set_scissor(r, t.width, t.height);
    if (state.stencil_test) {
        if (bound_ref_ != ref) vkCmdSetStencilReference(cmd_, VK_STENCIL_FACE_FRONT_AND_BACK, bound_ref_ = ref);
        if (bound_compare_ != compare_mask)
            vkCmdSetStencilCompareMask(cmd_, VK_STENCIL_FACE_FRONT_AND_BACK, bound_compare_ = compare_mask);
        if (bound_write_ != 0u) vkCmdSetStencilWriteMask(cmd_, VK_STENCIL_FACE_FRONT_AND_BACK, bound_write_ = 0u);
    }
    vkCmdDraw(cmd_, 3, 1, 0, 0);
}

void VkGpu::fill_alpha(GpuTarget target, const GeRect &r, std::uint8_t value) {
    GpuPipeline state;
    state.color_mask = 8u;
    const float color[4] = {0.0f, 0.0f, 0.0f, static_cast<float>(value) / 255.0f};
    fill_rect(at(target), r, state, color, 0u, 0u);
}

void VkGpu::stencil_to_alpha(GpuTarget target, const GeRect &r, std::uint32_t bits) {
    Target &t = at(target);
    fill_alpha(target, r, 0u);
    GpuPipeline state;
    state.color_mask = 8u;
    state.blend = true;
    state.blend_src = state.blend_dst = GpuBlendFactor::One;
    state.stencil_test = true;
    state.stencil_func = GpuCompare::Equal;
    for (std::uint32_t bit = 0; bit < 8u; ++bit) {
        if ((bits & (1u << bit)) == 0u) continue;
        const float color[4] = {0.0f, 0.0f, 0.0f, static_cast<float>(1u << bit) / 255.0f};
        fill_rect(t, r, state, color, static_cast<std::uint8_t>(1u << bit), static_cast<std::uint8_t>(1u << bit));
    }
}

void VkGpu::alpha_to_stencil(GpuTarget target, const GeRect &r, std::uint32_t bits) {
    Target &t = at(target);
    const GeRect c = r.clip(GeRect{0, 0, t.width - 1, t.height - 1});
    if (c.empty()) return;
    use(t.color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    begin_pass(Pass::Stencil, &t);
    VkClearAttachment clear{};
    clear.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    clear.clearValue.depthStencil = {0.0f, 0u};
    VkClearRect rect{};
    rect.rect = {{c.x0 * scale_, c.y0 * scale_},
                 {static_cast<std::uint32_t>(c.width() * scale_), static_cast<std::uint32_t>(c.height() * scale_)}};
    rect.layerCount = 1;
    vkCmdClearAttachments(cmd_, 1, &clear, 1, &rect);
    GpuPipeline state;
    state.color_mask = 0u;
    state.stencil_test = true;
    state.stencil_func = GpuCompare::Always;
    state.stencil_zpass = GpuStencilOp::Replace;
    const VkDescriptorSet set = descriptor(t.color.view, samplers_[0]);
    set_scissor(c, t.width, t.height);
    for (std::uint32_t bit = 0; bit < 8u; ++bit) {
        if ((bits & (1u << bit)) == 0u) continue;
        const std::int32_t push[4] = {static_cast<std::int32_t>(bit), 0, 0, 0};
        bind(pipeline(Program::AlphaToStencil, state), set, push, sizeof push);
        if (bound_ref_ != 0xFFu) vkCmdSetStencilReference(cmd_, VK_STENCIL_FACE_FRONT_AND_BACK, bound_ref_ = 0xFFu);
        if (bound_compare_ != 0xFFu) vkCmdSetStencilCompareMask(cmd_, VK_STENCIL_FACE_FRONT_AND_BACK, bound_compare_ = 0xFFu);
        if (bound_write_ != (1u << bit)) vkCmdSetStencilWriteMask(cmd_, VK_STENCIL_FACE_FRONT_AND_BACK, bound_write_ = 1u << bit);
        vkCmdDraw(cmd_, 3, 1, 0, 0);
    }
}

VkGpu::Image &VkGpu::texture_for(const GpuDraw &d) {
    switch (d.source) {
    case GpuDraw::Source::None: return white_;
    case GpuDraw::Source::Target: return at(d.texture_target).color;
    case GpuDraw::Source::Snapshot: {
        // Sampling the target being drawn: read from a copy of the sampled part.
        Target &t = at(d.texture_target);
        if (scratch_.width != t.color.width || scratch_.height != t.color.height) {
            destroy_image(scratch_);
            scratch_ = make_image(t.color.width, t.color.height, kColorFormat,
                                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SAMPLE_COUNT_1_BIT,
                                  VK_IMAGE_ASPECT_COLOR_BIT);
        }
        const GeRect w = d.snapshot;
        if (!w.empty()) {
            use(t.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            use(scratch_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkImageCopy region{};
            region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.srcOffset = region.dstOffset = {w.x0 * scale_, w.y0 * scale_, 0};
            region.extent = {static_cast<std::uint32_t>(w.width() * scale_), static_cast<std::uint32_t>(w.height() * scale_), 1};
            vkCmdCopyImage(commands(), t.color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, scratch_.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        }
        return scratch_;
    }
    case GpuDraw::Source::Texels: break;
    }
    if (textures_.size() > kMaxTextures) {
        for (auto &[key, texture] : textures_) destroy_image(texture.image);
        textures_.clear();
    }
    Texture &texture = textures_[d.texture_identity];
    if (texture.image.image == VK_NULL_HANDLE || texture.image.width != d.tex_width || texture.image.height != d.tex_height) {
        destroy_image(texture.image);
        texture.image = make_image(d.tex_width, d.tex_height, kColorFormat,
                                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SAMPLE_COUNT_1_BIT,
                                   VK_IMAGE_ASPECT_COLOR_BIT);
        texture.loaded = false;
    }
    if (!texture.loaded || texture.hash != d.texture_hash) {
        upload(texture.image, 0, 0, static_cast<int>(d.tex_width), static_cast<int>(d.tex_height),
               reinterpret_cast<const std::uint8_t *>(d.texels), d.tex_width);
        texture.hash = d.texture_hash;
        texture.loaded = true;
    }
    return texture.image;
}

void VkGpu::forget_texture(const void *identity) {
    const auto it = textures_.find(identity);
    if (it == textures_.end()) return;
    destroy_image(it->second.image);
    textures_.erase(it);
}

void VkGpu::draw(const GpuDraw &d) {
    if (d.indices.empty() || d.vertices.empty()) return;
    Target &t = at(d.target);
    const GeRect scissor = d.scissor.clip(GeRect{0, 0, t.width - 1, t.height - 1});
    if (scissor.empty()) return;
    if (staged_bytes_ > kStageLimit) submit_and_wait();
    Image &texture = texture_for(d);
    use(texture, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    begin_pass(Pass::Main, &t);
    const VkSampler sampler = samplers_[(d.linear ? 1 : 0) | (d.clamp_u ? 2 : 0) | (d.clamp_v ? 4 : 0)];
    bind(pipeline(Program::Ge, d.pipeline), descriptor(texture.view, sampler), d.params.v.data(), kPushBytes);
    set_scissor(scissor, t.width, t.height);
    VkCommandBuffer cb = cmd_;
    if (d.pipeline.stencil_test) {
        if (bound_ref_ != d.stencil_ref) vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, bound_ref_ = d.stencil_ref);
        if (bound_compare_ != d.stencil_compare_mask)
            vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, bound_compare_ = d.stencil_compare_mask);
        if (bound_write_ != d.stencil_write_mask)
            vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, bound_write_ = d.stencil_write_mask);
    }
    if (d.pipeline.blend && !std::equal(d.blend_constant.begin(), d.blend_constant.end(), bound_blend_)) {
        vkCmdSetBlendConstants(cb, d.blend_constant.data());
        std::copy(d.blend_constant.begin(), d.blend_constant.end(), bound_blend_);
    }
    const Slice vertices = stage(d.vertices.size_bytes(), 16u);
    std::memcpy(vertices.data, d.vertices.data(), d.vertices.size_bytes());
    const Slice indices = stage(d.indices.size_bytes(), 4u);
    std::memcpy(indices.data, d.indices.data(), d.indices.size_bytes());
    vkCmdBindVertexBuffers(cb, 0, 1, &vertices.buffer, &vertices.offset);
    vkCmdBindIndexBuffer(cb, indices.buffer, indices.offset, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cb, static_cast<std::uint32_t>(d.indices.size()), 1, 0, 0, 0);
}

void VkGpu::present(const GeSurfaces::PresentSource &source) {
    Image *image = nullptr;
    if (source.target != 0u) {
        image = &at(source.target).color;
    } else if (source.cpu_pixels != nullptr) {
        upload(present_image_, 0, 0, static_cast<int>(GeSurfaces::kDisplayWidth), static_cast<int>(GeSurfaces::kDisplayHeight),
               source.cpu_pixels, GeSurfaces::kDisplayWidth);
        image = &present_image_;
    }
    if (image != nullptr) use(*image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    begin_pass(Pass::Output, nullptr);
    if (image != nullptr) {
        const float rect[8] = {source.rect_w, source.rect_h, source.origin_x, source.origin_y,
                               static_cast<float>(source.target != 0u ? source.format : 3u), 0.0f, 0.0f, 0.0f};
        bind(pipeline(Program::Present, GpuPipeline{}), descriptor(image->view, samplers_[6]), rect, sizeof rect);
        const VkRect2D all{{0, 0}, {output_.width, output_.height}};
        vkCmdSetScissor(cmd_, 0, 1, &all);
        bound_scissor_ = all;
        vkCmdDraw(cmd_, 4, 1, 0, 0);
    }
    end_pass();
}

void VkGpu::read_output(std::vector<std::uint8_t> &rgba) {
    use(output_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(output_.width) * output_.height * 4u;
    const Slice s = stage(bytes, 16u);
    VkBufferImageCopy copy{};
    copy.bufferOffset = s.offset;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {output_.width, output_.height, 1};
    vkCmdCopyImageToBuffer(commands(), output_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.buffer, 1, &copy);
    submit_and_wait();
    rgba.assign(s.data, s.data + bytes);
}

// ---------------------------------------------------------------------------

GeVk::GeVk(psprecomp::GuestMemory &memory, const VkContext &context, const VkOptions &options)
    : GeSurfaces(memory, std::make_unique<VkGpu>(context, options)), vk_(static_cast<VkGpu &>(gpu())) {}

void GeVk::render_output(std::uint32_t address, std::uint32_t stride, std::uint32_t format) {
    vk_.present(present_source(address, stride, format));
}

VkHost::VkHost(psprecomp::GuestMemory &memory, int scale, const VkOptions &options, bool validation)
    : context_(VkContext::create_headless(validation)) {
    try {
        renderer_ = std::make_unique<GeVk>(memory, context_, options);
        renderer_->set_render_scale(scale);
    } catch (...) {
        renderer_.reset();
        context_.destroy();
        throw;
    }
}

VkHost::~VkHost() {
    renderer_.reset();
    context_.destroy();
}

void GeVk::capture(std::uint32_t address, std::uint32_t stride, std::uint32_t format, std::vector<std::uint8_t> &rgba) {
    render_output(address, stride, format);
    vk_.read_output(rgba);
}

} // namespace pspweb
