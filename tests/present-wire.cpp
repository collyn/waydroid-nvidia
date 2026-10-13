/* SPDX-License-Identifier: MIT */
#include "../src/hwc_present_io.h"
#include "../src/hwc_present_policy.h"
#include <cassert>
#include <dirent.h>
#include <fcntl.h>
#include <cstdio>
#include <initializer_list>
static unsigned fd_count() {
 DIR *d=opendir("/proc/self/fd");assert(d);unsigned n=0;
 while(auto p=readdir(d))if(p->d_name[0]!='.')n++;
 closedir(d);return n;
}
static void malformed(int socket,int fd,unsigned count) {
 char byte=0,control[CMSG_SPACE(12*sizeof(int))]={};iovec io={&byte,1};msghdr msg={};
 msg.msg_iov=&io;msg.msg_iovlen=1;
 if(count) {
  msg.msg_control=control;msg.msg_controllen=CMSG_SPACE(count*sizeof(int));
  auto c=CMSG_FIRSTHDR(&msg);c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(count*sizeof(int));
  for(unsigned i=0;i<count;i++)memcpy(CMSG_DATA(c)+i*sizeof(int),&fd,sizeof(fd));
 }
 assert(sendmsg(socket,&msg,MSG_NOSIGNAL)==1);
}
int main() {
 int pair[2];assert(!socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,pair));
 int original=open("/dev/null",O_RDONLY|O_CLOEXEC);assert(original>=0);
 unsigned baseline=fd_count();
 assert(send_fd(pair[0],original));int received=recv_fd(pair[1]);assert(received>=0);
 assert(fcntl(received,F_GETFD)&FD_CLOEXEC);close(received);assert(fd_count()==baseline);
 for(unsigned count: {0u,2u,10u}) {
  malformed(pair[0],original,count);assert(recv_fd(pair[1])<0);assert(fd_count()==baseline);
 }
 const char sent[]="ordered reply";char reply[sizeof(sent)];
 assert(send_all(pair[0],sent,sizeof(sent)));assert(read_all(pair[1],reply,sizeof(reply)));assert(!memcmp(sent,reply,sizeof(sent)));
 close(pair[1]);assert(!send_all(pair[0],sent,sizeof(sent)));assert(!read_all(pair[0],reply,sizeof(reply)));
 close(pair[0]);close(original);
 for(unsigned mask=0;mask<8;mask++)for(unsigned next=0;next<3;next++) {
  unsigned selected=hwc_present_free_slot(mask,next);
  if(mask==7)assert(selected==3);
  else{assert(selected<3);assert(!(mask&(1u<<selected)));}
 }
 assert(hwc_present_free_slot(0,3)==3);
 assert(hwc_present_free_slot(6,2)==0); // Wrap past two busy compositor buffers.
 puts("PASS: release-ring admission, fd ownership, truncation, disconnect");
}
