#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/epoll.h>
// C++
#include <vector>
#include <string>
#include <map>
// Customize
#include "redis_socket.h"
#include "redis_hash.h"
#include "redis_log.h"

static void connection_io(Conn *conn);
static int accept_new_conn(std::vector<Conn *> &fd_conn, int epfd, int fd);
static bool try_flush_buffer(Conn *conn);
static bool try_fill_buffer(Conn *conn);
static void update_epoll_events(int epfd, Conn *conn);

static int32_t parse_req(const uint8_t *data, size_t len, std::vector<std::string> &out);
static ResState do_get(std::vector<std::string> &cmd, uint8_t *res, uint32_t &reslen);
static ResState do_set(std::vector<std::string> &cmd, uint8_t *res, uint32_t &reslen);
static ResState do_del(std::vector<std::string> &cmd, uint8_t *res, uint32_t &reslen);
static bool cmd_is(std::string &str, const char *cmd) {
    return str.compare(cmd) == 0;
}

int main() {
    // 初始化一些变量
    int max_connect = 20;
    int port = 1234;
    char ip[] = "127.0.0.1";
    // 创建套接字
    int serv_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (serv_sock < 0) {
        REDIS_LOG(LogLevel::ERROR, "establish socket failed.");
        return 0;
    }
    // 设置地址复用 防止重启服务器的时候没法绑定在同一个地址
    int val =  1;
    setsockopt(serv_sock,  SOL_SOCKET,  SO_REUSEADDR,  &val,  sizeof(val));


    // 将套接字和IP、端口绑定
    sockaddr_in serv_addr;
    // 初始化serv_addr
    memset(&serv_addr, 0, sizeof(sockaddr_in));
    serv_addr.sin_family = AF_INET; // 使用IPv4地址
    serv_addr.sin_addr.s_addr = inet_addr(ip); // 具体的IP地址
    serv_addr.sin_port = htons(port); // 端口
    if(bind(serv_sock, (sockaddr*)&serv_addr, sizeof(serv_addr))) {
        REDIS_LOG(LogLevel::ERROR, "bind() error");
        return 0;
    }
    
    // 进入监听状态，等待用户发起请求
    if (listen(serv_sock, max_connect)) {
        REDIS_LOG(LogLevel::ERROR, "listen()");
        return 0;
    };
    // 将监听的文件描述符设置为非阻塞模式
    if (fd_set_nb(serv_sock)) {
        REDIS_LOG(LogLevel::WARN, "Failed to set the non-blocking mode");
    }
    else REDIS_LOG(LogLevel::DEBUG, "The service switches to non-blocking mode", "");

    // 存放所有客户端连接的映射 用文件描述符 fd 作为键
    std::vector<Conn*> fd_conn;

    // 创建 epoll 实例
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) {
        REDIS_LOG(LogLevel::ERROR, "epoll_create1() failed");
        close(serv_sock);
        return -1;
    }

    // 将监听套接字添加到 epoll 中
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = serv_sock;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, serv_sock, &ev) < 0) {
        REDIS_LOG(LogLevel::ERROR, "epoll_ctl() failed for server socket");
        close(epfd);
        close(serv_sock);
        return -1;
    }

    const int MAX_EVENTS = 64;
    struct epoll_event events[MAX_EVENTS];

    while (true) {
        // 等待事件发生
        int nfds = epoll_wait(epfd, events, MAX_EVENTS, 1000);
        if (nfds < 0) {
            REDIS_LOG(LogLevel::ERROR, "epoll_wait() failed");
            break;
        }

        // 处理所有活跃的事件
        for (int i = 0; i < nfds; ++i) {
            int fd = events[i].data.fd;
            uint32_t mask = events[i].events;

            if (fd == serv_sock) {
                // 监听套接字有新连接
                accept_new_conn(fd_conn, epfd, serv_sock);
            } else {
                // 客户端连接有事件
                if (fd < (int)fd_conn.size() && fd_conn[fd]) {
                    Conn *conn = fd_conn[fd];
                    connection_io(conn);
                    if (conn->state == ConnState::STATE_END) {
                        // 连接已关闭 或者处于异常状态
                        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
                        fd_conn[fd] = nullptr;
                        close(fd);
                        delete conn;
                    } else {
                        // 更新 epoll 事件
                        update_epoll_events(epfd, conn);
                    }
                }
            }
        }
    }

    close(epfd);

    return 0;
}

