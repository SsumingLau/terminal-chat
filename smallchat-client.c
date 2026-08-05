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

#define _POSIX_C_SOURCE 200809L /* Linux 严格 C99 下需要, 否则 fileno() 不声明 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/select.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>

#include "chatlib.h"
#include "linenoise.h"

/* 若终端窗口尺寸为 0 (部分终端/伪终端), linenoise 会走 \e[6n 查询回退路径:
 * 无超时会挂死, 且会吃掉用户已输入的首字符。主动设成 80x24, 让它走 ioctl
 * 路径, 彻底绕开这条有缺陷的路径。getColumns() 查的是 fd 1, 所以两个都设。 */
static void ensureWinsize(void) {
    struct winsize ws;
    for (int fd = STDIN_FILENO; fd <= STDOUT_FILENO; fd++) {
        if (ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col == 0) {
            ws.ws_col = 80;
            ws.ws_row = 24;
            ioctl(fd, TIOCSWINSZ, &ws);
        }
    }
}

/* 行编辑用 linenoise (antirez 同作者的库, redis-cli 同款):
 *   - UTF-8 按字符移动光标, 中文不会碎
 *   - 方向键/Home/End/Delete/退格齐全, ↑↓ 还有历史
 *   - 多路复用 API (EditStart/Feed/Stop + Hide/Show) 适配 select 循环,
 *     消息到达时隐藏输入行, 打印完恢复 */

int main(int argc, char **argv) {
    if (argc != 3) {
        printf("Usage: %s <host> <port>\n", argv[0]);
        exit(1);
    }

    /* Create a TCP connection with the server. */
    int s = TCPConnect(argv[1], atoi(argv[2]), 0);
    if (s == -1) {
        perror("Connecting to server");
        exit(1);
    }

    /* 提示符: 默认 you>, /nick 后变为 <昵称>> */
    char prompt[64];
    snprintf(prompt, sizeof(prompt), "you> ");
    ensureWinsize();

    struct linenoiseState l;
    char linebuf[512];
    int editing = 0;  /* 当前是否在编辑输入行 */
    int stdin_fd = fileno(stdin);
    FILE *dbgf = NULL;
    if (getenv("CHAT_DEBUG")) {
        dbgf = fopen("/tmp/chatdbg.log", "w");
        if (dbgf) setvbuf(dbgf, NULL, _IONBF, 0);
    }

    while (1) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(s, &readfds);
        FD_SET(stdin_fd, &readfds);
        int maxfd = s > stdin_fd ? s : stdin_fd;

        int num_events = select(maxfd + 1, &readfds, NULL, NULL, NULL);
        if (dbgf)
            fprintf(dbgf, "sel=%d s=%d in=%d edit=%d\n",
                    num_events,
                    FD_ISSET(s, &readfds) ? 1 : 0,
                    FD_ISSET(stdin_fd, &readfds) ? 1 : 0,
                    editing);
        if (num_events == -1) {
            perror("select() error");
            exit(1);
        } else if (num_events) {
            char buf[1024]; /* Generic buffer for both code paths. */

            if (FD_ISSET(s, &readfds)) {
                /* Data from the server? */
                ssize_t count = read(s, buf, sizeof(buf));
                if (dbgf) fprintf(dbgf, "  sock_read=%zd\n", count);
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
                if (editing) linenoiseHide(&l);
                write(fileno(stdout), buf, count);
                if (editing) linenoiseShow(&l);
            } else if (FD_ISSET(stdin_fd, &readfds)) {
                /* 用户输入: 交给 linenoise 编辑 */
                if (!editing) {
                    int r = linenoiseEditStart(&l, stdin_fd, fileno(stdout),
                                           linebuf, sizeof(linebuf), prompt);
                    if (dbgf) fprintf(dbgf, "  EditStart=%d\n", r);
                    if (r == -1) {
                        perror("linenoiseEditStart");
                        exit(1);
                    }
                    editing = 1;
                }
                char *res = linenoiseEditFeed(&l);
                if (dbgf)
                    fprintf(dbgf, "  feed=%s errno=%d\n",
                            res == NULL ? "NULL" :
                            res == linenoiseEditMore ? "MORE" : "LINE",
                            errno);
                if (res == NULL && errno == EINTR) {
                    /* 被信号(如 SIGWINCH)打断的读: 不是真退出, 继续编辑 */
                    if (dbgf) fprintf(dbgf, "  EINTR -> continue\n");
                    continue;
                }
                if (res == NULL) {
                    /* Ctrl+C / Ctrl+D: 退出 */
                    printf("\nBye.\n");
                    exit(0);
                }
                if (res == linenoiseEditMore) continue; /* 还没输完 */

                /* 一行输入完成 */
                linenoiseEditStop(&l);
                editing = 0;
                linenoiseHistoryAdd(res);

                /* 本地跟踪昵称: /nick 后提示符跟着变 */
                if (!strncmp(res, "/nick ", 6) && res[6])
                    snprintf(prompt, sizeof(prompt), "%s> ", res + 6);

                write(s, res, strlen(res));
                write(s, "\n", 1);
                free(res);
            }
        }
    }
    close(s);
    return 0;
}
