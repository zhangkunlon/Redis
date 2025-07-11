#include "redis_socket.h"
#include "redis_log.h"
#include <fcntl.h>

// 从fd读取n个字节到buf中
int32_t read_full(int fd, char *buf, size_t n) {
    while (n > 0) {
        ssize_t rv = read(fd, buf, n);
        if (rv <= 0) {
            // 从socket缓冲区读失败
            return -1;
        }
        assert((size_t)rv <= n);
        n -= (size_t)rv;
        buf += (size_t)rv;
    }
    return 0;
}

int32_t write_full(int fd, const char *buf, size_t n) {
    while (n > 0) {
        ssize_t rv = write(fd, buf, n);
        if (rv <= 0) {
            // 出错了
            return -1;
        }
        assert((size_t)rv <= n);
        n -= (size_t)rv;
        buf += (size_t)rv;
    }
    return 0;
}

// 把fd设置成非阻塞模式
int fd_set_nb(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    flags |= O_NONBLOCK;

    if (fcntl(fd, F_SETFL, flags) < 0){
        msg(LogLevel::ERROR, "fcntl set no_block failed");
        return -1;
    }
    return 0;
}

// 关闭非阻塞模式
int fd_close_nb(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;

    flags &= ~O_NONBLOCK; // 清除非阻塞标志

    if (fcntl(fd, F_SETFL, flags) < 0){
        msg(LogLevel::ERROR, "fcntl close no_block failed");
        return -1;
    }
    return 0;
}