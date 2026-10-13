/* SPDX-License-Identifier: MIT
 * Actual NVIDIA tiled -> linear Vulkan transfer and other-GPU EGL readback.
 * No Android, no fixed DRM node path, no window system. */
#define _GNU_SOURCE
#define EGL_EGLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>
#include <glob.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <assert.h>
#include "../src/virglrenderer-vtest/vtest_gpu_alloc.c"
static void fill(struct present_image *source,unsigned index,uint32_t w,uint32_t h) {
#define FN(n) PFN_vk##n n=(PFN_vk##n)alloc_vk.get_proc(alloc_vk.instance,"vk" #n)
 FN(CreateCommandPool);FN(AllocateCommandBuffers);FN(BeginCommandBuffer);
 FN(CmdPipelineBarrier);FN(CmdClearColorImage);FN(EndCommandBuffer);
 FN(GetDeviceQueue);FN(QueueSubmit);FN(QueueWaitIdle);FN(DestroyCommandPool);
 VkCommandPool pool;VkCommandPoolCreateInfo pi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=0};
 assert(CreateCommandPool(alloc_vk.device,&pi,NULL,&pool)==VK_SUCCESS);
 VkCommandBuffer cmd;VkCommandBufferAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
 assert(AllocateCommandBuffers(alloc_vk.device,&ai,&cmd)==VK_SUCCESS);
 VkCommandBufferBeginInfo bi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
 assert(BeginCommandBuffer(cmd,&bi)==VK_SUCCESS);
 VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
 VkImageMemoryBarrier bar={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
 .oldLayout=index?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_UNDEFINED,
 .newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
 .srcQueueFamilyIndex=index?VK_QUEUE_FAMILY_EXTERNAL:VK_QUEUE_FAMILY_IGNORED,
 .dstQueueFamilyIndex=index?0:VK_QUEUE_FAMILY_IGNORED,.image=source->image,.subresourceRange=range};
 CmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&bar);
 VkClearColorValue color={.float32={(index&1)?1.f:0.f,(index&2)?1.f:0.f,(index&4)?1.f:0.f,1.f}};
 CmdClearColorImage(cmd,source->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&color,1,&range);
 bar.oldLayout=bar.newLayout;bar.newLayout=VK_IMAGE_LAYOUT_GENERAL;bar.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;bar.dstAccessMask=0;
 bar.srcQueueFamilyIndex=0;bar.dstQueueFamilyIndex=VK_QUEUE_FAMILY_EXTERNAL;
 CmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,NULL,0,NULL,1,&bar);
 assert(EndCommandBuffer(cmd)==VK_SUCCESS);VkQueue queue;GetDeviceQueue(alloc_vk.device,0,0,&queue);
 VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cmd};
 assert(QueueSubmit(queue,1,&submit,VK_NULL_HANDLE)==VK_SUCCESS);assert(QueueWaitIdle(queue)==VK_SUCCESS);
 DestroyCommandPool(alloc_vk.device,pool,NULL);(void)w;(void)h;
