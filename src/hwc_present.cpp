/* SPDX-License-Identifier: MIT
 * Only presentation copies use LINEAR; all producer rendering stays tiled.
 * The release queue is dispatched solely on the HWC thread, so callbacks and
 * destruction never race. Older hosts fall back without protocol changes.
 */
#include "hwc_present.h"
#include "gralloc_handler.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include <cros_gralloc/cros_gralloc_handle.h>
#include <cutils/properties.h>
#include <log/log.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
struct present_slot {wl_buffer *buffer=nullptr;bool busy=false;};
struct hwc_present {
 int socket=-1;wl_display *display=nullptr;wl_event_queue *queue=nullptr;
 present_slot slots[3];unsigned current=0,next=0;
};
#include "hwc_present_io.h"
#include "hwc_present_policy.h"
static int connect_copy(void) {
 char path[PROPERTY_VALUE_MAX];property_get("mesa.vtest.socket.name",path,"/dev/venus/venus.sock");
 int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);if(fd<0)return -1;
 sockaddr_un addr={};addr.sun_family=AF_UNIX;
 if(strlen(path)>=sizeof(addr.sun_path)){close(fd);return -1;}strcpy(addr.sun_path,path);
 int result=connect(fd,(sockaddr*)&addr,sizeof(addr));
 if(result<0&&(errno==EINPROGRESS||errno==EWOULDBLOCK)) {
  pollfd poller={fd,POLLOUT,0};result=poll(&poller,1,1000);
  int error=0;socklen_t len=sizeof(error);
  if(result>0 && !getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&len) && !error)result=0;else result=-1;
 }
 if(result<0){close(fd);return -1;}
 fcntl(fd,F_SETFL,fcntl(fd,F_GETFL,0)&~O_NONBLOCK);
 timeval timeout={1,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
 const char name[]="hwc-present";uint32_t header[]={sizeof(name),8};
 if(!send_all(fd,header,sizeof(header))||!send_all(fd,name,sizeof(name))){close(fd);return -1;}
 return fd;
}
static void released(void *data,wl_buffer *){((present_slot*)data)->busy=false;}
static const wl_buffer_listener release_listener={released};
void destroy_present_buffer(hwc_present *p) {
 if(!p)return;
 for(auto &s:p->slots)if(s.buffer)wl_buffer_destroy(s.buffer);
 if(p->queue)wl_event_queue_destroy(p->queue);
 if(p->socket>=0)close(p->socket);
 delete p;
}
std::unique_ptr<buffer> create_present_wl_buffer(display *d,const buffer_metadata &metadata,buffer_handle_t handle) {
 if(d->gtype!=GrallocType::GRALLOC_CROS||!d->dmabuf)return nullptr;
 // NVIDIA compositors cannot bind LINEAR as sampler2D even if advertised.
 for(const auto &format:d->modifiers)
  for(uint64_t modifier:format.second)
   if((modifier>>56)==3)return nullptr;
 auto source=(const cros_gralloc_handle*)handle;
 if((source->format_modifier>>56)!=3 || source->num_planes!=1)return nullptr;
 uint32_t present_format=source->format;
 if(metadata.opaque) {
  if(present_format==0x34324241u)present_format=0x34324258u; // ABGR -> XBGR
  if(present_format==0x34325241u)present_format=0x34325258u; // ARGB -> XRGB
 }
 auto formats=d->modifiers.find(present_format);
 if(formats==d->modifiers.end()||std::find(formats->second.begin(),formats->second.end(),0)==formats->second.end())return nullptr;
 hwc_present *p=new hwc_present();p->display=d->display;p->socket=connect_copy();
 int fds[3]={-1,-1,-1};bool ok=false;
 do {
  if(p->socket<0)break;
  uint32_t request[]={7,46,metadata.width,metadata.height,source->format,source->strides[0],source->offsets[0],
     (uint32_t)source->format_modifier,(uint32_t)(source->format_modifier>>32)},reply[4];
  if(!send_all(p->socket,request,sizeof(request))||!send_fd(p->socket,source->fds[0])||!read_all(p->socket,reply,sizeof(reply)))break;
  if(reply[0]!=2||reply[1]!=46||reply[2]||reply[3]<metadata.width*4||reply[3]>131072||(reply[3]&255))break;
  for(unsigned i=0;i<3;i++){fds[i]=recv_fd(p->socket);if(fds[i]<0)break;}
  if(fds[0]<0||fds[1]<0||fds[2]<0)break;
  p->queue=wl_display_create_queue(d->display);if(!p->queue)break;
  for(unsigned i=0;i<3;i++) {
   auto params=zwp_linux_dmabuf_v1_create_params(d->dmabuf);
   wl_proxy_set_queue((wl_proxy*)params,p->queue);
   zwp_linux_buffer_params_v1_add(params,fds[i],0,0,reply[3],0,0);
   p->slots[i].buffer=zwp_linux_buffer_params_v1_create_immed(params,metadata.width,metadata.height,present_format,0);
   zwp_linux_buffer_params_v1_destroy(params);
   if(!p->slots[i].buffer)break;
   wl_buffer_add_listener(p->slots[i].buffer,&release_listener,&p->slots[i]);
  }
  ok=p->slots[0].buffer&&p->slots[1].buffer&&p->slots[2].buffer;
 }while(false);
 for(int fd:fds)if(fd>=0)close(fd);
 if(!ok){destroy_present_buffer(p);return nullptr;}
 std::unique_ptr<buffer> b(new buffer());b->metadata=metadata;b->handle=handle;b->present=p;b->wl_buffer=p->slots[0].buffer;
 ALOGI("GPU copy presentation: %ux%u, tiled source -> LINEAR destination ring",metadata.width,metadata.height);
 return b;
}
int update_present_buffer(buffer *b) {
 auto p=b->present;
 if(wl_display_dispatch_queue_pending(p->display,p->queue)<0)return -1;
 unsigned mask=0;
 for(unsigned i=0;i<3;i++)if(p->slots[i].busy)mask|=1u<<i;
 unsigned slot=hwc_present_free_slot(mask,p->next);
 if(slot==3)return 1; // Drop this presentation, never overwrite compositor data.
 uint32_t request[]={1,47,slot},reply[3];
 if(!send_all(p->socket,request,sizeof(request))||!read_all(p->socket,reply,sizeof(reply))||reply[0]!=1||reply[1]!=47||reply[2])return -1;
 p->current=slot;p->next=(slot+1)%3;b->wl_buffer=p->slots[slot].buffer;return 0;
}
void present_buffer_attached(buffer *b) {
 if(b->present)b->present->slots[b->present->current].busy=true;
}
