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

int32_t read_from_rbuf(Conn *conn, char *buf, size_t n) {
    // 从环形缓冲区读取n个字节到buf中
    if (n > conn->rbuf_size) {
        n = conn->rbuf_size;
    }
    if (conn->rbuf_start + n > sizeof(conn->rbuf)) {
        // 要读取的数据跨环形缓冲区的边界
        size_t n1 = sizeof(conn->rbuf) - conn->rbuf_start;
        memcpy(buf, &conn->rbuf[conn->rbuf_start], n1);
        size_t n2 = n - n1;
        memcpy(&buf[n1], &conn->rbuf[0], n2);
    } else {
        memcpy(buf, &conn->rbuf[conn->rbuf_start], n);
    }
    conn->rbuf_start = (conn->rbuf_start + n) % sizeof(conn->rbuf);
    conn->rbuf_size -= n;
    return n;
}

int32_t write_to_rbuf(Conn *conn, const char *buf, size_t n) {
    // 将n个字节从buf写入到环形缓冲区
    if (n > sizeof(conn->rbuf) - conn->rbuf_size) {
        n = sizeof(conn->rbuf) - conn->rbuf_size;
    }

    size_t write_pos = (conn->rbuf_start + conn->rbuf_size) % sizeof(conn->rbuf);
    if (write_pos + n > sizeof(conn->rbuf)) {
        // 要写入的数据跨环形缓冲区的边界
        size_t n1 = sizeof(conn->rbuf) - write_pos;
        memcpy(&conn->rbuf[write_pos], buf, n1);
        size_t n2 = n - n1;
        memcpy(&conn->rbuf[0], &buf[n1], n2);
    } else {
        memcpy(&conn->rbuf[write_pos], buf, n);
    }
    conn->rbuf_size += n;
    return n;
}

// 读缓冲区辅助函数
size_t rbuf_available_space(Conn *conn) {
    return sizeof(conn->rbuf) - conn->rbuf_size;
}

size_t rbuf_continuous_read_size(Conn *conn) {
    if (conn->rbuf_size == 0) return 0;

    size_t end_pos = (conn->rbuf_start + conn->rbuf_size) % sizeof(conn->rbuf);
    if (end_pos > conn->rbuf_start) {
        return conn->rbuf_size;  // 数据连续
    } else {
        return sizeof(conn->rbuf) - conn->rbuf_start;  // 数据跨边界，返回第一段长度
    }
}

// 写缓冲区环形操作函数
int32_t write_to_wbuf(Conn *conn, const char *buf, size_t n) {
    size_t available = wbuf_available_space(conn);
    if (n > available) {
        n = available;
    }

    size_t write_pos = (conn->wbuf_start + conn->wbuf_size) % sizeof(conn->wbuf);
    if (write_pos + n > sizeof(conn->wbuf)) {
        // 要写入的数据跨环形缓冲区的边界
        size_t n1 = sizeof(conn->wbuf) - write_pos;
        memcpy(&conn->wbuf[write_pos], buf, n1);
        size_t n2 = n - n1;
        memcpy(&conn->wbuf[0], &buf[n1], n2);
    } else {
        memcpy(&conn->wbuf[write_pos], buf, n);
    }
    conn->wbuf_size += n;
    return n;
}

size_t wbuf_available_space(Conn *conn) {
    return sizeof(conn->wbuf) - conn->wbuf_size;
}

size_t wbuf_continuous_write_size(Conn *conn) {
    size_t available = wbuf_available_space(conn);
    if (available == 0) return 0;

    size_t write_pos = (conn->wbuf_start + conn->wbuf_size) % sizeof(conn->wbuf);
    return sizeof(conn->wbuf) - write_pos;
}

char* wbuf_write_ptr(Conn *conn) {
    size_t write_pos = (conn->wbuf_start + conn->wbuf_size) % sizeof(conn->wbuf);
    return (char*)&conn->wbuf[write_pos];
}

char* wbuf_read_ptr(Conn *conn) {
    size_t read_pos = (conn->wbuf_start + conn->wbuf_sent) % sizeof(conn->wbuf);
    return (char*)&conn->wbuf[read_pos];
}

void wbuf_consume(Conn *conn, size_t n) {
    if (n > conn->wbuf_size - conn->wbuf_sent) {
        n = conn->wbuf_size - conn->wbuf_sent;
    }

    conn->wbuf_sent += n;

    // 如果所有数据都已发送，重置缓冲区
    if (conn->wbuf_sent == conn->wbuf_size) {
        conn->wbuf_start = 0;
        conn->wbuf_size = 0;
        conn->wbuf_sent = 0;
    }
}