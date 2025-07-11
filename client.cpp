#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
// C++
#include <vector>
#include <string>
// 自定义
#include "redis_socket.h"
#include "redis_log.h"

static int32_t send_req(int fd, const std::vector<std::string> &cmd) {
    uint32_t len = 4;
    for (const std::string &str : cmd) {
        len += 4 + str.size();
    }
    if (len > k_max_msg) return -1;

    char wbuf[4 + k_max_msg];
    memcpy(&wbuf[0], &len, 4);
    uint32_t n = cmd.size();
    memcpy(&wbuf[4], &n, 4);

    size_t cur = 8;
    for (auto str : cmd) {
        uint32_t p = (uint32_t)str.size();
        memcpy(&wbuf[cur], &p, 4);
        memcpy(&wbuf[cur + 4], str.data(), str.size());
        REDIS_LOG(LogLevel::DEBUG, "send len:%d, msg:%s", p, str.data());
        cur += 4 + str.size();
    }
    return write_full(fd, &wbuf[0], (size_t)len+4);
}


static int32_t read_res(int fd) {
    uint32_t len = 0;
    int32_t err = 0;
    ResState rescode;

    // 4字节的头部
    char rbuf[8 + k_max_msg + 1];
    if (err = read_full(fd, rbuf, 8)) {
        REDIS_LOG(LogLevel::ERROR,"read() error");
        return err;
    }

    memcpy(&len, rbuf, 4); // 获取数据长度
    memcpy(&rescode, &rbuf[4], 4); // 获取操作执行的状态码
    if (len > k_max_msg) {
        REDIS_LOG(LogLevel::ERROR, "too long");
        return -1;
    }

    // 数据体
    if(err = read_full(fd, &rbuf[8], len - 4)) {
        REDIS_LOG(LogLevel::ERROR, "server read() error");
        return err;
    }

    // 处理数据
    rbuf[4 + len] = '\0';
    REDIS_LOG(LogLevel::INFO, "server says: %s", &rbuf[8]);
    return 0;
}

int main(int argc, char **argv) {
    // 创建套接字
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    const char* hello = "hello";
    assert(sock > 0);
    set_log_level(LogLevel::INFO);
    // 向服务器发起请求
    sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr)); // 清空结构体
    serv_addr.sin_family = AF_INET; // 使用IPv4地址
    serv_addr.sin_addr.s_addr = inet_addr("127.0.0.1"); // 服务器具体的IP地址
    serv_addr.sin_port = htons(1234); // 服务端使用的端口
    if (connect(sock, (sockaddr*)&serv_addr, sizeof(serv_addr))) {
        REDIS_LOG(LogLevel::ERROR, "connect failed!");
        return -1;
    }
    REDIS_LOG(LogLevel::DEBUG, "connect %s", inet_ntoa(serv_addr.sin_addr));
    // 多个请求
    std::vector<std::string> cmd;
    for (int i = 1; i < argc; ++i) {
        cmd.push_back(argv[i]);
    }
    REDIS_LOG(LogLevel::DEBUG, "run cmd:%s",cmd[0].data());
    int32_t err = send_req(sock, cmd);
    REDIS_LOG(LogLevel::DEBUG, "send cmd:%s",cmd[0].data());
    if (err) goto end;
    if(read_res(sock)) goto end;
    REDIS_LOG(LogLevel::DEBUG, "read res:%s",cmd[0].data());
end:
    sleep(1);
    close(sock);
    return 0;
}