#undef FN
}
static int target_fd=-1;static struct gbm_device *gbm;static EGLDisplay dpy;
static void target_init(void) {
 glob_t paths={0};assert(!glob("/sys/class/drm/renderD*/device/vendor",0,NULL,&paths));
 for(size_t i=0;i<paths.gl_pathc;i++) {
  FILE *f=fopen(paths.gl_pathv[i],"r");unsigned vendor=0;if(f){if(fscanf(f,"%x",&vendor)!=1)vendor=0;fclose(f);}
  if(!vendor||vendor==0x10de)continue;
  const char *node=strstr(paths.gl_pathv[i],"renderD");char name[64];size_t n=strcspn(node,"/");
  snprintf(name,sizeof(name),"/dev/dri/%.*s",(int)n,node);target_fd=open(name,O_RDWR|O_CLOEXEC);if(target_fd>=0)break;
 }
 globfree(&paths);assert(target_fd>=0);gbm=gbm_create_device(target_fd);assert(gbm);
 PFNEGLGETPLATFORMDISPLAYEXTPROC get=(void*)eglGetProcAddress("eglGetPlatformDisplayEXT");
 dpy=get(EGL_PLATFORM_GBM_KHR,gbm,NULL);assert(eglInitialize(dpy,NULL,NULL));assert(eglBindAPI(EGL_OPENGL_ES_API));
 EGLint attrs[]={EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_SURFACE_TYPE,0,EGL_NONE};
 EGLConfig config;EGLint count;assert(eglChooseConfig(dpy,attrs,&config,1,&count)&&count);
 EGLint ctxattrs[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};EGLContext ctx=eglCreateContext(dpy,config,EGL_NO_CONTEXT,ctxattrs);assert(ctx!=EGL_NO_CONTEXT);
 assert(eglMakeCurrent(dpy,EGL_NO_SURFACE,EGL_NO_SURFACE,ctx));printf("compositor probe: %s\n",glGetString(GL_RENDERER));
}
static void verify(int fd,uint32_t w,uint32_t h,uint32_t stride,uint32_t fmt,unsigned value) {
 PFNEGLCREATEIMAGEKHRPROC create=(void*)eglGetProcAddress("eglCreateImageKHR");
 PFNEGLDESTROYIMAGEKHRPROC destroy=(void*)eglGetProcAddress("eglDestroyImageKHR");
 PFNGLEGLIMAGETARGETTEXTURE2DOESPROC bind=(void*)eglGetProcAddress("glEGLImageTargetTexture2DOES");
 EGLint a[]={EGL_WIDTH,w,EGL_HEIGHT,h,EGL_LINUX_DRM_FOURCC_EXT,fmt,
 EGL_DMA_BUF_PLANE0_FD_EXT,fd,EGL_DMA_BUF_PLANE0_OFFSET_EXT,0,EGL_DMA_BUF_PLANE0_PITCH_EXT,stride,
 EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,0,EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,0,EGL_NONE};
 EGLImageKHR im=create(dpy,EGL_NO_CONTEXT,EGL_LINUX_DMA_BUF_EXT,NULL,a);assert(im!=EGL_NO_IMAGE_KHR);
 GLuint tex,fbo;glGenTextures(1,&tex);glBindTexture(GL_TEXTURE_2D,tex);bind(GL_TEXTURE_2D,im);assert(!glGetError());
 glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
 glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
 GLuint color;glGenTextures(1,&color);glBindTexture(GL_TEXTURE_2D,color);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
 glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,color,0);
 assert(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE);
 const char *vs="attribute vec2 pos; varying vec2 uv;void main(){gl_Position=vec4(pos,0.,1.);uv=pos*.5+.5;}";
 const char *fs="precision mediump float;varying vec2 uv;uniform sampler2D image;void main(){gl_FragColor=texture2D(image,uv);}";
 GLuint vertex=glCreateShader(GL_VERTEX_SHADER),fragment=glCreateShader(GL_FRAGMENT_SHADER),program=glCreateProgram();
 glShaderSource(vertex,1,&vs,NULL);glCompileShader(vertex);glShaderSource(fragment,1,&fs,NULL);glCompileShader(fragment);
 glAttachShader(program,vertex);glAttachShader(program,fragment);glBindAttribLocation(program,0,"pos");glLinkProgram(program);
 GLint ok;glGetProgramiv(program,GL_LINK_STATUS,&ok);assert(ok);glUseProgram(program);
 glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,tex);glUniform1i(glGetUniformLocation(program,"image"),0);
 float points[]={-1,-1,1,-1,-1,1,1,1};glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,points);glEnableVertexAttribArray(0);
 glViewport(0,0,w,h);glDrawArrays(GL_TRIANGLE_STRIP,0,4);assert(!glGetError());
 for(unsigned i=0;i<5;i++) {
  unsigned char pixel[4];int x=i==4?w/2:(i&1?w-1:0),y=i==4?h/2:(i&2?h-1:0);
  glReadPixels(x,y,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);assert(!glGetError());
  for(unsigned j=0;j<3;j++)if(pixel[j]!=((value&(1u<<j))?255:0)) {
    fprintf(stderr,"mismatch %u,%u channel %u got %u expected %u\n",x,y,j,pixel[j],(value&(1u<<j))?255:0);abort();
  }
  assert(pixel[3]==255);
 }
 glDeleteProgram(program);glDeleteShader(vertex);glDeleteShader(fragment);glDeleteTextures(1,&color);glDeleteFramebuffers(1,&fbo);glDeleteTextures(1,&tex);destroy(dpy,im);
}

