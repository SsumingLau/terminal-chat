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

/* Raw mode: 1960 magic shit. */
int setRawMode(int fd, int enable) {
    /* We have a bit of global state (but local in scope) here.
     * This is needed to correctly set/undo raw mode. */
    static struct termios orig_termios; // Save original terminal status here.
    static int atexit_registered = 0;   // Avoid registering atexit() many times.
    static int rawmode_is_set = 0;      // True if raw mode was enabled.

    struct termios raw;

    /* If enable is zero, we just have to disable raw mode if it is
     * currently set. */
    if (enable == 0) {
        /* Don't even check the return value as it's too late. */
        if (rawmode_is_set && tcsetattr(fd,TCSAFLUSH,&orig_termios) != -1)
            rawmode_is_set = 0;
        return 0;
    }

    /* Enable raw mode. */
    if (!isatty(fd)) goto fatal;
    if (!atexit_registered) {
        atexit(disableRawModeAtExit);
        atexit_registered = 1;
    }
    if (tcgetattr(fd,&orig_termios) == -1) goto fatal;

    raw = orig_termios;  /* modify the original mode */
    /* input modes: no break, no CR to NL, no parity check, no strip char,
     * no start/stop output control. */
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    /* output modes - do nothing. We want post processing enabled so that
     * \n will be automatically translated to \r\n. */
    // raw.c_oflag &= ...
    /* control modes - set 8 bit chars */
    raw.c_cflag |= (CS8);
    /* local modes - choing off, canonical off, no extended functions,
     * but take signal chars (^Z,^C) enabled. */
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN);
    /* control chars - set return condition: min number of bytes and timer.
     * We want read to return every single byte, without timeout. */
    raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0; /* 1 byte, no timer */

    /* put terminal in raw mode after flushing */
    if (tcsetattr(fd,TCSAFLUSH,&raw) < 0) goto fatal;
    rawmode_is_set = 1;
    return 0;

fatal:
    errno = ENOTTY;
    return -1;
}

/* At exit we'll try to fix the terminal to the initial conditions. */
void disableRawModeAtExit(void) {
    setRawMode(STDIN_FILENO,0);
}

/* ============================================================================
 * Mininal line editing.
 * ========================================================================== */

void terminalCleanCurrentLine(void) {
    write(fileno(stdout),"\e[2K",4);
}

void terminalCursorAtLineStart(void) {
    write(fileno(stdout),"\r",1);
}

#define IB_MAX 128
struct InputBuffer {
    char buf[IB_MAX];       // Buffer holding the data.
    int len;                // Current length.
    int pos;                // 光标位置 (0..len).
};

/* inputBuffer*() return values: */
#define IB_ERR 0        // Sorry, unable to comply.
#define IB_OK 1         // Ok, got the new char, did the operation, ...
#define IB_GOTLINE 2    // Hey, now there is a well formed line to read.

/* Append the specified character to the buffer. */
int inputBufferAppend(struct InputBuffer *ib, int c) {
    if (ib->len >= IB_MAX) return IB_ERR; // No room.

    ib->buf[ib->len] = c;
    ib->len++;
    return IB_OK;
}

void inputBufferHide(struct InputBuffer *ib);
void inputBufferShow(struct InputBuffer *ib);
void inputBufferRedraw(struct InputBuffer *ib);

/* Process every new keystroke arriving from the keyboard. As a side effect
 * the input buffer state is modified in order to reflect the current line
 * the user is typing, so that reading the input buffer 'buf' for 'len'
 * bytes will contain it. */
