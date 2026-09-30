#pragma once

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <Windows.h>

#include <unordered_map>
#include <shared_mutex>
#include <atomic>
#include <cstring>
#include <vector>
#include <string>
#include <memory>
#include <mutex>
#include <iostream>

#ifndef VK_LAYER_EXPORT
#if defined(_WIN32)
#define VK_LAYER_EXPORT __declspec(dllexport)
#else
#define VK_LAYER_EXPORT __attribute__((visibility("default")))
#endif
#endif

#ifndef VK_RENDERING_SUSPENDING_BIT
#define VK_RENDERING_SUSPENDING_BIT 0x00000002
#endif

#ifndef VK_RENDERING_RESUMING_BIT
#define VK_RENDERING_RESUMING_BIT 0x00000004
#endif

namespace vkw {

    enum class OperationalMode {
        StandaloneLayer,
        InAppOverlay
    };

    struct LayerConfig {
        OperationalMode mode = OperationalMode::StandaloneLayer;
        bool enableCrashGuard = true;
        bool verboseLogging = false;
    };

    struct CrashState {
        std::atomic<uint32_t> currentRuleId{ 0 };
        std::atomic<const char*> targetApi{ nullptr };
        std::atomic<bool> isFallbackMode{ false };
    };

    inline CrashState g_crashState;

    inline void SetCrashContext(uint32_t ruleId, const char* api) {
        g_crashState.currentRuleId.store(ruleId, std::memory_order_release);
        g_crashState.targetApi.store(api, std::memory_order_release);
    }

    enum class QuirkFlags : uint32_t {
        None = 0,
        SanitizeRenderingInfo = 1 << 0,
        NullifyUnsupportedExt = 1 << 1
    };

    struct DriverInfo {
        uint32_t vendorID = 0;
        uint32_t deviceID = 0;
        uint32_t driverVersion = 0;
        std::string deviceName;
        uint32_t activeQuirks = 0;
    };

    inline uint32_t EvaluateQuirks(const DriverInfo& info) {
        uint32_t quirks = 0;
        if (info.vendorID == 0x5143) { // Qualcomm Adreno
            quirks |= static_cast<uint32_t>(QuirkFlags::SanitizeRenderingInfo);
        }
        if (info.vendorID == 0x13B5) { // ARM Mali
            quirks |= static_cast<uint32_t>(QuirkFlags::SanitizeRenderingInfo);
        }
        return quirks;
    }

    struct InstanceDispatchTable {
        PFN_vkGetInstanceProcAddr gIPA = nullptr;
        PFN_vkDestroyInstance destroyInstance = nullptr;
    };

    struct DeviceDispatchTable {
        PFN_vkGetDeviceProcAddr gDPA = nullptr;
        PFN_vkDestroyDevice destroyDevice = nullptr;
        PFN_vkAllocateCommandBuffers allocateCommandBuffers = nullptr;
        PFN_vkFreeCommandBuffers freeCommandBuffers = nullptr;
        PFN_vkCmdBeginRenderingKHR cmdBeginRenderingKHR = nullptr;
    };

    class LayerContext {
    public:
        static LayerContext& instance() {
            static LayerContext ctx;
            return ctx;
        }

        void setMode(OperationalMode mode) { m_config.mode = mode; }
        OperationalMode getMode() const { return m_config.mode; }

        void registerInstance(VkInstance instance, const InstanceDispatchTable& table) {
            std::unique_lock<std::shared_mutex> lock(m_instanceMutex);
            m_instanceTables[instance] = table;
        }

        void unregisterInstance(VkInstance instance) {
            std::unique_lock<std::shared_mutex> lock(m_instanceMutex);
            m_instanceTables.erase(instance);
        }

        const InstanceDispatchTable* getInstanceDispatchTable(VkInstance instance) const {
            std::shared_lock<std::shared_mutex> lock(m_instanceMutex);
            auto it = m_instanceTables.find(instance);
            return (it != m_instanceTables.end()) ? &it->second : nullptr;
        }

        void registerDevice(VkDevice device, const DeviceDispatchTable& table, const DriverInfo& info) {
            std::unique_lock<std::shared_mutex> lock(m_deviceMutex);
            m_deviceTables[device] = table;
            m_deviceInfos[device] = info;
        }

        void unregisterDevice(VkDevice device) {
            std::unique_lock<std::shared_mutex> lock(m_deviceMutex);
            m_deviceTables.erase(device);
            m_deviceInfos.erase(device);
        }

        const DeviceDispatchTable* getDispatchTable(VkDevice device) const {
            std::shared_lock<std::shared_mutex> lock(m_deviceMutex);
            auto it = m_deviceTables.find(device);
            return (it != m_deviceTables.end()) ? &it->second : nullptr;
        }

