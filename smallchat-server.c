/* smallchat-server.c — 带房间、密码、聊天记录的终端聊天服务端
 *
 * 基于 antirez/smallchat 修改
 * 新增:
 *   - /join <房间> [密码]  加入/创建房间（首次进入即创建）
 *   - /leave              离开房间回到大厅
 *   - /rooms              列出所有房间
 *   - 聊天记录写入 ./logs/<房间>.log，带时间戳
 *   - 房间常驻：最后一人离开/断线后房间不销毁，直到服务器重启
 *   - 加入房间时回放最近 20 条历史记录
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <sys/select.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <strings.h>
#include "chatlib.h"

/* ============================ Data structures ========================== */

#define MAX_CLIENTS 1000
#define MAX_ROOMS 100
#define SERVER_PORT 7711

struct client {
    int fd;
    char *nick;
    char *room;   /* 当前房间名，NULL = 大厅 */
};

struct room {
    char *name;
    char *password;   /* NULL = 无密码 */
};

struct chatState {
    int serversock;
    int numclients;
    int maxclient;
    struct client *clients[MAX_CLIENTS];
    struct room *rooms[MAX_ROOMS];
    int numrooms;
};

struct chatState *Chat;

/* ============================ Room helpers ============================= */

/* 查找房间，返回 index，不存在返回 -1 */
int findRoom(const char *name) {
    for (int i = 0; i < Chat->numrooms; i++) {
        if (!strcasecmp(Chat->rooms[i]->name, name)) return i;
    }
    return -1;
}

/* 创建房间，返回 index */
int createRoom(const char *name, const char *password) {
    struct room *r = chatMalloc(sizeof(*r));
    int namelen = strlen(name);
    r->name = chatMalloc(namelen + 1);
    memcpy(r->name, name, namelen + 1);
    if (password && password[0]) {
        int pwlen = strlen(password);
        r->password = chatMalloc(pwlen + 1);
        memcpy(r->password, password, pwlen + 1);
    } else {
        r->password = NULL;
    }
    Chat->rooms[Chat->numrooms] = r;
    Chat->numrooms++;
    return Chat->numrooms - 1;
}

/* 注：房间不销毁（内存只占名字+密码，最多 MAX_ROOMS 个），因此
 * 没有 destroyRoom()/roomHasClients()。 */

/* ============================ Logging ================================== */

FILE *openLogFile(const char *room) {
    mkdir("logs", 0755);   /* best-effort, ignore failure */
    char path[512];
    if (room) {
        snprintf(path, sizeof(path), "logs/%s.log", room);
    } else {
        snprintf(path, sizeof(path), "logs/lobby.log");
    }
    return fopen(path, "a");
}

void logMessage(const char *room, const char *nick, const char *msg) {
    FILE *f = openLogFile(room);
    if (!f) return;
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm);
    fprintf(f, "[%s] %s> %s", ts, nick, msg);
    fclose(f);
}

/* strdup 的 C99 可移植版本（glibc 在 -std=c99 下不声明 strdup） */
static char *xstrdup(const char *s) {
    size_t len = strlen(s);
    char *d = chatMalloc(len + 1);
    memcpy(d, s, len + 1);
    return d;
}

/* 加入房间时，回放最近 REPLAY_LINES 条历史记录（读文件，不占内存） */
#define REPLAY_LINES 20
void replayRoomHistory(int fd, const char *room) {
    char path[512];
    snprintf(path, sizeof(path), "logs/%s.log", room);
    FILE *f = fopen(path, "r");
    if (!f) return;   /* 新房间还没有记录 */

    char *ring[REPLAY_LINES];
    memset(ring, 0, sizeof(ring));
    char buf[1024];
    int n = 0;
    while (fgets(buf, sizeof(buf), f)) {
        free(ring[n % REPLAY_LINES]);
        ring[n % REPLAY_LINES] = xstrdup(buf);
        n++;
    }
    fclose(f);
    if (n == 0) return;

    int shown = n > REPLAY_LINES ? REPLAY_LINES : n;
    char head[128];
    int len = snprintf(head, sizeof(head),
                       "--- %s 的历史记录 (最近 %d 条) ---\n", room, shown);
    write(fd, head, len);
    for (int i = n - shown; i < n; i++) {
        char *line = ring[i % REPLAY_LINES];
        if (line) write(fd, line, strlen(line));
    }
    char *tail = "--- 历史记录结束 ---\n";
    write(fd, tail, strlen(tail));

    for (int i = 0; i < REPLAY_LINES; i++) free(ring[i]);
}

/* ============================ Core ===================================== */

struct client *createClient(int fd) {
    char nick[32];
    int nicklen = snprintf(nick, sizeof(nick), "user:%d", fd);
    struct client *c = chatMalloc(sizeof(*c));
    socketSetNonBlockNoDelay(fd);
    c->fd = fd;
    c->nick = chatMalloc(nicklen + 1);
    memcpy(c->nick, nick, nicklen);
    c->room = NULL;   /* 大厅 */
    assert(Chat->clients[c->fd] == NULL);
    Chat->clients[c->fd] = c;
    if (c->fd > Chat->maxclient) Chat->maxclient = c->fd;
    Chat->numclients++;
    return c;
}

