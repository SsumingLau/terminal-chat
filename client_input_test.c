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

    printf("ALL_PASS\n");
    return 0;
}
