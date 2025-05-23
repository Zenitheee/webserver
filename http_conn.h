#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H

#include <sys/epoll.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/uio.h>
#include <string.h>
#include <string>
#include <memory>
#include <map>
#include <regex>
#include <dirent.h>
#include "locker.h"

class http_conn
{
public:
  // 使用std::string后不再需要固定长度的文件名
  // static const int FILENAME_LEN = 200; // 文件名的最大长度

  static const int READ_BUFFER_SIZE = 2048;  // 读缓冲区的大小
  static const int WRITE_BUFFER_SIZE = 1024; // 写缓冲区的大小

  // 上传文件相关常量
  static const std::string UPLOAD_DIR;               // 上传文件的目录路径
  static const int MAX_FILE_SIZE = 10 * 1024 * 1024; // 最大文件大小限制(10MB)

  static int m_epollfd;    // 所有socket上的事件都被注册到同一个epoll内核事件中，所以设置成静态的
  static int m_user_count; // 统计用户的数量

  // HTTP请求方法
  enum METHOD
  {
    GET = 0,
    POST = 2,
    HEAD,
    PUT,
    DELETE,
    TRACE,
    OPTIONS,
    CONNECT
  };

  /*
      解析客户端请求时，主状态机的状态
      CHECK_STATE_REQUESTLINE:当前正在分析请求行
      CHECK_STATE_HEADER:当前正在分析头部字段
      CHECK_STATE_CONTENT:当前正在解析请求体
  */
  enum CHECK_STATE
  {
    CHECK_STATE_REQUESTLINE = 0,
    CHECK_STATE_HEADER,
    CHECK_STATE_CONTENT
  };

  /*
      服务器处理HTTP请求的可能结果，报文解析的结果
      NO_REQUEST          :   请求不完整，需要继续读取客户数据
      GET_REQUEST         :   表示获得了一个完成的客户请求
      BAD_REQUEST         :   表示客户请求语法错误
      NO_RESOURCE         :   表示服务器没有资源
      FORBIDDEN_REQUEST   :   表示客户对资源没有足够的访问权限
      FILE_REQUEST        :   文件请求,获取文件成功
      INTERNAL_ERROR      :   表示服务器内部错误
      CLOSED_CONNECTION   :   表示客户端已经关闭连接了
  */
  enum HTTP_CODE
  {
    NO_REQUEST,
    GET_REQUEST,
    BAD_REQUEST,
    NO_RESOURCE,
    FORBIDDEN_REQUEST,
    FILE_REQUEST,
    INTERNAL_ERROR,
    CLOSED_CONNECTION
  };

  http_conn() {}
  ~http_conn()
  {
    close_conn();
  }

  void init(int sockfd, const sockaddr_in &addr); // 初始化新接收的连接
  void close_conn();                              // 关闭连接
  bool read();                                    // 非阻塞的读
  bool write();                                   // 非阻塞的写
  void process();                                 // 处理客户端请求

private:
  int m_sockfd;                      // 该http连接的socket
  sockaddr_in m_address;             // 通信的socket地址
  char m_read_buf[READ_BUFFER_SIZE]; // 读缓冲区
  int m_read_idx;                    // 标识读缓冲区中已经读入的客户端数据的最后一个字节的下一个位置

  int m_checked_idx;         // 当前正在分析的字符在读缓冲区的位置
  int m_start_line;          // 当前正在解析的行的起始位置
  CHECK_STATE m_check_state; // 主状态机当前所处的状态
  METHOD m_method;           // 请求方法

  std::string m_real_file; // 客户请求的目标文件的完整路径，其内容等于 doc_root + m_url, doc_root是网站根目录
  std::string m_url;       // 请求目标文件的文件名
  std::string m_version;   // 协议版本，只支持http1.1
  std::string m_host;      // 主机名
  int m_content_length;    // HTTP请求的消息总长度
  bool m_linger;           // 判断HTTP请求是否要保持连接

  // 文件上传相关成员
  std::string m_content_type;     // Content-Type头部的值
  std::string m_boundary;         // 多部分表单数据的分界线
  std::string m_upload_file_name; // 上传的文件名
  bool m_is_upload_request;       // 是否是上传文件的请求

  char m_write_buf[WRITE_BUFFER_SIZE]; // 写缓冲区
  int m_write_idx;                     // 写缓冲区中待发送的字节数

  // 使用智能指针替代裸指针，通过自定义删除器确保正确调用munmap
  std::shared_ptr<char> m_file_address;
  struct stat m_file_stat; // 目标文件的状态。通过它我们可以判断文件是否存在、是否为目录、是否可读，并获取文件大小等信息
  struct iovec m_iv[2];    // 我们将采用writev来执行写操作，所以定义下面两个成员，其中m_iv_count表示被写内存块的数量。
  int m_iv_count;

  int bytes_to_send;   // 将要发送的数据的字节数
  int bytes_have_send; // 已经发送的字节数

  void init();                                              // 初始化连接其余的信息
  HTTP_CODE process_read();                                 // 解析HTTP请求
  bool process_write(HTTP_CODE ret);                        // 填充HTTP应答
                                                            // 下面这一组函数被process_read调用以分析HTTP请求
  HTTP_CODE parse_request_line(const std::string &request); // 解析请求首行
  HTTP_CODE parse_headers(const std::string &request);      // 解析请求头
  HTTP_CODE parse_content(const std::string &request);      // 解析请求体
  char *get_line() { return m_read_buf + m_start_line; }
  HTTP_CODE do_request();

  // 文件上传相关函数
  HTTP_CODE handle_file_upload(const std::string &request_body);
  std::map<std::string, std::string> parse_multipart_form_data(const std::string &request_body);
  bool save_uploaded_file(const std::string &file_content, const std::string &file_name);
  std::string generate_file_list_html();

  // 这一组函数被process_write调用以填充HTTP应答。
  bool add_status_line(int status, const char *title);
  void add_headers(int content_length);
  bool add_content_length(int content_length);
  bool add_content_type();
  bool add_linger();
  bool add_blank_line();
  bool add_content(const char *content);
  bool add_response(const char *format, ...);
};

#endif