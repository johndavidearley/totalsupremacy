#include "rts/Simulation.hpp"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef TOTALSUPREMACY_SHADER_DIR
#define TOTALSUPREMACY_SHADER_DIR "shaders"
#endif

namespace {

constexpr std::size_t kBatchSize = 32;
constexpr std::size_t kNodes = static_cast<std::size_t>(
    rts::Simulation::kTerrainColumns * rts::Simulation::kTerrainRows);
constexpr std::size_t kGridStride = 10;
static_assert(rts::Simulation::kTerrainColumns == 100 &&
              rts::Simulation::kTerrainRows == 70);
using Clock = std::chrono::steady_clock;

void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " failed: " +
                                 std::to_string(result));
    }
}

bool hasExtension(const std::vector<VkExtensionProperties>& extensions,
                  const char* name) {
    return std::any_of(extensions.begin(), extensions.end(),
        [name](const auto& extension) {
            return std::strcmp(extension.extensionName, name) == 0;
        });
}

class GpuPathBackend final : public rts::GridPathBackend {
public:
    GpuPathBackend() {
        try {
            initialize();
        } catch (...) {
            cleanup();
            throw;
        }
    }

    ~GpuPathBackend() override { cleanup(); }
    GpuPathBackend(const GpuPathBackend&) = delete;
    GpuPathBackend& operator=(const GpuPathBackend&) = delete;

    const std::string& deviceName() const { return deviceName_; }
    const std::vector<rts::GridPathResult>& lastResults() const { return lastResults_; }

