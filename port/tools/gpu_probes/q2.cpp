// Two-queue concurrency probe for MoltenVK: heavy work on queue 0, a chain of tiny dependent
// dispatches on queue 1 (the shape of ISS's async compute particles); A alone, B alone, both.
#include <vulkan/vulkan.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>

#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { std::printf("%s failed: %d\n", #x, r_); std::exit(1); } } while (0)

static std::vector<char> Read(const char* p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(f)), {});
}

int main(int argc, char** argv) {
    const unsigned heavy_iters = argc > 1 ? std::atoi(argv[1]) : 2000;
    const unsigned tiny_count = argc > 2 ? std::atoi(argv[2]) : 500;
    const unsigned tiny_iters = argc > 3 ? std::atoi(argv[3]) : 20;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    const char* exts[] = {VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME};
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 1;
    ici.ppEnabledExtensionNames = exts;
    VkInstance inst;
    CK(vkCreateInstance(&ici, nullptr, &inst));
    uint32_t n = 1;
    VkPhysicalDevice pd;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qf.data());
    for (uint32_t i = 0; i < qn; ++i)
        std::printf("family %u: flags %#x count %u\n", i, qf[i].queueFlags, qf[i].queueCount);
    const float prio[2] = {1.0f, 1.0f};
    VkDeviceQueueCreateInfo qcis[2] = {{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO},
                                       {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}};
    for (int i = 0; i < 2; ++i) {
        qcis[i].queueFamilyIndex = i;
        qcis[i].queueCount = 1;
        qcis[i].pQueuePriorities = prio;
    }
    const char* dexts[] = {"VK_KHR_portability_subset"};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 2;
    dci.pQueueCreateInfos = qcis;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dexts;
    VkDevice dev;
    CK(vkCreateDevice(pd, &dci, nullptr, &dev));
    VkQueue q[2];
    vkGetDeviceQueue(dev, 0, 0, &q[0]);
    vkGetDeviceQueue(dev, 1, 0, &q[1]);

    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    VkBuffer buf[2];
    VkDeviceMemory mem[2];
    for (int b = 0; b < 2; ++b) {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = 65536 * 4;
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        CK(vkCreateBuffer(dev, &bci, nullptr, &buf[b]));
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(dev, buf[b], &mr);
        uint32_t t = 0;
        while (!(mr.memoryTypeBits & (1u << t)) ||
               !(mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) ++t;
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = t;
        CK(vkAllocateMemory(dev, &mai, nullptr, &mem[b]));
        CK(vkBindBufferMemory(dev, buf[b], mem[b], 0));
    }
    VkDescriptorSetLayoutBinding lb{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo dlci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dlci.bindingCount = 1;
    dlci.pBindings = &lb;
    VkDescriptorSetLayout dsl;
    CK(vkCreateDescriptorSetLayout(dev, &dlci, nullptr, &dsl));
    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &dsl;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    VkPipelineLayout pl;
    CK(vkCreatePipelineLayout(dev, &plci, nullptr, &pl));
    VkPipeline pipe[2];
    const char* names[2] = {"heavy.spv", "tiny.spv"};
    for (int i = 0; i < 2; ++i) {
        auto code = Read(names[i]);
        VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smci.codeSize = code.size();
        smci.pCode = reinterpret_cast<const uint32_t*>(code.data());
        VkShaderModule sm;
        CK(vkCreateShaderModule(dev, &smci, nullptr, &sm));
        VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                      VK_SHADER_STAGE_COMPUTE_BIT, sm, "main"};
        cpci.layout = pl;
        CK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipe[i]));
    }
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 2;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &ps;
    VkDescriptorPool dp;
    CK(vkCreateDescriptorPool(dev, &dpci, nullptr, &dp));
    VkDescriptorSet ds[2];
    VkDescriptorSetLayout dsls[2] = {dsl, dsl};
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = dp;
    dsai.descriptorSetCount = 2;
    dsai.pSetLayouts = dsls;
    CK(vkAllocateDescriptorSets(dev, &dsai, ds));
    for (int i = 0; i < 2; ++i) {
        VkDescriptorBufferInfo bi{buf[i], 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = ds[i];
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = &bi;
        vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
    }
    VkCommandBuffer cb[3];
    for (int i = 0; i < 2; ++i) {
        VkCommandPool cp;
        VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cpi.queueFamilyIndex = i;
        CK(vkCreateCommandPool(dev, &cpi, nullptr, &cp));
        VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbai.commandPool = cp;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        CK(vkAllocateCommandBuffers(dev, &cbai, &cb[i]));
    }
    VkCommandBufferBeginInfo bbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    // A: heavy
    vkBeginCommandBuffer(cb[0], &bbi);
    vkCmdBindPipeline(cb[0], VK_PIPELINE_BIND_POINT_COMPUTE, pipe[0]);
    vkCmdBindDescriptorSets(cb[0], VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds[0], 0, nullptr);
    vkCmdPushConstants(cb[0], pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &heavy_iters);
    const unsigned a_parts = std::getenv("A_PARTS") ? std::atoi(std::getenv("A_PARTS")) : 1;
    for (unsigned i = 0; i < a_parts; ++i) {
        vkCmdDispatch(cb[0], 4096, 1, 1);
        vkCmdPipelineBarrier(cb[0], VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 0, nullptr);
    }
    vkEndCommandBuffer(cb[0]);
    // B: chain of tiny dependent dispatches
    vkBeginCommandBuffer(cb[1], &bbi);
    vkCmdBindPipeline(cb[1], VK_PIPELINE_BIND_POINT_COMPUTE, pipe[1]);
    vkCmdBindDescriptorSets(cb[1], VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds[1], 0, nullptr);
    vkCmdPushConstants(cb[1], pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &tiny_iters);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT,
                       VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
    for (unsigned i = 0; i < tiny_count; ++i) {
        vkCmdDispatch(cb[1], 1, 1, 1);
        if (!std::getenv("NO_BARRIER"))
        vkCmdPipelineBarrier(cb[1], VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    }
    vkEndCommandBuffer(cb[1]);
    VkFence f[2];
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    CK(vkCreateFence(dev, &fci, nullptr, &f[0]));
    CK(vkCreateFence(dev, &fci, nullptr, &f[1]));
    auto submit = [&](int qi, int ci) {
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb[ci];
        CK(vkQueueSubmit(q[qi], 1, &si, f[ci]));
    };
    auto wait = [&](int ci) {
        CK(vkWaitForFences(dev, 1, &f[ci], VK_TRUE, UINT64_MAX));
        vkResetFences(dev, 1, &f[ci]);
    };
    using clk = std::chrono::steady_clock;
    auto ms = [](clk::time_point a, clk::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    for (int rep = 0; rep < 6; ++rep) {
        auto t0 = clk::now(); submit(0, 0); wait(0);
        auto t1 = clk::now(); submit(1, 1); wait(1);
        auto t2 = clk::now(); submit(0, 0); submit(1, 1); wait(0); wait(1);
        auto t3 = clk::now(); submit(0, 0); submit(0, 1); wait(0); wait(1);
        auto t4 = clk::now();
        auto s0 = clk::now(); submit(1, 1); auto s1 = clk::now(); wait(1); auto s2 = clk::now();
        if (rep > 0) std::printf("B submit call %.2f ms, then wait %.2f ms\n", ms(s0, s1), ms(s1, s2));
        if (rep > 0)
            std::printf("A %.2f ms  B %.2f ms  A||B two queues %.2f ms  A;B one queue %.2f ms\n",
                        ms(t0, t1), ms(t1, t2), ms(t2, t3), ms(t3, t4));
    }
    return 0;
}
