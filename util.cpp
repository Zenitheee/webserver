#include "util.h"

void addsig(int sig, void(handler)(int))
{
  struct sigaction sa;
  memset(&sa, '\0', sizeof(sa));
  sa.sa_handler = handler;
  sigfillset(&sa.sa_mask);
  sigaction(sig, &sa, NULL);
}

void setnonblocking(int fd)
{
  int old_flag = fcntl(fd, F_GETFL);
  int new_flag = old_flag | O_NONBLOCK;
  fcntl(fd, F_SETFL, new_flag);
}

void addfd(int epollfd, int fd, bool one_shot)
{
  epoll_event event;
  event.data.fd = fd;
  event.events = EPOLLIN | EPOLLRDHUP;

  if (one_shot)
  {
    // 防止同一个通信被不同的线程处理
    event.events |= EPOLLONESHOT;
  }
  epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
  // 设置文件描述符非阻塞
  setnonblocking(fd);
}

void modfd(int epollfd, int fd, int ev)
{
  epoll_event event;
  event.data.fd = fd;
  event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
  epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

void removefd(int epollfd, int fd)
{
  epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
  close(fd);
}