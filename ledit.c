/* ledit.c -- 极简 UTF-8 行编辑器。
 *
 * 设计: 字节缓冲 + 字符光标 + 全行重绘 (每次按键 \r\e[K 后整行重画)。
 * 全行重绘让宽度错误最多视觉错位, 不会状态错乱。
 * 方向键/Home/End/Delete/退格/上下历史/Ctrl-A E K U W, 汉字按字符移动删除。
 *
 * ponytail: 输入行超过终端宽度时折行残留会错乱, 聊天场景行短, 不做滚动。
 * ponytail: emoji/ZWJ 宽度依赖系统 wcwidth, macOS 对复杂 emoji 可能算错, 可接受。
 *
 * 自检:   cc ledit.c -DLEDIT_TEST && ./a.out
 * 手感:   cc ledit.c -DLEDIT_DEMO && ./a.out   (raw mode 试编辑)
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <locale.h>
#include <unistd.h>
#include <assert.h>

#include "ledit.h"

/* ================= UTF-8 工具 ================= */

static int utf8_len(unsigned char b) {
    if (b < 0x80) return 1;
    if (b >= 0xc2 && b <= 0xdf) return 2;
    if (b >= 0xe0 && b <= 0xef) return 3;
    if (b >= 0xf0 && b <= 0xf4) return 4;
    return 1; /* 孤立续字节/非法: 当单字节 */
}

/* buf 上 p 处字符的显示宽度 (列数) */
static int char_width(const char *b, int len, int p) {
    int n = utf8_len((unsigned char)b[p]);
    if (p + n > len) n = 1;
    mbstate_t st = {0};
    wchar_t wc;
    if (mbrtowc(&wc, b + p, n, &st) != (size_t)n) return 1;
    int w = wcwidth(wc);
    return w < 0 ? 1 : w;
}

/* ================= 光标 / 编辑 ================= */

static void cursor_left(LEdit *e) {
    if (e->pos <= 0) { write(1, "\a", 1); return; }
    int p = e->pos - 1;
    while (p > 0 && (unsigned char)e->buf[p] >= 0x80 &&
           (unsigned char)e->buf[p] <= 0xbf) p--;  /* 跳续字节到字符起点 */
    e->pos = p;
}

static void cursor_right(LEdit *e) {
    if (e->pos >= e->len) { write(1, "\a", 1); return; }
    int n = utf8_len((unsigned char)e->buf[e->pos]);
    if (e->pos + n > e->len) n = 1;
    e->pos += n;
}

/* 退格: 删光标前一个字符 */
static void del_before(LEdit *e) {
    if (e->pos <= 0) { write(1, "\a", 1); return; }
    int p = e->pos - 1;
    while (p > 0 && (unsigned char)e->buf[p] >= 0x80 &&
           (unsigned char)e->buf[p] <= 0xbf) p--;
    memmove(e->buf + p, e->buf + e->pos, e->len - e->pos);
    e->len -= e->pos - p;
    e->pos = p;
    e->buf[e->len] = 0;
}

/* Delete: 删光标处一个字符 */
static void del_at(LEdit *e) {
    if (e->pos >= e->len) { write(1, "\a", 1); return; }
    int n = utf8_len((unsigned char)e->buf[e->pos]);
    if (e->pos + n > e->len) n = 1;
    memmove(e->buf + e->pos, e->buf + e->pos + n, e->len - e->pos - n);
    e->len -= n;
    e->buf[e->len] = 0;
}

/* Ctrl-W: 删光标前一个词 (空白分隔), 边界对齐字符 */
static void word_del(LEdit *e) {
    int p = e->pos;
    while (p > 0 && e->buf[p-1] == ' ') p--;
    while (p > 0 && e->buf[p-1] != ' ') p--;
    while (p > 0 && p < e->pos && (unsigned char)e->buf[p] >= 0x80 &&
           (unsigned char)e->buf[p] <= 0xbf) p--;
    if (p == e->pos) { write(1, "\a", 1); return; }
    memmove(e->buf + p, e->buf + e->pos, e->len - e->pos);
    e->len -= e->pos - p;
    e->pos = p;
    e->buf[e->len] = 0;
}

