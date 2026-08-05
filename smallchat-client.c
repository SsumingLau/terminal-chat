/* smallchat-client.c -- Client program for smallchat-server.
 *
 * Copyright (c) 2023, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the project name of nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/* 设计说明: 回到 antirez 原版架构 (select + 字节级输入缓冲)。
 * 原版经过长期使用验证: 命令/中文/发送全部正常。
 * 只加了三处小改动:
 *   1. 退格兼容 0x08 与 0x7f (部分终端发 0x08)
 *   2. 吞掉方向键/Delete 等 7位 ESC 序列, 防止垃圾进输入行
 *      (方向键本身无光标移动功能——原版就没有, 需要时再说)
 *   3. /nick 后回显前缀变为 <昵称>>, 新消息终端通知 (OSC9+OSC777+铃音)
 * 注意: 不处理 0x9b (8位CSI) 开头——那是少数终端的行为, 且 0x9b 也是
 * 合法中文的 UTF-8 续字节, 处理它得不偿失。 */

#define _POSIX_C_SOURCE 200809L /* Linux 严格 C99 下需要, 否则 fileno() 不声明 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <sys/select.h>
#include <unistd.h>
#include <termios.h>
#include <errno.h>

#include "chatlib.h"

/* ============================================================================
 * Low level terminal handling.
 * ========================================================================== */

void disableRawModeAtExit(void);

int setRawMode(int fd, int enable) {
    static struct termios orig_termios;
    static int atexit_registered = 0;
    static int rawmode_is_set = 0;

    struct termios raw;

    if (enable == 0) {
        if (rawmode_is_set && tcsetattr(fd, TCSAFLUSH, &orig_termios) != -1)
            rawmode_is_set = 0;
        return 0;
    }

    if (!isatty(fd)) goto fatal;
    if (!atexit_registered) {
        atexit(disableRawModeAtExit);
        atexit_registered = 1;
    }
    if (tcgetattr(fd, &orig_termios) == -1) goto fatal;

    raw = orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN);
    raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSAFLUSH, &raw) < 0) goto fatal;
    rawmode_is_set = 1;
    return 0;

fatal:
    errno = ENOTTY;
    return -1;
}

void disableRawModeAtExit(void) {
    setRawMode(STDIN_FILENO, 0);
}

/* ============================================================================
 * Minimal line editing.
 * ========================================================================== */

void terminalCleanCurrentLine(void) {
    write(fileno(stdout), "\e[2K", 4);
}

void terminalCursorAtLineStart(void) {
    write(fileno(stdout), "\r", 1);
}

#define IB_MAX 128
struct InputBuffer {
    char buf[IB_MAX];
    int len;
};

#define IB_ERR 0
#define IB_OK 1
#define IB_GOTLINE 2

int inputBufferAppend(struct InputBuffer *ib, int c) {
    if (ib->len >= IB_MAX) return IB_ERR;
    ib->buf[ib->len] = c;
    ib->len++;
    return IB_OK;
}

void inputBufferHide(struct InputBuffer *ib);
void inputBufferShow(struct InputBuffer *ib);

int inputBufferFeedChar(struct InputBuffer *ib, int c) {
    /* 吞掉方向键/Home/Delete 等 7位 ESC 序列 (\e[A, \e[3~ 等),
     * 防止垃圾进入输入行。只认 ESC (0x1b) 开头, 与 UTF-8 中文
     * (0x80+ 字节) 无交集, 不会误吞。 */
    static int escseq = 0; /* 0=空闲, 1=刚收 ESC, 2=CSI/SS3 序列中 */
    if (escseq) {
        if (escseq == 1) {
            escseq = (c == '[' || c == 'O') ? 2 : 0;
            return IB_OK;
        }
        if (c >= 0x40) escseq = 0; /* CSI 终结字节 (~ @ A-Z a-z) */
        return IB_OK;
    }
    if (c == '\e') { escseq = 1; return IB_OK; }

    switch(c) {
    case '\n':
        break;          // Ignored. We handle \r instead.
    case '\r':
        return IB_GOTLINE;
    case 8:             // Backspace (某些终端/Windows 发 0x08)
    case 127:           // Backspace (多数终端发 0x7f)
        if (ib->len > 0) {
            ib->len--;
            inputBufferHide(ib);
            inputBufferShow(ib);
        }
        break;
    default:
        if (inputBufferAppend(ib,c) == IB_OK)
            write(fileno(stdout),ib->buf+ib->len-1,1);
        break;
    }
    return IB_OK;
}