void freeClient(struct client *c) {
    free(c->nick);
    if (c->room) free(c->room);   /* 房间本身常驻，不销毁 */
    close(c->fd);
    Chat->clients[c->fd] = NULL;
    Chat->numclients--;
    if (Chat->maxclient == c->fd) {
        int j;
        for (j = Chat->maxclient - 1; j >= 0; j--) {
            if (Chat->clients[j] != NULL) {
                Chat->maxclient = j;
                break;
            }
        }
        if (j == -1) Chat->maxclient = -1;
    }
    free(c);
}

void initChat(void) {
    Chat = chatMalloc(sizeof(*Chat));
    memset(Chat, 0, sizeof(*Chat));
    Chat->maxclient = -1;
    Chat->numclients = 0;
    Chat->numrooms = 0;

    Chat->serversock = createTCPServer(SERVER_PORT);
    if (Chat->serversock == -1) {
        perror("Creating listening socket");
        exit(1);
    }
}

/* 发送消息给同一个房间的所有人（除了 excluded） */
void sendMsgToRoomBut(const char *room, int excluded, char *s, size_t len) {
    for (int j = 0; j <= Chat->maxclient; j++) {
        if (Chat->clients[j] == NULL || Chat->clients[j]->fd == excluded)
            continue;
        struct client *c = Chat->clients[j];
        /* 同一房间：双方 room 都为 NULL（大厅），或字符串相等 */
        int sameRoom = (c->room == NULL && room == NULL) ||
                       (c->room && room && !strcasecmp(c->room, room));
        if (!sameRoom) continue;
        write(c->fd, s, len);
    }
}

