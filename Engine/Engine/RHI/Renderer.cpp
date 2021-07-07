#include "Renderer.h"	
#include "../Common/Mesh.h"
#include "GraphicsPipeline.h"
#include "PipelineLayout.h"
#include "DescriptorSetLayout.h"
#include "DescriptorSet.h"
#include "RenderPass.h"
#include "VulkanUtils.h"
#include "../Application.h"
#include "SwapChain.h"
#include "../Common/RenderScene.h"

#include "../Vendor/imgui/imgui.h"
#include "../Vendor/imgui/imgui_impl_vulkan.h"

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <GLM/glm.hpp>
#include <GLM/gtc/matrix_transform.hpp>

#include <chrono>	

using namespace RHI;

Renderer::Renderer(const RendererContext& context,
	VkExtent2D extent,
	VkDescriptorSetLayout descriptorSetLayout,
	VkRenderPass renderPass)
	: context(context)
	, extent(extent)
	, renderPass(renderPass)
	, descriptorSetLayout(descriptorSetLayout)
	, hdriToCubeRenderer(context)
	, diffuseIrradianceRenderer(context)
	, environmentCubemap(context)
	, diffuseIrradianceCubemap(context)
{
}

Renderer::~Renderer()
{
	shutdown();
}


void Renderer::init(const UniformBufferObject* state, const RenderScene* scene)
{
	const Shader* pbrVertexShader = scene->getPBRVertexShader();
	const Shader* pbrFragmentShader = scene->getPBRFragmentShader();
	const Shader* skyboxVertexShader = scene->getSkyboxVertexShader();
	const Shader* skyboxFragmentShader = scene->getSkyboxFragmentShader();

	VkShaderStageFlags stage = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

	// Descriptor set Layout
	DescriptorSetLayout sceneDescriptorSetLayoutBuilder(context);
	sceneDescriptorSetLayoutBuilder.addDescriptorBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, stage);
	sceneDescriptorSetLayoutBuilder.addDescriptorBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, stage);
	sceneDescriptorSetLayoutBuilder.addDescriptorBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, stage);
	sceneDescriptorSetLayoutBuilder.addDescriptorBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, stage);
	sceneDescriptorSetLayoutBuilder.addDescriptorBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, stage);
	sceneDescriptorSetLayoutBuilder.addDescriptorBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, stage);
	sceneDescriptorSetLayoutBuilder.addDescriptorBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, stage);
	sceneDescriptorSetLayout = sceneDescriptorSetLayoutBuilder.build();

	// Pipeline Layout
	PipelineLayout pipelineLayoutBuilder(context);
	pipelineLayoutBuilder.addDescriptorSetLayout(descriptorSetLayout);
	pipelineLayoutBuilder.addDescriptorSetLayout(sceneDescriptorSetLayout);
	pipelineLayout = pipelineLayoutBuilder.build();
	
	// Graphic Pipeline
	GraphicsPipeline pbrPipelineBuilder(context, pipelineLayout, renderPass);
	pbrPipelineBuilder.addShaderStage(pbrVertexShader->getShaderModule(), VK_SHADER_STAGE_VERTEX_BIT);
	pbrPipelineBuilder.addShaderStage(pbrFragmentShader->getShaderModule(), VK_SHADER_STAGE_FRAGMENT_BIT);
	pbrPipelineBuilder.addVertexInput(Mesh::getVertexInputBindingDescription(), Mesh::getAttributeDescriptions());
	pbrPipelineBuilder.setInputAssemblyState(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	pbrPipelineBuilder.addDynamicState(VK_DYNAMIC_STATE_SCISSOR);
	pbrPipelineBuilder.addDynamicState(VK_DYNAMIC_STATE_VIEWPORT);
	pbrPipelineBuilder.addViewport(VkViewport());
	pbrPipelineBuilder.addScissor(VkRect2D());
	pbrPipelineBuilder.setRasterizerState(false, false, VK_POLYGON_MODE_FILL, 1.0f, VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE);
	pbrPipelineBuilder.setMultisampleState(context.msaaSamples, true);
	pbrPipelineBuilder.setDepthStencilState(true, true, VK_COMPARE_OP_LESS);
	pbrPipelineBuilder.addBlendColorAttachment();
	pbrPipeline = pbrPipelineBuilder.build();
		
	// Skybox Graphic Pipeline
	GraphicsPipeline skyboxPipelineBuilder(context, pipelineLayout, renderPass);
	skyboxPipelineBuilder.addShaderStage(skyboxVertexShader->getShaderModule(), VK_SHADER_STAGE_VERTEX_BIT);
	skyboxPipelineBuilder.addShaderStage(skyboxFragmentShader->getShaderModule(), VK_SHADER_STAGE_FRAGMENT_BIT);
	skyboxPipelineBuilder.addVertexInput(Mesh::getVertexInputBindingDescription(), Mesh::getAttributeDescriptions());
	skyboxPipelineBuilder.setInputAssemblyState(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	skyboxPipelineBuilder.addDynamicState(VK_DYNAMIC_STATE_SCISSOR);
	skyboxPipelineBuilder.addDynamicState(VK_DYNAMIC_STATE_VIEWPORT);
	skyboxPipelineBuilder.addViewport(VkViewport());
	skyboxPipelineBuilder.addScissor(VkRect2D());
	skyboxPipelineBuilder.setRasterizerState(false, false, VK_POLYGON_MODE_FILL, 1.0f, VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE);
	skyboxPipelineBuilder.setMultisampleState(context.msaaSamples, true);
	skyboxPipelineBuilder.setDepthStencilState(true, true, VK_COMPARE_OP_LESS);
	skyboxPipelineBuilder.addBlendColorAttachment();

	skyboxPipeline = skyboxPipelineBuilder.build();
	
	// Create scene descriptor set
	VkDescriptorSetAllocateInfo sceneDescriptorSetAllocInfo = {};
	sceneDescriptorSetAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	sceneDescriptorSetAllocInfo.descriptorPool = context.descriptorPool;
	sceneDescriptorSetAllocInfo.descriptorSetCount = 1;
	sceneDescriptorSetAllocInfo.pSetLayouts = &sceneDescriptorSetLayout;

	if (vkAllocateDescriptorSets(context.device, &sceneDescriptorSetAllocInfo, &sceneDescriptorSet) != VK_SUCCESS)
		throw std::runtime_error("Can't allocate scene descriptor set");

	initEnvironment(state, scene);

	std::array<const Texture*, 7> textures =
	{
		scene->getAlbedoTexture(),
		scene->getNormalTexture(),
		scene->getAOTexture(),
		scene->getShadingTexture(),
		scene->getEmissionTexture(),
		&environmentCubemap,
		&diffuseIrradianceCubemap,
	};

	for (int k = 0; k < textures.size(); k++)
		VulkanUtils::bindCombinedImageSampler(
			context,
			sceneDescriptorSet,
			k,
			textures[k]->getImageView(),
			textures[k]->getSampler());
}

void Renderer::initEnvironment(const UniformBufferObject* state, const RenderScene* scene)
{
	environmentCubemap.createCube(VK_FORMAT_R32G32B32A32_SFLOAT, 256, 256, 1);
	diffuseIrradianceCubemap.createCube(VK_FORMAT_R32G32B32A32_SFLOAT, 256, 256, 1);

	hdriToCubeRenderer.init(
		*scene->getCubeVertexShader(),
		*scene->getHDRIToFragmentShader(),
		environmentCubemap);

	diffuseIrradianceRenderer.init(
		*scene->getCubeVertexShader(),
		*scene->getDiffuseIrradianceFragmentShader(),
		diffuseIrradianceCubemap);

	setEnvironment(scene, state->currentEnvironment);
}

void Renderer::setEnvironment(const RenderScene* scene, int index)
{
	{
		VulkanUtils::transitionImageLayout(
			context,
			environmentCubemap.getImage(),
			environmentCubemap.getImageFormat(),
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			0, environmentCubemap.getNumMipLevels(),
			0, environmentCubemap.getNumLayers());

		hdriToCubeRenderer.render(*scene->getHDRTexture(index));

		VulkanUtils::transitionImageLayout(
			context,
			environmentCubemap.getImage(),
			environmentCubemap.getImageFormat(),
			VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			0, environmentCubemap.getNumMipLevels(),
			0, environmentCubemap.getNumLayers());
	}

	{
		VulkanUtils::transitionImageLayout(
			context,
			diffuseIrradianceCubemap.getImage(),
			diffuseIrradianceCubemap.getImageFormat(),
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			0, diffuseIrradianceCubemap.getNumMipLevels(),
			0, diffuseIrradianceCubemap.getNumLayers());

		diffuseIrradianceRenderer.render(environmentCubemap);

		VulkanUtils::transitionImageLayout(
			context,
			diffuseIrradianceCubemap.getImage(),
			diffuseIrradianceCubemap.getImageFormat(),
			VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			0, diffuseIrradianceCubemap.getNumMipLevels(),
			0, diffuseIrradianceCubemap.getNumLayers());
	}

	std::array<const Texture*, 2> textures =
	{
		&environmentCubemap,
		&diffuseIrradianceCubemap,
	};

	for (int k = 0; k < textures.size(); k++)
		VulkanUtils::bindCombinedImageSampler(
			context,
			sceneDescriptorSet,
			k + 5,
			textures[k]->getImageView(),
			textures[k]->getSampler());
}

void Renderer::shutdown()
{
	vkDestroyPipeline(context.device, pbrPipeline, nullptr);
	pbrPipeline = VK_NULL_HANDLE;

	vkDestroyPipeline(context.device, skyboxPipeline, nullptr);
	skyboxPipeline = VK_NULL_HANDLE;

	vkDestroyPipelineLayout(context.device, pipelineLayout, nullptr);
	pipelineLayout = VK_NULL_HANDLE;

	vkDestroyDescriptorSetLayout(context.device, sceneDescriptorSetLayout, nullptr);
	sceneDescriptorSetLayout = nullptr;

	vkFreeDescriptorSets(context.device, context.descriptorPool, 1, &sceneDescriptorSet);
	sceneDescriptorSet = VK_NULL_HANDLE;

	hdriToCubeRenderer.shutdown();
	diffuseIrradianceRenderer.shutdown();

	environmentCubemap.clearGPUData();
	diffuseIrradianceCubemap.clearGPUData();
}


void Renderer::update(UniformBufferObject* state, const RenderScene* scene)
{
	// Render state
	static auto startTime = std::chrono::high_resolution_clock::now();
	auto currentTime = std::chrono::high_resolution_clock::now();

	const float rotationSpeed = 0.3f;
	float time = std::chrono::duration<float, std::chrono::seconds::period>(currentTime - startTime).count();

	const glm::vec3& up = { 0.0f, 0.0f, 1.0f };
	const glm::vec3& zero = { 0.0f, 0.0f, 0.0f };

	const float aspect = extent.width / (float)extent.height;
	const float zNear = 0.1f;
	const float zFar = 1000.0f;

	const glm::vec3& cameraPos = glm::vec3(2.0f, 2.0f, 2.0f);
	const glm::mat4& rotation = glm::rotate(glm::mat4(1.0f), time * rotationSpeed * glm::radians(90.0f), up);

	state->world = glm::mat4(1.0f);
	state->view = glm::lookAt(cameraPos, zero, up) * rotation;
	state->proj = glm::perspective(glm::radians(60.0f), aspect, zNear, zFar);
	state->proj[1][1] *= -1;
	state->cameraPosWS = glm::vec3(glm::vec4(cameraPos, 1.0f) * rotation);

	if (currentEnvironment != state->currentEnvironment)
	{
		currentEnvironment = state->currentEnvironment;
		setEnvironment(scene, state->currentEnvironment);
	}
}

void Renderer::render(const UniformBufferObject* state, const RenderScene* scene, const VulkanRenderFrame& frame)
{
	VkCommandBuffer commandBuffer = frame.commandBuffer;
	VkFramebuffer frameBuffer = frame.frameBuffer;
	VkDeviceMemory uniformBufferMemory = frame.uniformBufferMemory;
	VkDescriptorSet descriptorSet = frame.descriptorSet;

	VkRenderPassBeginInfo renderPassInfo = {};
	renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	renderPassInfo.renderPass = renderPass;
	renderPassInfo.framebuffer = frameBuffer;
	renderPassInfo.renderArea.offset = { 0, 0 };
	renderPassInfo.renderArea.extent = extent;

	std::array<VkClearValue, 3> clearValues = {};
	clearValues[0].color = { 0.0f, 0.0f, 0.0f, 1.0f };
	clearValues[1].color = { 0.0f, 0.0f, 0.0f, 1.0f };
	clearValues[2].depthStencil = { 1.0f, 0 };
	renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
	renderPassInfo.pClearValues = clearValues.data();

	std::array<VkDescriptorSet, 2> sets = { descriptorSet, sceneDescriptorSet };

	VkViewport viewport = {};
	viewport.x = 0.0f;
	viewport.y = 0.0f;
	viewport.width = static_cast<float>(extent.width);
	viewport.height = static_cast<float>(extent.height);
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;

	VkRect2D scissor = {};
	scissor.offset = { 0, 0 };
	scissor.extent = extent;

	vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
	vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

	vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

	vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, skyboxPipeline);
	vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
	{
		const Mesh* skybox = scene->getSkybox();

		VkBuffer vertexBuffers[] = { skybox->getVertexBuffer() };
		VkBuffer indexBuffer = skybox->getIndexBuffer();
		VkDeviceSize offsets[] = { 0 };
		vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
		vkCmdBindIndexBuffer(commandBuffer, indexBuffer, 0, VK_INDEX_TYPE_UINT32);

		vkCmdDrawIndexed(commandBuffer, skybox->getNumIndices(), 1, 0, 0, 0);
	}
	vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pbrPipeline);
	vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
	{
		const Mesh* mesh = scene->getMesh();

		VkBuffer vertexBuffers[] = { mesh->getVertexBuffer() };
		VkBuffer indexBuffer = mesh->getIndexBuffer();
		VkDeviceSize offsets[] = { 0 };
		vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
		vkCmdBindIndexBuffer(commandBuffer, indexBuffer, 0, VK_INDEX_TYPE_UINT32);

		vkCmdDrawIndexed(commandBuffer, mesh->getNumIndices(), 1, 0, 0, 0);
	}

	vkCmdEndRenderPass(commandBuffer);
}

void Renderer::resize(const SwapChain* swapChain)
{
	extent = swapChain->getExtent();
}