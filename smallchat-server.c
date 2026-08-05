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

#define _POSIX_C_SOURCE 200809L /* Linux 严格 C99 下需要 strtok_r 等声明 */
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
#define WEB_PORT 7712          /* 网页端 (HTTP + SSE) 监听端口 */

struct client {
    int fd;
    char *nick;
    char *room;   /* 当前房间名，NULL = 大厅 */
    char inbuf[512]; /* 行缓冲: 一次 read 可能含多行或半行 */
    int inlen;
};

/* 网页登录用户: 独立于 fd 存活 (token 关联), 上行走短命 POST,
 * 下行长连接 SSE 由 ssefd 指向 (每次 /events 建立或复用)。 */
#define WEB_USERS 512
struct webuser {
    char token[33];
    char nick[32];
    char room[128];
    int ssefd;                 /* 活跃 SSE 连接 fd, -1=当前离线 */
};

/* HTTP 连接缓冲: 短命 (处理完请求即释放), 只挂在 fd 上 */
struct webclient {
    int fd;
    char inbuf[2048];
    int inlen;
    int wantlen;               /* Content-Length, -1=还没解析完 */
    int issse;                 /* 是否 SSE 长连接 */
    struct webuser *user;      /* SSE 连接关联的用户 */
};

struct room {
    char *name;
    char *password;   /* NULL = 无密码 */
};