// 将conn放入数组fd2conn的第 conn->fd 个位置 
static void conn_put(std::vector<Conn *> &fd2conn, Conn *conn) {
    if (fd2conn.size() <= (size_t)conn->fd) {
        fd2conn.resize(conn->fd + 1);
    }
    fd2conn[conn->fd] = conn;
}

// 从监听的连接符fd接受连接, 将连接存入Conn数组
static int accept_new_conn(std::vector<Conn *> &fd2conn, int epfd, int fd) {
    sockaddr_in client_addr = {};
    socklen_t socklen = sizeof(client_addr);

    int connfd = accept(fd, (sockaddr *)&client_addr, &socklen);
    if (connfd < 0) {
        REDIS_LOG(LogLevel::ERROR, "accept() error");
        return -1;
    }

    // 将新获取的连接符设置为非阻塞模式
    fd_set_nb(connfd);

    // 创建 Conn 结构体
    Conn *conn = new Conn();
    if (conn == nullptr) {
        REDIS_LOG(LogLevel::ERROR, "Insufficient memory, client address : %s",
                        inet_ntoa(client_addr.sin_addr));
        close(connfd);
        return -1;
    }
    conn->fd = connfd;
    conn->state = ConnState::STATE_REQ;
    conn->rbuf_size = 0;
    conn->rbuf_start = 0;
    conn->wbuf_size = 0;
    conn->wbuf_start = 0;
    conn->wbuf_sent = 0;
    conn_put(fd2conn, conn);

    // 将新连接添加到 epoll 中
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLERR;
    ev.data.fd = connfd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, connfd, &ev) < 0) {
        REDIS_LOG(LogLevel::ERROR, "epoll_ctl() failed for new connection");
        fd2conn[connfd] = nullptr;
        close(connfd);
        delete conn;
        return -1;
    }

    return 0;
}

// 更新连接在 epoll 中的事件
static void update_epoll_events(int epfd, Conn *conn) {
    struct epoll_event ev;
    ev.data.fd = conn->fd;
    ev.events = EPOLLERR;

    if (conn->state == ConnState::STATE_REQ) {
        ev.events |= EPOLLIN;
    } else if (conn->state == ConnState::STATE_RES) {
        ev.events |= EPOLLOUT;
    }

    if (epoll_ctl(epfd, EPOLL_CTL_MOD, conn->fd, &ev) < 0) {
        REDIS_LOG(LogLevel::ERROR, "epoll_ctl() MOD failed for fd %d", conn->fd);
    }
}

// 读数据到Conn的rbuf
static void state_req(Conn *conn) {
    while (try_fill_buffer(conn)) {}
}

// 发送数据到客户端
static void state_res(Conn *conn) {
    while (try_flush_buffer(conn)) {}
}

