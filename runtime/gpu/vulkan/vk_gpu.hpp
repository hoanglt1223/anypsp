#pragma once

// Vulkan backend for the GE: GeSurfaces (host/ge_surfaces.hpp) with a Vulkan device. It needs
// no window or surface: frames are presented into an offscreen output image, which a host
// reads back (headless runs) or copies to its swapchain (Android). Shaders are the GL
// backend's (host/shaders/), compiled to SPIR-V at build time.
//
// A render target is an RGBA8 colour image plus a depth/stencil image (and multisampled
// copies with MSAA, resolved into the colour image at the end of every render pass). PSP
// row 0 is image row 0. Commands are recorded into one command buffer that is submitted,
// and waited for, when the CPU needs GPU results (readbacks, presented frames).

#include "ge_gpu.hpp"
#include "ge_surfaces.hpp"
#include "psprecomp/guest_memory.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pspweb {

// The instance, device and queue the renderer draws with.
struct VkContext {
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    std::uint32_t queue_family{};
    std::string device_name;
    VkDebugUtilsMessengerEXT messenger{};
    bool owned{}; // destroy() releases the device and instance

    // A device without presentation support. `validation` enables the Khronos validation
    // layer when it is installed. ANYPSP_VK_DEVICE=<n> picks the n-th physical device.
    // Throws psprecomp::Error.
    static VkContext create_headless(bool validation);
    void destroy();
};

struct VkOptions {
    int msaa{1};                    // samples per pixel: 1, 2, 4 or 8 (lowered to what the device supports)
    std::string pipeline_cache;     // file the pipeline cache is loaded from and saved to; empty: none
};

class VkGpu final : public GeGpu {
public:
    // Throws psprecomp::Error when the device lacks something the renderer needs.
    VkGpu(const VkContext &context, const VkOptions &options);
    ~VkGpu() override;
    VkGpu(const VkGpu &) = delete;
    VkGpu &operator=(const VkGpu &) = delete;

    [[nodiscard]] int max_scale() const override;
    void set_scale(int scale) override;
    GpuTarget create_target(int width, int height) override;
    void destroy_target(GpuTarget target) override;
    void copy_target(GpuTarget from, int fx, int fy, GpuTarget to, int tx, int ty, int w, int h) override;
    void read_pixels(GpuTarget target, const GeRect &r, std::uint8_t *rgba) override;
    void write_pixels(GpuTarget target, const GeRect &r, const std::uint8_t *rgba) override;
    void reinterpret(GpuTarget into, GpuTarget from, const GeRect &hit, const GpuReinterpret &params) override;
    void fill_alpha(GpuTarget target, const GeRect &r, std::uint8_t value) override;
    void stencil_to_alpha(GpuTarget target, const GeRect &r, std::uint32_t bits) override;
    void alpha_to_stencil(GpuTarget target, const GeRect &r, std::uint32_t bits) override;
    void draw(const GpuDraw &draw) override;
    void forget_texture(const void *identity) override;

    // Draws `source` into the output image, (480 x 272) * scale, top row first.
    void present(const GeSurfaces::PresentSource &source);
    // Reads the output image back as RGBA rows, top row first.
    void read_output(std::vector<std::uint8_t> &rgba);
    // The output image (colour attachment optimal after present()) for hosts that show it.
    [[nodiscard]] VkImage output_image() const noexcept { return output_.image; }
    [[nodiscard]] int samples() const noexcept { return static_cast<int>(samples_); }
    // Submits recorded work and waits for it.
    void finish();
    [[nodiscard]] std::uint64_t pipelines_created() const noexcept { return pipelines_created_; }

private:
    class MemoryPool;
    struct Allocation {
        VkDeviceMemory memory{};
        VkDeviceSize offset{}, size{};
        std::uint32_t block{~0u};
    };
    struct Image {
        VkImage image{};
        VkImageView view{};
        Allocation memory;
        VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
        VkImageAspectFlags aspect{};
        VkFormat format{};
        std::uint32_t width{}, height{};
    };
    struct Target {
        int width{}, height{}; // PSP pixels
        Image color;           // single-sampled: sampled, copied and read
        Image depth;           // depth + stencil, multisampled with MSAA
        Image msaa;            // multisampled colour (MSAA only)
        VkFramebuffer main{}, stencil{}, unresolve{};
        GeRect msaa_stale;     // MSAA: where `color` is newer than `msaa`
        bool alive{};
    };
    struct Texture {
        Image image;
        std::uint64_t hash{};
        bool loaded{};
    };
    // Every program draws with one of these fixed-function setups.
    enum class Program : std::uint8_t { Ge, Reinterpret, Fill, AlphaToStencil, Present, Unresolve };
    enum class Pass : std::uint8_t { None, Main, Stencil, Unresolve, Output };
    struct PipelineKey {
        Program program{};
        GpuPipeline state;
        bool operator==(const PipelineKey &) const = default;
    };
    struct PipelineKeyHash {
        std::size_t operator()(const PipelineKey &k) const noexcept;
    };
    struct Slice {
        VkBuffer buffer{};
        VkDeviceSize offset{};
        std::uint8_t *data{};
    };