    std::vector<rts::GridPathResult> search(
        const std::vector<std::uint8_t>& blocked,
        const std::vector<float>& elevations,
        const std::vector<std::array<int, 8>>& transitionCosts,
        const std::vector<rts::GridPathQuery>& queries) override {
        if (blocked.size() != kNodes || elevations.size() != kNodes ||
            transitionCosts.size() != kNodes) {
            throw std::runtime_error("Unexpected GPU pathfinding grid dimensions.");
        }
        auto* grid = static_cast<std::int32_t*>(buffers_[0].mapped);
        for (std::size_t i = 0; i < kNodes; ++i) {
            grid[i * kGridStride] = blocked[i];
            grid[i * kGridStride + 1] = std::bit_cast<std::int32_t>(elevations[i]);
            for (std::size_t direction = 0; direction < 8; ++direction) {
                grid[i * kGridStride + 2 + direction] = transitionCosts[i][direction];
            }
        }

        std::vector<rts::GridPathResult> output(queries.size());
        for (std::size_t offset = 0; offset < queries.size(); offset += kBatchSize) {
            const auto count = std::min(kBatchSize, queries.size() - offset);
            auto* input = static_cast<std::int32_t*>(buffers_[1].mapped);
            for (std::size_t i = 0; i < count; ++i) {
                input[i * 2] = queries[offset + i].startNode;
                input[i * 2 + 1] = queries[offset + i].goalNode;
            }
            dispatch(static_cast<std::uint32_t>(count));
            const auto* work = static_cast<const std::int32_t*>(buffers_[2].mapped);
            const auto* costs = static_cast<const std::int32_t*>(buffers_[3].mapped);
            for (std::size_t i = 0; i < count; ++i) {
                if (costs[i] < 0) {
                    continue;
                }
                auto& result = output[offset + i];
                result.found = true;
                result.cost = static_cast<std::uint64_t>(costs[i]);
                const int start = queries[offset + i].startNode;
                int current = queries[offset + i].goalNode;
                const std::size_t parentOffset = i * 4 * kNodes + kNodes;
                while (current != start) {
                    if (current < 0 || current >= static_cast<int>(kNodes) ||
                        result.nodes.size() >= kNodes) {
                        throw std::runtime_error("GPU returned an invalid parent chain.");
                    }
                    result.nodes.push_back(current);
                    current = work[parentOffset + static_cast<std::size_t>(current)];
                }
                std::reverse(result.nodes.begin(), result.nodes.end());
            }
        }
        lastResults_ = output;
        return output;
    }

private:
    struct Buffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
        VkDeviceSize size = 0;
    };

    void initialize() {
        std::uint32_t extensionCount = 0;
        check(vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr),
              "Enumerate instance extensions");
        std::vector<VkExtensionProperties> extensions(extensionCount);
        check(vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount,
                                                     extensions.data()),
              "Read instance extensions");
        std::vector<const char*> enabled;
        VkInstanceCreateInfo instanceInfo{};
        instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        if (hasExtension(extensions, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
            enabled.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            instanceInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
        instanceInfo.enabledExtensionCount = static_cast<std::uint32_t>(enabled.size());
        instanceInfo.ppEnabledExtensionNames = enabled.data();
        check(vkCreateInstance(&instanceInfo, nullptr, &instance_), "Create instance");

        std::uint32_t deviceCount = 0;
        check(vkEnumeratePhysicalDevices(instance_, &deviceCount, nullptr),
              "Enumerate devices");
        std::vector<VkPhysicalDevice> devices(deviceCount);
        check(vkEnumeratePhysicalDevices(instance_, &deviceCount, devices.data()),
              "Read devices");
        for (const auto candidate : devices) {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(candidate, &properties);
            std::uint32_t familyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
            std::vector<VkQueueFamilyProperties> families(familyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount,
                                                      families.data());
            for (std::uint32_t i = 0; i < familyCount; ++i) {
                if ((families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0 &&
                    families[i].queueCount > 0 &&
                    properties.limits.maxStorageBufferRange >=
                        kBatchSize * 4 * kNodes * sizeof(std::int32_t) &&
                    properties.limits.maxComputeWorkGroupInvocations >= 32) {
                    physicalDevice_ = candidate;
                    queueFamily_ = i;
                    deviceName_ = properties.deviceName;
                    break;
                }
            }
            if (physicalDevice_ != VK_NULL_HANDLE) break;
        }
        if (physicalDevice_ == VK_NULL_HANDLE) {
            throw std::runtime_error("No Vulkan device supports the compute workload.");
        }
        std::uint32_t deviceExtensionCount = 0;
        check(vkEnumerateDeviceExtensionProperties(
                  physicalDevice_, nullptr, &deviceExtensionCount, nullptr),
              "Enumerate device extensions");
        std::vector<VkExtensionProperties> deviceExtensions(deviceExtensionCount);
        check(vkEnumerateDeviceExtensionProperties(
                  physicalDevice_, nullptr, &deviceExtensionCount,
                  deviceExtensions.data()), "Read device extensions");
        std::vector<const char*> deviceEnabled;
        if (hasExtension(deviceExtensions, "VK_KHR_portability_subset")) {
            deviceEnabled.push_back("VK_KHR_portability_subset");
        }
        const float priority = 1.0F;
        VkDeviceQueueCreateInfo queueInfo{};
        queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueFamilyIndex = queueFamily_;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        VkDeviceCreateInfo deviceInfo{};
        deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.enabledExtensionCount =
            static_cast<std::uint32_t>(deviceEnabled.size());
        deviceInfo.ppEnabledExtensionNames = deviceEnabled.data();
        check(vkCreateDevice(physicalDevice_, &deviceInfo, nullptr, &device_),
              "Create compute device");
        vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);

        const std::array<VkDeviceSize, 4> sizes{
            kNodes * kGridStride * sizeof(std::int32_t),
            kBatchSize * 2 * sizeof(std::int32_t),
            kBatchSize * 4 * kNodes * sizeof(std::int32_t),
            kBatchSize * sizeof(std::int32_t)
        };
        for (std::size_t i = 0; i < buffers_.size(); ++i) {
            createBuffer(buffers_[i], sizes[i]);
        }
        createPipeline();
        createCommands();
    }

    void createBuffer(Buffer& buffer, VkDeviceSize size) {
        buffer.size = size;
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = size;
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateBuffer(device_, &info, nullptr, &buffer.buffer), "Create buffer");
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device_, buffer.buffer, &requirements);
        VkPhysicalDeviceMemoryProperties memoryProperties{};
        vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memoryProperties);
        std::uint32_t type = memoryProperties.memoryTypeCount;
        constexpr VkMemoryPropertyFlags flags =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (std::uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
            if ((requirements.memoryTypeBits & (1U << i)) != 0 &&
                (memoryProperties.memoryTypes[i].propertyFlags & flags) == flags) {
                type = i;
                break;
            }
        }
        if (type == memoryProperties.memoryTypeCount) {
            throw std::runtime_error("No coherent host-visible Vulkan memory type.");
        }
        VkMemoryAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = type;
        check(vkAllocateMemory(device_, &allocation, nullptr, &buffer.memory),
              "Allocate buffer memory");
        check(vkBindBufferMemory(device_, buffer.buffer, buffer.memory, 0),
              "Bind buffer memory");
        check(vkMapMemory(device_, buffer.memory, 0, size, 0, &buffer.mapped),
              "Map buffer memory");
    }

    void createPipeline() {
        std::array<VkDescriptorSetLayoutBinding, 4> bindings{};
        for (std::uint32_t i = 0; i < bindings.size(); ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo descriptorInfo{};
        descriptorInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        descriptorInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
        descriptorInfo.pBindings = bindings.data();
        check(vkCreateDescriptorSetLayout(device_, &descriptorInfo, nullptr,
                                          &descriptorLayout_),
              "Create descriptor layout");
        VkPushConstantRange range{};
        range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        range.size = sizeof(std::uint32_t);
        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &descriptorLayout_;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &range;
        check(vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &pipelineLayout_),
              "Create pipeline layout");

        const auto shaderPath =
            std::filesystem::path(TOTALSUPREMACY_SHADER_DIR) / "pathfinding.comp.spv";
        std::ifstream shader(shaderPath, std::ios::binary | std::ios::ate);
        if (!shader) throw std::runtime_error("Unable to open pathfinding shader.");
        const auto length = shader.tellg();
        if (length <= 0 || length % 4 != 0) {
            throw std::runtime_error("Invalid pathfinding SPIR-V size.");
        }
        std::vector<std::uint32_t> code(static_cast<std::size_t>(length) / 4);
        shader.seekg(0);
        shader.read(reinterpret_cast<char*>(code.data()),
                    static_cast<std::streamsize>(length));
        if (!shader) throw std::runtime_error("Unable to read pathfinding shader.");
        VkShaderModuleCreateInfo moduleInfo{};
        moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        moduleInfo.codeSize = code.size() * sizeof(std::uint32_t);
        moduleInfo.pCode = code.data();
        check(vkCreateShaderModule(device_, &moduleInfo, nullptr, &shader_),
              "Create compute shader");
        VkPipelineShaderStageCreateInfo stage{};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = shader_;
        stage.pName = "main";
        VkComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineInfo.stage = stage;
        pipelineInfo.layout = pipelineLayout_;
        check(vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo,
                                        nullptr, &pipeline_), "Create compute pipeline");

        VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4};
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        check(vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_),
              "Create descriptor pool");
        VkDescriptorSetAllocateInfo descriptorAllocation{};
        descriptorAllocation.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        descriptorAllocation.descriptorPool = descriptorPool_;
        descriptorAllocation.descriptorSetCount = 1;
        descriptorAllocation.pSetLayouts = &descriptorLayout_;
        check(vkAllocateDescriptorSets(device_, &descriptorAllocation, &descriptorSet_),
              "Allocate descriptor set");
        std::array<VkDescriptorBufferInfo, 4> bufferInfos{};
        std::array<VkWriteDescriptorSet, 4> writes{};
        for (std::uint32_t i = 0; i < writes.size(); ++i) {
            bufferInfos[i] = {buffers_[i].buffer, 0, buffers_[i].size};
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = descriptorSet_;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &bufferInfos[i];
        }
        vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    }

    void createCommands() {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = queueFamily_;
        check(vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_),
              "Create command pool");
        VkCommandBufferAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocation.commandPool = commandPool_;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(device_, &allocation, &command_),
              "Allocate command buffer");
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        check(vkCreateFence(device_, &fenceInfo, nullptr, &fence_), "Create fence");
    }

    void dispatch(std::uint32_t count) {
        check(vkResetCommandBuffer(command_, 0), "Reset command buffer");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(command_, &begin), "Begin command buffer");
        vkCmdBindPipeline(command_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
        vkCmdBindDescriptorSets(command_, VK_PIPELINE_BIND_POINT_COMPUTE,
                                pipelineLayout_, 0, 1, &descriptorSet_, 0, nullptr);
        vkCmdPushConstants(command_, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(count), &count);
        vkCmdDispatch(command_, 1, 1, 1);
        VkMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier,
                             0, nullptr, 0, nullptr);
        check(vkEndCommandBuffer(command_), "End command buffer");
        check(vkResetFences(device_, 1, &fence_), "Reset fence");
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command_;
        check(vkQueueSubmit(queue_, 1, &submit, fence_), "Submit compute dispatch");
        check(vkWaitForFences(device_, 1, &fence_, VK_TRUE, 30000000000ULL),
              "Wait for compute dispatch");
    }

    void cleanup() noexcept {
        if (device_ != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device_);
            if (fence_ != VK_NULL_HANDLE) vkDestroyFence(device_, fence_, nullptr);
            if (commandPool_ != VK_NULL_HANDLE)
                vkDestroyCommandPool(device_, commandPool_, nullptr);
            if (descriptorPool_ != VK_NULL_HANDLE)
                vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
            if (pipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device_, pipeline_, nullptr);
            if (shader_ != VK_NULL_HANDLE) vkDestroyShaderModule(device_, shader_, nullptr);
            if (pipelineLayout_ != VK_NULL_HANDLE)
                vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
            if (descriptorLayout_ != VK_NULL_HANDLE)
                vkDestroyDescriptorSetLayout(device_, descriptorLayout_, nullptr);
            for (auto& buffer : buffers_) {
                if (buffer.mapped != nullptr) vkUnmapMemory(device_, buffer.memory);
                if (buffer.buffer != VK_NULL_HANDLE)
                    vkDestroyBuffer(device_, buffer.buffer, nullptr);
                if (buffer.memory != VK_NULL_HANDLE)
                    vkFreeMemory(device_, buffer.memory, nullptr);
            }
            vkDestroyDevice(device_, nullptr);
        }
        if (instance_ != VK_NULL_HANDLE) vkDestroyInstance(instance_, nullptr);
    }

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::uint32_t queueFamily_ = 0;
    std::string deviceName_;
    std::array<Buffer, 4> buffers_{};
    VkDescriptorSetLayout descriptorLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkShaderModule shader_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer command_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    std::vector<rts::GridPathResult> lastResults_;
};