// 负责解析并处理请求
// 目前只识别3种命令(get、set、del)
static int32_t do_request(const uint8_t *req, uint32_t reqlen, ResState &rescode,
                          uint8_t *res, uint32_t &reslen) {
    std::vector<std::string> cmd;
    std::string operate;
    if (parse_req(req, reqlen, cmd) != 0) {
        REDIS_LOG(LogLevel::WARN, "bad req");
        return -1;
    }
    
    for (auto str : cmd) {
        operate += str + " ";
    }
    REDIS_LOG(LogLevel::DEBUG, ": %s", operate.data());
    if (cmd.size() == 2 && cmd_is(cmd[0], "get")) {
        rescode = do_get(cmd, res, reslen);
    } else if (cmd.size() == 3 && cmd_is(cmd[0], "set")) {
        rescode = do_set(cmd, res, reslen);
    } else if (cmd.size() == 2 && cmd_is(cmd[0], "del")) {
        rescode = do_del(cmd, res, reslen);
    } else {
        // 未知命令
        rescode = ResState::RES_ERR;
        const char *REDIS_LOG = "Unknown cmd";
        strcpy((char *)res, REDIS_LOG);
        reslen = strlen(REDIS_LOG);
    }
    return (int)rescode;
}

static bool try_flush_buffer(Conn *conn) {
    if (conn->wbuf_size == conn->wbuf_sent) {
        // 没有数据需要发送
        return false;
    }

    ssize_t rv = 0;
    size_t remain = conn->wbuf_size - conn->wbuf_sent;

    // 计算连续可发送的数据长度
    size_t read_pos = (conn->wbuf_start + conn->wbuf_sent) % sizeof(conn->wbuf);
    size_t continuous_data = sizeof(conn->wbuf) - read_pos;
    size_t to_send = (remain < continuous_data) ? remain : continuous_data;

    rv = write(conn->fd, &conn->wbuf[read_pos], to_send);
    if (rv < 0 && errno == EAGAIN) {
        // 遇到EAGAIN 停止写入
        return false;
    }
    if (rv < 0) {
        REDIS_LOG(LogLevel::ERROR, "write() failed");
        conn->state = ConnState::STATE_END;
        return false;
    }

    // 更新已发送的数据量
    wbuf_consume(conn, (size_t)rv);

    if (conn->wbuf_size == 0) {
        // 响应已经发送完毕，切换回REQ状态
        conn->state = ConnState::STATE_REQ;
        return false;
    }

    // 写缓冲区还有数据 可以试着继续写入
    return true;
}

static bool try_one_request(Conn *conn) {
    // 尝试从环形缓冲区里解析出一个请求
    if (conn->rbuf_size < 4) {
        // 缓冲区数据不够，下次试试
        return false;
    }

    // 先读取长度字段，但不消费数据
    uint32_t len = 0;
    if (conn->rbuf_start + 4 > sizeof(conn->rbuf)) {
        // 长度字段跨边界
        size_t n1 = sizeof(conn->rbuf) - conn->rbuf_start;
        memcpy(&len, &conn->rbuf[conn->rbuf_start], n1);
        memcpy((char*)&len + n1, &conn->rbuf[0], 4 - n1);
    } else {
        memcpy(&len, &conn->rbuf[conn->rbuf_start], 4);
    }

    if (len > k_max_msg) {
        REDIS_LOG(LogLevel::WARN, "request is too long");
        conn->state = ConnState::STATE_END;
        return false;
    }

    if (len + 4 > conn->rbuf_size) {
        // 缓冲区数据不够 下次循环再试试
        return false;
    }

    // 现在真正消费长度字段
    read_from_rbuf(conn, (char*)&len, 4);

    // 读取请求数据到临时缓冲区
    char req_buf[k_max_msg];
    read_from_rbuf(conn, req_buf, len);

    // 生成响应
    ResState rescode = ResState::RES_OK;
    uint32_t wlen = 0;
    char res_buf[k_max_msg];
    int32_t err = do_request((uint8_t*)req_buf, len, rescode, (uint8_t*)res_buf, wlen);
    if (err < 0) {
        REDIS_LOG(LogLevel::ERROR, "request processing failed");
        conn->state = ConnState::STATE_END;
        return true;
    } else {
        rescode = (ResState)err;
    }

    // 将响应写入写缓冲区
    uint32_t total_len = wlen + 4;
    if (wbuf_available_space(conn) < total_len + 4) {
        REDIS_LOG(LogLevel::ERROR, "write buffer is not enough");
        conn->state = ConnState::STATE_END;
        return true;
    }

    // 写入响应长度和状态码
    write_to_wbuf(conn, (char*)&total_len, 4);
    write_to_wbuf(conn, (char*)&rescode, 4);
    write_to_wbuf(conn, res_buf, wlen);

    // 状态转换为写状态
    conn->state = ConnState::STATE_RES;
    state_res(conn);

    // 请求处理完了 继续外层循环
    return (conn->state == ConnState::STATE_REQ);
}