int inputBufferFeedChar(struct InputBuffer *ib, int c) {
    /* 终端控制序列 -> 行内编辑。收集 ESC[/ESC O/0x9b 开头的序列,
     * 到终结字节 (>=0x40) 后分派:
     *   左/右: 移动光标; Home/End: 行首/行尾; Delete(\e[3~): 删光标处;
     *   上/下: 无历史, 忽略。
     * 关键: 0x9b 既可能是 8位CSI 开头, 也可能是 UTF-8 续字节
     * ("些"=E4 BA 9B), 靠 utf8_pending 区分——多字节字符中间的
     * 0x9b 是文字, 不能当序列。 */
    static int utf8_pending = 0; /* 当前 UTF-8 字符还需多少个续字节 */
    static char seq[8];          /* 控制序列缓冲 */
    static int seql = 0;         /* 序列长度 */

    unsigned char u = (unsigned char)c;

    /* 序列开始: ESC, 或独立的 0x9b (8位CSI, 等价于 ESC [) */
    if (u == '\e' || (u == 0x9b && utf8_pending == 0)) {
        if (u == 0x9b) { seq[0] = '\e'; seq[1] = '['; seql = 2; }
        else           { seq[0] = '\e'; seql = 1; }
        utf8_pending = 0;
        return IB_OK;
    }
    if (seql > 0) {
        if (seql < (int)sizeof(seq)) seq[seql] = u;
        if (++seql > (int)sizeof(seq)) { seql = 0; return IB_OK; }
        /* ESC + 非 [ 非 O: 不是 CSI/SS3, 整个丢弃 */
        if (seql == 2 && seq[0] == '\e' && seq[1] != '[' && seq[1] != 'O') {
            seql = 0;
            return IB_OK;
        }
        if (u >= 0x40 && seql >= 3) { /* >=3: ESC+引导符([/O)+终结字节; '['本身也是0x5B不能算终结 */
            char *s = seq;
            int sl = seql;
            seql = 0;
            if (sl == 3 && (s[1] == '[' || s[1] == 'O')) {
                switch (s[2]) {
                case 'D': if (ib->pos > 0) { ib->pos--; inputBufferRedraw(ib); } return IB_OK; /* 左 */
                case 'C': if (ib->pos < ib->len) { ib->pos++; inputBufferRedraw(ib); } return IB_OK; /* 右 */
                case 'H': ib->pos = 0; inputBufferRedraw(ib); return IB_OK;           /* Home */
                case 'F': ib->pos = ib->len; inputBufferRedraw(ib); return IB_OK;     /* End */
                default:  return IB_OK; /* 上/下等, 无功能, 忽略 */
                }
            }
            if (sl == 4 && s[1] == '[' && s[2] == '3' && s[3] == '~') {
                /* Delete 键: 删除光标处字符 */
                if (ib->pos < ib->len) {
                    memmove(ib->buf + ib->pos, ib->buf + ib->pos + 1,
                            ib->len - ib->pos - 1);
                    ib->len--;
                    inputBufferRedraw(ib);
                }
                return IB_OK;
            }
            return IB_OK; /* 其他序列, 忽略 */
        }
        return IB_OK;
    }

    /* 维护 UTF-8 解码状态, 以便区分续字节与控制字节 */
    if (utf8_pending > 0) {
        utf8_pending--;
    } else if (u >= 0xc2 && u <= 0xdf) {
        utf8_pending = 1;
    } else if (u >= 0xe0 && u <= 0xef) {
        utf8_pending = 2;
    } else if (u >= 0xf0 && u <= 0xf4) {
        utf8_pending = 3;
    }

    switch(c) {
    case '\n':
        break;          // Ignored. We handle \r instead.
    case '\r':
        return IB_GOTLINE;
    case 8:             // Backspace (某些终端/Windows 发 0x08)
    case 127:           // Backspace (多数终端发 0x7f)
        if (ib->pos > 0) {
            memmove(ib->buf + ib->pos - 1, ib->buf + ib->pos,
                    ib->len - ib->pos);
            ib->len--;
            ib->pos--;
            inputBufferRedraw(ib);
        }
        break;
    default:
        if (ib->len < IB_MAX) {
            /* 在光标处插入 */
            memmove(ib->buf + ib->pos + 1, ib->buf + ib->pos,
                    ib->len - ib->pos);
            ib->buf[ib->pos] = c;
            ib->len++;
            ib->pos++;
            inputBufferRedraw(ib);
        }
        break;
    }
    return IB_OK;
}

/* Hide the line the user is typing. */
void inputBufferHide(struct InputBuffer *ib) {
    (void)ib; // Not used var, but is conceptually part of the API.
    terminalCleanCurrentLine();
    terminalCursorAtLineStart();
}

/* Show again the current line. Usually called after InputBufferHide(). */
void inputBufferShow(struct InputBuffer *ib) {
    write(fileno(stdout),ib->buf,ib->len);
    /* 光标不在行尾时, 移回到编辑位置 */
    if (ib->pos < ib->len) {
        char seq[16];
        int n = snprintf(seq,sizeof(seq),"\e[%dD",ib->len-ib->pos);
        write(fileno(stdout),seq,n);
    }
}

/* 清行重绘: 光标移动/插入/删除后刷新输入行显示 */
void inputBufferRedraw(struct InputBuffer *ib) {
    inputBufferHide(ib);
    inputBufferShow(ib);
}

/* Reset the buffer to be empty. */
void inputBufferClear(struct InputBuffer *ib) {
    ib->len = 0;
    ib->pos = 0;
    inputBufferHide(ib);
}

/* =============================================================================
 * Main program logic, finally :)
 * ========================================================================== */

int main(int argc, char **argv) {
    if (argc != 3) {
        printf("Usage: %s <host> <port>\n", argv[0]);
        exit(1);
    }

    /* Create a TCP connection with the server. */
    int s = TCPConnect(argv[1],atoi(argv[2]),0);
    if (s == -1) {
        perror("Connecting to server");
        exit(1);
    }

    /* Put the terminal in raw mode: this way we will receive every
     * single key stroke as soon as the user types it. No buffering
     * nor translation of escape sequences of any kind. */
    setRawMode(fileno(stdin),1);

    /* Wait for the standard input or the server socket to
     * have some data. */
    fd_set readfds;
    int stdin_fd = fileno(stdin);

    struct InputBuffer ib;
    inputBufferClear(&ib);

    /* 自己的昵称，/nick 后更新，用于回显前缀 (默认 you> ) */
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
            char buf[128]; /* Generic buffer for both code paths. */

            if (FD_ISSET(s, &readfds)) {
                /* Data from the server? */
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
                /* Data from the user typing on the terminal? */
                ssize_t count = read(stdin_fd,buf,sizeof(buf));
                for (int j = 0; j < count; j++) {
                    int res = inputBufferFeedChar(&ib,buf[j]);
                    switch(res) {
                    case IB_GOTLINE:
                        /* 本地跟踪昵称: 输入 /nick <名字> 后回显前缀跟着变 */
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
