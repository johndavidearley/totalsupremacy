#include <vulkan/vulkan_core.h>
#include "rts/Renderer3D.hpp"
#include "rts/SceneGeometry.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef TOTALSUPREMACY_SHADER_DIR
#define TOTALSUPREMACY_SHADER_DIR "shaders"
#endif

namespace rts {
namespace {

struct alignas(16) PushConstants {
    float viewport[2];
    float origin[2];
    float center[2];
    float target[2];
    float angles[4];
    float scale;
    float depthRange;
    float focalLength;
    float nearPlane;
    float farPlane;
    std::int32_t perspective;
    std::int32_t screenOverlay;
};

static_assert(sizeof(PushConstants) == 80);

void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) +
                                 " failed (Vulkan " + std::to_string(result) + ")");
    }
}

bool hasExtension(const std::vector<VkExtensionProperties>& extensions,
                  const char* name) {
    return std::any_of(extensions.begin(), extensions.end(),
        [name](const VkExtensionProperties& extension) {
            return std::strcmp(extension.extensionName, name) == 0;
        });
}

std::vector<char> readShader(const char* name) {
    const std::string path = std::string(TOTALSUPREMACY_SHADER_DIR) + "/" + name;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error("Unable to open compiled shader: " + path);
    }
    const auto size = file.tellg();
    if (size <= 0 || size % 4 != 0) {
        throw std::runtime_error("Compiled shader has invalid SPIR-V size: " + path);
    }
    std::vector<char> bytes(static_cast<std::size_t>(size));
    file.seekg(0);
    file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!file) {
        throw std::runtime_error("Unable to read compiled shader: " + path);
    }
    return bytes;
}

} // namespace

struct Renderer3D::Impl {
    struct Frame {
        VkCommandBuffer command = VK_NULL_HANDLE;
        VkSemaphore imageAvailable = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkQueryPool gpuTimestampQueries = VK_NULL_HANDLE;
        VkBuffer vertexBuffer = VK_NULL_HANDLE;
        VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
        VkDeviceSize vertexCapacity = 0;
        std::uint64_t submittedFrame = 0;
        bool gpuTimestampPending = false;
    };

    GLFWwindow* window;
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    RendererStats frameStats;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    VkQueue presentQueue = VK_NULL_HANDLE;
    std::uint32_t graphicsFamily = 0;
    std::uint32_t presentFamily = 0;
    std::uint32_t graphicsTimestampValidBits = 0;
    double timestampPeriodNanoseconds = 0.0;
    std::uint64_t submittedFrameCount = 0;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapchainFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    std::vector<VkImage> swapchainImages;
    std::vector<VkImageView> swapchainViews;
    std::vector<VkSemaphore> renderFinishedSemaphores;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    std::vector<VkImage> depthImages;
    std::vector<VkDeviceMemory> depthMemories;
    std::vector<VkImageView> depthViews;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline scenePipeline = VK_NULL_HANDLE;
    VkPipeline overlayPipeline = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    std::array<Frame, 2> frames{};
    std::vector<VkFence> imageFences;
    std::size_t frameIndex = 0;

    explicit Impl(GLFWwindow* target) : window(target) {
        try {
            initialize();
        } catch (...) {
            cleanup();
            throw;
        }
    }

    ~Impl() {
        cleanup();
    }

    void initialize() {
        if (glfwVulkanSupported() != GLFW_TRUE) {
            throw std::runtime_error(
                "GLFW reports no Vulkan loader. Install the MacPorts Vulkan "
                "loader and MoltenVK driver.");
        }
        createInstance();
        check(glfwCreateWindowSurface(instance, window, nullptr, &surface),
              "Create GLFW Vulkan surface");
        selectPhysicalDevice();
        createDevice();
        createCommandPool();
        createFrameSync();
        recreateSwapchain();
    }

