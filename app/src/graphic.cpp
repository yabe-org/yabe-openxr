#include "graphic.hpp"

#include <android/asset_manager.h>
#include <openxr/openxr.h>
#include <vulkan/vulkan_core.h>

#include <vulkan/vulkan_raii.hpp>
#include <vulkan/vulkan_structs.hpp>

#include "glm/vec3.hpp"
#include "log.hpp"
#include "openxr.hpp"
#include "vulkan/vulkan.hpp"

namespace handover {
Graphic::Graphic(
  std::unique_ptr<OpenXrApp>& openxr_app, AAssetManager* asset_manager) {
  init_instance(openxr_app);
  init_device(openxr_app);
  init_shader(asset_manager);
}
Graphic::~Graphic() {
  LOG_INFO("Destroying Graphic");
}

void Graphic::init_instance(std::unique_ptr<OpenXrApp>& openxr_app) {
  const auto available_extensions =
    this->context_.enumerateInstanceExtensionProperties();
  const auto required_extensions = openxr_app->get_vulkan_instance_extensions();
  std::vector<const char*> extensions;
  for (const auto& required_extension : required_extensions) {
    auto it =
      std::find_if(available_extensions.begin(), available_extensions.end(),
        [&required_extension](const vk::ExtensionProperties& e) {
          return strcmp(e.extensionName, required_extension.c_str()) == 0;
        });
    if (it == available_extensions.end()) {
      throw std::runtime_error(
        "Required Vulkan instance extension is not supported: " +
        required_extension);
    }
    extensions.push_back(it->extensionName);
  }

  std::vector<const char*> layers;
  const auto               available_layers =
    this->context_.enumerateInstanceLayerProperties();
  {
    auto it = std::find_if(available_layers.begin(), available_layers.end(),
      [](const vk::LayerProperties& e) {
        return strcmp(e.layerName, "VK_LAYER_KHRONOS_validation") == 0;
      });
    if (it == available_layers.end()) {
      LOG_WARN("Validation layer is not available");
    } else {
      layers.push_back(it->layerName);
    }
  }

  constexpr vk::ApplicationInfo kAppInfo{
    .pApplicationName   = "Handover",
    .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
    .pEngineName        = "none",
    .engineVersion      = VK_MAKE_VERSION(1, 0, 0),
    .apiVersion         = VK_API_VERSION_1_3,
  };
  vk::InstanceCreateInfo ici{
    .pApplicationInfo        = &kAppInfo,
    .enabledLayerCount       = static_cast<uint32_t>(layers.size()),
    .ppEnabledLayerNames     = layers.data(),
    .enabledExtensionCount   = static_cast<uint32_t>(extensions.size()),
    .ppEnabledExtensionNames = extensions.data(),
  };
  this->instance_ = vk::raii::Instance(this->context_, ici);

  LOG_DEBUG("Vulkan instance was created");
}

void Graphic::init_device(std::unique_ptr<OpenXrApp>& openxr_app) {
  // Physical Device
  auto physical_devices = this->instance_.enumeratePhysicalDevices();
  if (physical_devices.empty()) {
    throw std::runtime_error("No Physical Device found");
  }
  const auto* xr_physical_device =
    openxr_app->get_vulkan_physical_device(this->instance_);
  auto it = std::find_if(physical_devices.begin(), physical_devices.end(),
    [xr_physical_device](const vk::raii::PhysicalDevice& e) {
      return *e == xr_physical_device;
    });
  if (it == physical_devices.end()) {
    throw std::runtime_error("Physical Device from OpenXR is not found");
  }
  this->physical_device_ = *it;
  LOG_DEBUG("physical device: apiVersion = {:x}",
    this->physical_device_.getProperties().apiVersion);
  // Queue Family for graphic operation
  auto queue_family_properties =
    this->physical_device_.getQueueFamilyProperties();
  for (uint32_t i = 0;
    i < static_cast<uint32_t>(queue_family_properties.size()); i++) {
    // supports graphic operation
    if (queue_family_properties[i].queueFlags & vk::QueueFlagBits::eGraphics) {
      this->queue_family_index_ = i;
      break;
    }
  }
  // Logical Device
  float                     queue_priority = 0.5F;
  vk::DeviceQueueCreateInfo dqci{
    .queueFamilyIndex = this->queue_family_index_,
    .queueCount       = 1,
    .pQueuePriorities = &queue_priority,
  };
  const auto available_extensions =
    this->physical_device_.enumerateDeviceExtensionProperties();
  const auto required_extensions = openxr_app->get_vulkan_device_extensions();
  std::vector<const char*> extensions;
  for (const auto& required_extension : required_extensions) {
    auto it =
      std::find_if(available_extensions.begin(), available_extensions.end(),
        [&required_extension](const vk::ExtensionProperties& e) {
          return strcmp(e.extensionName, required_extension.c_str()) == 0;
        });
    if (it == available_extensions.end()) {
      throw std::runtime_error(
        "Required Vulkan device extension is not supported: " +
        required_extension);
    }
    extensions.push_back(it->extensionName);
  }
  vk::DeviceCreateInfo dci{
    .queueCreateInfoCount    = 1,
    .pQueueCreateInfos       = &dqci,
    .enabledExtensionCount   = static_cast<uint32_t>(extensions.size()),
    .ppEnabledExtensionNames = extensions.data(),
  };
  this->device_ = vk::raii::Device(this->physical_device_, dci);
  this->device_queue_ =
    vk::raii::Queue(this->device_, this->queue_family_index_, 0);
  // Command Pool
  vk::CommandPoolCreateInfo cpi{
    .flags            = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
    .queueFamilyIndex = this->queue_family_index_,
  };
  this->command_pool_ = vk::raii::CommandPool(this->device_, cpi);
  // Command Buffer
  vk::CommandBufferAllocateInfo cbai{
    .commandPool        = this->command_pool_,
    .level              = vk::CommandBufferLevel::ePrimary,
    .commandBufferCount = 1,
  };
  this->command_buffers_ = vk::raii::CommandBuffers(this->device_, cbai);
  // Fence
  vk::FenceCreateInfo fci{
    .flags = vk::FenceCreateFlagBits::eSignaled,
  };
  this->fence_ = vk::raii::Fence(this->device_, fci);
}

void Graphic::init_shader(AAssetManager* asset_manager) {
  AAsset* file =
    AAssetManager_open(asset_manager, "shaders/main.spv", AASSET_MODE_BUFFER);
  if (!file) {
    throw std::runtime_error("Failed to open shader file");
  }
  size_t            file_size = AAsset_getLength(file);
  std::vector<char> code(file_size);
  AAsset_read(file, static_cast<void*>(code.data()), file_size);
  AAsset_close(file);

  vk::ShaderModuleCreateInfo smci{
    .codeSize = code.size(),
    .pCode    = reinterpret_cast<const uint32_t*>(code.data()),
  };
  this->shader_module_ = vk::raii::ShaderModule(this->device_, smci);
}

void Graphic::init_pipeline(uint32_t color_format, uint32_t depth_format) {
  std::array<vk::PipelineShaderStageCreateInfo, 2> shader_stages{
    vk::PipelineShaderStageCreateInfo{
      .stage  = vk::ShaderStageFlagBits::eVertex,
      .module = this->shader_module_,
      .pName  = "vert_main",
    },
    vk::PipelineShaderStageCreateInfo{
      .stage  = vk::ShaderStageFlagBits::eFragment,
      .module = this->shader_module_,
      .pName  = "frag_main",
    },
  };

  // vertex input state
  vk::VertexInputBindingDescription vertex_descriptions{
    .binding   = 0,
    .stride    = sizeof(float) * 3,
    .inputRate = vk::VertexInputRate::eVertex,
  };
  std::array<vk::VertexInputAttributeDescription, 1>
    vertex_attribute_descriptions{
      vk::VertexInputAttributeDescription{
        .location = 0,
        .binding  = 0,
        .format   = vk::Format::eR32G32B32Sfloat,
        .offset   = 0,
      },
    };
  vk::PipelineVertexInputStateCreateInfo vertex_input{
    .vertexBindingDescriptionCount   = 1,
    .pVertexBindingDescriptions      = &vertex_descriptions,
    .vertexAttributeDescriptionCount = vertex_attribute_descriptions.size(),
    .pVertexAttributeDescriptions    = vertex_attribute_descriptions.data(),
  };

  vk::PipelineInputAssemblyStateCreateInfo input_assembly{
    .topology = vk::PrimitiveTopology::eTriangleList,
  };

  vk::PipelineTessellationStateCreateInfo tessellation{};

  vk::PipelineViewportStateCreateInfo viewport_state{
    .viewportCount = 1,
    .scissorCount  = 1,
  };

  vk::PipelineRasterizationStateCreateInfo rasterizer{
    .depthClampEnable        = vk::False,
    .rasterizerDiscardEnable = vk::False,
    .polygonMode             = vk::PolygonMode::eFill,
    .cullMode                = vk::CullModeFlagBits::eNone,
    .frontFace               = vk::FrontFace::eClockwise,
    .depthBiasEnable         = vk::False,
    .lineWidth               = 1.0F,
  };

  vk::PipelineMultisampleStateCreateInfo multisampling{
    .rasterizationSamples = vk::SampleCountFlagBits::e1,
    .sampleShadingEnable  = vk::False,
  };

  vk::PipelineDepthStencilStateCreateInfo depth_stencil{
    .depthTestEnable       = vk::True,
    .depthWriteEnable      = vk::True,
    .depthCompareOp        = vk::CompareOp::eLessOrEqual,
    .depthBoundsTestEnable = vk::False,
    .stencilTestEnable     = vk::False,
    .minDepthBounds        = 0.0F,
    .maxDepthBounds        = 1.0F,
  };

  // color blend state
  vk::PipelineColorBlendAttachmentState color_blend_attachment{
    .blendEnable = vk::False,
    .colorWriteMask =
      vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
      vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
  };
  vk::PipelineColorBlendStateCreateInfo color_blending{
    .logicOpEnable   = vk::False,
    .attachmentCount = 1,
    .pAttachments    = &color_blend_attachment,
  };

  // pipeline layout
  vk::DescriptorSetLayoutCreateInfo dslci{};
  this->descriptor_set_layout_ = vk::raii::DescriptorSetLayout(device_, dslci);
  vk::PipelineLayoutCreateInfo plci{
    .setLayoutCount         = 1,
    .pSetLayouts            = &*this->descriptor_set_layout_,
    .pushConstantRangeCount = 0,
    .pPushConstantRanges    = nullptr,
  };
  this->pipeline_layout_ = vk::raii::PipelineLayout(device_, plci);

  // dynamic state
  std::array<vk::DynamicState, 2> dynamic_states{
    vk::DynamicState::eViewport,
    vk::DynamicState::eScissor,
  };
  vk::PipelineDynamicStateCreateInfo dynamic_state{
    .dynamicStateCount = dynamic_states.size(),
    .pDynamicStates    = dynamic_states.data(),
  };

  // render pass
  std::array<vk::AttachmentDescription, 2> color_attachment{
    vk::AttachmentDescription{
      .format         = static_cast<vk::Format>(color_format),
      .samples        = vk::SampleCountFlagBits::e1,
      .loadOp         = vk::AttachmentLoadOp::eClear,
      .storeOp        = vk::AttachmentStoreOp::eStore,
      .stencilLoadOp  = vk::AttachmentLoadOp::eDontCare,
      .stencilStoreOp = vk::AttachmentStoreOp::eDontCare,
      .initialLayout  = vk::ImageLayout::eColorAttachmentOptimal,
      .finalLayout    = vk::ImageLayout::eColorAttachmentOptimal,
    },
    vk::AttachmentDescription{
      .format         = static_cast<vk::Format>(depth_format),
      .samples        = vk::SampleCountFlagBits::e1,
      .loadOp         = vk::AttachmentLoadOp::eClear,
      .storeOp        = vk::AttachmentStoreOp::eStore,
      .stencilLoadOp  = vk::AttachmentLoadOp::eDontCare,
      .stencilStoreOp = vk::AttachmentStoreOp::eDontCare,
      .initialLayout  = vk::ImageLayout::eDepthStencilAttachmentOptimal,
      .finalLayout    = vk::ImageLayout::eDepthStencilAttachmentOptimal,
    }};
  vk::AttachmentReference color_reference{
    .attachment = 0,
    .layout     = vk::ImageLayout::eColorAttachmentOptimal,
  };
  vk::AttachmentReference depth_reference{
    .attachment = 1,
    .layout     = vk::ImageLayout::eDepthStencilAttachmentOptimal,
  };
  vk::SubpassDescription subpass_description{
    .pipelineBindPoint       = vk::PipelineBindPoint::eGraphics,
    .colorAttachmentCount    = 1,
    .pColorAttachments       = &color_reference,
    .pDepthStencilAttachment = &depth_reference,
  };
  vk::SubpassDependency subpass_dependency{
    .srcSubpass    = vk::SubpassExternal,
    .dstSubpass    = 0,
    .srcStageMask  = vk::PipelineStageFlagBits::eColorAttachmentOutput,
    .dstStageMask  = vk::PipelineStageFlagBits::eColorAttachmentOutput,
    .srcAccessMask = {},
    .dstAccessMask = vk::AccessFlagBits::eColorAttachmentRead |
                     vk::AccessFlagBits::eColorAttachmentWrite,
  };
  vk::RenderPassCreateInfo rpci{
    .attachmentCount = color_attachment.size(),
    .pAttachments    = color_attachment.data(),
    .subpassCount    = 1,
    .pSubpasses      = &subpass_description,
    .dependencyCount = 1,
    .pDependencies   = &subpass_dependency,
  };
  this->render_pass_ = vk::raii::RenderPass(this->device_, rpci);

  vk::GraphicsPipelineCreateInfo gpci{
    .stageCount          = shader_stages.size(),
    .pStages             = shader_stages.data(),
    .pVertexInputState   = &vertex_input,
    .pInputAssemblyState = &input_assembly,
    .pTessellationState  = &tessellation,
    .pViewportState      = &viewport_state,
    .pRasterizationState = &rasterizer,
    .pMultisampleState   = &multisampling,
    .pDepthStencilState  = &depth_stencil,
    .pColorBlendState    = &color_blending,
    .pDynamicState       = &dynamic_state,
    .layout              = this->pipeline_layout_,
    .renderPass          = this->render_pass_,
  };
  this->pipeline_ = vk::raii::Pipeline(this->device_, nullptr, gpci);
}

XrGraphicsBindingVulkanKHR Graphic::get_graphics_binding() const {
  return XrGraphicsBindingVulkanKHR{
    .type             = XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR,
    .next             = nullptr,
    .instance         = *this->instance_,
    .physicalDevice   = *this->physical_device_,
    .device           = *this->device_,
    .queueFamilyIndex = this->queue_family_index_,
    .queueIndex       = 0,  // FIXME: what's this?
  };
}

XrSwapchainImageBaseHeader* Graphic::acquire_swapchain_image(
  XrSwapchain swapchain, SwapchainType type, uint32_t count) {
  this->swapchains_.try_emplace(
    swapchain, SwapchainInfo{
                 .type   = type,
                 .images = std::vector<XrSwapchainImageVulkanKHR>(count,
                   {
                     .type  = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR,
                     .next  = nullptr,
                     .image = VK_NULL_HANDLE,
                   }),
               });
  return reinterpret_cast<XrSwapchainImageBaseHeader*>(
    this->swapchains_.at(swapchain).images.data());
}

vk::raii::ImageView Graphic::create_swapchain_image_views(
  XrSwapchain swapchain, uint32_t index, SwapchainType type, uint32_t format) {
  vk::ImageViewCreateInfo ivci{
    .image    = this->swapchains_.at(swapchain).images[index].image,
    .viewType = vk::ImageViewType::e2D,
    .format   = static_cast<vk::Format>(format),
    .subresourceRange{
      .aspectMask     = (type == SwapchainType::COLOR) ?
                          vk::ImageAspectFlagBits::eColor :
                          vk::ImageAspectFlagBits::eDepth,
      .baseMipLevel   = 0,
      .levelCount     = 1,
      .baseArrayLayer = 0,
      .layerCount     = 1,
    },
  };
  auto image_view = vk::raii::ImageView(this->device_, ivci);
  this->image_views_.emplace(*image_view, ivci);
  return image_view;
}

Graphic::Buffer Graphic::create_buffer(
  const void* data, vk::DeviceSize size, vk::BufferUsageFlags usage) {
  vk::BufferCreateInfo bci{
    .size  = size,
    .usage = usage,
  };
  auto buffer              = vk::raii::Buffer(this->device_, bci);
  auto memory_requirements = buffer.getMemoryRequirements();

  auto           memory_props = this->physical_device_.getMemoryProperties();
  constexpr auto kRequiredPropsBit = vk::MemoryPropertyFlagBits::eHostVisible |
                                     vk::MemoryPropertyFlagBits::eHostCoherent;
  uint32_t memory_type_index = UINT32_MAX;
  for (uint32_t i = 0; i < memory_props.memoryTypeCount; i++) {
    if ((memory_requirements.memoryTypeBits & (1 << i)) == 0) {
      continue;
    }
    if ((memory_props.memoryTypes.at(i).propertyFlags & kRequiredPropsBit) !=
        kRequiredPropsBit) {
      continue;
    }
    memory_type_index = i;
    break;
  }
  if (memory_type_index == UINT32_MAX) {
    throw std::runtime_error("Failed to find suitable memory type for buffer");
  }

  vk::MemoryAllocateInfo mai{
    .allocationSize  = memory_requirements.size,
    .memoryTypeIndex = memory_type_index,
  };
  auto  memory        = vk::raii::DeviceMemory(this->device_, mai);
  auto* mapped_memory = memory.mapMemory(0, size);
  std::memcpy(mapped_memory, data, static_cast<size_t>(size));
  memory.unmapMemory();
  buffer.bindMemory(*memory, 0);
  return Buffer{std::move(buffer), std::move(memory)};
}

void Graphic::rendering_scope(
  const std::function<void(vk::raii::CommandBuffer&)>& func) {
  while (this->device_.waitForFences(*this->fence_, VK_TRUE, UINT64_MAX) ==
         vk::Result::eTimeout) {}
  this->device_.resetFences(*this->fence_);
  this->frame_buffer_.release();
  auto& cmd = this->command_buffers_[0];
  cmd.reset();

  cmd.begin({});
  func(cmd);
  if (this->is_render_pass_started_) {
    cmd.endRenderPass();
  }
  cmd.end();
  this->is_render_pass_started_ = false;

  vk::SubmitInfo si{
    .commandBufferCount = 1,
    .pCommandBuffers    = &*cmd,
  };
  this->device_queue_.submit(si, *this->fence_);
}

void Graphic::set_render_attachments(vk::raii::CommandBuffer& cmd,
  vk::raii::ImageView& color_view, vk::raii::ImageView& depth_view,
  uint32_t width, uint32_t height, float clear_r, float clear_g, float clear_b,
  float clear_a, float clear_depth) {
  std::array<vk::ImageView, 2> attachments{color_view, depth_view};
  vk::FramebufferCreateInfo    fci{
       .renderPass      = this->render_pass_,
       .attachmentCount = attachments.size(),
       .pAttachments    = attachments.data(),
       .width           = width,
       .height          = height,
       .layers          = 1,
  };
  this->frame_buffer_ = vk::raii::Framebuffer(this->device_, fci);

  std::array<vk::ClearValue, 2> clear_values{
    vk::ClearColorValue{
      std::array<float, 4>{clear_r, clear_g, clear_b, clear_a}},
    vk::ClearDepthStencilValue{.depth = clear_depth, .stencil = 0},
  };
  vk::RenderPassBeginInfo rpbi{
    .renderPass  = this->render_pass_,
    .framebuffer = this->frame_buffer_,
    .renderArea{
      .offset = vk::Offset2D{.x = 0, .y = 0},
      .extent = vk::Extent2D{.width = width, .height = height},
    },
    .clearValueCount = static_cast<uint32_t>(clear_values.size()),
    .pClearValues    = clear_values.data(),
  };
  cmd.beginRenderPass(rpbi, vk::SubpassContents::eInline);
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, this->pipeline_);
  this->is_render_pass_started_ = true;
}