        const DriverInfo* getDriverInfo(VkDevice device) const {
            std::shared_lock<std::shared_mutex> lock(m_deviceMutex);
            auto it = m_deviceInfos.find(device);
            return (it != m_deviceInfos.end()) ? &it->second : nullptr;
        }

        void registerCommandBuffers(VkDevice device, uint32_t count, const VkCommandBuffer* pCmdBufs) {
            std::unique_lock<std::shared_mutex> lock(m_cmdMutex);
            for (uint32_t i = 0; i < count; ++i) {
                if (pCmdBufs[i]) m_cmdMap[pCmdBufs[i]] = device;
            }
        }

        void unregisterCommandBuffers(uint32_t count, const VkCommandBuffer* pCmdBufs) {
            std::unique_lock<std::shared_mutex> lock(m_cmdMutex);
            for (uint32_t i = 0; i < count; ++i) {
                if (pCmdBufs[i]) m_cmdMap.erase(pCmdBufs[i]);
            }
        }

        VkDevice getDeviceFromCommandBuffer(VkCommandBuffer cmdBuf) const {
            std::shared_lock<std::shared_mutex> lock(m_cmdMutex);
            auto it = m_cmdMap.find(cmdBuf);
            return (it != m_cmdMap.end()) ? it->second : VK_NULL_HANDLE;
        }

    private:
        LayerContext() = default;
        LayerConfig m_config;

        std::unordered_map<VkInstance, InstanceDispatchTable> m_instanceTables;
        mutable std::shared_mutex m_instanceMutex;

        std::unordered_map<VkDevice, DeviceDispatchTable> m_deviceTables;
        std::unordered_map<VkDevice, DriverInfo> m_deviceInfos;
        mutable std::shared_mutex m_deviceMutex;

        std::unordered_map<VkCommandBuffer, VkDevice> m_cmdMap;
        mutable std::shared_mutex m_cmdMutex;
    };

    // Declarations
    VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkw_GetDeviceProcAddr(VkDevice device, const char* pName);
    VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkw_GetInstanceProcAddr(VkInstance instance, const char* pName);

    inline PFN_vkVoidFunction HookDeviceProcAddr(VkDevice device, const char* pName, PFN_vkVoidFunction realProc) {
        PFN_vkVoidFunction layerProc = vkw_GetDeviceProcAddr(device, pName);
        return layerProc ? layerProc : realProc;
    }

} // namespace vkw

#if defined(VKWRAITH_IMPLEMENTATION) || defined(VKWRAITH_BUILD_DLL)

namespace vkw {

    inline VKAPI_ATTR void VKAPI_CALL vkw_CmdBeginRenderingKHR(
        VkCommandBuffer commandBuffer, const VkRenderingInfoKHR* pRenderingInfo)
    {
        if (!pRenderingInfo) return;

        OutputDebugStringA("[VkWraith] Intercepted vkCmdBeginRenderingKHR!\n");
        std::cout << "[VkWraith] Intercepted vkCmdBeginRendering / KHR! CommandBuffer: "
            << commandBuffer << std::endl;

        VkDevice device = LayerContext::instance().getDeviceFromCommandBuffer(commandBuffer);
        const auto* table = LayerContext::instance().getDispatchTable(device);
        const auto* info = LayerContext::instance().getDriverInfo(device);

        if (!table || !table->cmdBeginRenderingKHR || !info) return;

        if (g_crashState.isFallbackMode.load(std::memory_order_acquire)) {
            table->cmdBeginRenderingKHR(commandBuffer, pRenderingInfo);
            return;
        }

        VkRenderingInfoKHR sanitizedInfo = *pRenderingInfo;

        if (info->activeQuirks & static_cast<uint32_t>(QuirkFlags::SanitizeRenderingInfo)) {
            SetCrashContext(1, "vkCmdBeginRenderingKHR");
            std::cout << "[VkWraith] Sanitizing RenderingInfo for Device: " << info->deviceName << std::endl;
            sanitizedInfo.flags &= ~(VK_RENDERING_SUSPENDING_BIT | VK_RENDERING_RESUMING_BIT);
        }

        table->cmdBeginRenderingKHR(commandBuffer, &sanitizedInfo);
    }