int main(void) {
    initChat();

    while (1) {
        fd_set readfds;
        struct timeval tv;
        int retval;

        FD_ZERO(&readfds);
        FD_SET(Chat->serversock, &readfds);
        for (int j = 0; j <= Chat->maxclient; j++) {
            if (Chat->clients[j]) FD_SET(j, &readfds);
        }

        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int maxfd = Chat->maxclient;
        if (maxfd < Chat->serversock) maxfd = Chat->serversock;
        retval = select(maxfd + 1, &readfds, NULL, NULL, &tv);
        if (retval == -1) {
            perror("select() error");
            exit(1);
        } else if (retval) {

            if (FD_ISSET(Chat->serversock, &readfds)) {
                int fd = acceptClient(Chat->serversock);
                struct client *c = createClient(fd);
                char *welcome_msg =
                    "=== Terminal Chat ===\n"
                    "/nick <昵称>     设置昵称\n"
                    "/join <房间> [密码] 加入/创建房间\n"
                    "/leave           离开房间回大厅\n"
                    "/rooms           查看所有房间\n"
                    "====================\n";
                write(c->fd, welcome_msg, strlen(welcome_msg));
                printf("Connected client fd=%d\n", fd);
            }

            char readbuf[512];
            for (int j = 0; j <= Chat->maxclient; j++) {
                if (Chat->clients[j] == NULL) continue;
                if (FD_ISSET(j, &readfds)) {
                    int nread = read(j, readbuf, sizeof(readbuf) - 1);

                    if (nread <= 0) {
                        printf("Disconnected client fd=%d, nick=%s\n",
                               j, Chat->clients[j]->nick);
                        freeClient(Chat->clients[j]);
                    } else {
                        struct client *c = Chat->clients[j];
                        readbuf[nread] = 0;

                        if (readbuf[0] == '/') {
                            /* 去掉尾部 \r \n */
                            char *p;
                            p = strchr(readbuf, '\r'); if (p) *p = 0;
                            p = strchr(readbuf, '\n'); if (p) *p = 0;

                            char *arg = strchr(readbuf, ' ');
                            if (arg) { *arg = 0; arg++; }

                            if (!strcmp(readbuf, "/nick") && arg) {
                                free(c->nick);
                                int nicklen = strlen(arg);
                                c->nick = chatMalloc(nicklen + 1);
                                memcpy(c->nick, arg, nicklen + 1);
                                char ok[256];
                                int len = snprintf(ok, sizeof(ok),
                                                   "[系统] 昵称已改为: %s\n", c->nick);
                                write(c->fd, ok, len);

                            } else if (!strcmp(readbuf, "/join") && arg) {
                                /* 解析: /join <房间名> [密码] */
                                char *pass = strchr(arg, ' ');
                                char roomname[128], password[128];
                                if (pass) {
                                    *pass = 0; pass++;
                                    snprintf(roomname, sizeof(roomname), "%s", arg);
                                    snprintf(password, sizeof(password), "%s", pass);
                                } else {
                                    snprintf(roomname, sizeof(roomname), "%s", arg);
                                    password[0] = 0;
                                }

                                /* 已经在同一个房间？ */
                                if (c->room && !strcasecmp(c->room, roomname)) {
                                    char *err = "[系统] 你已经在这个房间里了\n";
                                    write(c->fd, err, strlen(err));
                                    continue;
                                }

                                /* 先离开旧房间 */
                                if (c->room) {
                                    char leavemsg[256];
                                    int len = snprintf(leavemsg, sizeof(leavemsg),
                                                       "[系统] %s 离开了 %s\n",
                                                       c->nick, c->room);
                                    sendMsgToRoomBut(c->room, c->fd, leavemsg, len);
                                    printf("%s", leavemsg);
                                    free(c->room);
                                    c->room = NULL;
                                }

                                int idx = findRoom(roomname);
                                if (idx == -1) {
                                    /* 房间不存在，创建 */
                                    if (Chat->numrooms >= MAX_ROOMS) {
                                        char *err = "[系统] 房间数量已达上限\n";
                                        write(c->fd, err, strlen(err));
                                        continue;
                                    }
                                    createRoom(roomname, password[0] ? password : NULL);
                                    char info[256];
                                    int len;
                                    if (password[0])
                                        len = snprintf(info, sizeof(info),
                                                       "[系统] 创建带密码房间: %s\n", roomname);
                                    else
                                        len = snprintf(info, sizeof(info),
                                                       "[系统] 创建房间: %s\n", roomname);
                                    write(c->fd, info, len);
                                } else {
                                    /* 房间存在，检查密码 */
                                    struct room *r = Chat->rooms[idx];
                                    if (r->password) {
                                        if (!password[0] ||
                                            strcmp(r->password, password)) {
                                            char *err = "[系统] 密码错误\n";
                                            write(c->fd, err, strlen(err));
                                            continue;
                                        }
                                    }
                                }
                                /* 进入房间 */
                                c->room = chatMalloc(strlen(roomname) + 1);
                                memcpy(c->room, roomname, strlen(roomname) + 1);
                                char joinmsg[256];
                                int len = snprintf(joinmsg, sizeof(joinmsg),
                                                   "[系统] %s 加入了 %s\n",
                                                   c->nick, roomname);
                                printf("%s", joinmsg);
                                sendMsgToRoomBut(c->room, -1, joinmsg, len);

                                /* 回放该房间最近的历史记录 */
                                replayRoomHistory(c->fd, c->room);

                            } else if (!strcmp(readbuf, "/leave")) {
                                if (c->room) {
                                    char leavemsg[256];
                                    int len = snprintf(leavemsg, sizeof(leavemsg),
                                                       "[系统] %s 离开了 %s\n",
                                                       c->nick, c->room);
                                    sendMsgToRoomBut(c->room, c->fd, leavemsg, len);
                                    printf("%s", leavemsg);
                                    free(c->room);
                                    c->room = NULL;
                                    char *ok = "[系统] 已回到大厅\n";
                                    write(c->fd, ok, strlen(ok));
                                } else {
                                    char *err = "[系统] 你不在任何房间里\n";
                                    write(c->fd, err, strlen(err));
                                }

                            } else if (!strcmp(readbuf, "/rooms")) {
                                char list[1024];
                                int len = snprintf(list, sizeof(list),
                                                   "=== 房间列表 (%d 个) ===\n", Chat->numrooms);
                                write(c->fd, list, len);
                                for (int i = 0; i < Chat->numrooms; i++) {
                                    struct room *r = Chat->rooms[i];
                                    int cnt = 0;
                                    for (int k = 0; k <= Chat->maxclient; k++) {
                                        if (Chat->clients[k] && Chat->clients[k]->room &&
                                            !strcasecmp(Chat->clients[k]->room, r->name))
                                            cnt++;
                                    }
                                    len = snprintf(list, sizeof(list),
                                                   "  %s%s (%d人)\n",
                                                   r->name,
                                                   r->password ? " [加密]" : "",
                                                   cnt);
                                    write(c->fd, list, len);
                                }
                                char *end = "======================\n";
                                write(c->fd, end, strlen(end));

                            } else {
                                char *errmsg = "[系统] 未知命令。可用: /nick /join /leave /rooms\n";
                                write(c->fd, errmsg, strlen(errmsg));
                            }
                        } else {
                            /* 普通消息 */
                            char msg[512];
                            int msglen = snprintf(msg, sizeof(msg),
                                                  "%s> %s", c->nick, readbuf);
                            if (msglen >= (int)sizeof(msg))
                                msglen = sizeof(msg) - 1;
                            printf("%s", msg);

                            /* 写入日志 */
                            logMessage(c->room, c->nick, readbuf);

                            /* 发给同房间所有人 */
                            sendMsgToRoomBut(c->room, j, msg, msglen);
                        }
                    }
                }
            }
        }
    }
    return 0;
}
