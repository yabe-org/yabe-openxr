#include "graphic.hpp"

#include <openxr/openxr.h>
#include <vulkan/vulkan_core.h>

#include <vulkan/vulkan_raii.hpp>

#include "log.hpp"
#include "openxr.hpp"
#include "vulkan/vulkan.hpp"

namespace handover {
Graphic::Graphic(std::unique_ptr<OpenXrApp>& openxr_app) {
  init_instance(openxr_app);
  init_device(openxr_app);
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

  constexpr vk::ApplicationInfo kAppInfo{
    .pApplicationName   = "Handover",
    .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
    .pEngineName        = "none",
    .engineVersion      = VK_MAKE_VERSION(1, 0, 0),
    .apiVersion         = VK_API_VERSION_1_3,
  };
  vk::InstanceCreateInfo ici{
    .pApplicationInfo        = &kAppInfo,
    .enabledLayerCount       = 0,
    .ppEnabledLayerNames     = nullptr,
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

void Graphic::rendering_scope(vk::ImageView image_view, uint32_t width,
  uint32_t height, const std::function<void()>& func) {
  while (this->device_.waitForFences(*this->fence_, VK_TRUE, UINT64_MAX) ==
         vk::Result::eTimeout) {}
  this->device_.resetFences(*this->fence_);
  const auto& cmd = this->command_buffers_[0];
  cmd.reset();

  vk::RenderingAttachmentInfo color_attachment{
    .imageView   = image_view,
    .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
    .loadOp      = vk::AttachmentLoadOp::eClear,
    .storeOp     = vk::AttachmentStoreOp::eStore,
    .clearValue  = vk::ClearValue(vk::ClearColorValue(0.0F, 0.5F, 0.0F, 1.0F)),
  };
  vk::RenderingInfo ri{
    .renderArea =
      {
        .offset = vk::Offset2D{.x = 0, .y = 0},
        .extent = vk::Extent2D{.width = width, .height = height},
      },
    .layerCount           = 1,
    .colorAttachmentCount = 1,
    .pColorAttachments    = &color_attachment,
  };

  cmd.begin({});
  cmd.beginRendering(ri);
  func();
  cmd.endRendering();
  cmd.end();

  const vk::PipelineStageFlags wait_stage =
    vk::PipelineStageFlagBits::eColorAttachmentOutput;
  vk::SubmitInfo submit_info{
    .waitSemaphoreCount   = 1,
    .pWaitSemaphores      = &*this->image_available_semaphore_,
    .pWaitDstStageMask    = &wait_stage,
    .commandBufferCount   = 1,
    .pCommandBuffers      = &*cmd,
    .signalSemaphoreCount = 1,
    .pSignalSemaphores    = &*this->render_finished_semaphore_,
  };
  this->device_queue_.submit(submit_info, *this->fence_);
}

void Graphic::clear_color(
  vk::raii::ImageView& image_view, float r, float g, float b, float a) {
  auto image = this->image_views_.at(*image_view).image;

  while (this->device_.waitForFences(*this->fence_, VK_TRUE, UINT64_MAX) ==
         vk::Result::eTimeout) {}
  this->device_.resetFences(*this->fence_);
  auto& cmd = this->command_buffers_[0];

  cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

  // (undefined) -> TransferDstOptimal
  vk::ImageMemoryBarrier barrier_former{
    .srcAccessMask = {},
    .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
    .oldLayout = vk::ImageLayout::eUndefined,  // or the actual current layout
    .newLayout = vk::ImageLayout::eTransferDstOptimal,
    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .image               = image,
    .subresourceRange =
      vk::ImageSubresourceRange{
        .aspectMask     = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel   = 0,
        .levelCount     = 1,
        .baseArrayLayer = 0,
        .layerCount     = 1,
      },
  };
  cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
    vk::PipelineStageFlagBits::eTransfer, {}, nullptr, nullptr, barrier_former);

  vk::ClearColorValue clear_value(std::array<float, 4>{r, g, b, a});
  cmd.clearColorImage(image, vk::ImageLayout::eTransferDstOptimal, clear_value,
    vk::ImageSubresourceRange{
      .aspectMask     = vk::ImageAspectFlagBits::eColor,
      .baseMipLevel   = 0,
      .levelCount     = 1,
      .baseArrayLayer = 0,
      .layerCount     = 1,
    });

  // TransferDstOptimal -> ColorAttachmentOptimal
  vk::ImageMemoryBarrier barrier_later{
    .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
    .dstAccessMask = vk::AccessFlagBits::eMemoryRead,
    .oldLayout     = vk::ImageLayout::eTransferDstOptimal,
    .newLayout =
      vk::ImageLayout::eColorAttachmentOptimal,  // set to what OpenXR expects
    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .image               = image,
    .subresourceRange =
      vk::ImageSubresourceRange{
        .aspectMask     = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel   = 0,
        .levelCount     = 1,
        .baseArrayLayer = 0,
        .layerCount     = 1,
      },
  };
  cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
    vk::PipelineStageFlagBits::eColorAttachmentOutput, {}, nullptr, nullptr,
    barrier_later);

  cmd.end();

  const vk::PipelineStageFlags wait_stage =
    vk::PipelineStageFlagBits::eColorAttachmentOutput;
  vk::SubmitInfo si{
    .commandBufferCount = 1,
    .pCommandBuffers    = &*cmd,
  };
  this->device_queue_.submit(si, *this->fence_);
}

void Graphic::clear_depth(vk::raii::ImageView& image_view, float depth) {
}

}  // namespace handover