    inline VKAPI_ATTR VkResult VKAPI_CALL vkw_CreateInstance(
        const VkInstanceCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkInstance* pInstance)
    {
        if (!pCreateInfo || !pInstance) return VK_ERROR_INITIALIZATION_FAILED;

        OutputDebugStringA("[VkWraith] Intercepted vkCreateInstance!\n");
        std::cout << "[VkWraith] Intercepted vkCreateInstance!" << std::endl;

        const VkLayerInstanceCreateInfo* layerCreateInfo =
            reinterpret_cast<const VkLayerInstanceCreateInfo*>(pCreateInfo->pNext);

        while (layerCreateInfo &&
            (layerCreateInfo->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO ||
                layerCreateInfo->function != VK_LAYER_LINK_INFO))
        {
            layerCreateInfo = reinterpret_cast<const VkLayerInstanceCreateInfo*>(layerCreateInfo->pNext);
        }

        if (!layerCreateInfo || !layerCreateInfo->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;

        PFN_vkGetInstanceProcAddr nextGIPA = layerCreateInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
        if (!nextGIPA) return VK_ERROR_INITIALIZATION_FAILED;

        PFN_vkCreateInstance nextCreateInstance =
            reinterpret_cast<PFN_vkCreateInstance>(nextGIPA(VK_NULL_HANDLE, "vkCreateInstance"));

        if (!nextCreateInstance) return VK_ERROR_INITIALIZATION_FAILED;

        VkInstanceCreateInfo chainCreateInfo = *pCreateInfo;
        VkLayerInstanceCreateInfo* chainLayerInfo = const_cast<VkLayerInstanceCreateInfo*>(layerCreateInfo);
        chainLayerInfo->u.pLayerInfo = chainLayerInfo->u.pLayerInfo->pNext;

        VkResult result = nextCreateInstance(&chainCreateInfo, pAllocator, pInstance);
        if (result != VK_SUCCESS || !(*pInstance)) return result;

        InstanceDispatchTable table{};
        table.gIPA = nextGIPA;
        table.destroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(nextGIPA(*pInstance, "vkDestroyInstance"));

        LayerContext::instance().registerInstance(*pInstance, table);

        return VK_SUCCESS;
    }

    inline VKAPI_ATTR VkResult VKAPI_CALL vkw_CreateDevice(
        VkPhysicalDevice physicalDevice,
        const VkDeviceCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkDevice* pDevice)
    {
        if (!pCreateInfo || !pDevice) return VK_ERROR_INITIALIZATION_FAILED;

        OutputDebugStringA("[VkWraith] Intercepted vkCreateDevice!\n");
        std::cout << "[VkWraith] Intercepted vkCreateDevice!" << std::endl;

        const VkLayerDeviceCreateInfo* layerCreateInfo =
            reinterpret_cast<const VkLayerDeviceCreateInfo*>(pCreateInfo->pNext);

        while (layerCreateInfo &&
            (layerCreateInfo->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO ||
                layerCreateInfo->function != VK_LAYER_LINK_INFO))
        {
            layerCreateInfo = reinterpret_cast<const VkLayerDeviceCreateInfo*>(layerCreateInfo->pNext);
        }

        if (!layerCreateInfo || !layerCreateInfo->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;

        PFN_vkGetInstanceProcAddr nextGIPA = layerCreateInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
        PFN_vkGetDeviceProcAddr nextGDPA = layerCreateInfo->u.pLayerInfo->pfnNextGetDeviceProcAddr;

        if (!nextGIPA || !nextGDPA) return VK_ERROR_INITIALIZATION_FAILED;

        PFN_vkCreateDevice nextCreateDevice =
            reinterpret_cast<PFN_vkCreateDevice>(nextGIPA(VK_NULL_HANDLE, "vkCreateDevice"));

        if (!nextCreateDevice) return VK_ERROR_INITIALIZATION_FAILED;

        VkDeviceCreateInfo chainCreateInfo = *pCreateInfo;
        VkLayerDeviceCreateInfo* chainLayerInfo = const_cast<VkLayerDeviceCreateInfo*>(layerCreateInfo);
        chainLayerInfo->u.pLayerInfo = chainLayerInfo->u.pLayerInfo->pNext;

        VkResult result = nextCreateDevice(physicalDevice, &chainCreateInfo, pAllocator, pDevice);
        if (result != VK_SUCCESS || !(*pDevice)) return result;

        DriverInfo driverInfo{};
        auto getProps = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
            nextGIPA(VK_NULL_HANDLE, "vkGetPhysicalDeviceProperties")
            );

        if (getProps != nullptr) {
            VkPhysicalDeviceProperties props{};
            SetCrashContext(999, "vkGetPhysicalDeviceProperties");
            getProps(physicalDevice, &props);

            driverInfo.vendorID = props.vendorID;
            driverInfo.deviceID = props.deviceID;
            driverInfo.driverVersion = props.driverVersion;
            driverInfo.deviceName = props.deviceName;

            std::cout << "[VkWraith] Device Created Successfully: " << driverInfo.deviceName
                << " (Vendor: 0x" << std::hex << driverInfo.vendorID << std::dec << ")" << std::endl;
        }

        driverInfo.activeQuirks = EvaluateQuirks(driverInfo);

        DeviceDispatchTable table{};
        table.gDPA = nextGDPA;
        table.destroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(nextGDPA(*pDevice, "vkDestroyDevice"));
        table.allocateCommandBuffers = reinterpret_cast<PFN_vkAllocateCommandBuffers>(nextGDPA(*pDevice, "vkAllocateCommandBuffers"));
        table.freeCommandBuffers = reinterpret_cast<PFN_vkFreeCommandBuffers>(nextGDPA(*pDevice, "vkFreeCommandBuffers"));

        table.cmdBeginRenderingKHR = reinterpret_cast<PFN_vkCmdBeginRenderingKHR>(nextGDPA(*pDevice, "vkCmdBeginRenderingKHR"));
        if (!table.cmdBeginRenderingKHR) {
            table.cmdBeginRenderingKHR = reinterpret_cast<PFN_vkCmdBeginRenderingKHR>(nextGDPA(*pDevice, "vkCmdBeginRendering"));
        }

        LayerContext::instance().registerDevice(*pDevice, table, driverInfo);

        return VK_SUCCESS;
    }

    inline VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkw_GetDeviceProcAddr(VkDevice device, const char* pName) {
        if (!pName) return nullptr;

        if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0)
            return reinterpret_cast<PFN_vkVoidFunction>(vkw_GetDeviceProcAddr);
        if (std::strcmp(pName, "vkCreateDevice") == 0)
            return reinterpret_cast<PFN_vkVoidFunction>(vkw_CreateDevice);
        if (std::strcmp(pName, "vkCmdBeginRendering") == 0 || std::strcmp(pName, "vkCmdBeginRenderingKHR") == 0) {
            return reinterpret_cast<PFN_vkVoidFunction>(vkw_CmdBeginRenderingKHR);
        }

        const DeviceDispatchTable* table = LayerContext::instance().getDispatchTable(device);
        return (table && table->gDPA) ? table->gDPA(device, pName) : nullptr;
    }