struct chatState {
    int serversock;
    int webserver;
    int numclients;
    int maxclient;
    struct client *clients[MAX_CLIENTS];
    struct webclient *web[MAX_CLIENTS];
    struct webuser *users[WEB_USERS];
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

/* 把控制字符替换成下划线，防止终端转义垃圾进入房间名/昵称/消息 */
void sanitize(char *s) {
    for (char *p = s; *p; p++) {
        unsigned char ch = *p;
        if (ch < 0x20 && ch != ' ') *p = '_';
        if (ch == 0x7f) *p = '_';
    }
}

/* 去掉密码里的方括号: 创建/加入时无论带不带 []，都归一化成同一种写法 */
void stripBrackets(char *s) {
    char *dst = s;
    for (char *p = s; *p; p++) {
        if (*p != '[' && *p != ']') *dst++ = *p;
    }
    *dst = 0;
}

/* ============================ Web (HTTP+SSE) ============================ */

/* 前向声明 (定义在后面) */
void logMessage(const char *room, const char *nick, const char *msg);
void sendMsgToRoomBut(const char *room, int excluded, char *s, size_t len);

/* form-urlencoded 解码: %XX -> 字节, + -> 空格, 就地写回 */
void urldecode(char *s) {
    char *dst = s;
    for (char *p = s; *p; p++) {
        if (*p == '+') { *dst++ = ' '; continue; }
        if (*p == '%' && p[1] && p[2]) {
            int hi = p[1], lo = p[2];
            int h = (hi >= '0' && hi <= '9') ? hi - '0' :
                    (hi >= 'a' && hi <= 'f') ? hi - 'a' + 10 : hi - 'A' + 10;
            int l = (lo >= '0' && lo <= '9') ? lo - '0' :
                    (lo >= 'a' && lo <= 'f') ? lo - 'a' + 10 : lo - 'A' + 10;
            if (h >= 0 && h <= 15 && l >= 0 && l <= 15) {
                *dst++ = (h << 4) | l;
                p += 2;
                continue;
            }
        }
        *dst++ = *p;
    }
    *dst = 0;
}

/* 简易 token: 时间 + 计数器 + fd, 转 hex (局域网小圈子够用, 无随机源) */
static unsigned int token_counter = 0;
void genToken(char *out, size_t size, int fd) {
    snprintf(out, size, "%08x%08x%08x", (unsigned)time(NULL),
             (unsigned)(token_counter++), (unsigned)fd);
}

struct webuser *findUserByToken(const char *token) {
    for (int i = 0; i < WEB_USERS; i++) {
        if (Chat->users[i] && !strcmp(Chat->users[i]->token, token))
            return Chat->users[i];
    }
    return NULL;
}

/* 同昵称+房间的重复登录复用旧用户 (刷新页面/重连), 否则新建 */
struct webuser *getOrCreateUser(const char *nick, const char *roomname) {
    for (int i = 0; i < WEB_USERS; i++) {
        if (Chat->users[i] && !strcmp(Chat->users[i]->nick, nick) &&
            !strcmp(Chat->users[i]->room, roomname))
            return Chat->users[i];
    }
    for (int i = 0; i < WEB_USERS; i++) {
        if (Chat->users[i] == NULL) {
            Chat->users[i] = calloc(1, sizeof(struct webuser));
            if (Chat->users[i] == NULL) return NULL;
            genToken(Chat->users[i]->token, sizeof(Chat->users[i]->token), 0);
            Chat->users[i]->ssefd = -1;
            return Chat->users[i];
        }
    }
    return NULL;
}

/* SSE 推送: 广播给同房间所有有活跃 SSE 连接的网页用户 */
void pushWebToRoom(const char *room, const char *s, size_t len) {
    for (int i = 0; i < WEB_USERS; i++) {
        struct webuser *u = Chat->users[i];
        if (u == NULL || u->ssefd < 0) continue;
        int sameRoom = (u->room[0] == 0 && room == NULL) ||
                       (u->room[0] && room && !strcasecmp(u->room, room));
        if (!sameRoom) continue;
        char buf[2560];
        while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r')) len--;
        int n = snprintf(buf, sizeof(buf), "data: %.*s\n\n", (int)len, s);
        /* ponytail: 非阻塞写, 失败即断开 (网页端极简, 可接受) */
        if (write(u->ssefd, buf, n) == -1) {
            close(u->ssefd);
            if (Chat->web[u->ssefd]) {
                Chat->web[u->ssefd] = NULL;
                free(Chat->web[u->ssefd]);
            }
            u->ssefd = -1;
        }
    }
}

/* HTTP 响应助手 */
void httpReply(int fd, const char *status, const char *ctype,
               const char *body, size_t blen) {
    char head[256];
    int hl = snprintf(head, sizeof(head),
        "HTTP/1.1 %s\r\nContent-Type: %s\r\n"
        "Content-Length: %zu\r\nConnection: close\r\n\r\n",
        status, ctype, blen);
    write(fd, head, hl);
    if (blen) write(fd, body, blen);
}

/* 登录: 校验/创建房间, 输出 token + 历史回放 (纯文本) */
int webLogin(int fd, const char *nick, const char *roomname,
             const char *password, char *out, size_t outsize) {
    char errmsg[256];
    if (nick[0] == 0 || roomname[0] == 0) {
        snprintf(errmsg, sizeof(errmsg), "昵称和房间名不能为空");
        httpReply(fd, "400 Bad Request", "text/plain; charset=utf-8",
                  errmsg, strlen(errmsg));
        return -1;
    }
    int ri = findRoom(roomname);
    if (ri == -1) {
        createRoom(roomname, password);
    } else {
        struct room *r = Chat->rooms[ri];
        if (r->password && (!password[0] || strcmp(r->password, password))) {
            snprintf(errmsg, sizeof(errmsg), "房间密码错误");
            httpReply(fd, "403 Forbidden", "text/plain; charset=utf-8",
                      errmsg, strlen(errmsg));
            return -1;
        }
    }

    struct webuser *u = getOrCreateUser(nick, roomname);
    if (u == NULL) return -1;
    snprintf(u->nick, sizeof(u->nick), "%s", nick);
    snprintf(u->room, sizeof(u->room), "%s", roomname);
    /* 复用旧用户时, 作废旧 SSE 连接 */
    if (u->ssefd >= 0) {
        if (Chat->web[u->ssefd]) {
            Chat->web[u->ssefd] = NULL;
            free(Chat->web[u->ssefd]);
        }
        close(u->ssefd);
        u->ssefd = -1;
    }

    /* 广播"加入了"给同房间其他人 */
    char joinmsg[256];
    int jl = snprintf(joinmsg, sizeof(joinmsg), "[系统] %s 加入了 %s\n",
                      nick, roomname);
    sendMsgToRoomBut(roomname, -1, joinmsg, jl);
    printf("%s", joinmsg);

    /* 响应: token + 历史回放 (历史走 login 响应, 避免 SSE 时序问题) */
    int n = snprintf(out, outsize, "%s\n", u->token);
    char path[512];
    snprintf(path, sizeof(path), "logs/%s.log", roomname);
    FILE *f = fopen(path, "r");
    if (f) {
        char line[1024];
        while (fgets(line, sizeof(line), f) && n < (int)outsize - 1)
            n += snprintf(out + n, outsize - n, "%s", line);
        fclose(f);
    }
    return 0;
}

/* 从 form 表单中提取字段 (nick/room/password/token/msg) */
void parseForm(char *body, char *fields[][2], int maxfields) {
    int n = 0;
    char *save = NULL;
    for (char *p = strtok_r(body, "&", &save); p && n < maxfields;
         p = strtok_r(NULL, "&", &save)) {
        char *eq = strchr(p, '=');
        if (eq == NULL) continue;
        *eq = 0;
        urldecode(eq + 1);
        fields[n][0] = p;
        fields[n][1] = eq + 1;
        n++;
    }
    fields[n][0] = NULL;
}

/* 处理一个 HTTP 请求 (解析完 body 后调用) */
void webHandle(int fd, struct webclient *w) {
    char *buf = w->inbuf;
    if (w->issse) return; /* SSE 连接不应有请求 */

    char method[8], path[256];
    if (sscanf(buf, "%7s %255s", method, path) != 2) {
        httpReply(fd, "400 Bad Request", "text/plain", "bad request", 11);
        return;
    }

    if (!strcmp(method, "GET") && !strcmp(path, "/")) {
        extern const char INDEX_HTML[];
        httpReply(fd, "200 OK", "text/html; charset=utf-8",
                  INDEX_HTML, strlen(INDEX_HTML));
        return;
    }

    if (!strcmp(method, "GET") && !strncmp(path, "/events", 7)) {
        char *tk = strstr(path, "token=");
        char token[64];
        if (tk == NULL) {
            httpReply(fd, "401 Unauthorized", "text/plain", "no token", 8);
            return;
        }
        snprintf(token, sizeof(token), "%s", tk + 6);
        char *amp = strchr(token, '&');
        if (amp) *amp = 0;
        struct webuser *u = findUserByToken(token);
        if (u == NULL) {
            httpReply(fd, "401 Unauthorized", "text/plain", "bad token", 9);
            return;
        }
        /* 作废旧 SSE, 新 fd 接管 */
        if (u->ssefd >= 0 && u->ssefd != fd) {
            if (Chat->web[u->ssefd]) {
                Chat->web[u->ssefd] = NULL;
                free(Chat->web[u->ssefd]);
            }
            close(u->ssefd);
        }
        u->ssefd = fd;
        w->issse = 1;
        w->user = u;
        const char *ssehdr = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                   "Cache-Control: no-cache\r\nConnection: keep-alive\r\n\r\n";
        write(fd, ssehdr, strlen(ssehdr));
        return;
    }

    if (!strcmp(method, "POST") && (!strcmp(path, "/login") ||
                                    !strcmp(path, "/send"))) {
        char *body = strstr(buf, "\r\n\r\n");
        if (body == NULL) body = strstr(buf, "\n\n");
        if (body) body += (body[0] == '\n') ? 2 : 4;
        else body = "";

        char *fields[8][2];
        parseForm(body, fields, 8);
        char nick[128] = "", roomname[128] = "", password[128] = "";
        char token[64] = "", msg[512] = "";
        for (int i = 0; fields[i][0]; i++) {
            if (!strcmp(fields[i][0], "nick")) snprintf(nick, sizeof(nick), "%s", fields[i][1]);
            else if (!strcmp(fields[i][0], "room")) snprintf(roomname, sizeof(roomname), "%s", fields[i][1]);
            else if (!strcmp(fields[i][0], "password")) snprintf(password, sizeof(password), "%s", fields[i][1]);
            else if (!strcmp(fields[i][0], "token")) snprintf(token, sizeof(token), "%s", fields[i][1]);
            else if (!strcmp(fields[i][0], "msg")) snprintf(msg, sizeof(msg), "%s", fields[i][1]);
        }

        if (!strcmp(path, "/login")) {
            sanitize(nick);
            sanitize(roomname);
            stripBrackets(password);
            char out[8192];
            if (webLogin(fd, nick, roomname, password, out, sizeof(out)) == -1)
                return;
            httpReply(fd, "200 OK", "text/plain; charset=utf-8", out, strlen(out));
            return;
        }

        /* /send */
        struct webuser *u = findUserByToken(token);
        if (u == NULL) {
            httpReply(fd, "401 Unauthorized", "text/plain", "bad token", 9);
            return;
        }
        sanitize(msg);
        if (msg[0] == 0) {
            httpReply(fd, "400 Bad Request", "text/plain", "empty", 5);
            return;
        }
        char fm[520];
        int ml = snprintf(fm, sizeof(fm), "%s> %s\n", u->nick, msg);
        if (ml >= (int)sizeof(fm)) ml = sizeof(fm) - 1;
        logMessage(u->room, u->nick, msg);
        sendMsgToRoomBut(u->room, -1, fm, ml);
        httpReply(fd, "200 OK", "text/plain", "ok", 2);
        return;
    }

    httpReply(fd, "404 Not Found", "text/plain", "not found", 9);
}

/* 主循环里分发 web fd 的可读事件 */
void webReadable(int fd) {
    struct webclient *w = Chat->web[fd];
    if (w == NULL) {
        w = calloc(1, sizeof(*w));
        if (w == NULL) return;
        w->fd = fd;
        w->wantlen = -1;
        Chat->web[fd] = w;
    }
    if (w->issse) {
        /* SSE 长连接: 读到 EOF 即断开, 用户保留 (EventSource 会重连) */
        char tmp[256];
        ssize_t r = read(fd, tmp, sizeof(tmp));
        if (r <= 0) {
            if (w->user) w->user->ssefd = -1;
            close(fd);
            if (Chat->web[fd] == w) Chat->web[fd] = NULL;
            free(w);
        }
        return;
    }
    ssize_t r = read(fd, w->inbuf + w->inlen,
                      sizeof(w->inbuf) - w->inlen - 1);
    if (r <= 0) {
        close(fd);
        if (Chat->web[fd] == w) Chat->web[fd] = NULL;
        free(w);
        return;
    }
    w->inlen += r;
    w->inbuf[w->inlen] = 0;

    /* 找 Content-Length */
    if (w->wantlen < 0) {
        char *cl = strstr(w->inbuf, "Content-Length:");
        if (cl) {
            w->wantlen = atoi(cl + 15);
            if (w->wantlen < 0) w->wantlen = 0;
        }
    }
    /* GET 请求 (无 body) 或 body 齐了: 处理 */
    int needbody = (w->wantlen >= 0) ? w->wantlen : 0;
    char *hdrEnd = strstr(w->inbuf, "\r\n\r\n");
    if (hdrEnd == NULL) hdrEnd = strstr(w->inbuf, "\n\n");
    int hdrlen = hdrEnd ? (hdrEnd - w->inbuf + (hdrEnd[0] == '\n' ? 2 : 4)) : 0;
    if (hdrlen > 0 && w->inlen >= hdrlen + needbody) {
        webHandle(fd, w);
        if (w->issse) return; /* SSE: 长连接, 保留 */
        close(fd);
        if (Chat->web[fd] == w) Chat->web[fd] = NULL;
        free(w);
    }
}

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
    fprintf(f, "[%s] %s> %s\n", ts, nick, msg);
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

/* ============================ Web page ================================= */

const char INDEX_HTML[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Terminal Chat</title>"
"<style>"
"body{background:#111;color:#ddd;font-family:ui-monospace,Menlo,monospace;margin:0;height:100vh;height:100dvh;display:flex;flex-direction:column}"
"#login{max-width:320px;margin:auto;padding:24px;background:#1a1a1a;border:1px solid #333;border-radius:8px}"
"#login h1{font-size:16px;margin:0 0 16px;color:#7fb3e8}"
"#login input{display:block;width:100%;box-sizing:border-box;margin:8px 0;padding:8px;background:#111;border:1px solid #333;color:#ddd;border-radius:4px;font-size:14px}"
"#login button{width:100%;padding:9px;background:#2b5f8f;border:0;color:#fff;border-radius:4px;font-size:14px;cursor:pointer}"
"#login .err{color:#e88080;font-size:12px;margin-top:8px;min-height:14px}"
"#chat{display:none;flex:1;flex-direction:column;max-width:720px;margin:0 auto;padding:12px;box-sizing:border-box;min-height:0;width:100%}"
"#msgs{flex:1;overflow-y:auto;-webkit-overflow-scrolling:touch;padding:4px 0;min-height:0}"
".msg{white-space:pre-wrap;word-break:break-all;font-size:14px;line-height:1.6}"
".ts{color:#666;font-size:12px;margin-right:6px}"
".sys{color:#888}"
"#inp{width:100%;box-sizing:border-box;padding:9px;background:#1a1a1a;border:1px solid #333;color:#ddd;border-radius:4px;font-size:14px;outline:none}"
"#inp:focus{border-color:#2b5f8f}"
"</style></head><body>"
"<div id='login'><h1>Terminal Chat</h1>"
"<input id='nick' placeholder='昵称' maxlength='24'>"
"<input id='room' placeholder='房间名' maxlength='64'>"
"<input id='pass' placeholder='密码 (可选)' type='password'>"
"<button onclick='enter()'>进入</button>"
"<div class='err' id='err'></div></div>"
"<div id='chat'><div id='msgs'></div>"
"<input id='inp' placeholder='消息, 回车发送' autocomplete='off'></div>"
"<script>"
"var token='';"
"var colors=['#87CA64','#EE813C','#EE849E','#992C5C','#E54073'];"
"var colorOf={};"
"function nickColor(n){if(colorOf[n]===undefined){var used={},k;"
"for(k in colorOf)used[colorOf[k]]=1;var free=[],i;"
"for(i=0;i<colors.length;i++)if(!used[i])free.push(i);"
"colorOf[n]=free.length?free[Math.floor(Math.random()*free.length)]:-1;}"
"return colorOf[n];}"
"function esc(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/\"/g,'&quot;');}"
"function colored(t){"
"if(t.indexOf('[系统]')===0)return {h:esc(t),s:1};"
"var m=t.match(/^([^>]+)> (.*)$/);"
"if(!m)return {h:esc(t),s:0};"
"var ci=nickColor(m[1]);var c=ci>=0?colors[ci]:'#ddd';"
"return {h:'<span style=\"color:'+c+'\">'+esc(m[1])+'</span>> '+esc(m[2]),s:0};}"
"function add(t,c){var d=document.createElement('div');d.className='msg '+(c||'');"
"if(t.indexOf('[系统]')===0){d.textContent=t;}"
"else{var n=new Date();var ts=('0'+n.getHours()).slice(-2)+':'+('0'+n.getMinutes()).slice(-2);"
"d.innerHTML='<span class=\"ts\">['+ts+'] </span>'+colored(t).h;}"
"document.getElementById('msgs').appendChild(d);"
"var m=document.getElementById('msgs');m.scrollTop=m.scrollHeight;}"
"function enter(){var n=document.getElementById('nick').value;"
"var r=document.getElementById('room').value;"
"var p=document.getElementById('pass').value;"
"if(!n||!r){document.getElementById('err').textContent='昵称和房间名必填';return;}"
"var body='nick='+encodeURIComponent(n)+'&room='+encodeURIComponent(r)"
"+(p?'&password='+encodeURIComponent(p):'');"
"fetch('/login',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body})"
".then(function(x){if(!x.ok)return x.text().then(function(t){throw t});return x.text()})"
".then(function(t){var i=t.indexOf('\\n');token=t.slice(0,i);"
"document.getElementById('login').style.display='none';"
"document.getElementById('chat').style.display='flex';"
"document.getElementById('inp').focus();"
"if(i>=0){var hs=t.slice(i+1).split('\\n');"
"for(var hi=0;hi<hs.length;hi++){if(!hs[hi])continue;"
"var hd=document.createElement('div');hd.className='msg sys';"
"var hm=hs[hi].match(/^\\[(.*?)\\] (.*)$/);"
"if(hm){var hr=colored(hm[2]);"
"hd.innerHTML='<span class=\"ts\">['+hm[1]+']</span> '+hr.h;}"
"else hd.textContent=hs[hi];"
"document.getElementById('msgs').appendChild(hd);}}"
"var es=new EventSource('/events?token='+encodeURIComponent(token));"
"es.onmessage=function(e){add(e.data);};"
"es.onerror=function(){ /* EventSource 自动重连 */ };})"
".catch(function(e){document.getElementById('err').textContent=e;});}"
"var inp=document.getElementById('inp');"
"inp.addEventListener('keydown',function(e){if(e.key==='Enter'){"
"var m=inp.value;if(!m.trim())return;inp.value='';"
"fetch('/send',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
"body:'token='+encodeURIComponent(token)+'&msg='+encodeURIComponent(m)})"
".catch(function(){add('[系统] 发送失败');});}});"
"</script></body></html>";

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

void sendMsgToRoomBut(const char *room, int excluded, char *s, size_t len);

void freeClient(struct client *c) {
    /* 断线(非 /leave)也要通知同房间其他人 */
    if (c->room) {
        char leavemsg[256];
        int len = snprintf(leavemsg, sizeof(leavemsg),
                           "[系统] %s 断线离开了 %s\n", c->nick, c->room);
        sendMsgToRoomBut(c->room, c->fd, leavemsg, len);
        printf("%s", leavemsg);
    }
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
    Chat->webserver = createTCPServer(WEB_PORT);
    if (Chat->webserver == -1) {
        perror("Creating web socket");
        exit(1);
    }
}

/* 发送消息给同一个房间的所有人（除了 excluded），并推给网页用户 */
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
    pushWebToRoom(room, s, len);
}

int main(void) {
    initChat();

    while (1) {
        fd_set readfds;
        struct timeval tv;
        int retval;

        FD_ZERO(&readfds);
        FD_SET(Chat->serversock, &readfds);
        FD_SET(Chat->webserver, &readfds);
        for (int j = 0; j <= Chat->maxclient; j++) {
            if (Chat->clients[j]) FD_SET(j, &readfds);
        }
        for (int j = 0; j < MAX_CLIENTS; j++) {
            if (Chat->web[j] && Chat->web[j]->fd >= 0)
                FD_SET(Chat->web[j]->fd, &readfds);
        }

        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int maxfd = Chat->maxclient;
        if (maxfd < Chat->serversock) maxfd = Chat->serversock;
        if (maxfd < Chat->webserver) maxfd = Chat->webserver;
        for (int j = 0; j < MAX_CLIENTS; j++) {
            if (Chat->web[j] && Chat->web[j]->fd > maxfd)
                maxfd = Chat->web[j]->fd;
        }
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
                    "/who             查看当前房间在线用户\n"
                    "====================\n";
                write(c->fd, welcome_msg, strlen(welcome_msg));
                printf("Connected client fd=%d\n", fd);
            }

            if (FD_ISSET(Chat->webserver, &readfds)) {
                int fd = acceptClient(Chat->webserver);
                struct webclient *w = calloc(1, sizeof(*w));
                if (w == NULL) {
                    close(fd);
                } else {
                    w->fd = fd;
                    w->wantlen = -1;
                    Chat->web[fd] = w;
                    printf("Web connect fd=%d\n", fd);
                }
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
                        /* 累积到每客户端行缓冲, 按 \n 逐行处理
                         * (一次 read 可能含多行, 也可能只含半行) */
                        if (c->inlen + nread >= (int)sizeof(c->inbuf))
                            c->inlen = 0; /* 超长行: 丢弃防滥用 */
                        memcpy(c->inbuf + c->inlen, readbuf, nread);
                        c->inlen += nread;
                        while (c->inlen > 0) {
                            char *nl = memchr(c->inbuf, '\n', c->inlen);
                            if (nl == NULL) break; /* 半行, 等下一包 */
                            *nl = 0;
                            int used = nl - c->inbuf + 1;
                            char *line = c->inbuf;

                        if (line[0] == '/') {
                            /* 去掉尾部 \r (行尾 \n 已在上方消费) */
                            char *p;
                            p = strchr(line, '\r'); if (p) *p = 0;

                            char *arg = strchr(line, ' ');
                            if (arg) { *arg = 0; arg++; }

                            if (!strcmp(line, "/nick") && arg && arg[0]) {
                                sanitize(arg);
                                free(c->nick);
                                int nicklen = strlen(arg);
                                c->nick = chatMalloc(nicklen + 1);
                                memcpy(c->nick, arg, nicklen + 1);
                                char ok[256];
                                int len = snprintf(ok, sizeof(ok),
                                                   "[系统] 昵称已改为: %s\n", c->nick);
                                write(c->fd, ok, len);

                            } else if (!strcmp(line, "/join") && arg) {
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
                                /* 清洗: 防止终端转义垃圾进房间名, 密码去方括号归一化 */
                                sanitize(roomname);
                                sanitize(password);
                                stripBrackets(password);

                                /* 已经在同一个房间？ */
                                if (c->room && !strcasecmp(c->room, roomname)) {
                                    char *err = "[系统] 你已经在这个房间里了\n";
                                    write(c->fd, err, strlen(err));
                                    goto done;
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
                                        goto done;
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
                                        stripBrackets(r->password); /* 自愈: 老房间密码带括号也能进 */
                                        if (!password[0] ||
                                            strcmp(r->password, password)) {
                                            char *err =
                                                "[系统] 密码错误（密码直接跟在房间名后，不带方括号）\n";
                                            write(c->fd, err, strlen(err));
                                            goto done;
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

                            } else if (!strcmp(line, "/leave")) {
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

                            } else if (!strcmp(line, "/rooms")) {
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

                            } else if (!strcmp(line, "/who")) {
                                /* 列出当前房间(或大厅)的在线用户 */
                                char list[1024];
                                int len = snprintf(list, sizeof(list),
                                                   "=== %s 在线 ===\n",
                                                   c->room ? c->room : "大厅");
                                write(c->fd, list, len);
                                for (int k = 0; k <= Chat->maxclient; k++) {
                                    struct client *o = Chat->clients[k];
                                    if (o == NULL) continue;
                                    if (c->room == NULL) {
                                        if (o->room != NULL) continue;
                                    } else {
                                        if (o->room == NULL ||
                                            strcasecmp(o->room, c->room))
                                            continue;
                                    }
                                    len = snprintf(list, sizeof(list), "  %s\n", o->nick);
                                    write(c->fd, list, len);
                                }
                                char *end = "=====================\n";
                                write(c->fd, end, strlen(end));

                            } else {
                                char *errmsg = "[系统] 未知命令。可用: /nick /join /leave /rooms /who\n";
                                write(c->fd, errmsg, strlen(errmsg));
                            }
                        } else {
                            /* 普通消息 */
                            sanitize(line); /* 防止转义垃圾进日志和别人的终端 */
                            char msg[512];
                            int msglen = snprintf(msg, sizeof(msg),
                                                  "%s> %s\n", c->nick, line);
                            if (msglen >= (int)sizeof(msg))
                                msglen = sizeof(msg) - 1;
                            printf("%s", msg);

                            /* 写入日志 */
                            logMessage(c->room, c->nick, line);

                            /* 发给同房间所有人 */
                            sendMsgToRoomBut(c->room, j, msg, msglen);
                        }
                        done: ; /* goto 目标 */
                        /* 处理完再前移剩余部分 (先 memmove 会盖掉行内容) */
                        memmove(c->inbuf, nl + 1, c->inlen - used);
                        c->inlen -= used;
                        c->inbuf[c->inlen] = 0;
                    }
                }
            }
            } /* 终端客户端 for 结束 */

            /* Web (HTTP/SSE) 连接 */
            for (int j = 0; j < MAX_CLIENTS; j++) {
                struct webclient *w = Chat->web[j];
                if (w == NULL || w->fd < 0) continue;
                if (FD_ISSET(w->fd, &readfds)) webReadable(w->fd);
            }
        }
    }
    return 0;
}