static bool try_fill_buffer(Conn *conn) {
    // 尝试填充环形缓冲区
    assert(conn->rbuf_size < sizeof(conn->rbuf));

    size_t available = rbuf_available_space(conn);
    if (available == 0) {
        return false;  // 缓冲区已满
    }

    ssize_t rv = 0;
    do {
        // 计算连续可写入的空间
        size_t write_pos = (conn->rbuf_start + conn->rbuf_size) % sizeof(conn->rbuf);
        size_t continuous_space = sizeof(conn->rbuf) - write_pos;
        size_t to_read = (available < continuous_space) ? available : continuous_space;

        rv = read(conn->fd, &conn->rbuf[write_pos], to_read);
    } while (rv < 0 && errno == EINTR);

    // EINTR : 系统调用被信号中断（如 read() 被用户按下 Ctrl+C 终止）
    // EAGAIN: 资源暂时不可用（非阻塞调用需重试，如 read() 无数据）
    if (rv < 0 && errno == EAGAIN) {
        // 遇到了EAGAIN 停止读取
        return false;
    }

    if (rv < 0) {
        REDIS_LOG(LogLevel::ERROR, "read() failed");
        conn->state = ConnState::STATE_END;
        return false;
    }

    if (rv == 0) {
        if (conn->rbuf_size > 0) {
            REDIS_LOG(LogLevel::INFO, "Unexpected EOF");
        } else {
            REDIS_LOG(LogLevel::INFO, "EOF, client closed");
        }
        conn->state = ConnState::STATE_END;
        return false;
    }

    conn->rbuf_size += (size_t)rv;
    assert(conn->rbuf_size <= sizeof(conn->rbuf));

    // 尝试逐个处理请求
    while (try_one_request(conn)) {}

    return conn->state == ConnState::STATE_REQ;
}

// 连接状态机
static void connection_io(Conn *conn) {
    switch (conn->state)
    {
    case ConnState::STATE_REQ:
        state_req(conn);
        break;
    case ConnState::STATE_RES:
        state_res(conn);
        break;
    default:
        REDIS_LOG(LogLevel::ERROR, "Undefined ConnState: %d", (int)conn->state);
        assert(0); // 直接中断服务
        break;
    }
}

/**
 * +------+-----+------+-----+------+-----+-----+------+
 *| nstr | len | str1 | len | str2 | ... | len | strn |
 *+------+-----+------+-----+------+-----+-----+------+
 *
*/
static int32_t parse_req(const uint8_t *data, size_t len, std::vector<std::string> &out) {
    if (len < 4) return -1;

    uint32_t n = 0; // 获取请求总数n
    memcpy(&n, &data[0], 4);

    if (n > k_max_msg) return -1;

    size_t pos = 4;
    while (n--) { //循环读取n个元素
        if (pos + 4 > len) return -1;
        uint32_t sz = 0; // 获取单个元素长度
        memcpy(&sz, &data[pos], 4);
        if (pos + 4+ sz > len) return -1;

        out.push_back(std::string((char *)&data[pos + 4], sz));
        pos += 4 + sz;
    }

    if (pos != len) return -1; // 有多余的无用数据
    return 0;
}

// FNV hash
static uint64_t str_hash(const uint8_t *data, size_t len) {
    uint32_t h = 0x811C9DC5;
    for (size_t i = 0; i < len; i++) {
        h = (h + data[i]) * 0x01000193;
    }
    return h;
}