void Graphic::set_viewport(vk::raii::CommandBuffer& cmd, float x, float y,
  float width, float height, float min_depth, float max_depth) {
  auto viewport = vk::Viewport{
    .x        = x,
    .y        = y,
    .width    = width,
    .height   = height,
    .minDepth = min_depth,
    .maxDepth = max_depth,
  };
  cmd.setViewport(0, viewport);
}

void Graphic::set_scissor(vk::raii::CommandBuffer& cmd, float offset_x,
  float offset_y, uint32_t width, uint32_t height) {
  auto scissor = vk::Rect2D{
    .offset =
      vk::Offset2D{
        .x = static_cast<int32_t>(offset_x),
        .y = static_cast<int32_t>(offset_y),
      },
    .extent =
      vk::Extent2D{
        .width  = width,
        .height = height,
      },
  };
  cmd.setScissor(0, scissor);
}

void Graphic::set_buffers(vk::raii::CommandBuffer& cmd,
  const Graphic::Buffer& vertex_buffer, const Graphic::Buffer& index_buffer) {
  cmd.bindVertexBuffers(
    /*firstBinding=*/0, vertex_buffer.buffer(), /*offsets=*/{0});
  cmd.bindIndexBuffer(
    index_buffer.buffer(), /*offset=*/0, vk::IndexType::eUint16);
}

void Graphic::draw_indexed(vk::raii::CommandBuffer& cmd, uint32_t index_count) {
  cmd.drawIndexed(index_count,
    /*instanceCount=*/1, /*firstIndex=*/0, /*vertexOffset=*/0,
    /*firstInstance=*/0);
}

}  // namespace handover
