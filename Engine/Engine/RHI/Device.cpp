#define VK_USE_PLATFORM_WIN32_KHR
#include "Device.h"
#include "VulkanUtils.h"
#include "../Vendor/vma/vk_mem_alloc.h"

#include <array>
#include <vector>
#include <iostream>
#include <set>

#include <stdexcept>

namespace RHI
{
	static int maxCombinedImageSamplers = 32;
	static int maxUniformBuffers = 32;
	static int maxDescriptorSets = 512;

	static std::vector<const char*> requiredInstanceExtensions = {
		VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
		VK_KHR_SURFACE_EXTENSION_NAME,
	};

	static std::vector<const char*> requiredPhysicalDeviceExtensions = {
		VK_KHR_SWAPCHAIN_EXTENSION_NAME,
	};

	static std::vector<const char*> requiredValidationLayers = {
		"VK_LAYER_KHRONOS_validation",
	};

	const char* Device::getInstanceExtension()
	{
		return "VK_KHR_win32_surface";
	}

	VkSurfaceKHR Device::createSurface(void* native_window)
	{
		VkSurfaceKHR surface = VK_NULL_HANDLE;

		VkWin32SurfaceCreateInfoKHR surfaceInfo = {};
		surfaceInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
		surfaceInfo.hwnd = reinterpret_cast<HWND>(native_window);
		surfaceInfo.hinstance = GetModuleHandle(nullptr);

		if (vkCreateWin32SurfaceKHR(instance, &surfaceInfo, nullptr, &surface) != VK_SUCCESS)
			std::cerr << "Platform::createSurface(): vkCreateWin32SurfaceKHR failed" << std::endl;

		return surface;
	}

	void Device::destroySurface(VkSurfaceKHR surface)
	{
		vkDestroySurfaceKHR(instance, surface, nullptr);
	}

	void Device::init(const char* applicationName, const char* engineName)
	{
		// Check required instance extensions
		requiredInstanceExtensions.push_back(getInstanceExtension());
		if (!Utils::checkInstanceExtensions(requiredInstanceExtensions, true))
			throw std::runtime_error("This device doesn't have required Vulkan extensions");

		// Check required instance validation layers
		if (!Utils::checkInstanceValidationLayers(requiredValidationLayers, true))
			throw std::runtime_error("This device doesn't have required Vulkan validation layers");

		// Fill instance structures
		VkApplicationInfo appInfo = {};
		appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
		appInfo.pApplicationName = applicationName;
		appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
		appInfo.pEngineName = engineName;
		appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
		appInfo.apiVersion = VK_API_VERSION_1_0;

		VkInstanceCreateInfo instanceInfo = {};
		instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
		instanceInfo.pApplicationInfo = &appInfo;
		instanceInfo.enabledExtensionCount = static_cast<uint32_t>(requiredInstanceExtensions.size());
		instanceInfo.ppEnabledExtensionNames = requiredInstanceExtensions.data();
		instanceInfo.enabledLayerCount = static_cast<uint32_t>(requiredValidationLayers.size());
		instanceInfo.ppEnabledLayerNames = requiredValidationLayers.data();
		instanceInfo.pNext = nullptr;

		// Create Vulkan instance
		VkResult result = vkCreateInstance(&instanceInfo, nullptr, &instance);
		if (result != VK_SUCCESS)
			throw std::runtime_error("Failed to create Vulkan instance");

		// Enumerate physical devices
		uint32_t deviceCount = 0;
		vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);

		if (deviceCount == 0)
			throw std::runtime_error("Failed to find GPUs with Vulkan support");

		std::vector<VkPhysicalDevice> devices(deviceCount);
		vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

		// Pick the best physical device
		int estimate = -1;
		for (const auto& device : devices)
		{
			int currentEstimate = examinePhysicalDevice(device);
			if (currentEstimate == -1)
				continue;

			if (estimate > currentEstimate)
				continue;

			estimate = currentEstimate;
			physicalDevice = device;
		}

		if (physicalDevice == VK_NULL_HANDLE)
			throw std::runtime_error("Failed to find a suitable GPU");

		// Create logical device
		graphicsQueueFamily = Utils::getGraphicsQueueFamily(physicalDevice);
		const float queuePriority = 1.0f;

		VkDeviceQueueCreateInfo graphicsQueueInfo = {};
		graphicsQueueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		graphicsQueueInfo.queueFamilyIndex = graphicsQueueFamily;
		graphicsQueueInfo.queueCount = 1;
		graphicsQueueInfo.pQueuePriorities = &queuePriority;

		VkPhysicalDeviceFeatures deviceFeatures = {};
		deviceFeatures.samplerAnisotropy = VK_TRUE;
		deviceFeatures.sampleRateShading = VK_TRUE;