static ResState do_get(std::vector<std::string> &cmd, uint8_t *res, uint32_t &reslen) {
    if (cmd.size() < 2) return ResState::RES_ERR;
    std::string response;

    // 创建查找用的临时Entry
    Entry search_key;
    search_key.key = cmd[1];  // 不使用swap，保留原始数据
    search_key.node.hcode = str_hash((uint8_t *)search_key.key.data(), search_key.key.size());

    // 在哈希表中查找对应的Entry（会遍历链表找到匹配的key）
    HNode *found_node = hm_lookup(&g_data.db, &search_key.node, &entry_eq);

    if (!found_node) {
        // 未找到对应的key
        response = "not found " + search_key.key;
        assert(response.size() <= k_max_msg);
        memcpy(res, response.data(), response.size());
        reslen = (uint32_t)response.size();
        return ResState::RES_NX;
    }

    // 找到了对应的Entry，返回其值
    Entry *found_entry = container_of(found_node, Entry, node);
    response = "get " + found_entry->val;
    assert(response.size() <= k_max_msg);
    memcpy(res, response.data(), response.size());
    reslen = (uint32_t)response.size();
    return ResState::RES_OK;
}

static ResState do_set(std::vector<std::string> &cmd, uint8_t *res, uint32_t &reslen) {
    if (cmd.size() < 3) return ResState::RES_ERR;
    std::string response;

    // 创建查找用的临时Entry
    Entry search_key;
    search_key.key = cmd[1];  // 不使用swap，保留原始数据
    search_key.node.hcode = str_hash((uint8_t *)search_key.key.data(), search_key.key.size());

    // 查找是否已存在相同key的Entry
    HNode *existing_node = hm_lookup(&g_data.db, &search_key.node, &entry_eq);

    if (existing_node) {
        // key已存在，更新值
        Entry *existing_entry = container_of(existing_node, Entry, node);
        existing_entry->val = cmd[2];
        response = "changed " + existing_entry->key + " to " + existing_entry->val;
    }
    else {
        // key不存在，创建新Entry并插入到链表中（如果pos相同会追加到链表头部）
        Entry *new_entry = new Entry();
        new_entry->key = cmd[1];
        new_entry->val = cmd[2];
        new_entry->node.hcode = search_key.node.hcode;
        hm_insert(&g_data.db, &new_entry->node);
        response = "new " + new_entry->key + " to " + new_entry->val;
    }

    assert(response.size() <= k_max_msg);
    memcpy(res, response.data(), response.size());
    reslen = (uint32_t)response.size();
    return ResState::RES_OK;
}

static ResState do_del(std::vector<std::string> &cmd, uint8_t *res, uint32_t &reslen) {
    if (cmd.size() < 2) return ResState::RES_ERR;
    std::string response;

    // 创建查找用的临时Entry
    Entry search_key;
    search_key.key = cmd[1];  // 不使用swap，保留原始数据
    search_key.node.hcode = str_hash((uint8_t *)search_key.key.data(), search_key.key.size());

    // 在哈希表中查找要删除的Entry（会遍历链表找到匹配的key）
    HNode *found_node = hm_lookup(&g_data.db, &search_key.node, &entry_eq);

    if (found_node) {
        // 找到了要删除的Entry
        Entry *entry_to_delete = container_of(found_node, Entry, node);
        response = "deleted " + entry_to_delete->key;

        // 从哈希表中移除该Entry（会正确处理链表中的删除）
        hm_pop(&g_data.db, &search_key.node, &entry_eq);

        // 释放内存
        delete entry_to_delete;
    }
    else {
        // 未找到要删除的key
        response = "not found " + search_key.key;
    }

    assert(response.size() <= k_max_msg);
    memcpy(res, response.data(), response.size());
    reslen = (uint32_t)response.size();
    return ResState::RES_OK;
}