void inputBufferHide(struct InputBuffer *ib) {
    (void)ib;
    terminalCleanCurrentLine();
    terminalCursorAtLineStart();
}

void inputBufferShow(struct InputBuffer *ib) {
    write(fileno(stdout),ib->buf,ib->len);
}

void inputBufferClear(struct InputBuffer *ib) {
    ib->len = 0;
    inputBufferHide(ib);
}

/* =============================================================================
 * Main program logic.
 * ========================================================================== */

int main(int argc, char **argv) {
    if (argc != 3) {
        printf("Usage: %s <host> <port>\n", argv[0]);
        exit(1);
    }

    int s = TCPConnect(argv[1],atoi(argv[2]),0);
    if (s == -1) {
        perror("Connecting to server");
        exit(1);
    }

    setRawMode(fileno(stdin),1);

    fd_set readfds;
    int stdin_fd = fileno(stdin);

    struct InputBuffer ib;
    inputBufferClear(&ib);

    /* 自己的昵称, /nick 后更新, 用于回显前缀 (默认 you> ) */
    char mynick[32] = "you";
    int mynicklen = 3;

    while(1) {
        FD_ZERO(&readfds);
        FD_SET(s, &readfds);
        FD_SET(stdin_fd, &readfds);
        int maxfd = s > stdin_fd ? s : stdin_fd;

        int num_events = select(maxfd+1, &readfds, NULL, NULL, NULL);
        if (num_events == -1) {
            perror("select() error");
            exit(1);
        } else if (num_events) {
            char buf[1024];

            if (FD_ISSET(s, &readfds)) {
                ssize_t count = read(s,buf,sizeof(buf));
                if (count <= 0) {
                    printf("Connection lost\n");
                    exit(1);
                }
                /* 终端通知: 仅真实消息(以昵称开头), 跳过 [系统] 和历史回放。
                 * 兼容性三连发: 铃音 + OSC9(iTerm2等) + OSC777(Ghostty/WezTerm)。
                 * 不支持的终端会静默忽略这些序列。 */
                if (count > 0 && buf[0] != '[') {
                    int blen = 0;
                    while (blen < count && blen < 60 && buf[blen] != '\n') blen++;
                    char nbody[64];
                    memcpy(nbody, buf, blen);
                    nbody[blen] = 0;
                    char nseq[192];
                    int nlen = snprintf(nseq, sizeof(nseq),
                        "\a\e]9;%s\a\e]777;notify;Chat;%s\a", nbody, nbody);
                    if (nlen < (int)sizeof(nseq)) /* 截断会损坏转义序列, 宁可不发 */
                        write(fileno(stdout), nseq, nlen);
                }
                inputBufferHide(&ib);
                write(fileno(stdout),buf,count);
                inputBufferShow(&ib);
            } else if (FD_ISSET(stdin_fd, &readfds)) {
                ssize_t count = read(stdin_fd,buf,sizeof(buf));
                for (int j = 0; j < count; j++) {
                    int res = inputBufferFeedChar(&ib,buf[j]);
                    switch(res) {
                    case IB_GOTLINE:
                        /* 本地跟踪昵称: /nick 后回显前缀跟着变 */
                        if (ib.len > 6 && !memcmp(ib.buf, "/nick ", 6)) {
                            int alen = ib.len - 6; /* 去掉 "/nick " */
                            if (alen > 0 && alen < (int)sizeof(mynick)) {
                                memcpy(mynick, ib.buf + 6, alen);
                                mynick[alen] = 0;
                                mynicklen = alen;
                            }
                        }
                        inputBufferAppend(&ib,'\n');
                        inputBufferHide(&ib);
                        write(fileno(stdout),mynick,mynicklen);
                        write(fileno(stdout),"> ", 2);
                        write(fileno(stdout),ib.buf,ib.len);
                        write(s,ib.buf,ib.len);
                        inputBufferClear(&ib);
                        break;
                    case IB_OK:
                        break;
                    }
                }
            }
        }
    }

    close(s);
    return 0;
}