static void insert_byte(LEdit *e, int c) {
    if (e->len >= LEDIT_MAX) { write(1, "\a", 1); return; }
    memmove(e->buf + e->pos + 1, e->buf + e->pos, e->len - e->pos);
    e->buf[e->pos] = c;
    e->pos++;
    e->len++;
    e->buf[e->len] = 0;
}

/* ================= 历史 ================= */

static void hist_load(LEdit *e, int idx) {
    int l = strlen(e->hist[idx]);
    memcpy(e->buf, e->hist[idx], l);
    e->buf[l] = 0;
    e->len = l;
    e->pos = l;
}

static void hist_prev(LEdit *e) {
    if (e->hlen == 0) { write(1, "\a", 1); return; }
    if (e->hcur < 0) {                 /* 编辑模式 -> 进入历史 */
        e->savlen = e->len;
        memcpy(e->saved, e->buf, e->len);
        e->hcur = e->hlen - 1;
    } else if (e->hcur == 0) {
        write(1, "\a", 1);
        return;
    } else {
        e->hcur--;
    }
    hist_load(e, e->hcur);
}

static void hist_next(LEdit *e) {
    if (e->hcur < 0) return;
    e->hcur++;
    if (e->hcur >= e->hlen) {          /* 越过最新 -> 回编辑行 */
        e->hcur = -1;
        memcpy(e->buf, e->saved, e->savlen);
        e->buf[e->savlen] = 0;
        e->len = e->savlen;
        e->pos = e->savlen;
    } else {
        hist_load(e, e->hcur);
    }
}

/* ================= ESC 序列 ================= */

static void esc_feed(LEdit *e, int c) {
    if (e->esc == 1) {                 /* 刚收 ESC, 等 [ 或 O */
        e->csilen = 0;
        e->esc = (c == '[' || c == 'O') ? 2 : 0;
        return;
    }
    if (c >= 0x30 && c <= 0x3f) {      /* CSI 参数字节 */
        if (e->csilen < 4) e->csi[e->csilen++] = c;
        return;
    }
    int p0 = e->csilen ? e->csi[0] : 0;
    e->esc = 0;
    switch (c) {
    case 'A': hist_prev(e); break;         /* 上 */
    case 'B': hist_next(e); break;         /* 下 */
    case 'C': cursor_right(e); break;      /* 右 */
    case 'D': cursor_left(e); break;       /* 左 */
    case 'H': e->pos = 0; break;           /* Home */
    case 'F': e->pos = e->len; break;      /* End */
    case '~':
        if (p0 == '1' || p0 == '7') {      /* \e[1~ \e[7~ Home */
            e->pos = 0;
        } else if (p0 == '3') {            /* Delete */
            del_at(e);
        } else if (p0 == '4' || p0 == '8') {
            e->pos = e->len;               /* End */
        }
        break;
    default: break;                        /* 其它序列吞掉 */
    }
}

/* ================= 渲染 ================= */

static void widths(const LEdit *e, int *total, int *before) {
    int t = e->promptw, b = e->promptw, p = 0;
    while (p < e->len) {
        int w = char_width(e->buf, e->len, p);
        if (p < e->pos) b += w;
        t += w;
        p += utf8_len((unsigned char)e->buf[p]);
    }
    *total = t;
    *before = b;
}

static void redraw(const LEdit *e) {
    char out[LEDIT_MAX + 96];
    int n = 0, total, before;
    widths(e, &total, &before);
    out[n++] = '\r';
    out[n++] = 0x1b; out[n++] = '['; out[n++] = 'K';
    memcpy(out + n, e->prompt, e->promptw); n += e->promptw;
    memcpy(out + n, e->buf, e->len); n += e->len;
    int back = total - before;             /* 光标移回: 总宽 - 光标前宽 */
    if (back > 0) {
        out[n++] = 0x1b; out[n++] = '[';
        n += snprintf(out + n, sizeof(out) - n, "%dD", back);
    }
    write(1, out, n);
}

/* ================= 公共 API ================= */

LEdit *ledit_new(void) {
    LEdit *e = calloc(1, sizeof(*e));
    setlocale(LC_CTYPE, "");               /* 让 wcwidth 认识 CJK */
    e->hcur = -1;
    return e;
}

