#include "http_conn.h"
#include <regex>

// 定义HTTP响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file from this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the requested file.\n";

// 所有的客户数
int http_conn::m_epollfd = -1;
// 所有socket上的事件都被注册到同一个epoll内核事件中，所以设置成静态的
int http_conn::m_user_count = 0;

// 网站根目录
const std::string doc_root = "/home/zen/webserver/resources";

// 设置文件描述符非阻塞
void setnonblocking(int fd)
{
  int old_flag = fcntl(fd, F_GETFL);
  int new_flag = old_flag | O_NONBLOCK;
  fcntl(fd, F_SETFL, new_flag);
}

// 向epoll中添加需要监听的文件描述符
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

// 从epoll中移除监听的文件描述符
void removefd(int epollfd, int fd)
{
  epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
  close(fd);
}

// 修改文件描述符,重置socket上EPOLLONESHOT事件，以确保下次可读时EPOLLIN事件能被触发
void modfd(int epollfd, int fd, int ev)
{
  epoll_event event;
  event.data.fd = fd;
  event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
  epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

// 初始化连接
void http_conn::init(int sockfd, const sockaddr_in &addr)
{
  m_sockfd = sockfd;
  m_address = addr;

  // 端口复用
  int reuse = 1;
  setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  // 添加到epoll对象中
  addfd(m_epollfd, m_sockfd, true);
  m_user_count++; // 总用户数+1

  init();
}

void http_conn::init()
{
  // 不再需要手动释放文件映射
  m_file_address.reset();

  bytes_to_send = 0;
  bytes_have_send = 0;

  m_check_state = CHECK_STATE_REQUESTLINE; // 初始化状态为解析请求首行
  m_linger = false;                        // 默认不保持链接  Connection : keep-alive保持连接
  m_start_line = 0;
  m_checked_idx = 0;
  m_read_idx = 0;
  m_write_idx = 0;
  m_method = GET; // 默认请求方式为GET
  m_url.clear();
  m_version.clear();
  m_content_length = 0;
  m_host.clear();
  bzero(m_read_buf, READ_BUFFER_SIZE);
  bzero(m_write_buf, READ_BUFFER_SIZE);
  m_real_file.clear();
}

// 关闭连接
void http_conn::close_conn()
{
  if (m_sockfd != -1)
  {
    removefd(m_epollfd, m_sockfd);
    m_sockfd = -1;
    m_user_count--; // 用户数-1

    // 智能指针会自动清理资源
    m_file_address.reset();
  }
}

// 循环读取客户数据，直到无数据可读或者对方关闭连接
bool http_conn::read()
{
  if (m_read_idx >= READ_BUFFER_SIZE)
  {
    return false;
  }
  // 读取到的字节
  int bytes_read = 0;
  while (true)
  {
    // 从m_read_buf + m_read_idx索引出开始保存数据，大小是READ_BUFFER_SIZE - m_read_idx
    bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
    if (bytes_read == -1)
    {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
      {
        // 没有数据
        break;
      }
      return false;
    }
    else if (bytes_read == 0)
    {
      // 对方关闭连接
      return false;
    }
    m_read_idx += bytes_read;
  }
  printf("读取到了数据:%s\n", m_read_buf);
  return true;
}

bool http_conn::write()
{
  int temp = 0;

  if (bytes_to_send == 0)
  {
    // 将要发送的字节为0，这一次响应结束。
    modfd(m_epollfd, m_sockfd, EPOLLIN);
    init();
    return true;
  }

  while (1)
  {
    // 分散写
    temp = writev(m_sockfd, m_iv, m_iv_count);
    if (temp <= -1)
    {
      // 如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件，虽然在此期间，
      // 服务器无法立即接收到同一客户的下一个请求，但可以保证连接的完整性。
      if (errno == EAGAIN)
      {
        modfd(m_epollfd, m_sockfd, EPOLLOUT);
        return true;
      }
      // 智能指针会自动释放资源
      unmap();
      return false;
    }
    bytes_to_send -= temp;
    bytes_have_send += temp;

    if (bytes_have_send >= m_iv[0].iov_len)
    {
      m_iv[0].iov_len = 0;
      m_iv[1].iov_base = m_file_address.get() + (bytes_have_send - m_write_idx);
      m_iv[1].iov_len = bytes_to_send;
    }
    else
    {
      m_iv[0].iov_base = m_write_buf + bytes_have_send;
      m_iv[0].iov_len = m_iv[0].iov_len - temp;
    }

    if (bytes_to_send <= 0)
    {
      // 没有数据要发送了
      unmap();
      modfd(m_epollfd, m_sockfd, EPOLLIN);

      if (m_linger)
      {
        init();
        return true;
      }
      else
      {
        return false;
      }
    }
  }
}

// 主状态机，解析请求 - 使用正则表达式
http_conn::HTTP_CODE http_conn::process_read()
{
  // 初始化缓冲区末尾，确保字符串正确终止
  m_read_buf[m_read_idx] = '\0';

  // 使用字符串视图存储完整的请求
  std::string request(m_read_buf, m_read_idx);

  // 检查是否包含完整的HTTP请求（至少包含\r\n\r\n）
  if (request.find("\r\n\r\n") == std::string::npos)
  {
    return NO_REQUEST;
  }

  // 解析请求行
  HTTP_CODE ret = parse_request_line(request);
  if (ret == BAD_REQUEST)
  {
    return BAD_REQUEST;
  }

  // 如果请求行解析成功，则继续解析请求头
  if (ret == GET_REQUEST)
  {
    // 解析请求头
    ret = parse_headers(request);
    if (ret == BAD_REQUEST)
    {
      return BAD_REQUEST;
    }

    // 如果请求头解析成功且是GET请求，直接处理请求
    if (ret == GET_REQUEST)
    {
      return do_request();
    }

    // 如果有请求体，则需要确认请求体是否完整
    if (m_content_length > 0)
    {
      // 检查是否接收到足够的数据
      size_t header_end = request.find("\r\n\r\n");
      if (header_end != std::string::npos &&
          static_cast<size_t>(m_read_idx) < (static_cast<size_t>(m_content_length) + header_end + 4))
      {
        return NO_REQUEST;
      }
      // 请求体完整，处理请求
      return do_request();
    }
  }

  return NO_REQUEST;
}

// 解析HTTP请求行，获得请求方法、目标URL、HTTP版本
http_conn::HTTP_CODE http_conn::parse_request_line(const std::string &request)
{
  // 正则表达式匹配HTTP请求行: 方法 URL HTTP版本
  std::regex request_line_regex("^([A-Z]+)\\s+([^\\s]+)\\s+HTTP/([0-9]\\.[0-9])\\r\\n");
  std::smatch matches;

  if (!std::regex_search(request, matches, request_line_regex))
  {
    return BAD_REQUEST;
  }

  // 解析方法（当前只支持GET）
  std::string method = matches[1];
  if (method == "GET")
  {
    m_method = GET;
  }
  else
  {
    return BAD_REQUEST;
  }

  // 解析URL
  std::string url = matches[2];
  // 处理带有http://的URL
  if (url.compare(0, 7, "http://") == 0)
  {
    size_t pos = url.find('/', 7);
    if (pos != std::string::npos)
    {
      url = url.substr(pos);
    }
    else
    {
      return BAD_REQUEST;
    }
  }

  // 处理URL，必须以/开头
  if (url.empty() || url[0] != '/')
  {
    return BAD_REQUEST;
  }

  // 直接赋值给std::string成员变量
  m_url = url;

  // 解析HTTP版本（只支持HTTP/1.1）
  std::string version = matches[3];
  if (version != "1.1")
  {
    return BAD_REQUEST;
  }

  // 直接赋值给std::string成员变量
  m_version = version;

  return GET_REQUEST;
}

// 解析HTTP请求的头部信息
http_conn::HTTP_CODE http_conn::parse_headers(const std::string &request)
{
  // 找到请求行结束和头部开始的位置
  size_t header_start = request.find("\r\n") + 2;
  if (header_start >= request.length())
  {
    return BAD_REQUEST;
  }

  // 头部和正文的分界
  size_t header_end = request.find("\r\n\r\n");
  if (header_end == std::string::npos)
  {
    return NO_REQUEST;
  }

  // 截取头部部分
  std::string headers = request.substr(header_start, header_end - header_start);

  // 用正则表达式匹配各个头部字段
  std::regex header_regex("([^:\\r\\n]+):\\s*([^\\r\\n]*)\\r\\n");
  std::regex_iterator<std::string::iterator> it(headers.begin(), headers.end(), header_regex);
  std::regex_iterator<std::string::iterator> end;

  while (it != end)
  {
    std::string header_name = (*it)[1];
    std::string header_value = (*it)[2];

    // 处理Connection头部
    if (header_name == "Connection")
    {
      if (header_value == "keep-alive")
      {
        m_linger = true;
      }
    }
    // 处理Content-Length头部
    else if (header_name == "Content-Length")
    {
      m_content_length = std::stoi(header_value);
    }
    // 处理Host头部
    else if (header_name == "Host")
    {
      // 直接赋值给std::string成员变量
      m_host = header_value;
    }

    ++it;
  }

  // 如果解析完头部后，且没有请求体，则认为是一个完整的GET请求
  if (m_content_length == 0)
  {
    return GET_REQUEST;
  }

  return NO_REQUEST;
}

// 解析HTTP请求的消息体
http_conn::HTTP_CODE http_conn::parse_content(const std::string &request)
{
  // 找到消息体开始的位置
  size_t content_start = request.find("\r\n\r\n") + 4;
  if (content_start == 4 || content_start <= 4) // 如果没找到\r\n\r\n或位置不正确
  {
    return BAD_REQUEST;
  }

  // 检查消息体是否完整接收
  if (request.length() - content_start >= (size_t)m_content_length)
  {
    return GET_REQUEST;
  }

  return NO_REQUEST;
}

// 由于我们现在使用正则表达式直接处理完整的HTTP请求，不再需要按行解析
// 但为了保持兼容性，我们保留这个函数，它总是返回一个完整的行
http_conn::LINE_STATUS http_conn::parse_line()
{
  return LINE_OK;
}

// 对内存映射区执行munmap操作
void http_conn::unmap()
{
  // 使用智能指针自动管理，只需要重置指针
  m_file_address.reset();
}

// 当得到一个完整、正确的HTTP请求时，我们就分析目标文件的属性，
// 如果目标文件存在、对所有用户可读，且不是目录，则使用mmap将其
// 映射到内存地址m_file_address处，并告诉调用者获取文件成功
http_conn::HTTP_CODE http_conn::do_request()
{
  // 构造请求文件路径
  m_real_file = doc_root + m_url;

  // 获取文件状态信息
  if (stat(m_real_file.c_str(), &m_file_stat) < 0)
  {
    return NO_RESOURCE;
  }

  // 判断访问权限
  if (!(m_file_stat.st_mode & S_IROTH))
  {
    return FORBIDDEN_REQUEST;
  }

  // 判断是否是目录
  if (S_ISDIR(m_file_stat.st_mode))
  {
    return BAD_REQUEST;
  }

  // 以只读方式打开文件
  int fd = open(m_real_file.c_str(), O_RDONLY);
  if (fd < 0)
  {
    return NO_RESOURCE;
  }

  // 先清除之前的映射
  m_file_address.reset();

  // 创建内存映射
  char *addr = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);

  if (addr == MAP_FAILED)
  {
    return INTERNAL_ERROR;
  }

  // 使用自定义删除器的智能指针
  m_file_address = std::shared_ptr<char>(addr, [this](char *p)
                                         {
    if (p != nullptr && p != MAP_FAILED) {
      munmap(p, m_file_stat.st_size);
    } });

  return FILE_REQUEST;
}

