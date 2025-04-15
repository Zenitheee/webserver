#ifndef UTIL_H
#define UTIL_H

#include <sys/epoll.h> // 用于epoll操作
#include <fcntl.h>     // 用于文件描述符操作
#include <signal.h>    // 用于信号处理
#include <unistd.h>    // 用于系统调用
#include <string.h>
// 添加信号捕捉
void addsig(int sig, void(handler)(int));

// 设置文件描述符非阻塞
void setnonblocking(int fd);

// 向epoll中添加需要监听的文件描述符
void addfd(int epollfd, int fd, bool one_shot);

// 修改文件描述符,重置socket上EPOLLONESHOT事件，以确保下次可读时EPOLLIN事件能被触发
void modfd(int epollfd, int fd, int ev);

// 从epoll中移除监听的文件描述符
void removefd(int epollfd, int fd);
#endif