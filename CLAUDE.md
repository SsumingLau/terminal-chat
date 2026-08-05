# terminal-chat

终端 + 网页聊天工具(单文件服务器, 零依赖 C)。

## 架构
- `smallchat-server.c`(单文件, ~950 行): TCP 7711(终端客户端, 行协议)+ HTTP 7712(内嵌网页, 表单登录 + SSE 下行 + POST 上行)。网页与终端共用房间/密码/消息路由。端口写死 `SERVER_PORT`/`WEB_PORT`。
- `smallchat-client.c` + `ledit.c/h`: 终端客户端, ledit 是自写 UTF-8 行编辑器(方向键/Delete/历史, wcwidth 列宽, 零依赖)。
- `chatlib.c/h`: TCP 工具。
- 网页 HTML 内嵌在服务器二进制的 `INDEX_HTML` 字符串里 —— **改网页必须重新编译服务器**。

## 部署(生产: 用户自己的服务器)
- SSH: 用户名与公网地址见本地 memory(服务器条目)—— **不要写进 git**(用户要求)。
- 服务器 Ubuntu 20.04 x86_64, 源码在 `~/terminal-chat/`, 端口 7711/7712 已在阿里云安全组放行。
- **必须传源码、在服务器上编译**(本地二进制是 Mach-O, Linux 跑不了; 反过来亦然)。
- 重启服务器进程:`ssh <用户>@<服务器> 'cd ~/terminal-chat && (setsid ./smallchat-server > /tmp/tc.log 2>&1 </dev/null &)'`。杀旧进程用 `ss -tlnp | grep 7711` 找 pid 再 kill(pkill -x 匹配不到, 进程名被内核截断成 smallchat-serve)。
- 注意: 无 systemd, 服务器重启后需手动拉起; 协议明文(公网密码裸奔, 已知取舍)。

## 已知约定
- 服务器广播消息带 `\n`(终端输入行重绘会覆盖无换行消息); SSE 推送剥掉尾部 `\n`。
- 服务器 glibc 严格 C99 需要 `_POSIX_C_SOURCE 200809L`(strtok_r), `strcasestr` 不可用(换 strstr)。
- make 时间戳秒级相同会漏重建, 必要时 `make -B`。
- 集成测试用 pty(测试脚本经验: 抓取要 select 循环 drain, 一次非阻塞读会漏数据; 客户端 pty 不 drain 会写阻塞; pty 下 `\n` 被 ONLCR 变 `\r\n`)。

## Git / Release
- remote: `git@github.com:SsumingLau/terminal-chat.git`(SSH 免密 push)。
- Release v1.0.0 已建; 更新附件 = 重新打 tar 放 `~/Downloads/terminal-chat-src.tar.gz` + macOS universal 客户端, 网页上传覆盖。