		VkDeviceCreateInfo deviceCreateInfo = {};
		deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
		deviceCreateInfo.pEnabledFeatures = &deviceFeatures;
		deviceCreateInfo.queueCreateInfoCount = 1;
		deviceCreateInfo.pQueueCreateInfos = &graphicsQueueInfo;
		deviceCreateInfo.enabledExtensionCount = static_cast<uint32_t>(requiredPhysicalDeviceExtensions.size());
		deviceCreateInfo.ppEnabledExtensionNames = requiredPhysicalDeviceExtensions.data();

		// next two parameters are ignored, but it's still good to pass layers for backward compatibility
		deviceCreateInfo.enabledLayerCount = static_cast<uint32_t>(requiredValidationLayers.size());
		deviceCreateInfo.ppEnabledLayerNames = requiredValidationLayers.data();

		result = vkCreateDevice(physicalDevice, &deviceCreateInfo, nullptr, &device);
		if (result != VK_SUCCESS)
			throw std::runtime_error("Can't create logical device");

		// Get graphics queue
		vkGetDeviceQueue(device, graphicsQueueFamily, 0, &graphicsQueue);
		if (graphicsQueue == VK_NULL_HANDLE)
			throw std::runtime_error("Can't get graphics queue from logical device");

		// Create command pool
		VkCommandPoolCreateInfo commandPoolInfo = {};
		commandPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		commandPoolInfo.queueFamilyIndex = graphicsQueueFamily;
		commandPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

		if (vkCreateCommandPool(device, &commandPoolInfo, nullptr, &commandPool) != VK_SUCCESS)
			throw std::runtime_error("Can't create command pool");

		// Create descriptor pools
		std::array<VkDescriptorPoolSize, 2> descriptorPoolSizes = {};
		descriptorPoolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		descriptorPoolSizes[0].descriptorCount = maxUniformBuffers;
		descriptorPoolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		descriptorPoolSizes[1].descriptorCount = maxCombinedImageSamplers;

		VkDescriptorPoolCreateInfo descriptorPoolInfo = {};
		descriptorPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		descriptorPoolInfo.poolSizeCount = static_cast<uint32_t>(descriptorPoolSizes.size());
		descriptorPoolInfo.pPoolSizes = descriptorPoolSizes.data();
		descriptorPoolInfo.maxSets = maxDescriptorSets;
		descriptorPoolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;

		if (vkCreateDescriptorPool(device, &descriptorPoolInfo, nullptr, &descriptorPool) != VK_SUCCESS)
			throw std::runtime_error("Can't create descriptor pool");

		maxMSAASamples = Utils::getMaxUsableSampleCount(physicalDevice);

		VmaAllocatorCreateInfo allocatorInfo = {};
		allocatorInfo.physicalDevice = physicalDevice;
		allocatorInfo.device = device;
		allocatorInfo.instance = instance;

		if (vmaCreateAllocator(&allocatorInfo, &vram_allocator) != VK_SUCCESS)
			throw std::runtime_error("Can't create VRAM allocator");
	}

	void Device::shutdown()
	{
		vmaDestroyAllocator(vram_allocator);
		vram_allocator = VK_NULL_HANDLE;

		vkDestroyDescriptorPool(device, descriptorPool, nullptr);
		descriptorPool = VK_NULL_HANDLE;

		vkDestroyCommandPool(device, commandPool, nullptr);
		commandPool = VK_NULL_HANDLE;

		vkDestroyDevice(device, nullptr);
		device = VK_NULL_HANDLE;

		vkDestroyInstance(instance, nullptr);
		instance = VK_NULL_HANDLE;

		graphicsQueueFamily = 0xFFFF;
		graphicsQueue = VK_NULL_HANDLE;

		maxMSAASamples = VK_SAMPLE_COUNT_1_BIT;
		physicalDevice = VK_NULL_HANDLE;
	}

	void Device::wait()
	{
		vkDeviceWaitIdle(device);
	}

	int Device::examinePhysicalDevice(VkPhysicalDevice physicalDevice) const
	{
		if (!Utils::checkPhysicalDeviceExtensions(physicalDevice, requiredPhysicalDeviceExtensions))
			return -1;

		VkPhysicalDeviceProperties physicalDeviceProperties;
		vkGetPhysicalDeviceProperties(physicalDevice, &physicalDeviceProperties);

		VkPhysicalDeviceFeatures physicalDeviceFeatures;
		vkGetPhysicalDeviceFeatures(physicalDevice, &physicalDeviceFeatures);

		int estimate = 0;

		switch (physicalDeviceProperties.deviceType)
		{
		case VK_PHYSICAL_DEVICE_TYPE_OTHER:
		case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
		case VK_PHYSICAL_DEVICE_TYPE_CPU: estimate = 10; break;

		case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: estimate = 100; break;
		case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: estimate = 1000; break;
		}

		// TODO: add more estimates here
		if (physicalDeviceFeatures.geometryShader)
			estimate++;

		if (physicalDeviceFeatures.tessellationShader)
			estimate++;

		return estimate;
	}

}