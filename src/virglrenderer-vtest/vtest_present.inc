/* SPDX-License-Identifier: MIT
 * GPU-only tiled-to-linear presentation. Rendering never targets LINEAR.
 * A session owns one imported source and a release-tracked destination ring.
 * The caller waits the producer's acquire fence and never copies into a busy
 * Wayland buffer. Fence completion flushes the destination before handoff.
 */
#define PRESENT_SLOTS 3
struct present_image { VkImage image; VkDeviceMemory memory; };
struct vtest_present {
   struct present_image source, dest[PRESENT_SLOTS];
   VkCommandPool pool;
   VkCommandBuffer command;
   VkFence fence;
   uint32_t width, height;
   bool initialized[PRESENT_SLOTS], poisoned;
};
#define PRESENT_FN(name) PFN_vk##name name = (PFN_vk##name)alloc_vk.get_proc(alloc_vk.instance, "vk" #name)
static void present_image_free(struct present_image *im) {
   if (im->image) alloc_vk.DestroyImage(alloc_vk.device, im->image, NULL);
   if (im->memory) alloc_vk.FreeMemory(alloc_vk.device, im->memory, NULL);
   memset(im, 0, sizeof(*im));
}
/* The fd is consumed only by a successful Vulkan allocation. */
static int present_import(struct present_image *out, int fd, uint32_t w,
                          uint32_t h, uint32_t fmt, uint32_t stride,
                          uint32_t offset, uint64_t modifier, VkImageUsageFlags usage) {
   struct alloc_vk *vk = &alloc_vk;
   PRESENT_FN(GetMemoryFdPropertiesKHR);
   if (!GetMemoryFdPropertiesKHR) {close(fd);return -ENOTSUP;}
   VkSubresourceLayout plane = {.offset=offset, .rowPitch=stride};
   VkImageDrmFormatModifierExplicitCreateInfoEXT mod = {
      .sType=VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
      .drmFormatModifier=modifier, .drmFormatModifierPlaneCount=1,
      .pPlaneLayouts=&plane};
   VkExternalMemoryImageCreateInfo external = {
      .sType=VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, .pNext=&mod,
      .handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
   VkImageCreateInfo image = {.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .pNext=&external, .imageType=VK_IMAGE_TYPE_2D, .format=drm_format_to_vk(fmt),
      .extent={w,h,1}, .mipLevels=1, .arrayLayers=1, .samples=VK_SAMPLE_COUNT_1_BIT,
      .tiling=VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT, .usage=usage,
      .sharingMode=VK_SHARING_MODE_EXCLUSIVE, .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
   if (vk->CreateImage(vk->device,&image,NULL,&out->image)!=VK_SUCCESS) {close(fd);return -ENOTSUP;}
   VkMemoryRequirements req;
   vk->GetImageMemoryRequirements(vk->device,out->image,&req);
   VkMemoryFdPropertiesKHR props={.sType=VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
   if (GetMemoryFdPropertiesKHR(vk->device,VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,fd,&props)!=VK_SUCCESS) goto fail;
   uint32_t bits=props.memoryTypeBits & req.memoryTypeBits;
   if (!bits) goto fail;
   uint32_t type=0;while(!(bits&(1u<<type)))++type;
   off_t size=lseek(fd,0,SEEK_END);if(size<=0 || (uint64_t)size<req.size)goto fail;
   VkMemoryDedicatedAllocateInfo dedicated={.sType=VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,.image=out->image};
   VkImportMemoryFdInfoKHR import={.sType=VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
      .pNext=&dedicated,.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,.fd=fd};
   VkMemoryAllocateInfo alloc={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext=&import,.allocationSize=(uint64_t)size,.memoryTypeIndex=type};
   if(vk->AllocateMemory(vk->device,&alloc,NULL,&out->memory)!=VK_SUCCESS)goto fail;
   if(vk->BindImageMemory(vk->device,out->image,out->memory,0)!=VK_SUCCESS){present_image_free(out);return -EIO;}
   return 0;
fail:
   close(fd);present_image_free(out);return -ENOTSUP;
}
static void present_destroy_locked(struct vtest_present *p) {
   PRESENT_FN(DestroyCommandPool);PRESENT_FN(DestroyFence);PRESENT_FN(DeviceWaitIdle);
   if(p->poisoned) DeviceWaitIdle(alloc_vk.device);
   if(p->pool)DestroyCommandPool(alloc_vk.device,p->pool,NULL);
   if(p->fence)DestroyFence(alloc_vk.device,p->fence,NULL);
   present_image_free(&p->source);
   for(unsigned i=0;i<PRESENT_SLOTS;i++)present_image_free(&p->dest[i]);
   free(p);
}
void vtest_present_destroy(struct vtest_present *p) {
   if(!p)return;
   pthread_mutex_lock(&alloc_vk_mutex);present_destroy_locked(p);pthread_mutex_unlock(&alloc_vk_mutex);
}
int vtest_present_create(int source_fd,uint32_t width,uint32_t height,uint32_t fmt,
                        uint32_t stride,uint32_t offset,uint64_t modifier,
                        struct vtest_present **out,uint32_t *out_stride,int fds[3]) {
   *out=NULL;for(unsigned i=0;i<PRESENT_SLOTS;i++)fds[i]=-1;
   if(!width||!height||width>8192||height>8192||(uint64_t)width*height>16777216 ||
      (fmt!=VTEST_FORMAT_ABGR8888 && fmt!=VTEST_FORMAT_ARGB8888 &&
       fmt!=VTEST_FORMAT_XBGR8888 && fmt!=VTEST_FORMAT_XRGB8888) ||
      stride<width*4 || stride>131072)return -EINVAL;
   struct vtest_present *p=calloc(1,sizeof(*p));if(!p)return -ENOMEM;
   int ret=-EIO;
   for(unsigned i=0;i<PRESENT_SLOTS;i++) {
      uint64_t mod,size;const char *place;
      ret=vtest_gpu_alloc_image(width,height,fmt,true,false,out_stride,&mod,&size,&fds[i],&place);
      if(ret || mod!=0 || strcmp(place,"sysmem"))goto early_fail;
   }
   pthread_mutex_lock(&alloc_vk_mutex);
   int duplicate=fcntl(source_fd,F_DUPFD_CLOEXEC,0);
   if(duplicate<0){ret=-errno;goto fail;}
   ret=present_import(&p->source,duplicate,width,height,fmt,stride,offset,modifier,VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
   if(ret)goto fail;
   for(unsigned i=0;i<PRESENT_SLOTS;i++) {
      duplicate=fcntl(fds[i],F_DUPFD_CLOEXEC,0);
      if(duplicate<0){ret=-errno;goto fail;}
      ret=present_import(&p->dest[i],duplicate,width,height,fmt,*out_stride,0,0,VK_IMAGE_USAGE_TRANSFER_DST_BIT);
      if(ret)goto fail;
   }
   PRESENT_FN(CreateCommandPool);PRESENT_FN(AllocateCommandBuffers);PRESENT_FN(CreateFence);
   VkCommandPoolCreateInfo pool={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=0};
   if(CreateCommandPool(alloc_vk.device,&pool,NULL,&p->pool)!=VK_SUCCESS){ret=-EIO;goto fail;}
   VkCommandBufferAllocateInfo cmd={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool=p->pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
   if(AllocateCommandBuffers(alloc_vk.device,&cmd,&p->command)!=VK_SUCCESS){ret=-EIO;goto fail;}
   VkFenceCreateInfo fence={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   if(CreateFence(alloc_vk.device,&fence,NULL,&p->fence)!=VK_SUCCESS){ret=-EIO;goto fail;}
   p->width=width;p->height=height;*out=p;
   pthread_mutex_unlock(&alloc_vk_mutex);return 0;
fail:
   present_destroy_locked(p);pthread_mutex_unlock(&alloc_vk_mutex);p=NULL;
early_fail:
   free(p);for(unsigned i=0;i<PRESENT_SLOTS;i++){if(fds[i]>=0)close(fds[i]);fds[i]=-1;}
   return ret?ret:-ENOTSUP;
}
int vtest_present_copy(struct vtest_present *p,unsigned slot) {
   if(!p||slot>=PRESENT_SLOTS||p->poisoned)return -EINVAL;
   pthread_mutex_lock(&alloc_vk_mutex);
   PRESENT_FN(ResetCommandBuffer);PRESENT_FN(BeginCommandBuffer);PRESENT_FN(CmdPipelineBarrier);
   PRESENT_FN(CmdCopyImage);PRESENT_FN(EndCommandBuffer);PRESENT_FN(ResetFences);
   PRESENT_FN(GetDeviceQueue);PRESENT_FN(QueueSubmit);PRESENT_FN(WaitForFences);
   int ret=-EIO;
   if(ResetCommandBuffer(p->command,0)!=VK_SUCCESS)goto fail;
   VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
   if(BeginCommandBuffer(p->command,&begin)!=VK_SUCCESS)goto fail;
   VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
   VkImageMemoryBarrier barriers[2]={
      {.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.srcAccessMask=0,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT,
       .oldLayout=VK_IMAGE_LAYOUT_GENERAL,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
       .srcQueueFamilyIndex=VK_QUEUE_FAMILY_EXTERNAL,.dstQueueFamilyIndex=0,.image=p->source.image,.subresourceRange=range},
      {.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.srcAccessMask=0,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
       .oldLayout=p->initialized[slot]?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_UNDEFINED,
       .newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
       .srcQueueFamilyIndex=p->initialized[slot]?VK_QUEUE_FAMILY_EXTERNAL:VK_QUEUE_FAMILY_IGNORED,
       .dstQueueFamilyIndex=p->initialized[slot]?0:VK_QUEUE_FAMILY_IGNORED,
       .image=p->dest[slot].image,.subresourceRange=range}};
   CmdPipelineBarrier(p->command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,2,barriers);
   VkImageCopy copy={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
      .dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.extent={p->width,p->height,1}};
   CmdCopyImage(p->command,p->source.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                p->dest[slot].image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
   barriers[0].srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
   barriers[1].srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
   for(unsigned i=0;i<2;i++) {
      barriers[i].dstAccessMask=0;barriers[i].oldLayout=barriers[i].newLayout;
      barriers[i].newLayout=VK_IMAGE_LAYOUT_GENERAL;
      barriers[i].srcQueueFamilyIndex=0;barriers[i].dstQueueFamilyIndex=VK_QUEUE_FAMILY_EXTERNAL;
   }
   CmdPipelineBarrier(p->command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,NULL,0,NULL,2,barriers);
   if(EndCommandBuffer(p->command)!=VK_SUCCESS)goto fail;
   if(ResetFences(alloc_vk.device,1,&p->fence)!=VK_SUCCESS)goto fail;
   VkQueue queue;GetDeviceQueue(alloc_vk.device,0,0,&queue);
   VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&p->command};
   VkResult status=QueueSubmit(queue,1,&submit,p->fence);
   if(status==VK_SUCCESS)status=WaitForFences(alloc_vk.device,1,&p->fence,VK_TRUE,1000000000);
   if(status!=VK_SUCCESS) {
      fprintf(stderr,"vtest_present: GPU copy failed: %s (vk=%d)\n",
              status==VK_ERROR_DEVICE_LOST?"VK_ERROR_DEVICE_LOST":"queue/fence error",status);
      p->poisoned=true;goto fail;
   }
   p->initialized[slot]=true;ret=0;
fail:
   pthread_mutex_unlock(&alloc_vk_mutex);return ret;
}
#undef PRESENT_FN
