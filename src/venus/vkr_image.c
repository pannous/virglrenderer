/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "vkr_image.h"

#include "vkr_image_gen.h"
#include "vkr_device_memory.h"
#include "vkr_context.h"
#include "vkr_physical_device.h"

#ifdef __APPLE__
#include <stdlib.h>
#include <IOSurface/IOSurface.h>
#include <vulkan/vulkan_metal.h>
#endif

#ifdef __APPLE__
static bool
vkr_image_should_export_iosurface(const struct vkr_physical_device *physical_dev,
                                  const VkImageCreateInfo *info)
{
   if (!physical_dev->use_host_pointer_import || !physical_dev->EXT_metal_objects)
      return false;

   if (!getenv("VKR_USE_IOSURFACE"))
      return false;

   if (vkr_find_struct(info->pNext, VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT))
      return false;

   if (info->imageType != VK_IMAGE_TYPE_2D)
      return false;

   if (info->samples != VK_SAMPLE_COUNT_1_BIT)
      return false;

   if (!(info->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
      return false;

   if (info->arrayLayers != 1)
      return false;

   return true;
}

static void
vkr_image_strip_drm_modifier(struct vkr_physical_device *physical_dev,
                             VkImageCreateInfo *info)
{
   if (!physical_dev->use_host_pointer_import)
      return;

   if (info->tiling != VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT)
      return;

   VkBaseInStructure *prev =
      vkr_find_prev_struct(info, VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT);
   if (prev && prev->pNext) {
      prev->pNext = prev->pNext->pNext;
      vkr_log("stripped VkImageDrmFormatModifierExplicitCreateInfoEXT from VkImageCreateInfo");
   }

   prev =
      vkr_find_prev_struct(info, VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT);
   if (prev && prev->pNext) {
      prev->pNext = prev->pNext->pNext;
      vkr_log("stripped VkImageDrmFormatModifierListCreateInfoEXT from VkImageCreateInfo");
   }

   info->tiling = VK_IMAGE_TILING_LINEAR;
   vkr_log("forcing VkImageCreateInfo tiling to VK_IMAGE_TILING_LINEAR for MoltenVK");
}

static void
vkr_image_strip_external_memory(struct vkr_physical_device *physical_dev,
                                VkImageCreateInfo *info)
{
   if (!physical_dev->use_host_pointer_import)
      return;

   /* MoltenVK does not support external-memory image creation for the
    * handle types we expose to the guest (DMA_BUF/host pointer), so
    * vkCreateImage can fail even though we will import memory later.
    * We strip VkExternalMemoryImageCreateInfo to let image creation
    * succeed, then rely on the host-pointer import path at allocate/bind.
    * Alternative paths:
    *  - Guest uses VK_EXT_external_memory_host (if advertised) and creates
    *    non-external images, importing host pointers only at allocation.
    *  - Implement a MoltenVK-side extension/patch to accept external images
    *    for these handle types.
    *  - Use buffer+copy or other blit paths (not zero-copy).
    */
   VkBaseInStructure *prev =
      vkr_find_prev_struct(info, VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO);
   if (prev && prev->pNext) {
      prev->pNext = prev->pNext->pNext;
      vkr_log("stripped VkExternalMemoryImageCreateInfo from VkImageCreateInfo");
   }
}

static void
vkr_image_try_export_iosurface(struct vkr_context *ctx,
                               struct vkr_device *dev,
                               VkImage image,
                               uint32_t res_id)
{
   uint32_t existing = 0;
   if (!res_id)
      return;

   if (vkr_context_get_resource_iosurface_id(ctx, res_id, &existing) && existing)
      return;

   if (!getenv("VKR_USE_IOSURFACE"))
      return;

   PFN_vkExportMetalObjectsEXT export_fn =
      (PFN_vkExportMetalObjectsEXT)dev->physical_device->proc_table.GetDeviceProcAddr(
         dev->base.handle.device, "vkExportMetalObjectsEXT");
   if (!export_fn)
      return;

   VkExportMetalIOSurfaceInfoEXT ios_info = {
      .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_IO_SURFACE_INFO_EXT,
      .image = image,
      .ioSurface = NULL,
   };
   VkExportMetalObjectsInfoEXT metal_info = {
      .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
      .pNext = &ios_info,
   };

   export_fn(dev->base.handle.device, &metal_info);

   if (ios_info.ioSurface) {
      const uint32_t ios_id = IOSurfaceGetID(ios_info.ioSurface);
      if (ios_id) {
         vkr_context_set_resource_iosurface_id(ctx, res_id, ios_id);
         vkr_log("IOSurface export: res_id=%u iosurface_id=%u", res_id, ios_id);
      }
   }
}
#endif

static void
vkr_dispatch_vkCreateImage(struct vn_dispatch_context *dispatch,
                           struct vn_command_vkCreateImage *args)
{
   /* XXX If VkExternalMemoryImageCreateInfo is chained by the app, all is
    * good.  If it is not chained, we might still bind an external memory to
    * the image, because vkr_dispatch_vkAllocateMemory makes any HOST_VISIBLE
    * memory external.  That is a spec violation.
    *
    * The discussions in vkr_dispatch_vkCreateBuffer are applicable to both
    * buffers and images.  Additionally, drivers usually use
    * VkExternalMemoryImageCreateInfo to pick a well-defined image layout for
    * interoperability with foreign queues.  However, a well-defined layout
    * might not exist for some images.  When it does, it might still require a
    * dedicated allocation or might have a degraded performance.
    *
    * On the other hand, binding an external memory to an image created
    * without VkExternalMemoryImageCreateInfo usually works.  Yes, it will
    * explode if the external memory is accessed by foreign queues due to the
    * lack of a well-defined image layout.  But we never end up in that
    * situation because the app does not consider the memory external.
    */

   struct vkr_context *ctx = dispatch->data;
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   VkImageCreateInfo *info = (VkImageCreateInfo *)args->pCreateInfo;
#ifdef __APPLE__
   vkr_image_strip_drm_modifier(dev->physical_device, info);
   vkr_image_strip_external_memory(dev->physical_device, info);

   if (vkr_image_should_export_iosurface(dev->physical_device, info)) {
      const void *orig_next = info->pNext;
      VkExportMetalObjectCreateInfoEXT metal_export_info = {
         .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT,
         .pNext = orig_next,
         .exportObjectType = VK_EXPORT_METAL_OBJECT_TYPE_METAL_IOSURFACE_BIT_EXT,
      };
      info->pNext = &metal_export_info;
      vkr_image_create_and_add(ctx, args);
      info->pNext = orig_next;
      return;
   }
#endif

   vkr_image_create_and_add(ctx, args);
}

static void
vkr_dispatch_vkDestroyImage(struct vn_dispatch_context *dispatch,
                            struct vn_command_vkDestroyImage *args)
{
   vkr_image_destroy_and_remove(dispatch->data, args);
}

static void
vkr_dispatch_vkGetImageMemoryRequirements(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetImageMemoryRequirements *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetImageMemoryRequirements_args_handle(args);
   vk->GetImageMemoryRequirements(args->device, args->image, args->pMemoryRequirements);
}

static void
vkr_dispatch_vkGetImageMemoryRequirements2(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetImageMemoryRequirements2 *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetImageMemoryRequirements2_args_handle(args);
   vk->GetImageMemoryRequirements2(args->device, args->pInfo, args->pMemoryRequirements);
}

static void
vkr_dispatch_vkGetImageSparseMemoryRequirements(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetImageSparseMemoryRequirements *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetImageSparseMemoryRequirements_args_handle(args);
   vk->GetImageSparseMemoryRequirements(args->device, args->image,
                                        args->pSparseMemoryRequirementCount,
                                        args->pSparseMemoryRequirements);
}

static void
vkr_dispatch_vkGetImageSparseMemoryRequirements2(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetImageSparseMemoryRequirements2 *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetImageSparseMemoryRequirements2_args_handle(args);
   vk->GetImageSparseMemoryRequirements2(args->device, args->pInfo,
                                         args->pSparseMemoryRequirementCount,
                                         args->pSparseMemoryRequirements);
}

static void
vkr_dispatch_vkBindImageMemory(struct vn_dispatch_context *dispatch,
                               struct vn_command_vkBindImageMemory *args)
{
   struct vkr_context *ctx = dispatch->data;
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;
   struct vkr_device_memory *mem = vkr_device_memory_from_handle(args->memory);

   vn_replace_vkBindImageMemory_args_handle(args);
   args->ret =
      vk->BindImageMemory(args->device, args->image, args->memory, args->memoryOffset);

#ifdef __APPLE__
   if (args->ret == VK_SUCCESS && mem && mem->imported_res_id)
      vkr_image_try_export_iosurface(ctx, dev, args->image, mem->imported_res_id);
#endif
}

static void
vkr_dispatch_vkBindImageMemory2(struct vn_dispatch_context *dispatch,
                                struct vn_command_vkBindImageMemory2 *args)
{
   struct vkr_context *ctx = dispatch->data;
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;
   uint32_t *imported_res_ids = NULL;
   uint32_t bind_count = args->bindInfoCount;

#ifdef __APPLE__
   if (bind_count) {
      imported_res_ids = calloc(bind_count, sizeof(*imported_res_ids));
      if (imported_res_ids) {
         for (uint32_t i = 0; i < bind_count; i++) {
            struct vkr_device_memory *mem =
               vkr_device_memory_from_handle(args->pBindInfos[i].memory);
            if (mem)
               imported_res_ids[i] = mem->imported_res_id;
         }
      }
   }
#endif

   vn_replace_vkBindImageMemory2_args_handle(args);
   args->ret = vk->BindImageMemory2(args->device, args->bindInfoCount, args->pBindInfos);

#ifdef __APPLE__
   if (args->ret == VK_SUCCESS && imported_res_ids) {
      for (uint32_t i = 0; i < bind_count; i++) {
         if (imported_res_ids[i])
            vkr_image_try_export_iosurface(ctx, dev, args->pBindInfos[i].image,
                                           imported_res_ids[i]);
      }
   }

   free(imported_res_ids);
#endif
}

static void
vkr_dispatch_vkGetImageSubresourceLayout(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetImageSubresourceLayout *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetImageSubresourceLayout_args_handle(args);
   vk->GetImageSubresourceLayout(args->device, args->image, args->pSubresource,
                                 args->pLayout);
}

static void
vkr_dispatch_vkGetImageSubresourceLayout2(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetImageSubresourceLayout2 *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetImageSubresourceLayout2_args_handle(args);
   vk->GetImageSubresourceLayout2(args->device, args->image, args->pSubresource,
                                  args->pLayout);
}

static void
vkr_dispatch_vkGetDeviceImageSubresourceLayout(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetDeviceImageSubresourceLayout *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetDeviceImageSubresourceLayout_args_handle(args);
   vk->GetDeviceImageSubresourceLayout(args->device, args->pInfo, args->pLayout);
}

static void
vkr_dispatch_vkGetImageDrmFormatModifierPropertiesEXT(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetImageDrmFormatModifierPropertiesEXT *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetImageDrmFormatModifierPropertiesEXT_args_handle(args);
   args->ret = vk->GetImageDrmFormatModifierPropertiesEXT(args->device, args->image,
                                                          args->pProperties);
}

static void
vkr_dispatch_vkCreateImageView(struct vn_dispatch_context *dispatch,
                               struct vn_command_vkCreateImageView *args)
{
   vkr_image_view_create_and_add(dispatch->data, args);
}

static void
vkr_dispatch_vkDestroyImageView(struct vn_dispatch_context *dispatch,
                                struct vn_command_vkDestroyImageView *args)
{
   vkr_image_view_destroy_and_remove(dispatch->data, args);
}

static void
vkr_dispatch_vkCreateSampler(struct vn_dispatch_context *dispatch,
                             struct vn_command_vkCreateSampler *args)
{
   vkr_sampler_create_and_add(dispatch->data, args);
}

static void
vkr_dispatch_vkDestroySampler(struct vn_dispatch_context *dispatch,
                              struct vn_command_vkDestroySampler *args)
{
   vkr_sampler_destroy_and_remove(dispatch->data, args);
}

static void
vkr_dispatch_vkCreateSamplerYcbcrConversion(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkCreateSamplerYcbcrConversion *args)
{
   vkr_sampler_ycbcr_conversion_create_and_add(dispatch->data, args);
}

static void
vkr_dispatch_vkDestroySamplerYcbcrConversion(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkDestroySamplerYcbcrConversion *args)
{
   vkr_sampler_ycbcr_conversion_destroy_and_remove(dispatch->data, args);
}

static void
vkr_dispatch_vkGetDeviceImageMemoryRequirements(
   UNUSED struct vn_dispatch_context *ctx,
   struct vn_command_vkGetDeviceImageMemoryRequirements *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetDeviceImageMemoryRequirements_args_handle(args);
   vk->GetDeviceImageMemoryRequirements(args->device, args->pInfo,
                                        args->pMemoryRequirements);
}

static void
vkr_dispatch_vkGetDeviceImageSparseMemoryRequirements(
   UNUSED struct vn_dispatch_context *ctx,
   struct vn_command_vkGetDeviceImageSparseMemoryRequirements *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetDeviceImageSparseMemoryRequirements_args_handle(args);
   vk->GetDeviceImageSparseMemoryRequirements(args->device, args->pInfo,
                                              args->pSparseMemoryRequirementCount,
                                              args->pSparseMemoryRequirements);
}

void
vkr_context_init_image_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateImage = vkr_dispatch_vkCreateImage;
   dispatch->dispatch_vkDestroyImage = vkr_dispatch_vkDestroyImage;
   dispatch->dispatch_vkGetImageMemoryRequirements =
      vkr_dispatch_vkGetImageMemoryRequirements;
   dispatch->dispatch_vkGetImageMemoryRequirements2 =
      vkr_dispatch_vkGetImageMemoryRequirements2;
   dispatch->dispatch_vkGetImageSparseMemoryRequirements =
      vkr_dispatch_vkGetImageSparseMemoryRequirements;
   dispatch->dispatch_vkGetImageSparseMemoryRequirements2 =
      vkr_dispatch_vkGetImageSparseMemoryRequirements2;
   dispatch->dispatch_vkBindImageMemory = vkr_dispatch_vkBindImageMemory;
   dispatch->dispatch_vkBindImageMemory2 = vkr_dispatch_vkBindImageMemory2;
   dispatch->dispatch_vkGetImageSubresourceLayout =
      vkr_dispatch_vkGetImageSubresourceLayout;
   dispatch->dispatch_vkGetImageSubresourceLayout2 =
      vkr_dispatch_vkGetImageSubresourceLayout2;
   dispatch->dispatch_vkGetDeviceImageSubresourceLayout =
      vkr_dispatch_vkGetDeviceImageSubresourceLayout;

   dispatch->dispatch_vkGetImageDrmFormatModifierPropertiesEXT =
      vkr_dispatch_vkGetImageDrmFormatModifierPropertiesEXT;
   dispatch->dispatch_vkGetDeviceImageMemoryRequirements =
      vkr_dispatch_vkGetDeviceImageMemoryRequirements;
   dispatch->dispatch_vkGetDeviceImageSparseMemoryRequirements =
      vkr_dispatch_vkGetDeviceImageSparseMemoryRequirements;
}

void
vkr_context_init_image_view_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateImageView = vkr_dispatch_vkCreateImageView;
   dispatch->dispatch_vkDestroyImageView = vkr_dispatch_vkDestroyImageView;
}

void
vkr_context_init_sampler_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateSampler = vkr_dispatch_vkCreateSampler;
   dispatch->dispatch_vkDestroySampler = vkr_dispatch_vkDestroySampler;
}

void
vkr_context_init_sampler_ycbcr_conversion_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateSamplerYcbcrConversion =
      vkr_dispatch_vkCreateSamplerYcbcrConversion;
   dispatch->dispatch_vkDestroySamplerYcbcrConversion =
      vkr_dispatch_vkDestroySamplerYcbcrConversion;
}