    void createInstance() {
        std::uint32_t glfwExtensionCount = 0;
        const char** glfwExtensions =
            glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
        if (glfwExtensions == nullptr || glfwExtensionCount == 0) {
            throw std::runtime_error("GLFW did not provide Vulkan surface extensions.");
        }
        std::vector<const char*> extensions(glfwExtensions,
                                            glfwExtensions + glfwExtensionCount);
        std::uint32_t extensionCount = 0;
        check(vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr),
              "Enumerate Vulkan instance extensions");
        std::vector<VkExtensionProperties> available(extensionCount);
        check(vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount,
                                                     available.data()),
              "Read Vulkan instance extensions");
        VkInstanceCreateFlags flags = 0;
        if (hasExtension(available, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
            extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }

        VkApplicationInfo application{};
        application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        application.pApplicationName = "Total Supremacy";
        application.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
        application.pEngineName = "Total Supremacy";
        application.engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
        application.apiVersion = VK_API_VERSION_1_0;

        VkInstanceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.flags = flags;
        createInfo.pApplicationInfo = &application;
        createInfo.enabledExtensionCount =
            static_cast<std::uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();
        check(vkCreateInstance(&createInfo, nullptr, &instance), "Create Vulkan instance");
    }

    void selectPhysicalDevice() {
        std::uint32_t count = 0;
        check(vkEnumeratePhysicalDevices(instance, &count, nullptr),
              "Enumerate Vulkan physical devices");
        if (count == 0) {
            throw std::runtime_error("No Vulkan physical device is available.");
        }
        std::vector<VkPhysicalDevice> devices(count);
        check(vkEnumeratePhysicalDevices(instance, &count, devices.data()),
              "Read Vulkan physical devices");

        for (const auto candidate : devices) {
            std::uint32_t familyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
            std::vector<VkQueueFamilyProperties> families(familyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount,
                                                      families.data());
            std::uint32_t graphics = std::numeric_limits<std::uint32_t>::max();
            std::uint32_t present = std::numeric_limits<std::uint32_t>::max();
            for (std::uint32_t i = 0; i < familyCount; ++i) {
                if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
                    graphics = i;
                }
                VkBool32 supportsPresent = VK_FALSE;
                check(vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface,
                                                           &supportsPresent),
                      "Check Vulkan presentation support");
                if (supportsPresent == VK_TRUE) {
                    present = i;
                }
            }
            if (graphics == std::numeric_limits<std::uint32_t>::max() ||
                present == std::numeric_limits<std::uint32_t>::max()) {
                continue;
            }

            std::uint32_t deviceExtensionCount = 0;
            check(vkEnumerateDeviceExtensionProperties(candidate, nullptr,
                                                        &deviceExtensionCount, nullptr),
                  "Enumerate Vulkan device extensions");
            std::vector<VkExtensionProperties> deviceExtensions(deviceExtensionCount);
            check(vkEnumerateDeviceExtensionProperties(candidate, nullptr,
                                                        &deviceExtensionCount,
                                                        deviceExtensions.data()),
                  "Read Vulkan device extensions");
            if (!hasExtension(deviceExtensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
                continue;
            }

            std::uint32_t formatCount = 0;
            std::uint32_t modeCount = 0;
            check(vkGetPhysicalDeviceSurfaceFormatsKHR(candidate, surface,
                                                        &formatCount, nullptr),
                  "Check Vulkan surface formats");
            check(vkGetPhysicalDeviceSurfacePresentModesKHR(candidate, surface,
                                                             &modeCount, nullptr),
                  "Check Vulkan present modes");
            if (formatCount == 0 || modeCount == 0) {
                continue;
            }
            physicalDevice = candidate;
            graphicsFamily = graphics;
            presentFamily = present;
            graphicsTimestampValidBits = families[graphics].timestampValidBits;
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(candidate, &properties);
            timestampPeriodNanoseconds = properties.limits.timestampPeriod;
            return;
        }
        throw std::runtime_error(
            "No Vulkan device supports graphics, presentation, and swapchains.");
    }

    void createDevice() {
        const std::array<float, 1> priorities{1.0F};
        std::array<std::uint32_t, 2> families{graphicsFamily, presentFamily};
        const std::uint32_t familyCount = graphicsFamily == presentFamily ? 1U : 2U;
        std::array<VkDeviceQueueCreateInfo, 2> queueInfos{};
        for (std::uint32_t i = 0; i < familyCount; ++i) {
            queueInfos[i].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueInfos[i].queueFamilyIndex = families[i];
            queueInfos[i].queueCount = 1;
            queueInfos[i].pQueuePriorities = priorities.data();
        }

        std::uint32_t extensionCount = 0;
        check(vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr,
                                                    &extensionCount, nullptr),
              "Enumerate Vulkan device extensions");
        std::vector<VkExtensionProperties> available(extensionCount);
        check(vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr,
                                                    &extensionCount, available.data()),
              "Read Vulkan device extensions");
        std::vector<const char*> extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        constexpr const char* portabilitySubset = "VK_KHR_portability_subset";
        if (hasExtension(available, portabilitySubset)) {
            extensions.push_back(portabilitySubset);
        }

        VkPhysicalDeviceFeatures features{};
        VkDeviceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.queueCreateInfoCount = familyCount;
        createInfo.pQueueCreateInfos = queueInfos.data();
        createInfo.enabledExtensionCount =
            static_cast<std::uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();
        createInfo.pEnabledFeatures = &features;
        check(vkCreateDevice(physicalDevice, &createInfo, nullptr, &device),
              "Create Vulkan logical device");
        vkGetDeviceQueue(device, graphicsFamily, 0, &graphicsQueue);
        vkGetDeviceQueue(device, presentFamily, 0, &presentQueue);
    }

    void createCommandPool() {
        VkCommandPoolCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        createInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        createInfo.queueFamilyIndex = graphicsFamily;
        check(vkCreateCommandPool(device, &createInfo, nullptr, &commandPool),
              "Create Vulkan command pool");

        std::array<VkCommandBuffer, 2> commands{};
        VkCommandBufferAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocation.commandPool = commandPool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = static_cast<std::uint32_t>(commands.size());
        check(vkAllocateCommandBuffers(device, &allocation, commands.data()),
              "Allocate Vulkan command buffers");
        for (std::size_t i = 0; i < frames.size(); ++i) {
            frames[i].command = commands[i];
        }
    }

    void createFrameSync() {
        VkSemaphoreCreateInfo semaphore{};
        semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkFenceCreateInfo fence{};
        fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        for (auto& frame : frames) {
            check(vkCreateSemaphore(device, &semaphore, nullptr,
                                    &frame.imageAvailable),
                  "Create image-available semaphore");
            check(vkCreateFence(device, &fence, nullptr, &frame.fence),
                  "Create frame fence");
        }
        if (graphicsTimestampValidBits > 0) {
            VkQueryPoolCreateInfo queryPool{};
            queryPool.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            queryPool.queryType = VK_QUERY_TYPE_TIMESTAMP;
            queryPool.queryCount = 2;
            for (auto& frame : frames) {
                check(vkCreateQueryPool(device, &queryPool, nullptr,
                                        &frame.gpuTimestampQueries),
                      "Create GPU timestamp query pool");
            }
        }
    }

    std::uint32_t memoryType(std::uint32_t filter,
                             VkMemoryPropertyFlags properties) const {
        VkPhysicalDeviceMemoryProperties available{};
        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &available);
        for (std::uint32_t i = 0; i < available.memoryTypeCount; ++i) {
            if ((filter & (1U << i)) != 0 &&
                (available.memoryTypes[i].propertyFlags & properties) == properties) {
                return i;
            }
        }
        throw std::runtime_error("No Vulkan memory type satisfies the resource requirements.");
    }

    void createDepthResources(std::size_t imageCount) {
        constexpr std::array<VkFormat, 3> candidates{
            VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT,
            VK_FORMAT_D24_UNORM_S8_UINT
        };
        for (const auto format : candidates) {
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &properties);
            if ((properties.optimalTilingFeatures &
                 VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0) {
                depthFormat = format;
                break;
            }
        }
        if (depthFormat == VK_FORMAT_UNDEFINED) {
            throw std::runtime_error("No supported Vulkan depth attachment format.");
        }
        depthImages.resize(imageCount, VK_NULL_HANDLE);
        depthMemories.resize(imageCount, VK_NULL_HANDLE);
        depthViews.resize(imageCount, VK_NULL_HANDLE);
        for (std::size_t i = 0; i < imageCount; ++i) {
            VkImageCreateInfo image{};
            image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            image.imageType = VK_IMAGE_TYPE_2D;
            image.extent = {extent.width, extent.height, 1};
            image.mipLevels = 1;
            image.arrayLayers = 1;
            image.format = depthFormat;
            image.tiling = VK_IMAGE_TILING_OPTIMAL;
            image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            image.samples = VK_SAMPLE_COUNT_1_BIT;
            image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            check(vkCreateImage(device, &image, nullptr, &depthImages[i]),
                  "Create depth image");
            VkMemoryRequirements requirements{};
            vkGetImageMemoryRequirements(device, depthImages[i], &requirements);
            VkMemoryAllocateInfo allocation{};
            allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = memoryType(
                requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            check(vkAllocateMemory(device, &allocation, nullptr, &depthMemories[i]),
                  "Allocate depth image memory");
            check(vkBindImageMemory(device, depthImages[i], depthMemories[i], 0),
                  "Bind depth image memory");

            VkImageViewCreateInfo view{};
            view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view.image = depthImages[i];
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = depthFormat;
            view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            view.subresourceRange.levelCount = 1;
            view.subresourceRange.layerCount = 1;
            check(vkCreateImageView(device, &view, nullptr, &depthViews[i]),
                  "Create depth image view");
        }
    }

    void createSwapchain() {
        VkSurfaceCapabilitiesKHR capabilities{};
        check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface,
                                                         &capabilities),
              "Read Vulkan surface capabilities");
        std::uint32_t formatCount = 0;
        check(vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface,
                                                    &formatCount, nullptr),
              "Read Vulkan surface formats");
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        check(vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface,
                                                    &formatCount, formats.data()),
              "Read Vulkan surface formats");
        std::uint32_t modeCount = 0;
        check(vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface,
                                                         &modeCount, nullptr),
              "Read Vulkan present modes");
        std::vector<VkPresentModeKHR> modes(modeCount);
        check(vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface,
                                                         &modeCount, modes.data()),
              "Read Vulkan present modes");

        VkSurfaceFormatKHR surfaceFormat = formats.front();
        for (const auto& candidate : formats) {
            if (candidate.format == VK_FORMAT_B8G8R8A8_SRGB &&
                candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                surfaceFormat = candidate;
                break;
            }
        }
        VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
        if (std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_MAILBOX_KHR) != modes.end()) {
            presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
        }

        if (capabilities.currentExtent.width !=
            std::numeric_limits<std::uint32_t>::max()) {
            extent = capabilities.currentExtent;
        } else {
            int width = 0;
            int height = 0;
            glfwGetFramebufferSize(window, &width, &height);
            extent.width = std::clamp(static_cast<std::uint32_t>(std::max(0, width)),
                                      capabilities.minImageExtent.width,
                                      capabilities.maxImageExtent.width);
            extent.height = std::clamp(static_cast<std::uint32_t>(std::max(0, height)),
                                       capabilities.minImageExtent.height,
                                       capabilities.maxImageExtent.height);
        }
        std::uint32_t imageCount = capabilities.minImageCount + 1;
        if (capabilities.maxImageCount > 0) {
            imageCount = std::min(imageCount, capabilities.maxImageCount);
        }

        VkSwapchainCreateInfoKHR createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        createInfo.surface = surface;
        createInfo.minImageCount = imageCount;
        createInfo.imageFormat = surfaceFormat.format;
        createInfo.imageColorSpace = surfaceFormat.colorSpace;
        createInfo.imageExtent = extent;
        createInfo.imageArrayLayers = 1;
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        const std::array<std::uint32_t, 2> queueFamilies{
            graphicsFamily, presentFamily
        };
        if (graphicsFamily != presentFamily) {
            createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
            createInfo.queueFamilyIndexCount =
                static_cast<std::uint32_t>(queueFamilies.size());
            createInfo.pQueueFamilyIndices = queueFamilies.data();
        } else {
            createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        }
        createInfo.preTransform = capabilities.currentTransform;
        constexpr std::array<VkCompositeAlphaFlagBitsKHR, 4> alphaModes{
            VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
        };
        createInfo.compositeAlpha = alphaModes.front();
        for (const auto alpha : alphaModes) {
            if ((capabilities.supportedCompositeAlpha & alpha) != 0) {
                createInfo.compositeAlpha = alpha;
                break;
            }
        }
        createInfo.presentMode = presentMode;
        createInfo.clipped = VK_TRUE;
        check(vkCreateSwapchainKHR(device, &createInfo, nullptr, &swapchain),
              "Create Vulkan swapchain");
        swapchainFormat = surfaceFormat.format;

        check(vkGetSwapchainImagesKHR(device, swapchain, &imageCount, nullptr),
              "Get Vulkan swapchain images");
        swapchainImages.resize(imageCount);
        check(vkGetSwapchainImagesKHR(device, swapchain, &imageCount,
                                      swapchainImages.data()),
              "Read Vulkan swapchain images");
        renderFinishedSemaphores.resize(swapchainImages.size(), VK_NULL_HANDLE);
        VkSemaphoreCreateInfo semaphore{};
        semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        for (auto& finished : renderFinishedSemaphores) {
            check(vkCreateSemaphore(device, &semaphore, nullptr, &finished),
                  "Create swapchain render-finished semaphore");
        }
        swapchainViews.resize(swapchainImages.size(), VK_NULL_HANDLE);
        for (std::size_t i = 0; i < swapchainImages.size(); ++i) {
            VkImageViewCreateInfo view{};
            view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view.image = swapchainImages[i];
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = swapchainFormat;
            view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            view.subresourceRange.levelCount = 1;
            view.subresourceRange.layerCount = 1;
            check(vkCreateImageView(device, &view, nullptr, &swapchainViews[i]),
                  "Create swapchain image view");
        }
        createDepthResources(swapchainImages.size());
    }

    void createRenderPass() {
        VkAttachmentDescription color{};
        color.format = swapchainFormat;
        color.samples = VK_SAMPLE_COUNT_1_BIT;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentDescription depth{};
        depth.format = depthFormat;
        depth.samples = VK_SAMPLE_COUNT_1_BIT;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        const std::array<VkAttachmentDescription, 2> attachments{color, depth};
        VkAttachmentReference colorReference{};
        colorReference.attachment = 0;
        colorReference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference depthReference{};
        depthReference.attachment = 1;
        depthReference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorReference;
        subpass.pDepthStencilAttachment = &depthReference;
        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.dstStageMask = dependency.srcStageMask;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        createInfo.attachmentCount = static_cast<std::uint32_t>(attachments.size());
        createInfo.pAttachments = attachments.data();
        createInfo.subpassCount = 1;
        createInfo.pSubpasses = &subpass;
        createInfo.dependencyCount = 1;
        createInfo.pDependencies = &dependency;
        check(vkCreateRenderPass(device, &createInfo, nullptr, &renderPass),
              "Create Vulkan render pass");
    }

    VkShaderModule createShaderModule(const std::vector<char>& code) {
        VkShaderModuleCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        createInfo.codeSize = code.size();
        createInfo.pCode = reinterpret_cast<const std::uint32_t*>(code.data());
        VkShaderModule module = VK_NULL_HANDLE;
        check(vkCreateShaderModule(device, &createInfo, nullptr, &module),
              "Create Vulkan shader module");
        return module;
    }

    VkPipeline createGraphicsPipeline(VkShaderModule vertexShader,
                                      VkShaderModule fragmentShader,
                                      bool depthTest) {
        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertexShader;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragmentShader;
        stages[1].pName = "main";

        VkVertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = sizeof(SceneVertex);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        std::array<VkVertexInputAttributeDescription, 2> attributes{};
        attributes[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT,
                         static_cast<std::uint32_t>(offsetof(SceneVertex, x))};
        attributes[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT,
                         static_cast<std::uint32_t>(offsetof(SceneVertex, red))};
        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount =
            static_cast<std::uint32_t>(attributes.size());
        vertexInput.pVertexAttributeDescriptions = attributes.data();
        VkPipelineInputAssemblyStateCreateInfo assembly{};
        assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo viewport{};
        viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rasterization{};
        rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterization.polygonMode = VK_POLYGON_MODE_FILL;
        rasterization.cullMode = VK_CULL_MODE_NONE;
        rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterization.lineWidth = 1.0F;
        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depth{};
        depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth.depthTestEnable = depthTest ? VK_TRUE : VK_FALSE;
        depth.depthWriteEnable = depthTest ? VK_TRUE : VK_FALSE;
        depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                                         VK_COLOR_COMPONENT_G_BIT |
                                         VK_COLOR_COMPONENT_B_BIT |
                                         VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{};
        blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blend.attachmentCount = 1;
        blend.pAttachments = &blendAttachment;
        constexpr std::array<VkDynamicState, 2> dynamicStates{
            VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR
        };
        VkPipelineDynamicStateCreateInfo dynamic{};
        dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount =
            static_cast<std::uint32_t>(dynamicStates.size());
        dynamic.pDynamicStates = dynamicStates.data();
        VkGraphicsPipelineCreateInfo pipeline{};
        pipeline.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipeline.stageCount = static_cast<std::uint32_t>(stages.size());
        pipeline.pStages = stages.data();
        pipeline.pVertexInputState = &vertexInput;
        pipeline.pInputAssemblyState = &assembly;
        pipeline.pViewportState = &viewport;
        pipeline.pRasterizationState = &rasterization;
        pipeline.pMultisampleState = &multisample;
        pipeline.pDepthStencilState = &depth;
        pipeline.pColorBlendState = &blend;
        pipeline.pDynamicState = &dynamic;
        pipeline.layout = pipelineLayout;
        pipeline.renderPass = renderPass;
        pipeline.subpass = 0;
        VkPipeline result = VK_NULL_HANDLE;
        check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline,
                                        nullptr, &result),
              "Create Vulkan graphics pipeline");
        return result;
    }

    void createPipelines() {
        VkPushConstantRange range{};
        range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        range.offset = 0;
        range.size = sizeof(PushConstants);
        VkPipelineLayoutCreateInfo layout{};
        layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout.pushConstantRangeCount = 1;
        layout.pPushConstantRanges = &range;
        check(vkCreatePipelineLayout(device, &layout, nullptr, &pipelineLayout),
              "Create Vulkan pipeline layout");

        const auto vertexCode = readShader("scene.vert.spv");
        const auto fragmentCode = readShader("scene.frag.spv");
        const auto vertexShader = createShaderModule(vertexCode);
        VkShaderModule fragmentShader = VK_NULL_HANDLE;
        try {
            fragmentShader = createShaderModule(fragmentCode);
            scenePipeline = createGraphicsPipeline(vertexShader, fragmentShader, true);
            overlayPipeline = createGraphicsPipeline(vertexShader, fragmentShader, false);
        } catch (...) {
            vkDestroyShaderModule(device, vertexShader, nullptr);
            if (fragmentShader != VK_NULL_HANDLE) {
                vkDestroyShaderModule(device, fragmentShader, nullptr);
            }
            throw;
        }
        vkDestroyShaderModule(device, vertexShader, nullptr);
        vkDestroyShaderModule(device, fragmentShader, nullptr);
    }

    void createFramebuffers() {
        framebuffers.resize(swapchainViews.size(), VK_NULL_HANDLE);
        for (std::size_t i = 0; i < framebuffers.size(); ++i) {
            const std::array<VkImageView, 2> attachments{
                swapchainViews[i], depthViews[i]
            };
            VkFramebufferCreateInfo createInfo{};
            createInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            createInfo.renderPass = renderPass;
            createInfo.attachmentCount =
                static_cast<std::uint32_t>(attachments.size());
            createInfo.pAttachments = attachments.data();
            createInfo.width = extent.width;
            createInfo.height = extent.height;
            createInfo.layers = 1;
            check(vkCreateFramebuffer(device, &createInfo, nullptr, &framebuffers[i]),
                  "Create Vulkan framebuffer");
        }
        imageFences.assign(framebuffers.size(), VK_NULL_HANDLE);
    }

    void recreateSwapchain() {
        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        while (width == 0 || height == 0) {
            glfwWaitEvents();
            glfwGetFramebufferSize(window, &width, &height);
        }
        check(vkDeviceWaitIdle(device), "Wait for Vulkan device before resize");
        destroySwapchainResources();
        createSwapchain();
        createRenderPass();
        createPipelines();
        createFramebuffers();
        frameStats.renderWidth = extent.width;
        frameStats.renderHeight = extent.height;
    }

    void destroySwapchainResources() noexcept {
        if (device == VK_NULL_HANDLE) {
            return;
        }
        for (const auto framebuffer : framebuffers) {
            if (framebuffer != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(device, framebuffer, nullptr);
            }
        }
        framebuffers.clear();
        if (scenePipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, scenePipeline, nullptr);
            scenePipeline = VK_NULL_HANDLE;
        }
        if (overlayPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, overlayPipeline, nullptr);
            overlayPipeline = VK_NULL_HANDLE;
        }
        if (pipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
            pipelineLayout = VK_NULL_HANDLE;
        }
        if (renderPass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(device, renderPass, nullptr);
            renderPass = VK_NULL_HANDLE;
        }
        for (const auto view : depthViews) {
            if (view != VK_NULL_HANDLE) {
                vkDestroyImageView(device, view, nullptr);
            }
        }
        depthViews.clear();
        for (const auto image : depthImages) {
            if (image != VK_NULL_HANDLE) {
                vkDestroyImage(device, image, nullptr);
            }
        }
        depthImages.clear();
        for (const auto memory : depthMemories) {
            if (memory != VK_NULL_HANDLE) {
                vkFreeMemory(device, memory, nullptr);
            }
        }
        depthMemories.clear();
        for (const auto view : swapchainViews) {
            if (view != VK_NULL_HANDLE) {
                vkDestroyImageView(device, view, nullptr);
            }
        }
        swapchainViews.clear();
        for (const auto semaphore : renderFinishedSemaphores) {
            if (semaphore != VK_NULL_HANDLE) {
                vkDestroySemaphore(device, semaphore, nullptr);
            }
        }
        renderFinishedSemaphores.clear();
        swapchainImages.clear();
        if (swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(device, swapchain, nullptr);
            swapchain = VK_NULL_HANDLE;
        }
        imageFences.clear();
    }

    void ensureVertexBuffer(Frame& frame, VkDeviceSize required) {
        if (required <= frame.vertexCapacity) {
            return;
        }
        if (frame.vertexBuffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device, frame.vertexBuffer, nullptr);
            vkFreeMemory(device, frame.vertexMemory, nullptr);
            frame.vertexBuffer = VK_NULL_HANDLE;
            frame.vertexMemory = VK_NULL_HANDLE;
        }
        VkDeviceSize capacity = 1U << 20U;
        while (capacity < required) {
            capacity *= 2;
        }
        VkBufferCreateInfo buffer{};
        buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer.size = capacity;
        buffer.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateBuffer(device, &buffer, nullptr, &frame.vertexBuffer),
              "Create dynamic vertex buffer");
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, frame.vertexBuffer, &requirements);
        VkMemoryAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memoryType(
            requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(vkAllocateMemory(device, &allocation, nullptr, &frame.vertexMemory),
              "Allocate dynamic vertex buffer memory");
        check(vkBindBufferMemory(device, frame.vertexBuffer, frame.vertexMemory, 0),
              "Bind dynamic vertex buffer memory");
        frame.vertexCapacity = capacity;
    }

    PushConstants cameraConstants(const Camera3D& camera) const {
        PushConstants constants{};
        constants.viewport[0] = static_cast<float>(extent.width);
        constants.viewport[1] = static_cast<float>(extent.height);
        constants.origin[0] = camera.originX;
        constants.origin[1] = camera.originY;
        constants.center[0] = camera.centerX;
        constants.center[1] = camera.centerY;
        const auto target = camera.ground(camera.centerX, camera.centerY);
        constants.target[0] = target.x;
        constants.target[1] = target.z;
        constexpr float degreesToRadians = 0.017453292519943295F;
        const float yaw = camera.yaw * degreesToRadians;
        const float pitch = camera.pitch * degreesToRadians;
        constants.angles[0] = std::cos(yaw);
        constants.angles[1] = std::sin(yaw);
        constants.angles[2] = std::cos(pitch);
        constants.angles[3] = std::sin(pitch);
        constants.scale = camera.scale;
        constants.depthRange = camera.depthRange;
        constants.focalLength = camera.focalLength;
        constants.nearPlane = Camera3D::nearPlane;
        constants.farPlane = camera.farPlane();
        constants.perspective = camera.perspective ? 1 : 0;
        return constants;
    }

    void recordCommands(Frame& frame, std::uint32_t imageIndex,
                        const Camera3D& camera, std::uint32_t sceneVertexCount,
                        std::uint32_t overlayVertexCount) {
        check(vkResetCommandBuffer(frame.command, 0), "Reset Vulkan command buffer");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(frame.command, &begin), "Begin Vulkan command buffer");

        if (frame.gpuTimestampQueries != VK_NULL_HANDLE) {
            vkCmdResetQueryPool(frame.command, frame.gpuTimestampQueries, 0, 2);
            vkCmdWriteTimestamp(frame.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                frame.gpuTimestampQueries, 0);
        }
        std::array<VkClearValue, 2> clearValues{};
        clearValues[0].color.float32[0] = 0.13F;
        clearValues[0].color.float32[1] = 0.18F;
        clearValues[0].color.float32[2] = 0.23F;
        clearValues[0].color.float32[3] = 1.0F;
        clearValues[1].depthStencil.depth = 1.0F;
        clearValues[1].depthStencil.stencil = 0;
        VkRenderPassBeginInfo render{};
        render.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        render.renderPass = renderPass;
        render.framebuffer = framebuffers[imageIndex];
        render.renderArea.extent = extent;
        render.clearValueCount = static_cast<std::uint32_t>(clearValues.size());
        render.pClearValues = clearValues.data();
        vkCmdBeginRenderPass(frame.command, &render, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport viewport{};
        viewport.width = static_cast<float>(extent.width);
        viewport.height = static_cast<float>(extent.height);
        viewport.minDepth = 0.0F;
        viewport.maxDepth = 1.0F;
        VkRect2D scissor{};
        scissor.extent = extent;
        vkCmdSetViewport(frame.command, 0, 1, &viewport);
        vkCmdSetScissor(frame.command, 0, 1, &scissor);
        const VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(frame.command, 0, 1, &frame.vertexBuffer, &offset);
        auto constants = cameraConstants(camera);
        vkCmdBindPipeline(frame.command, VK_PIPELINE_BIND_POINT_GRAPHICS, scenePipeline);
        vkCmdPushConstants(frame.command, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(constants), &constants);
        vkCmdDraw(frame.command, sceneVertexCount, 1, 0, 0);
        if (overlayVertexCount > 0) {
            constants.screenOverlay = 1;
            vkCmdBindPipeline(frame.command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              overlayPipeline);
            vkCmdPushConstants(frame.command, pipelineLayout,
                               VK_SHADER_STAGE_VERTEX_BIT, 0,
                               sizeof(constants), &constants);
            vkCmdDraw(frame.command, overlayVertexCount, 1, sceneVertexCount, 0);
        }
        vkCmdEndRenderPass(frame.command);
        if (frame.gpuTimestampQueries != VK_NULL_HANDLE) {
            vkCmdWriteTimestamp(frame.command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                frame.gpuTimestampQueries, 1);
        }
        check(vkEndCommandBuffer(frame.command), "End Vulkan command buffer");
    }

    void readGpuTimestamps(Frame& frame) {
        if (!frame.gpuTimestampPending ||
            frame.gpuTimestampQueries == VK_NULL_HANDLE) {
            return;
        }
        std::array<std::uint64_t, 2> timestamps{};
        check(vkGetQueryPoolResults(device, frame.gpuTimestampQueries, 0, 2,
                                    sizeof(timestamps), timestamps.data(),
                                    sizeof(std::uint64_t),
                                    VK_QUERY_RESULT_64_BIT),
              "Read completed GPU timestamps");
        const std::uint32_t validBits = graphicsTimestampValidBits;
        const std::uint64_t difference = validBits >= 64
            ? timestamps[1] - timestamps[0]
            : (timestamps[1] - timestamps[0]) &
                  ((std::uint64_t{1} << validBits) - 1);
        frameStats.gpuRenderMilliseconds =
            static_cast<double>(difference) * timestampPeriodNanoseconds / 1.0e6;
        frameStats.gpuTimingFrame = frame.submittedFrame;
        frameStats.gpuRenderTimingValid = true;
        frame.gpuTimestampPending = false;
    }

    void draw(const Simulation& simulation, const Camera3D& camera,
              const ScreenRectangle* selectionRectangle) {
        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        if (width <= 0 || height <= 0) {
            return;
        }
        if (static_cast<std::uint32_t>(width) != extent.width ||
            static_cast<std::uint32_t>(height) != extent.height) {
            recreateSwapchain();
        }

        const auto sceneBuildStart = std::chrono::steady_clock::now();
        auto scene = buildSceneGeometry(simulation);
        auto overlay = selectionRectangle == nullptr
            ? std::vector<SceneVertex>{}
            : buildSelectionOverlayGeometry(*selectionRectangle);
        const auto sceneBuildEnd = std::chrono::steady_clock::now();
        frameStats.sceneBuildMilliseconds =
            std::chrono::duration<double, std::milli>(
                sceneBuildEnd - sceneBuildStart).count();
        frameStats.sceneVertexCount = scene.size();
        frameStats.renderWidth = extent.width;
        frameStats.renderHeight = extent.height;
        const VkDeviceSize sceneBytes = scene.size() * sizeof(SceneVertex);
        const VkDeviceSize overlayBytes = overlay.size() * sizeof(SceneVertex);
        const VkDeviceSize totalBytes = sceneBytes + overlayBytes;
        auto& frame = frames[frameIndex];
        check(vkWaitForFences(device, 1, &frame.fence, VK_TRUE,
                              std::numeric_limits<std::uint64_t>::max()),
              "Wait for Vulkan frame");
        readGpuTimestamps(frame);

        std::uint32_t imageIndex = 0;
        const VkResult acquired = vkAcquireNextImageKHR(
            device, swapchain, std::numeric_limits<std::uint64_t>::max(),
            frame.imageAvailable, VK_NULL_HANDLE, &imageIndex);
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
            recreateSwapchain();
            return;
        }
        if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
            check(acquired, "Acquire Vulkan swapchain image");
        }
        if (imageFences[imageIndex] != VK_NULL_HANDLE) {
            check(vkWaitForFences(device, 1, &imageFences[imageIndex], VK_TRUE,
                                  std::numeric_limits<std::uint64_t>::max()),
                  "Wait for acquired Vulkan image");
        }

        const auto uploadStart = std::chrono::steady_clock::now();
        ensureVertexBuffer(frame, totalBytes);
        void* mapped = nullptr;
        check(vkMapMemory(device, frame.vertexMemory, 0, totalBytes, 0, &mapped),
              "Map dynamic vertex buffer");
        std::memcpy(mapped, scene.data(), static_cast<std::size_t>(sceneBytes));
        if (overlayBytes > 0) {
            std::memcpy(static_cast<char*>(mapped) + sceneBytes, overlay.data(),
                        static_cast<std::size_t>(overlayBytes));
        }
        vkUnmapMemory(device, frame.vertexMemory);
        const auto uploadEnd = std::chrono::steady_clock::now();
        frameStats.vertexUploadMilliseconds =
            std::chrono::duration<double, std::milli>(
                uploadEnd - uploadStart).count();

        check(vkResetFences(device, 1, &frame.fence), "Reset Vulkan frame fence");
        const auto recordStart = std::chrono::steady_clock::now();
        recordCommands(frame, imageIndex, camera,
                       static_cast<std::uint32_t>(scene.size()),
                       static_cast<std::uint32_t>(overlay.size()));
        const auto recordEnd = std::chrono::steady_clock::now();
        frameStats.commandRecordMilliseconds =
            std::chrono::duration<double, std::milli>(
                recordEnd - recordStart).count();
        imageFences[imageIndex] = frame.fence;
        constexpr VkPipelineStageFlags waitStage =
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &frame.imageAvailable;
        submit.pWaitDstStageMask = &waitStage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &frame.command;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &renderFinishedSemaphores[imageIndex];
        check(vkQueueSubmit(graphicsQueue, 1, &submit, frame.fence),
              "Submit Vulkan frame");
        frame.submittedFrame = ++submittedFrameCount;
        frameStats.submittedFrame = frame.submittedFrame;
        frame.gpuTimestampPending =
            frame.gpuTimestampQueries != VK_NULL_HANDLE;

        VkPresentInfoKHR present{};
        present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &renderFinishedSemaphores[imageIndex];
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain;
        present.pImageIndices = &imageIndex;
        const VkResult result = vkQueuePresentKHR(presentQueue, &present);
        frameIndex = (frameIndex + 1) % frames.size();
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR ||
            acquired == VK_SUBOPTIMAL_KHR) {
            recreateSwapchain();
        } else {
            check(result, "Present Vulkan frame");
        }
    }

    void cleanup() noexcept {
        if (device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device);
            destroySwapchainResources();
            for (auto& frame : frames) {
                if (frame.gpuTimestampQueries != VK_NULL_HANDLE) {
                    vkDestroyQueryPool(device, frame.gpuTimestampQueries, nullptr);
                }
                if (frame.vertexBuffer != VK_NULL_HANDLE) {
                    vkDestroyBuffer(device, frame.vertexBuffer, nullptr);
                }
                if (frame.vertexMemory != VK_NULL_HANDLE) {
                    vkFreeMemory(device, frame.vertexMemory, nullptr);
                }
                if (frame.imageAvailable != VK_NULL_HANDLE) {
                    vkDestroySemaphore(device, frame.imageAvailable, nullptr);
                }
                if (frame.fence != VK_NULL_HANDLE) {
                    vkDestroyFence(device, frame.fence, nullptr);
                }
            }
            if (commandPool != VK_NULL_HANDLE) {
                vkDestroyCommandPool(device, commandPool, nullptr);
            }
            vkDestroyDevice(device, nullptr);
            device = VK_NULL_HANDLE;
        }
        if (surface != VK_NULL_HANDLE && instance != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance, surface, nullptr);
            surface = VK_NULL_HANDLE;
        }
        if (instance != VK_NULL_HANDLE) {
            vkDestroyInstance(instance, nullptr);
            instance = VK_NULL_HANDLE;
        }
    }
};

Renderer3D::Renderer3D(GLFWwindow* window) : impl_(std::make_unique<Impl>(window)) {}
Renderer3D::~Renderer3D() = default;

void Renderer3D::draw(const Simulation& simulation, const Camera3D& camera,
                      const ScreenRectangle* selectionRectangle) {
    impl_->draw(simulation, camera, selectionRectangle);
}

RendererStats Renderer3D::stats() const noexcept {
    return impl_->frameStats;
}

} // namespace rts