    inline VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkw_GetInstanceProcAddr(VkInstance instance, const char* pName) {
        if (!pName) return nullptr;

        if (std::strcmp(pName, "vkGetInstanceProcAddr") == 0)
            return reinterpret_cast<PFN_vkVoidFunction>(vkw_GetInstanceProcAddr);
        if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0)
            return reinterpret_cast<PFN_vkVoidFunction>(vkw_GetDeviceProcAddr);
        if (std::strcmp(pName, "vkCreateInstance") == 0)
            return reinterpret_cast<PFN_vkVoidFunction>(vkw_CreateInstance);
        if (std::strcmp(pName, "vkCreateDevice") == 0)
            return reinterpret_cast<PFN_vkVoidFunction>(vkw_CreateDevice);

        const InstanceDispatchTable* table = LayerContext::instance().getInstanceDispatchTable(instance);
        return (table && table->gIPA) ? table->gIPA(instance, pName) : nullptr;
    }

} // namespace vkw

extern "C" {

    VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkwraith_GetInstanceProcAddr(VkInstance instance, const char* pName) {
        return vkw::vkw_GetInstanceProcAddr(instance, pName);
    }

    VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkwraith_GetDeviceProcAddr(VkDevice device, const char* pName) {
        return vkw::vkw_GetDeviceProcAddr(device, pName);
    }

#if defined(VKWRAITH_BUILD_DLL)
    VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL VK_LAYER_HERLIENCE_VKWRAITH_Negotiate(
        VkNegotiateLayerInterface* pVersionStruct)
    {
        OutputDebugStringA("[VkWraith] Layer Negotiate Called by Vulkan Loader!\n");
        if (!pVersionStruct || pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        pVersionStruct->pfnGetInstanceProcAddr = vkw::vkw_GetInstanceProcAddr;
        pVersionStruct->pfnGetDeviceProcAddr = vkw::vkw_GetDeviceProcAddr;
        pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;

        if (pVersionStruct->loaderLayerInterfaceVersion > CURRENT_LOADER_LAYER_INTERFACE_VERSION) {
            pVersionStruct->loaderLayerInterfaceVersion = CURRENT_LOADER_LAYER_INTERFACE_VERSION;
        }

        return VK_SUCCESS;
    }
#endif 

} // extern "C"

#endif