double milliseconds(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

void runCase(GpuPathBackend& backend, std::size_t population,
             rts::Vec2 destination, const char* scenario, int trials) {
    std::vector<std::uint32_t> ids(population);
    std::iota(ids.begin(), ids.end(), 1U);
    std::vector<double> cpuTimes;
    std::vector<double> gpuTimes;
    for (int trial = 0; trial < trials; ++trial) {
        rts::Simulation cpu(population);
        rts::Simulation gpu(population);
        if (cpu.selectUnits(ids) != population || gpu.selectUnits(ids) != population) {
            throw std::runtime_error("Unable to select benchmark units.");
        }
        const auto cpuStart = Clock::now();
        cpu.moveSelected(destination);
        const auto cpuEnd = Clock::now();
        const auto gpuStart = Clock::now();
        gpu.moveSelected(destination, backend);
        const auto gpuEnd = Clock::now();
        if (cpu.lastMoveCommand().unitsRouted != population ||
            gpu.lastMoveCommand().unitsRouted != population ||
            cpu.lastPathfindingMetrics().pathCost != gpu.lastPathfindingMetrics().pathCost) {
            throw std::runtime_error("GPU and CPU route totals differ.");
        }
        const auto columns = static_cast<std::size_t>(
            std::ceil(std::sqrt(static_cast<double>(population))));
        const float center = static_cast<float>(columns - 1) * 0.5F;
        rts::Simulation reference(population);
        for (std::size_t i = 0; i < population; ++i) {
            const auto& cpuUnit = cpu.units()[i];
            const auto& gpuUnit = gpu.units()[i];
            if (cpuUnit.destination.x != gpuUnit.destination.x ||
                cpuUnit.destination.y != gpuUnit.destination.y) {
                throw std::runtime_error("GPU and CPU routed destinations differ.");
            }
            const rts::Vec2 target{
                std::clamp(destination.x +
                    (static_cast<float>(i % columns) - center) * 14.0F,
                    0.0F, rts::Simulation::kMapWidth),
                std::clamp(destination.y +
                    (static_cast<float>(i / columns) - center) * 14.0F,
                    0.0F, rts::Simulation::kMapHeight)
            };
            if (!reference.selectUnit(ids[i])) {
                throw std::runtime_error("Unable to select reference unit.");
            }
            reference.moveSelected(target);
            if (reference.lastMoveCommand().unitsRouted != 1 ||
                reference.lastPathfindingMetrics().pathCost != backend.lastResults()[i].cost) {
                throw std::runtime_error("GPU and CPU per-unit route costs differ.");
            }
        }
        cpuTimes.push_back(milliseconds(cpuStart, cpuEnd));
        gpuTimes.push_back(milliseconds(gpuStart, gpuEnd));
    }
    std::cout << population << ',' << scenario << ',' << std::fixed
              << std::setprecision(3) << median(cpuTimes) << ','
              << median(gpuTimes) << '\n' << std::flush;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 3 || std::string(argv[1]) != "--gpu") {
            throw std::runtime_error("Usage: totalsupremacy_gpu_benchmark --gpu UNIT_COUNT");
        }
        std::size_t parsed = 0;
        const auto count = std::stoul(argv[2], &parsed);
        if (parsed != std::string(argv[2]).size() || count == 0 || count > 1800) {
            throw std::runtime_error("Unit count must be between 1 and 1800.");
        }
#if defined(__APPLE__)
        if (std::getenv("VK_DRIVER_FILES") == nullptr &&
            std::getenv("VK_ICD_FILENAMES") == nullptr &&
            std::filesystem::exists("/opt/local/share/vulkan/icd.d/MoltenVK_icd.json")) {
            setenv("VK_DRIVER_FILES", "/opt/local/share/vulkan/icd.d/MoltenVK_icd.json", 0);
        }
#endif
        GpuPathBackend backend;
        std::cout << "device," << backend.deviceName() << '\n'
                  << "batch_size," << kBatchSize << '\n'
                  << "trials,3\n"
                  << "units,scenario,cpu_median_ms,gpu_median_ms\n" << std::flush;
        runCase(backend, count, {520.0F, 350.0F}, "nearby", 3);
        runCase(backend, count, {1000.0F, 700.0F}, "center", 3);
        runCase(backend, count, {1800.0F, 1300.0F}, "far-corner", 3);
        runCase(backend, 1, {390.0F, 250.0F}, "same-cell", 1);
        const auto edgeCount = std::min<std::size_t>(count, 4);
        runCase(backend, edgeCount, {0.0F, 0.0F}, "map-edge", 1);
        const rts::Simulation terrain(0);
        const auto& obstacle = terrain.terrainObstacles().front();
        runCase(backend, edgeCount,
                {(obstacle.minimum.x + obstacle.maximum.x) * 0.5F,
                 (obstacle.minimum.y + obstacle.maximum.y) * 0.5F},
                "obstacle", 1);
    } catch (const std::exception& error) {
        std::cerr << "GPU benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
