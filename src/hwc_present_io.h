/* SPDX-License-Identifier: MIT */
#pragma once
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>
static bool send_all(int fd,const void *data,size_t n) {
 const char *p=(const char*)data;
 while(n){ssize_t r=send(fd,p,n,MSG_NOSIGNAL);if(r<0&&errno==EINTR)continue;if(r<=0)return false;p+=r;n-=r;}
 return true;
}
static bool read_all(int fd,void *data,size_t n) {
 char *p=(char*)data;
 while(n){ssize_t r=recv(fd,p,n,0);if(r<0&&errno==EINTR)continue;if(r<=0)return false;p+=r;n-=r;}
 return true;
}
static bool send_fd(int socket,int fd) {
 char byte=0,control[CMSG_SPACE(sizeof(int))]={};iovec io={&byte,1};msghdr msg={};
 msg.msg_iov=&io;msg.msg_iovlen=1;msg.msg_control=control;msg.msg_controllen=sizeof(control);
 auto c=CMSG_FIRSTHDR(&msg);c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(sizeof(int));
 memcpy(CMSG_DATA(c),&fd,sizeof(fd));ssize_t r;
 do{r=sendmsg(socket,&msg,MSG_NOSIGNAL);}while(r<0&&errno==EINTR);
 return r==1;
}
static int recv_fd(int socket) {
 char byte,control[CMSG_SPACE(8*sizeof(int))]={};iovec io={&byte,1};msghdr msg={};
 msg.msg_iov=&io;msg.msg_iovlen=1;msg.msg_control=control;msg.msg_controllen=sizeof(control);
 ssize_t r;do{r=recvmsg(socket,&msg,MSG_CMSG_CLOEXEC);}while(r<0&&errno==EINTR);
 if(r!=1)return -1;
 int result=-1,count=0;
 for(auto c=CMSG_FIRSTHDR(&msg);c;c=CMSG_NXTHDR(&msg,c)) {
  if(c->cmsg_level!=SOL_SOCKET||c->cmsg_type!=SCM_RIGHTS)continue;
  for(size_t i=0;i+sizeof(int)<=c->cmsg_len-CMSG_LEN(0);i+=sizeof(int)) {
   int fd;memcpy(&fd,CMSG_DATA(c)+i,sizeof(fd));if(count++)close(fd);else result=fd;
  }
 }
 if(count!=1 || (msg.msg_flags&MSG_CTRUNC)){if(result>=0)close(result);return -1;}
 return result;
}
