/* ledit.h -- 极简 UTF-8 行编辑器。
 *
 * 字节缓冲 + 字符光标 + 全行重绘, wcwidth 列宽。
 * 宿主负责: raw mode、select 循环、hide/show 时机 (消息到达时)。
 * 零依赖, 仅 libc。
 */

#ifndef LEDIT_H
#define LEDIT_H

#define LEDIT_MAX 512   /* 输入行字节上限 */
#define LEDIT_HIST 16   /* 历史条数 */

typedef struct LEdit {
    char buf[LEDIT_MAX];
    int len;                    /* 字节长度 */
    int pos;                    /* 光标字节偏移 */
    char prompt[32];            /* 输入前缀, 只支持 ASCII */
    int promptw;
    char hist[LEDIT_HIST][LEDIT_MAX];
    int hlen;
    int hcur;                   /* -1=编辑新行, 否则浏览历史 */
    char saved[LEDIT_MAX];      /* 进历史前保存的编辑行 */
    int savlen;
    int esc;                    /* ESC 序列状态机: 0=无 1=ESC 2=CSI/SS3 */
    int csi[4], csilen;         /* CSI 参数字节 (用于 1~/3~ 等) */
} LEdit;

LEdit *ledit_new(void);
void ledit_free(LEdit *e);
/* 喂一个字节。返回 0=继续, 1=整行就绪 (\r), -1=EOF (Ctrl-D 且行为空) */
int ledit_feed(LEdit *e, int c);
const char *ledit_line(LEdit *e);       /* feed 返回 1 后取行 (NUL 结尾) */
/* 提交当前行进历史并清空, 重绘空行。返回新行长度 (总是 0) */
int ledit_accept(LEdit *e);
void ledit_hide(LEdit *e);              /* 清掉输入行 */
void ledit_show(LEdit *e);              /* 重绘输入行 */
void ledit_set_prompt(LEdit *e, const char *p);

#endif
