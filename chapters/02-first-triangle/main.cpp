#include <vulkan/vulkan.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + ": VkResult " + std::to_string(result));
}

template<class T, class Query>
std::vector<T> enumerate(Query query, const char* operation) {
    uint32_t count = 0;
    std::vector<T> values;
    VkResult result;
    do {
        check(query(&count, static_cast<T*>(nullptr)), operation);
        if (count == 0) return {};
        values.resize(count);
        result = query(&count, values.data());
    } while (result == VK_INCOMPLETE);
    check(result, operation);
    values.resize(count);
    return values;
}

bool hasExtension(const std::vector<VkExtensionProperties>& extensions, const char* name) {
    return std::ranges::any_of(extensions, [name](const auto& extension) {
        return std::strcmp(extension.extensionName, name) == 0;
    });
}

class Triangle {
public:
    ~Triangle() {
        if (device) {
            vkDeviceWaitIdle(device);
            for (auto semaphore : finished) vkDestroySemaphore(device, semaphore, nullptr);
            if (available) vkDestroySemaphore(device, available, nullptr);
            if (frameDone) vkDestroyFence(device, frameDone, nullptr);
            if (pool) vkDestroyCommandPool(device, pool, nullptr);
            if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
            if (layout) vkDestroyPipelineLayout(device, layout, nullptr);
            if (vertex) vkDestroyShaderModule(device, vertex, nullptr);
            if (fragment) vkDestroyShaderModule(device, fragment, nullptr);
            for (auto view : views) vkDestroyImageView(device, view, nullptr);
            if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (surface) vkDestroySurfaceKHR(instance, surface, nullptr);
        if (instance) vkDestroyInstance(instance, nullptr);
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
    }

    void run(unsigned frameLimit) {
        initialize();
        unsigned frames = 0;
        while (!glfwWindowShouldClose(window) && (!frameLimit || frames < frameLimit)) {
            glfwPollEvents();
            if (!draw()) break;
            ++frames;
        }
        check(vkDeviceWaitIdle(device), "vkDeviceWaitIdle");
        std::cout << "Presented " << frames << " frame(s)\n";
        if (frameLimit && frames != frameLimit) throw std::runtime_error("Frame limit not reached");
    }

private:
    GLFWwindow* window = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkSurfaceFormatKHR format{};
    VkExtent2D extent{};
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    VkShaderModule vertex = VK_NULL_HANDLE;
    VkShaderModule fragment = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence frameDone = VK_NULL_HANDLE;
    VkSemaphore available = VK_NULL_HANDLE;
    std::vector<VkSemaphore> finished;

    void initialize() {
        if (!glfwInit() || !glfwVulkanSupported()) throw std::runtime_error("GLFW cannot find Vulkan");
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
        window = glfwCreateWindow(800, 520, "Vulkan Cookbook - Chapter 02", nullptr, nullptr);
        if (!window) throw std::runtime_error("Cannot open a window");
        createInstance();
        check(glfwCreateWindowSurface(instance, window, nullptr, &surface), "glfwCreateWindowSurface");
        createDevice();
        createSwapchain();
        createPipeline();
        createCommandsAndSync();
    }

    void createInstance() {
        uint32_t count = 0;
        const auto names = glfwGetRequiredInstanceExtensions(&count);
        if (!names || !count) throw std::runtime_error("No GLFW surface extensions");
        std::vector<const char*> required(names, names + count);
        auto extensions = enumerate<VkExtensionProperties>([](auto* n, auto* data) {
            return vkEnumerateInstanceExtensionProperties(nullptr, n, data);
        }, "vkEnumerateInstanceExtensionProperties");
        VkInstanceCreateFlags flags = 0;
        if (hasExtension(extensions, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
            required.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "Vulkan Cookbook";
        app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        info.flags = flags;
        info.pApplicationInfo = &app;
        info.enabledExtensionCount = static_cast<uint32_t>(required.size());
        info.ppEnabledExtensionNames = required.data();
        check(vkCreateInstance(&info, nullptr, &instance), "vkCreateInstance");
    }

    void createDevice() {
        auto devices = enumerate<VkPhysicalDevice>([this](auto* n, auto* data) {
            return vkEnumeratePhysicalDevices(instance, n, data);
        }, "vkEnumeratePhysicalDevices");
        for (auto candidate : devices) {
            VkPhysicalDeviceProperties properties;
            vkGetPhysicalDeviceProperties(candidate, &properties);
            if (properties.apiVersion < VK_API_VERSION_1_3) continue;
            VkPhysicalDeviceDynamicRenderingFeatures dynamic{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
            VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            features.pNext = &dynamic;
            vkGetPhysicalDeviceFeatures2(candidate, &features);
            if (!dynamic.dynamicRendering) continue;

            auto extensions = enumerate<VkExtensionProperties>([candidate](auto* n, auto* data) {
                return vkEnumerateDeviceExtensionProperties(candidate, nullptr, n, data);
            }, "vkEnumerateDeviceExtensionProperties");
            if (!hasExtension(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;

            uint32_t count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, nullptr);
            std::vector<VkQueueFamilyProperties> families(count);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, families.data());
            for (uint32_t i = 0; i < count; ++i) {
                if (!families[i].queueCount || !(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
                VkBool32 canPresent = false;
                check(vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface, &canPresent),
                      "vkGetPhysicalDeviceSurfaceSupportKHR");
                if (!canPresent) continue;

                physical = candidate;
                family = i;
                std::vector<const char*> required{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
                if (hasExtension(extensions, "VK_KHR_portability_subset"))
                    required.push_back("VK_KHR_portability_subset");
                const float priority = 1.f;
                VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
                queueInfo.queueFamilyIndex = family;
                queueInfo.queueCount = 1;
                queueInfo.pQueuePriorities = &priority;
                VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
                info.pNext = &dynamic;
                info.queueCreateInfoCount = 1;
                info.pQueueCreateInfos = &queueInfo;
                info.enabledExtensionCount = static_cast<uint32_t>(required.size());
                info.ppEnabledExtensionNames = required.data();
                check(vkCreateDevice(physical, &info, nullptr, &device), "vkCreateDevice");
                vkGetDeviceQueue(device, family, 0, &queue);
                std::cout << "Vulkan device: " << properties.deviceName << '\n';
                return;
            }
        }
        throw std::runtime_error("Need Vulkan 1.3 with dynamic rendering and one graphics/present queue");
    }

    void createSwapchain() {
        VkSurfaceCapabilitiesKHR capabilities;
        check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &capabilities),
              "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
        if (!(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
            throw std::runtime_error("Surface images do not support color attachments");
        auto formats = enumerate<VkSurfaceFormatKHR>([this](auto* n, auto* data) {
            return vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, n, data);
        }, "vkGetPhysicalDeviceSurfaceFormatsKHR");
        if (formats.empty()) throw std::runtime_error("No surface formats");
        auto preferred = std::ranges::find_if(formats, [](auto f) {
            return f.format == VK_FORMAT_B8G8R8A8_SRGB && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        });
        format = preferred == formats.end() ? formats.front() : *preferred;
        if (format.format == VK_FORMAT_UNDEFINED) format.format = VK_FORMAT_B8G8R8A8_SRGB;
        extent = capabilities.currentExtent;
        if (extent.width == std::numeric_limits<uint32_t>::max()) {
            int width, height;
            glfwGetFramebufferSize(window, &width, &height);
            extent.width = std::clamp(static_cast<uint32_t>(width), capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
            extent.height = std::clamp(static_cast<uint32_t>(height), capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
        }
        uint32_t count = capabilities.minImageCount + 1;
        if (capabilities.maxImageCount) count = std::min(count, capabilities.maxImageCount);
        const std::array alphaModes{VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                                    VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR};
        auto alpha = std::ranges::find_if(alphaModes, [&](auto mode) { return capabilities.supportedCompositeAlpha & mode; });
        if (alpha == alphaModes.end()) throw std::runtime_error("No composite alpha mode");
        VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        info.surface = surface;
        info.minImageCount = count;
        info.imageFormat = format.format;
        info.imageColorSpace = format.colorSpace;
        info.imageExtent = extent;
        info.imageArrayLayers = 1;
        info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.preTransform = capabilities.currentTransform;
        info.compositeAlpha = *alpha;
        info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        info.clipped = VK_TRUE;
        check(vkCreateSwapchainKHR(device, &info, nullptr, &swapchain), "vkCreateSwapchainKHR");

        images = enumerate<VkImage>([this](auto* n, auto* data) {
            return vkGetSwapchainImagesKHR(device, swapchain, n, data);
        }, "vkGetSwapchainImagesKHR");
        for (auto image : images) {
            VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            viewInfo.image = image;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = format.format;
            viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkImageView view;
            check(vkCreateImageView(device, &viewInfo, nullptr, &view), "vkCreateImageView");
            views.push_back(view);
        }
    }

    VkShaderModule loadShader(const char* filename) {
        const std::string path = std::string(SHADER_DIR) + "/" + filename;
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) throw std::runtime_error("Cannot open " + path);
        const auto bytes = static_cast<std::streamoff>(file.tellg());
        if (bytes <= 0 || bytes % sizeof(uint32_t)) throw std::runtime_error("Invalid SPIR-V: " + path);
        std::vector<uint32_t> words(bytes / sizeof(uint32_t));
        if (!file.seekg(0).read(reinterpret_cast<char*>(words.data()), bytes))
            throw std::runtime_error("Cannot read " + path);
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = static_cast<size_t>(bytes);
        info.pCode = words.data();
        VkShaderModule module;
        check(vkCreateShaderModule(device, &info, nullptr, &module), "vkCreateShaderModule");
        return module;
    }

    void createPipeline() {
        vertex = loadShader("triangle.vert.spv");
        fragment = loadShader("triangle.frag.spv");
        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertex;
        stages[0].pName = "main";
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragment;
        stages[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        const VkViewport viewport{0, 0, float(extent.width), float(extent.height), 0, 1};
        const VkRect2D scissor{{0, 0}, extent};
        VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewportState.viewportCount = viewportState.scissorCount = 1;
        viewportState.pViewports = &viewport;
        viewportState.pScissors = &scissor;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.lineWidth = 1.f;
        VkPipelineMultisampleStateCreateInfo sampling{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        sampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState color{};
        color.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1;
        blend.pAttachments = &color;
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        check(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout), "vkCreatePipelineLayout");
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &format.format;
        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        info.pNext = &rendering;
        info.stageCount = static_cast<uint32_t>(stages.size());
        info.pStages = stages.data();
        info.pVertexInputState = &input;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewportState;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &sampling;
        info.pColorBlendState = &blend;
        info.layout = layout;
        check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline), "vkCreateGraphicsPipelines");
    }

    VkSemaphore createSemaphore() {
        VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore semaphore;
        check(vkCreateSemaphore(device, &info, nullptr, &semaphore), "vkCreateSemaphore");
        return semaphore;
    }

    void createCommandsAndSync() {
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = family;
        check(vkCreateCommandPool(device, &poolInfo, nullptr, &pool), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool = pool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(device, &alloc, &cmd), "vkAllocateCommandBuffers");
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        check(vkCreateFence(device, &fence, nullptr, &frameDone), "vkCreateFence");
        available = createSemaphore();
        // Acquiring the same image again makes its previous present wait safe to reuse.
        for (size_t i = 0; i < images.size(); ++i) finished.push_back(createSemaphore());
    }

    void record(uint32_t index) {
        check(vkResetCommandBuffer(cmd, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer");

        // The frame clears the whole image, so its previous contents can be discarded.
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = images[index];
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = views[index];
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color = {{0.02f, 0.035f, 0.055f, 1.0f}};
        VkRenderingInfo renderingInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
        renderingInfo.renderArea = {{0, 0}, extent};
        renderingInfo.layerCount = 1;
        renderingInfo.colorAttachmentCount = 1;
        renderingInfo.pColorAttachments = &color;

        vkCmdBeginRendering(cmd, &renderingInfo);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);

        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = 0;
        barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
    }

    bool draw() {
        check(vkWaitForFences(device, 1, &frameDone, VK_TRUE, UINT64_MAX), "vkWaitForFences");
        uint32_t index;
        VkResult acquired = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, available, VK_NULL_HANDLE, &index);
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR) return false;
        if (acquired != VK_SUBOPTIMAL_KHR) check(acquired, "vkAcquireNextImageKHR");
        record(index);

        const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submission{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submission.waitSemaphoreCount = 1;
        submission.pWaitSemaphores = &available;
        submission.pWaitDstStageMask = &waitStage;
        submission.commandBufferCount = 1;
        submission.pCommandBuffers = &cmd;
        submission.signalSemaphoreCount = 1;
        submission.pSignalSemaphores = &finished[index];
        check(vkResetFences(device, 1, &frameDone), "vkResetFences");
        check(vkQueueSubmit(queue, 1, &submission, frameDone), "vkQueueSubmit");

        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &finished[index];
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain;
        present.pImageIndices = &index;
        VkResult result = vkQueuePresentKHR(queue, &present);
        if (result == VK_ERROR_OUT_OF_DATE_KHR) return false;
        if (result != VK_SUBOPTIMAL_KHR) check(result, "vkQueuePresentKHR");
        return true;
    }
};
} // namespace

int main(int argc, char** argv) {
    try {
        unsigned frames = 0;
        if (argc == 3 && std::string(argv[1]) == "--frames") {
            const std::string value = argv[2];
            size_t end;
            const auto count = std::stoul(value, &end);
            if (end != value.size() || count == 0 || count > std::numeric_limits<unsigned>::max())
                throw std::runtime_error("--frames requires a positive frame count");
            frames = static_cast<unsigned>(count);
        } else if (argc != 1) {
            throw std::runtime_error("Usage: chapter02_triangle [--frames N]");
        }
        Triangle triangle;
        triangle.run(frames);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
