#include "http_conn.h"
#include <regex>
#include <dirent.h>
#include <algorithm>

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

// 上传文件目录
const char *http_conn::UPLOAD_DIR = "/home/zen/webserver/resources/uploads";

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

  // 初始化文件上传相关成员
  m_content_type.clear();
  m_boundary.clear();
  m_upload_file_name.clear();
  m_is_upload_request = false;

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

  // 解析请求头
  ret = parse_headers(request);
  if (ret == BAD_REQUEST)
  {
    return BAD_REQUEST;
  }

  // 如果是GET请求且没有请求体，直接处理请求
  if (m_method == GET && m_content_length == 0)
  {
    return do_request();
  }

  // 处理POST请求或包含请求体的GET请求
  if (m_content_length > 0)
  {
    // 检查是否接收到足够的数据
    size_t header_end = request.find("\r\n\r\n");
    if (header_end != std::string::npos &&
        static_cast<size_t>(m_read_idx) < (static_cast<size_t>(m_content_length) + header_end + 4))
    {
      return NO_REQUEST; // 请求体数据不完整，继续读取
    }

    // 解析请求体
    ret = parse_content(request);
    if (ret == BAD_REQUEST)
    {
      return BAD_REQUEST;
    }

    // 请求体完整，处理请求
    return do_request();
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

  // 解析方法（支持GET和POST）
  std::string method = matches[1];
  if (method == "GET")
  {
    m_method = GET;
  }
  else if (method == "POST")
  {
    m_method = POST;
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

  return m_method == GET ? GET_REQUEST : NO_REQUEST;
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
    // 处理Content-Type头部，用于文件上传
    else if (header_name == "Content-Type")
    {
      m_content_type = header_value;

      // 检查是否是multipart/form-data表单提交
      if (m_content_type.find("multipart/form-data") != std::string::npos)
      {
        m_is_upload_request = true;

        // 提取boundary值
        size_t boundary_pos = m_content_type.find("boundary=");
        if (boundary_pos != std::string::npos)
        {
          m_boundary = m_content_type.substr(boundary_pos + 9);
        }
      }
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
  if (content_start == std::string::npos + 4 || content_start <= 4) // 如果没找到\r\n\r\n或位置不正确
  {
    return BAD_REQUEST;
  }

  // 检查消息体是否完整接收
  if (request.length() - content_start >= (size_t)m_content_length)
  {
    // 提取请求体内容
    std::string body = request.substr(content_start, m_content_length);

    // 处理文件上传请求
    if (m_is_upload_request && !m_boundary.empty())
    {
      return handle_file_upload(body);
    }
    // 处理文件删除请求
    else if (m_url == "/delete")
    {
      printf("接收到删除文件请求: %s\n", body.c_str());
      // 设置checked_idx位置，让do_request能正确解析请求体
      m_checked_idx = content_start;
    }
    else
    {
      printf("接收到POST请求体: %s\n", body.c_str());
    }

    // 成功解析POST请求，返回GET_REQUEST表示一个完整的请求
    return GET_REQUEST;
  }

  return NO_REQUEST;
}

// 处理文件上传请求
http_conn::HTTP_CODE http_conn::handle_file_upload(const std::string &request_body)
{
  // 检查文件上传目录是否存在
  struct stat dir_stat;
  if (stat(UPLOAD_DIR, &dir_stat) < 0 || !S_ISDIR(dir_stat.st_mode))
  {
    // 目录不存在，尝试创建
    if (mkdir(UPLOAD_DIR, 0755) < 0)
    {
      printf("创建上传目录失败: %s\n", strerror(errno));
      return INTERNAL_ERROR;
    }
  }

  // 解析多部分表单数据
  std::map<std::string, std::string> form_data = parse_multipart_form_data(request_body);

  // 检查是否找到上传的文件
  if (form_data.find("file_content") != form_data.end() && !m_upload_file_name.empty())
  {
    // 保存上传的文件
    bool save_result = save_uploaded_file(form_data["file_content"], m_upload_file_name);
    if (!save_result)
    {
      printf("保存上传文件失败\n");
      return INTERNAL_ERROR;
    }

    printf("文件上传成功: %s\n", m_upload_file_name.c_str());

    // 设置响应页面为上传成功页面
    m_real_file = doc_root + "/post_response.html";
    return FILE_REQUEST;
  }

  return BAD_REQUEST;
}

// 解析多部分表单数据
std::map<std::string, std::string> http_conn::parse_multipart_form_data(const std::string &request_body)
{
  std::map<std::string, std::string> form_data;

  // 构造分界线字符串
  std::string boundary = "--" + m_boundary;

  // 查找第一个分界线
  size_t pos = request_body.find(boundary);
  if (pos == std::string::npos)
  {
    return form_data;
  }

  // 循环解析每个部分
  while (pos != std::string::npos)
  {
    // 找到该部分的起始位置
    size_t part_start = pos + boundary.length();

    // 查找下一个分界线
    size_t next_boundary = request_body.find(boundary, part_start);
    if (next_boundary == std::string::npos)
    {
      break;
    }

    // 提取该部分的内容
    std::string part = request_body.substr(part_start, next_boundary - part_start);

    // 提取头部和内容
    size_t headers_end = part.find("\r\n\r\n");
    if (headers_end != std::string::npos)
    {
      std::string headers = part.substr(0, headers_end);
      std::string content = part.substr(headers_end + 4);

      // 检查是否包含Content-Disposition头
      if (headers.find("Content-Disposition:") != std::string::npos)
      {
        // 检查是否是文件字段
        if (headers.find("filename=") != std::string::npos)
        {
          // 提取文件名
          m_upload_file_name = extract_filename_from_content_disposition(headers);

          // 如果文件内容是二进制，移除结尾的\r\n
          if (!content.empty() && content.substr(content.length() - 2) == "\r\n")
          {
            content = content.substr(0, content.length() - 2);
          }

          // 保存文件内容
          form_data["file_content"] = content;
        }
        else
        {
          // 普通表单字段，提取字段名和值
          std::regex field_regex("name=\"([^\"]+)\"");
          std::smatch matches;
          if (std::regex_search(headers, matches, field_regex))
          {
            std::string field_name = matches[1];

            // 移除结尾的\r\n
            if (!content.empty() && content.substr(content.length() - 2) == "\r\n")
            {
              content = content.substr(0, content.length() - 2);
            }

            form_data[field_name] = content;
          }
        }
      }
    }

    // 移动到下一个部分
    pos = next_boundary;
  }

  return form_data;
}

// 从Content-Disposition头中提取文件名
std::string http_conn::extract_filename_from_content_disposition(const std::string &header)
{
  std::regex filename_regex("filename=\"([^\"]+)\"");
  std::smatch matches;

  if (std::regex_search(header, matches, filename_regex))
  {
    return matches[1];
  }

  // 如果没找到文件名，返回一个默认名称
  return "unknown_file";
}

// 保存上传的文件
bool http_conn::save_uploaded_file(const std::string &file_content, const std::string &file_name)
{
  // 构造完整的文件路径
  std::string file_path = std::string(UPLOAD_DIR) + "/" + file_name;

  // 打开文件准备写入
  FILE *fp = fopen(file_path.c_str(), "wb");
  if (!fp)
  {
    printf("无法创建文件: %s\n", file_path.c_str());
    return false;
  }

  // 写入文件内容
  size_t written = fwrite(file_content.c_str(), 1, file_content.length(), fp);
  fclose(fp);

  // 检查是否写入成功
  if (written != file_content.length())
  {
    printf("写入文件失败: %s\n", file_path.c_str());
    return false;
  }

  return true;
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

// 生成文件列表HTML
std::string generate_file_list_html()
{
  std::string file_list_html = "";

  // 打开uploads目录
  DIR *dir = opendir(http_conn::UPLOAD_DIR);
  if (dir == NULL)
  {
    return "<p>无法访问上传目录。</p>";
  }

  // 存储文件列表
  std::vector<std::string> files;
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL)
  {
    // 跳过.和..目录
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
    {
      continue;
    }

    // 构建完整路径
    std::string full_path = std::string(http_conn::UPLOAD_DIR) + "/" + entry->d_name;

    // 检查是否是普通文件
    struct stat file_stat;
    if (stat(full_path.c_str(), &file_stat) == 0 && S_ISREG(file_stat.st_mode))
    {
      files.push_back(entry->d_name);
    }
  }
  closedir(dir);

  // 如果没有文件，显示相应信息
  if (files.empty())
  {
    return "<p>目前没有文件，请到表单页面上传文件。</p>";
  }

  // 按字母顺序排序文件
  std::sort(files.begin(), files.end());

  // 构建文件列表HTML
  file_list_html = "<ul class=\"files\">\n";
  for (const auto &file : files)
  {
    // 文件大小信息
    std::string full_path = std::string(http_conn::UPLOAD_DIR) + "/" + file;
    struct stat file_stat;
    stat(full_path.c_str(), &file_stat);

    // 计算可读的文件大小
    std::string size_str;
    if (file_stat.st_size < 1024)
    {
      size_str = std::to_string(file_stat.st_size) + " B";
    }
    else if (file_stat.st_size < 1024 * 1024)
    {
      size_str = std::to_string(file_stat.st_size / 1024) + " KB";
    }
    else
    {
      size_str = std::to_string(file_stat.st_size / (1024 * 1024)) + " MB";
    }

    // 添加文件链接、大小和删除按钮
    file_list_html += "  <li>\n";
    file_list_html += "    <div>\n";
    file_list_html += "      <a href=\"/uploads/" + file + "\">" + file + "</a>\n";
    file_list_html += "      <span class=\"file-size\">" + size_str + "</span>\n";
    file_list_html += "    </div>\n";
    file_list_html += "    <div class=\"file-actions\">\n";
    file_list_html += "      <form action=\"/delete\" method=\"POST\">\n";
    file_list_html += "        <input type=\"hidden\" name=\"filename\" value=\"" + file + "\">\n";
    file_list_html += "        <button type=\"submit\" class=\"delete-btn\">删除</button>\n";
    file_list_html += "      </form>\n";
    file_list_html += "    </div>\n";
    file_list_html += "  </li>\n";
  }
  file_list_html += "</ul>\n";

  return file_list_html;
}

// 当得到一个完整、正确的HTTP请求时，我们就分析目标文件的属性，
// 如果目标文件存在、对所有用户可读，且不是目录，则使用mmap将其
// 映射到内存地址m_file_address处，并告诉调用者获取文件成功
http_conn::HTTP_CODE http_conn::do_request()
{
  // 构造请求文件路径
  m_real_file = doc_root + m_url;

  // 对于根目录"/"，自动定向到index.html
  if (m_url == "/")
  {
    m_real_file = doc_root + "/index.html";
  }

  // 处理上传文件夹的请求
  if (m_url.compare(0, 9, "/uploads/") == 0)
  {
    std::string filename = m_url.substr(8); // 去掉/uploads前缀，保留/
    m_real_file = std::string(UPLOAD_DIR) + filename;
  }

  // 对于POST请求，可以根据URL路径和请求体内容做特殊处理
  if (m_method == POST)
  {
    printf("处理POST请求: %s\n", m_url.c_str());

    // 处理上传请求
    if (m_url == "/upload" && m_is_upload_request)
    {
      // 上传处理已经在parse_content阶段的handle_file_upload中完成
      // 这里设置响应页面
      m_real_file = doc_root + "/post_response.html";
    }
    // 处理文件删除请求
    else if (m_url == "/delete")
    {
      printf("处理文件删除请求\n");

      // 从请求体中提取文件名
      std::string request_body(m_read_buf + m_checked_idx, m_content_length);

      // 解析表单数据 - application/x-www-form-urlencoded 格式
      std::string filename;
      size_t pos = request_body.find("filename=");
      if (pos != std::string::npos)
      {
        pos += 9; // 跳过"filename="
        size_t end_pos = request_body.find("&", pos);
        if (end_pos == std::string::npos)
        {
          end_pos = request_body.length();
        }
        filename = request_body.substr(pos, end_pos - pos);

        // 处理URL编码
        for (size_t i = 0; i < filename.length(); ++i)
        {
          if (filename[i] == '+')
          {
            filename[i] = ' ';
          }
          else if (filename[i] == '%' && i + 2 < filename.length())
          {
            int hex_val = 0;
            sscanf(filename.substr(i + 1, 2).c_str(), "%x", &hex_val);
            filename.replace(i, 3, 1, static_cast<char>(hex_val));
          }
        }
      }

      printf("尝试删除文件: %s\n", filename.c_str());

      // 如果有文件名，尝试删除文件
      if (!filename.empty())
      {
        std::string file_path = std::string(UPLOAD_DIR) + "/" + filename;

        // 检查文件是否存在并且是常规文件
        struct stat file_stat;
        if (stat(file_path.c_str(), &file_stat) == 0 && S_ISREG(file_stat.st_mode))
        {
          // 尝试删除文件
          if (unlink(file_path.c_str()) == 0)
          {
            printf("文件 %s 成功删除\n", filename.c_str());
          }
          else
          {
            printf("文件 %s 删除失败: %s\n", filename.c_str(), strerror(errno));
          }
        }
        else
        {
          printf("文件 %s 不存在或不是普通文件\n", filename.c_str());
        }
      }

      // 设置删除响应页面
      m_real_file = doc_root + "/delete_response.html";
    }
    else
    {
      // 其他POST请求，返回固定的POST响应页面
      m_real_file = doc_root + "/post_response.html";
    }

    // 如果特定响应页面不存在，使用默认页面
    if (stat(m_real_file.c_str(), &m_file_stat) < 0)
    {
      m_real_file = doc_root + "/index.html";
    }
  }

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

  // 特殊处理index.html，动态插入文件列表
  if (m_real_file == doc_root + "/index.html")
  {
    // 读取原始index.html内容
    int fd = open(m_real_file.c_str(), O_RDONLY);
    if (fd < 0)
    {
      return NO_RESOURCE;
    }

    // 分配内存保存文件内容
    char *file_content = new char[m_file_stat.st_size + 1];
    int bytes_read = ::read(fd, file_content, m_file_stat.st_size);
    close(fd);

    if (bytes_read < 0)
    {
      delete[] file_content;
      return NO_RESOURCE;
    }

    file_content[bytes_read] = '\0';
    std::string html_content(file_content);
    delete[] file_content;

    // 查找文件列表占位符
    size_t file_list_pos = html_content.find("<div class=\"file-list\">");
    if (file_list_pos != std::string::npos)
    {
      // 找到文件列表的标题后面的位置
      size_t content_pos = html_content.find("<p>", file_list_pos);
      if (content_pos != std::string::npos)
      {
        // 找到段落结束的位置
        size_t end_pos = html_content.find("</p>", content_pos);
        if (end_pos != std::string::npos)
        {
          // 替换占位符内容为实际文件列表
          std::string file_list = generate_file_list_html();
          html_content.replace(content_pos, end_pos + 4 - content_pos, file_list);

          // 创建临时文件存储修改后的内容
          char temp_path[128];
          sprintf(temp_path, "/tmp/index_%d.html", m_sockfd);
          int temp_fd = open(temp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
          if (temp_fd > 0)
          {
            ::write(temp_fd, html_content.c_str(), html_content.length());
            close(temp_fd);

            // 替换m_real_file为临时文件
            m_real_file = temp_path;

            // 更新文件状态
            stat(m_real_file.c_str(), &m_file_stat);
          }
        }
      }
    }
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
  // 获取文件扩展名
  size_t dot_pos = m_real_file.find_last_of('.');
  std::string content_type = "text/html";
  bool add_disposition = false;
  std::string filename = "";

  // 提取文件名，用于Content-Disposition头
  if (m_url.compare(0, 9, "/uploads/") == 0)
  {
    size_t last_slash = m_url.find_last_of('/');
    if (last_slash != std::string::npos)
    {
      filename = m_url.substr(last_slash + 1);

      // 对于txt文件，强制浏览器下载而不是内联显示
      if (dot_pos != std::string::npos)
      {
        std::string extension = m_real_file.substr(dot_pos);
        std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
        if (extension == ".txt")
        {
          add_disposition = true;
        }
      }
    }
  }

  if (dot_pos != std::string::npos)
  {
    std::string extension = m_real_file.substr(dot_pos);

    // 转换为小写
    std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);

    // 根据扩展名设置不同的MIME类型
    if (extension == ".html" || extension == ".htm")
    {
      content_type = "text/html; charset=UTF-8";
    }
    else if (extension == ".txt")
    {
      content_type = "text/plain; charset=UTF-8";
    }
    else if (extension == ".jpg" || extension == ".jpeg")
    {
      content_type = "image/jpeg";
    }
    else if (extension == ".png")
    {
      content_type = "image/png";
    }
    else if (extension == ".gif")
    {
      content_type = "image/gif";
    }
    else if (extension == ".css")
    {
      content_type = "text/css; charset=UTF-8";
    }
    else if (extension == ".js")
    {
      content_type = "application/javascript; charset=UTF-8";
    }
    else if (extension == ".pdf")
    {
      content_type = "application/pdf";
    }
    else if (extension == ".mp3")
    {
      content_type = "audio/mpeg";
    }
    else if (extension == ".mp4")
    {
      content_type = "video/mp4";
    }
    else
    {
      // 默认作为二进制流处理
      content_type = "application/octet-stream";
    }
  }
  else
  {
    // 没有扩展名，对于上传文件夹的文件，默认使用UTF-8编码的文本
    if (m_url.compare(0, 9, "/uploads/") == 0)
    {
      content_type = "text/plain; charset=UTF-8";
    }
  }

  add_response("Content-Type: %s\r\n", content_type.c_str());

  // 添加Content-Disposition头，强制浏览器以附件形式处理而不是直接显示
  if (add_disposition && !filename.empty())
  {
    add_response("Content-Disposition: attachment; filename=\"%s\"\r\n", filename.c_str());
  }

  return true;
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