void ledit_free(LEdit *e) {
    free(e);
}

int ledit_feed(LEdit *e, int c) {
    c &= 0xff;  /* 调用方可能传 signed char (0xe4 会变负数), 归一化 */
    if (e->esc) { esc_feed(e, c); redraw(e); return 0; }
    if (c == 0x1b) { e->esc = 1; return 0; }

    switch (c) {
    case '\r': return 1;
    case '\n': return 0;               /* 忽略 */
    case 0x08: case 0x7f:              /* 退格 */
        del_before(e);
        break;
    case 0x01:                         /* Ctrl-A 行首 */
        e->pos = 0;
        break;
    case 0x05:                         /* Ctrl-E 行尾 */
        e->pos = e->len;
        break;
    case 0x0b:                         /* Ctrl-K 删到行尾 */
        if (e->pos < e->len) { e->len = e->pos; e->buf[e->len] = 0; }
        break;
    case 0x15:                         /* Ctrl-U 删整行 */
        if (e->len) { e->len = e->pos = 0; e->buf[0] = 0; }
        break;
    case 0x17:                         /* Ctrl-W 删词 */
        word_del(e);
        break;
    case 0x04:                         /* Ctrl-D */
        if (e->len == 0) return -1;    /* 空行 EOF, 非空忽略 */
        return 0;
    default:
        if (c < 0x20) return 0;        /* 其余控制字节: 忽略 (防终端注入) */
        insert_byte(e, c);
        break;
    }
    redraw(e);
    return 0;
}

const char *ledit_line(LEdit *e) {
    return e->buf;
}

int ledit_accept(LEdit *e) {
    if (e->len > 0) {
        if (e->hlen == 0 || strcmp(e->hist[e->hlen-1], e->buf) != 0) {
            if (e->hlen < LEDIT_HIST) {
                memcpy(e->hist[e->hlen++], e->buf, e->len + 1);
            } else {
                memmove(e->hist[0], e->hist[1], (LEDIT_HIST-1) * LEDIT_MAX);
                memcpy(e->hist[LEDIT_HIST-1], e->buf, e->len + 1);
            }
        }
    }
    e->len = e->pos = 0;
    e->buf[0] = 0;
    e->hcur = -1;
    redraw(e);
    return e->len;
}

void ledit_hide(LEdit *e) {
    (void)e;
    write(1, "\r\x1b[2K", 5);
}

void ledit_show(LEdit *e) {
    redraw(e);
}

void ledit_set_prompt(LEdit *e, const char *p) {
    int n = strlen(p);
    if (n > (int)sizeof(e->prompt) - 1) n = sizeof(e->prompt) - 1;
    memcpy(e->prompt, p, n);
    e->prompt[n] = 0;
    e->promptw = n;   /* 要求 ASCII; CJK prompt 宽度会算错, 聊天场景不出现 */
}

/* ================= 自检 ================= */

