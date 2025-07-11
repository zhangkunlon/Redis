#pragma once
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>

// 最大的读写缓冲的长度 8KB
const size_t k_max_msg = 8 << 10;

// 连接状态
enum class ConnState{
            STATE_REQ, // query, 读请求
            STATE_RES, // send，发送响应
            STATE_END, // 标记准备删除的连接
} ;

enum class ResState{
    RES_OK,
    RES_ERR,
    RES_NX
};

struct Conn {
    int fd = -1;
    ConnState state = ConnState::STATE_REQ;
    // 读缓冲区
    size_t rbuf_size = 0;
    u_int8_t rbuf[4 + k_max_msg];

    // 写缓冲区
    size_t wbuf_size = 0;
    size_t wbuf_sent = 0;
    u_int8_t wbuf[4 + k_max_msg];
};

// 从fd接收n个字节到buf中
int32_t read_full(int fd, char *buf, size_t n);
// 将buf中的n个字节写入到fd中，发送出去
int32_t write_full(int fd, const char *buf, size_t n);
// 把fd设置成非阻塞模式
int fd_set_nb(int fd);
// 关闭非阻塞模式
int fd_close_nb(int fd);