// 往写缓冲中写入待发送的数据
bool http_conn::add_response(const char *format, ...)
{
  if (m_write_idx >= WRITE_BUFFER_SIZE)
  {
    return false;
  }
  va_list arg_list;
  va_start(arg_list, format);
  int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
  if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
  {
    return false;
  }
  m_write_idx += len;
  va_end(arg_list);
  return true;
}

bool http_conn::add_status_line(int status, const char *title)
{
  return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

void http_conn::add_headers(int content_len)
{
  add_content_length(content_len);
  add_content_type();
  add_linger();
  add_blank_line();
}

bool http_conn::add_content_length(int content_len)
{
  return add_response("Content-Length: %d\r\n", content_len);
}

bool http_conn::add_linger()
{
  return add_response("Connection: %s\r\n", (m_linger == true) ? "keep-alive" : "close");
}

bool http_conn::add_blank_line()
{
  return add_response("%s", "\r\n");
}

bool http_conn::add_content(const char *content)
{
  return add_response("%s", content);
}

bool http_conn::add_content_type()
{
  return add_response("Content-Type:%s\r\n", "text/html");
}

// 根据服务器处理HTTP请求的结果，决定返回给客户端的内容
bool http_conn::process_write(HTTP_CODE ret)
{
  switch (ret)
  {
  case INTERNAL_ERROR:
    add_status_line(500, error_500_title);
    add_headers(strlen(error_500_form));
    if (!add_content(error_500_form))
    {
      return false;
    }
    break;
  case BAD_REQUEST:
    add_status_line(400, error_400_title);
    add_headers(strlen(error_400_form));
    if (!add_content(error_400_form))
    {
      return false;
    }
    break;
  case NO_RESOURCE:
    add_status_line(404, error_404_title);
    add_headers(strlen(error_404_form));
    if (!add_content(error_404_form))
    {
      return false;
    }
    break;
  case FORBIDDEN_REQUEST:
    add_status_line(403, error_403_title);
    add_headers(strlen(error_403_form));
    if (!add_content(error_403_form))
    {
      return false;
    }
    break;
  case FILE_REQUEST:
    add_status_line(200, ok_200_title);
    add_headers(m_file_stat.st_size);
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv[1].iov_base = m_file_address.get();
    m_iv[1].iov_len = m_file_stat.st_size;
    m_iv_count = 2;
    bytes_to_send = m_write_idx + m_file_stat.st_size;
    return true;
  default:
    return false;
  }

  m_iv[0].iov_base = m_write_buf;
  m_iv[0].iov_len = m_write_idx;
  m_iv_count = 1;
  bytes_to_send = m_write_idx;

  return true;
}

// 由线程池中的工作线程调用，这是处理http请求的入口函数
void http_conn::process()
{
  // 解析HTTP请求
  HTTP_CODE read_ret = process_read();
  if (read_ret == NO_REQUEST)
  {
    modfd(m_epollfd, m_sockfd, EPOLLIN);
    return;
  }

  // 生成响应
  bool write_ret = process_write(read_ret);
  if (!write_ret)
  {
    close_conn();
  }
  modfd(m_epollfd, m_sockfd, EPOLLOUT);
}