#ifdef LEDIT_TEST
int main(void) {
    LEdit *e = ledit_new();

    /* UTF-8 工具 */
    assert(utf8_len('a') == 1);
    assert(utf8_len(0xe4) == 3);            /* '中' 首字节 */
    assert(utf8_len(0xb8) == 1);            /* 孤立续字节 */
    const char *zh = "\xe4\xb8\xad";        /* '中' */
    assert(char_width(zh, 3, 0) == 2);
    assert(char_width("a", 1, 0) == 1);

    /* 插入 + 方向键 + 退格 (删光标前一个字符) */
    ledit_feed(e, 'a');
    ledit_feed(e, 'b');
    assert(e->len == 2 && e->pos == 2);
    ledit_feed(e, 0x1b); ledit_feed(e, '['); ledit_feed(e, 'D');   /* 左 */
    assert(e->pos == 1);
    ledit_feed(e, 0x1b); ledit_feed(e, '['); ledit_feed(e, 'D');
    assert(e->pos == 0);
    ledit_feed(e, 0x1b); ledit_feed(e, '['); ledit_feed(e, 'C');   /* 右 */
    assert(e->pos == 1);
    ledit_feed(e, 0x7f);                                            /* 删 'a' */
    assert(e->len == 1 && e->pos == 0 && e->buf[0] == 'b');

    /* 汉字整删 (退格跨 3 字节) */
    ledit_feed(e, 0x15);                                            /* Ctrl-U 清空 */
    ledit_feed(e, 'b');
    for (int i = 0; i < 3; i++) ledit_feed(e, (unsigned char)zh[i]);
    assert(e->len == 4 && e->pos == 4);                             /* "b中" */
    ledit_feed(e, 0x7f);                                            /* 删 '中' */
    assert(e->len == 1 && e->pos == 1 && e->buf[0] == 'b');

    /* Delete 删光标处 */
    ledit_feed(e, 0x15);                                            /* Ctrl-U 清空 */
    ledit_feed(e, 'b');
    for (int i = 0; i < 3; i++) ledit_feed(e, (unsigned char)zh[i]);
    assert(e->len == 4);                                            /* "b中" */
    ledit_feed(e, 0x1b); ledit_feed(e, '['); ledit_feed(e, 'D');   /* pos=1 */
    assert(e->pos == 1);
    ledit_feed(e, 0x1b); ledit_feed(e, '['); ledit_feed(e, '3');
    ledit_feed(e, '~');                                             /* 删光标处 '中' */
    assert(e->len == 1 && e->pos == 1);

    /* Home/End */
    ledit_feed(e, 'x'); ledit_feed(e, 'y');                          /* "bxy" */
    ledit_feed(e, 0x1b); ledit_feed(e, '['); ledit_feed(e, 'H');
    assert(e->pos == 0);
    ledit_feed(e, 0x1b); ledit_feed(e, '['); ledit_feed(e, 'F');
    assert(e->pos == 3);

    /* 历史: 上/下翻 */
    ledit_feed(e, 0x15);                                            /* Ctrl-U 清空 */
    assert(e->len == 0);
    const char *m1 = "hello";
    for (const char *p = m1; *p; p++) ledit_feed(e, *p);
    assert(ledit_feed(e, '\r') == 1);
    ledit_accept(e);
    const char *m2 = "world";
    for (const char *p = m2; *p; p++) ledit_feed(e, *p);
    assert(ledit_feed(e, '\r') == 1);
    ledit_accept(e);
    ledit_feed(e, 0x1b); ledit_feed(e, '['); ledit_feed(e, 'A');   /* 上 */
    assert(strcmp(e->buf, "world") == 0);
    ledit_feed(e, 0x1b); ledit_feed(e, '['); ledit_feed(e, 'A');
    assert(strcmp(e->buf, "hello") == 0);
    ledit_feed(e, 0x1b); ledit_feed(e, '['); ledit_feed(e, 'B');   /* 下 */
    assert(strcmp(e->buf, "world") == 0);
    ledit_feed(e, 0x1b); ledit_feed(e, '['); ledit_feed(e, 'B');   /* 回编辑行 */
    assert(e->len == 0);

    /* EOF */
    assert(ledit_feed(e, 0x04) == -1);

    printf("ledit: all tests passed\n");
    ledit_free(e);
    return 0;
}
#endif

/* ================= 手感 demo ================= */

#ifdef LEDIT_DEMO
#include <termios.h>

static void rawmode(int on) {
    static struct termios orig;
    struct termios raw;
    if (on) {
        tcgetattr(0, &orig);
        raw = orig;
        raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
        raw.c_lflag &= ~(ECHO | ICANON | IEXTEN);
        raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0;
        tcsetattr(0, TCSAFLUSH, &raw);
    } else {
        tcsetattr(0, TCSAFLUSH, &orig);
    }
}

int main(void) {
    LEdit *e = ledit_new();
    ledit_set_prompt(e, "demo> ");
    rawmode(1);
    ledit_show(e);
    while (1) {
        unsigned char c;
        if (read(0, &c, 1) <= 0) break;
        int r = ledit_feed(e, c);
        if (r == 1) {
            ledit_hide(e);
            printf("got: %s\n", ledit_line(e));
            ledit_accept(e);
        } else if (r == -1) {
            break;
        }
    }
    rawmode(0);
    printf("bye\n");
    ledit_free(e);
    return 0;
}
#endif