static int wire_socket=-1;
static int wire_io(void *data,size_t size,bool write_data) {
 char *p=data;while(size){ssize_t n=write_data?send(wire_socket,p,size,MSG_NOSIGNAL):recv(wire_socket,p,size,0);if(n<0&&errno==EINTR)continue;if(n<=0)return -EIO;p+=n;size-=n;}return 0;
}
static int wire_fd(int fd,bool send_fd) {
 char byte=0,control[CMSG_SPACE(sizeof(int))]={0};struct iovec io={&byte,1};struct msghdr msg={0};
 msg.msg_iov=&io;msg.msg_iovlen=1;msg.msg_control=control;msg.msg_controllen=sizeof(control);
 if(send_fd){struct cmsghdr *c=CMSG_FIRSTHDR(&msg);c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(sizeof(int));memcpy(CMSG_DATA(c),&fd,sizeof(fd));}
 ssize_t n=send_fd?sendmsg(wire_socket,&msg,MSG_NOSIGNAL):recvmsg(wire_socket,&msg,MSG_CMSG_CLOEXEC);assert(n==1);
 if(send_fd)return 0;
 struct cmsghdr *c=CMSG_FIRSTHDR(&msg);assert(c&&c->cmsg_type==SCM_RIGHTS);memcpy(&fd,CMSG_DATA(c),sizeof(fd));return fd;
}
static int test_present_create(int fd,uint32_t w,uint32_t h,uint32_t fmt,uint32_t stride,uint32_t offset,uint64_t mod,struct vtest_present **p,uint32_t *outstride,int out[3]) {
 const char *path=getenv("PRESENT_TEST_SOCKET");if(!path)return vtest_present_create(fd,w,h,fmt,stride,offset,mod,p,outstride,out);
 wire_socket=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);assert(wire_socket>=0);
 struct sockaddr_un addr={.sun_family=AF_UNIX};assert(strlen(path)<sizeof(addr.sun_path));strcpy(addr.sun_path,path);assert(!connect(wire_socket,(struct sockaddr*)&addr,sizeof(addr)));
 char name[]="present-probe";uint32_t header[]={sizeof(name),8};assert(!wire_io(header,sizeof(header),true));assert(!wire_io(name,sizeof(name),true));
 uint32_t request[]={7,46,w,h,fmt,stride,offset,(uint32_t)mod,(uint32_t)(mod>>32)},reply[4];
 assert(!wire_io(request,sizeof(request),true));wire_fd(fd,true);assert(!wire_io(reply,sizeof(reply),false));assert(reply[0]==2&&reply[1]==46);
 if(reply[2])return -(int)reply[2];
 for(unsigned i=0;i<3;i++)out[i]=wire_fd(-1,false);
 *outstride=reply[3];*p=NULL;return 0;
}
static int test_present_copy(struct vtest_present *p,unsigned slot) {
 if(!getenv("PRESENT_TEST_SOCKET"))return vtest_present_copy(p,slot);
 uint32_t request[]={1,47,slot},reply[3];assert(!wire_io(request,sizeof(request),true));assert(!wire_io(reply,sizeof(reply),false));assert(reply[0]==1&&reply[1]==47);return -(int)reply[2];
}
static void test_present_destroy(struct vtest_present *p) {
 if(!getenv("PRESENT_TEST_SOCKET"))vtest_present_destroy(p);else{close(wire_socket);wire_socket=-1;}
}
int main(void) {
 setenv("VTEST_ALLOC_GPU","NVIDIA",1);setenv("WAYDROID_NVIDIA_PRESENT","block-linear",1);
 target_init();const uint32_t widths[]={256,1984,1088},heights[]={256,1164,1920};
 for(unsigned k=0;k<3;k++) {
  uint32_t w=widths[k],h=heights[k],stride,outstride;uint64_t mod,size;int fd;
  assert(!vtest_gpu_alloc_gpu(w,h,VTEST_FORMAT_ABGR8888,&stride,&mod,&size,&fd));assert(mod);
  struct present_image source={0};assert(!present_import(&source,dup(fd),w,h,VTEST_FORMAT_ABGR8888,stride,0,mod,VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT));
  struct vtest_present *p;int dest[3];assert(!test_present_create(fd,w,h,VTEST_FORMAT_ABGR8888,stride,0,mod,&p,&outstride,dest));close(fd);
  double total=0;
  for(unsigned i=0;i<24;i++) {
   fill(&source,i,w,h);struct timespec begin,end;clock_gettime(CLOCK_MONOTONIC,&begin);
   assert(!test_present_copy(p,i%3));clock_gettime(CLOCK_MONOTONIC,&end);
   total+=(end.tv_sec-begin.tv_sec)*1000.+(end.tv_nsec-begin.tv_nsec)/1e6;
   verify(dest[i%3],w,h,outstride,VTEST_FORMAT_ABGR8888,i);
  }
  printf("PASS %ux%u: 24 changing frames, 3 slots, NVIDIA->other GPU, copy mean %.3f ms\n",w,h,total/24.);
  assert(test_present_copy(p,3)==-EINVAL);
  test_present_destroy(p);present_image_free(&source);for(unsigned i=0;i<3;i++)close(dest[i]);
 }
 puts("PASS: GPU presentation pixel/lifetime test");return 0;
}
