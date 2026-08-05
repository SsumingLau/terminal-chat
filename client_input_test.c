/* esc_test.c -- 用真实 inputBufferFeedChar 验证控制序列吞除。
 * 编译: cc -Dmain=chat_real_main smallchat-client.c chatlib.c esc_test.c -o esc_test */
#include <stdio.h>
#include <string.h>
#include <assert.h>

/* 与 smallchat-client.c 中相同的定义 */
#define IB_MAX 128
struct InputBuffer {
    char buf[IB_MAX];
    int len;
    int pos;
};

int inputBufferFeedChar(struct InputBuffer *ib, int c);
void inputBufferClear(struct InputBuffer *ib);

int main(void) {
    struct InputBuffer ib;
    inputBufferClear(&ib);

    /* 1. 方向键 \e[A (3字节) + 文字, 序列应被吞, 文字进入 */
    const char *seq1 = "\033[Aabc";
    for (const char *p = seq1; *p; p++) inputBufferFeedChar(&ib, *p);
    assert(ib.len == 3 && memcmp(ib.buf, "abc", 3) == 0);

    /* 2. Delete \e[3~ (4字节) + 文字 */
    const char *seq2 = "\033[3~def";
    for (const char *p = seq2; *p; p++) inputBufferFeedChar(&ib, *p);
    assert(ib.len == 6 && memcmp(ib.buf, "abcdef", 6) == 0);

    /* 3. 8位 CSI 方向键 \x9bA (2字节) + 文字 */
    const char *seq3 = "\x9b" "Aghi";
    for (const char *p = seq3; *p; p++) inputBufferFeedChar(&ib, *p);
    assert(ib.len == 9 && memcmp(ib.buf, "abcdefghi", 9) == 0);

    /* 4. 普通字符 + 退格 (0x7f 和 0x08 都要能删) */
    inputBufferClear(&ib);
    inputBufferFeedChar(&ib, 'x'); inputBufferFeedChar(&ib, 'y');
    inputBufferFeedChar(&ib, 0x7f); inputBufferFeedChar(&ib, 0x08);
    assert(ib.len == 0);

    /* 5. 含 0x9b 续字节的中文不能被吞 ("一些"=E4 B8 80 E4 BA 9B) */
    inputBufferClear(&ib);
    const char *cn = "一些";
    for (const char *p = cn; *p; p++) inputBufferFeedChar(&ib, *p);
    assert(ib.len == 6 && memcmp(ib.buf, "一些", 6) == 0);

    /* 6. 8位CSI + 中文混合: 只吞序列, 文字完整 */
    inputBufferClear(&ib);
    const char *mix = "\x9b" "A好";
    for (const char *p = mix; *p; p++) inputBufferFeedChar(&ib, *p);
    assert(ib.len == 3 && memcmp(ib.buf, "好", 3) == 0);

    /* 7. ESC方向键 + 中文混合 */
    inputBufferClear(&ib);
    const char *mix2 = "\033[A很好";
    for (const char *p = mix2; *p; p++) inputBufferFeedChar(&ib, *p);
    assert(ib.len == 6 && memcmp(ib.buf, "很好", 6) == 0);

    /* 8. 左右方向键移动光标 + 中间插入 */
    inputBufferClear(&ib);
    const char *s8 = "abc";
    for (const char *p = s8; *p; p++) inputBufferFeedChar(&ib, *p);
    const char *left = "\033[D";  /* ESC [ D 左移 */
    for (const char *p = left; *p; p++) inputBufferFeedChar(&ib, *p);
    assert(ib.pos == 2);
    inputBufferFeedChar(&ib, 'x'); /* 在 b 和 c 之间插入 */
    assert(ib.len == 4 && ib.pos == 3 && memcmp(ib.buf, "abxc", 4) == 0);
    const char *right = "\033[C"; /* ESC [ C 右移 */
    for (const char *p = right; *p; p++) inputBufferFeedChar(&ib, *p);
    assert(ib.pos == 4);

    /* 9. Delete 键 (\e[3~) 删除光标处字符 */
    inputBufferClear(&ib);
    const char *s9 = "abcd";
    for (const char *p = s9; *p; p++) inputBufferFeedChar(&ib, *p);
    for (const char *p = left; *p; p++) inputBufferFeedChar(&ib, *p); /* pos 3 */
    const char *del = "\033[3~";
    for (const char *p = del; *p; p++) inputBufferFeedChar(&ib, *p); /* 删 'd' */
    assert(ib.len == 3 && ib.pos == 3 && memcmp(ib.buf, "abc", 3) == 0);

    /* 10. Home/End */
    inputBufferClear(&ib);
    const char *s10 = "abc";
    for (const char *p = s10; *p; p++) inputBufferFeedChar(&ib, *p);
    const char *home = "\033[H";
    for (const char *p = home; *p; p++) inputBufferFeedChar(&ib, *p);
    assert(ib.pos == 0);
    inputBufferFeedChar(&ib, 'X');
    assert(ib.len == 4 && memcmp(ib.buf, "Xabc", 4) == 0);
    const char *end = "\033[F";
    for (const char *p = end; *p; p++) inputBufferFeedChar(&ib, *p);
    assert(ib.pos == 4);

    /* 11. 8位CSI 左方向键 (0x9b D) */
    inputBufferClear(&ib);
    const char *s11 = "abcd";
    for (const char *p = s11; *p; p++) inputBufferFeedChar(&ib, *p);
    const char *left8 = "\x9b" "D";
    for (const char *p = left8; *p; p++) inputBufferFeedChar(&ib, *p);
    assert(ib.pos == 3);

    printf("ALL_PASS\n");
    return 0;
}
