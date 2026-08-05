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

/* 设计说明: 基于 antirez 原版架构 (select + 字节级输入缓冲)。
 * 行编辑交给 ledit.c: UTF-8 感知的方向键/Home/End/Delete/退格/历史。
 * 改动:
 *   1. 输入行带 prompt (mynick> ), /nick 后跟随更新
 *   2. /nick 后回显前缀变为 <昵称>>, 新消息终端通知 (OSC9+OSC777+铃音)
 *   3. Ctrl-D (空行) 退出 */

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
#include "ledit.h"

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

    /* 行编辑器: UTF-8 感知的方向键/删除/历史 (ledit.c) */
    LEdit *e = ledit_new();
    ledit_set_prompt(e, "you> ");

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
                ledit_hide(e);
                write(fileno(stdout),buf,count);
                ledit_show(e);
            } else if (FD_ISSET(stdin_fd, &readfds)) {
                ssize_t count = read(stdin_fd,buf,sizeof(buf));
                for (int j = 0; j < count; j++) {
                    int res = ledit_feed(e,(unsigned char)buf[j]);
                    switch(res) {
                    case 1: /* 行就绪 */
                        /* 本地跟踪昵称: /nick 后回显前缀跟着变 */
                        if (e->len > 6 && !memcmp(e->buf, "/nick ", 6)) {
                            int alen = e->len - 6; /* 去掉 "/nick " */
                            if (alen > 0 && alen < 28) { /* 留出 "> " 和 NUL */
                                memcpy(mynick, e->buf + 6, alen);
                                mynick[alen] = 0;
                                mynicklen = alen;
                                char np[32];
                                snprintf(np, sizeof(np), "%s> ", mynick);
                                ledit_set_prompt(e, np);
                            }
                        }
                        ledit_hide(e);
                        write(fileno(stdout),mynick,mynicklen);
                        write(fileno(stdout),"> ", 2);
                        write(fileno(stdout),e->buf,e->len);
                        write(fileno(stdout),"\n", 1);
                        write(s,e->buf,e->len);
                        write(s,"\n", 1);
                        ledit_accept(e);
                        break;
                    case -1: /* EOF: 空行 Ctrl-D */
                        write(fileno(stdout),"\n", 1);
                        exit(0);
                    default:
                        break;
                    }
                }
            }
        }
    }

    close(s);
    return 0;
}