    [[nodiscard]] Target &at(GpuTarget target) { return targets_[target - 1u]; }
    void check(VkResult result, const char *what) const;
    Image make_image(std::uint32_t width, std::uint32_t height, VkFormat format, VkImageUsageFlags usage,
                     VkSampleCountFlagBits samples, VkImageAspectFlags aspect);
    void destroy_image(Image &image); // deferred until the GPU is done with it
    void later(std::function<void()> release);
    VkCommandBuffer commands();
    void submit_and_wait();
    // Host-visible memory for this submission (vertices, uploads, readbacks).
    Slice stage(VkDeviceSize bytes, VkDeviceSize alignment);
    // Moves `image` to `layout`, ordering it after all earlier work on it.
    void use(Image &image, VkImageLayout layout);
    void begin_pass(Pass pass, Target *target);
    void end_pass();
    VkPipeline pipeline(Program program, const GpuPipeline &state);
    VkDescriptorSet descriptor(VkImageView view, VkSampler sampler);
    void bind(VkPipeline pipeline, VkDescriptorSet set, const void *push, std::uint32_t push_bytes);
    void set_scissor(const GeRect &r, int width, int height); // PSP pixels of a width x height target
    // Brings the multisampled colour of `t` up to date with its single-sampled colour.
    void unresolve(Target &t);
    void fill_rect(Target &t, const GeRect &r, const GpuPipeline &state, const float color[4], std::uint8_t ref,
                   std::uint8_t compare_mask);
    Image &texture_for(const GpuDraw &d);
    void upload(Image &image, int x, int y, int w, int h, const std::uint8_t *rgba, std::uint32_t row_pixels);
    VkShaderModule shader(const unsigned char *code, std::size_t size);
    void load_pipeline_cache(const std::string &path);
    void save_pipeline_cache() const;

    VkContext context_;
    VkDevice device_{};
    VkPhysicalDeviceProperties properties_{};
    VkPhysicalDeviceMemoryProperties memory_properties_{};
    std::unique_ptr<MemoryPool> pool_;
    VkFormat depth_format_{};
    VkSampleCountFlagBits samples_{VK_SAMPLE_COUNT_1_BIT};
    int scale_{1};

    VkCommandPool command_pool_{};
    VkCommandBuffer cmd_{};
    VkFence fence_{};
    bool recording_{};
    std::vector<std::function<void()>> garbage_;

    struct StageChunk {
        VkBuffer buffer{};
        VkDeviceMemory memory{};
        std::uint8_t *data{};
        VkDeviceSize size{}, used{};
    };
    std::vector<StageChunk> stage_;
    VkDeviceSize staged_bytes_{};

    VkRenderPass main_pass_{}, stencil_pass_{}, unresolve_pass_{}, output_pass_{};
    VkDescriptorSetLayout set_layout_{};
    VkPipelineLayout pipeline_layout_{};
    VkPipelineCache pipeline_cache_{};
    std::string pipeline_cache_path_;
    std::unordered_map<PipelineKey, VkPipeline, PipelineKeyHash> pipelines_;
    std::uint64_t pipelines_created_{};
    VkShaderModule ge_vert_{}, ge_frag_{}, fullscreen_vert_{}, reinterpret_frag_{}, fill_frag_{}, stencil_frag_{}, copy_frag_{};
    VkShaderModule present_vert_{}, present_frag_{};
    VkSampler samplers_[8]{}; // linear | clamp_u << 1 | clamp_v << 2
    std::vector<VkDescriptorPool> descriptor_pools_;
    std::size_t descriptor_pool_{};
    std::uint32_t descriptors_left_{};

    // Recording state, reset by end_pass and submissions.
    Pass pass_{Pass::None};
    Target *pass_target_{};
    VkPipeline bound_pipeline_{};
    VkDescriptorSet bound_set_{};
    VkImageView last_view_{};
    VkSampler last_sampler_{};
    VkDescriptorSet last_set_{};
    std::vector<std::uint8_t> bound_push_;
    VkRect2D bound_scissor_{{-1, -1}, {0, 0}};
    std::uint32_t bound_ref_{~0u}, bound_compare_{~0u}, bound_write_{~0u};
    float bound_blend_[4]{-1, -1, -1, -1};

    std::deque<Target> targets_; // handle - 1; references stay valid as it grows
    std::vector<GpuTarget> free_targets_;
    std::unordered_map<const void *, Texture> textures_;
    Image white_;          // sampled by draws without a texture
    Image scratch_;        // snapshots of the target being drawn
    Image staging_;        // native-size pixels on their way in or out at scales above 1
    Image present_image_;  // CPU-drawn framebuffers
    Image output_;
    VkFramebuffer output_framebuffer_{};
};

class GeVk final : public GeSurfaces {
public:
    GeVk(psprecomp::GuestMemory &memory, const VkContext &context, const VkOptions &options);

    // Presents the framebuffer at `address` into the output image and reads it back as RGBA
    // rows, top row first, (480 x 272) * scale. Alpha is opaque.
    void capture(std::uint32_t address, std::uint32_t stride, std::uint32_t format, std::vector<std::uint8_t> &rgba);
    // Only the presentation half, for hosts that show output_image() themselves.
    void render_output(std::uint32_t address, std::uint32_t stride, std::uint32_t format);
    [[nodiscard]] VkGpu &device() noexcept { return vk_; }

private:
    VkGpu &vk_;
};

// A headless context and the renderer on it, for runners. Throws psprecomp::Error.
class VkHost {
public:
    VkHost(psprecomp::GuestMemory &memory, int scale, const VkOptions &options, bool validation);
    ~VkHost();
    VkHost(const VkHost &) = delete;
    VkHost &operator=(const VkHost &) = delete;

    [[nodiscard]] GeVk &renderer() noexcept { return *renderer_; }
    [[nodiscard]] const std::string &device() const noexcept { return context_.device_name; }
    [[nodiscard]] int scale() const noexcept { return renderer_->render_scale(); }

private:
    VkContext context_;
    std::unique_ptr<GeVk> renderer_;
};

} // namespace pspweb
