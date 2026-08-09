# terminal-chat

终端 + 网页聊天工具(单文件服务器, 零依赖 C)。

## 架构
- `smallchat-server.c`(单文件, ~950 行): TCP 7711(终端客户端, 行协议)+ HTTP 7712(内嵌网页, 表单登录 + SSE 下行 + POST 上行)。网页与终端共用房间/密码/消息路由。端口写死 `SERVER_PORT`/`WEB_PORT`。
- `smallchat-client.c` + `ledit.c/h`: 终端客户端, ledit 是自写 UTF-8 行编辑器(方向键/Delete/历史, wcwidth 列宽, 零依赖)。
- `chatlib.c/h`: TCP 工具。
- 网页 HTML 内嵌在服务器二进制的 `INDEX_HTML` 字符串里 —— **改网页必须重新编译服务器**。

## 部署(生产: 用户自己的服务器)
- SSH: 用户名与公网地址见本地 memory(服务器条目)—— **不要写进 git**(用户要求)。
- 服务器 Ubuntu 20.04 x86_64, 源码在 `~/terminal-chat/`。安全组已收口为 22 + 443(7711/7712 不对公网)。
- **必须传源码、在服务器上编译**(本地二进制是 Mach-O, Linux 跑不了; 反过来亦然)。
- 重启服务器进程:`ssh <用户>@<服务器> 'cd ~/terminal-chat && (setsid ./smallchat-server > /tmp/tc.log 2>&1 </dev/null &)'`。杀旧进程用 `ss -tlnp | grep 7711` 找 pid 再 kill(pkill -x 匹配不到, 进程名被内核截断成 smallchat-serve)。
- 注意: 无 systemd, 服务器重启后需手动拉起。网页已加密(`https://<IP>/chat`, 自签名 + Apache 反代, 见下节)。
- 更新服务器二进制: **先 kill 再 make/scp**(覆盖运行中的二进制会 ETXTBSY)。

## 已知约定(含 2026-08 加密功能踩坑)
- 服务器广播消息带 `\n`(终端输入行重绘会覆盖无换行消息); SSE 推送剥掉尾部 `\n`。
- 服务器 glibc 严格 C99 需要 `_POSIX_C_SOURCE 200809L`(strtok_r), `strcasestr` 不可用(换 strstr)。
- make 时间戳秒级相同会漏重建, 必要时 `make -B`。
- 集成测试用 pty(测试脚本经验: 抓取要 select 循环 drain, 一次非阻塞读会漏数据; 客户端 pty 不 drain 会写阻塞; pty 下 `\n` 被 ONLCR 变 `\r\n`)。
- **Apache 配置不支持行内注释**(nginx 支持, 容易记混)—— 行内 `#` 后面的内容会被当参数, 报 "Invalid ProxyPass parameter"/"RedirectMatch takes two or three arguments"。踩过两次。
- `ProxyTimeout` 只能放 server context(vhost 里报错); vhost 内用 `ProxyPass … timeout=86400`。
- 自签名证书必须带 SAN: `openssl req … -addext "subjectAltName=IP:<IP>"`, 否则 Chrome 拒收(CN 无效)。
- SSE 反代参数: `disablereuse=On`(防后端连接复用串流)+ `timeout=86400`(防长连接掐断)+ `SetEnv proxy-sendchunked 1`。
- 反代挂子路径时无尾斜杠 404: `/chat` 要 `RedirectMatch ^/chat$ /chat/`。
- 网页 favicon 404: INDEX_HTML 的 `<head>` 加 `<link rel='icon' href='data:,'>`(一行, 免浏览器请求 /favicon.ico)。
- **curl 重定向到文件是块缓冲**: 小数据等 curl 退出才落盘,"文件 0 字节"≠没收到。测 SSE 用 `curl -N` 或等退出再读, 否则会把正常当 bug(本次误诊一大圈)。
- `pkill -f <串>` 会误杀自身 ssh 会话(命令行里含匹配串)—— 杀进程用精确 pid。
- `strace -p` attach 被 yama 挡(非 root 不能 attach 非子进程)—— 用 `strace … ./smallchat-server` 直接启动; 零权限看进程卡点用 `/proc/<pid>/syscall`。
- 服务器上没有 `lsm` 别名(别名只配在用户本机)—— **嵌套 ssh(服务器上再 ssh lsm)必失败**, 用本机单层 ssh。
- 调试 printf 不 flush: stdout 非 tty 块缓冲, 日志看不到输出。用 `fflush(stdout)` 或 `stdbuf -oL` 启动。

## 加密: 零代码方案(已实施, 2026-08)

原计划(应用层 ChaCha20 自实现 ~300 行)放弃: 自写密码学是 3am bug 农场, 且要改双端协议。改用原生加密:

- **终端 = SSH 隧道**(`tunnel.sh`, 免密 key 认证的前提)。
- **网页 = HTTPS**: 无域名, 自签名证书 + **Apache** 反代 443→7712, 挂 `/chat` 路径(`deploy-https.sh`, 服务器上跑一次, 可重复跑)。用户服务器已装 Apache(无 nginx)。
  - 访问地址 `https://<IP>/chat`; 根路径跳回 http 站。
  - 浏览器首次访问点"高级 → 继续"; 加密有效但证书不受信任。
  - 浏览器通知(Notification API)在自签名下不可用(http/自签名都不是 secure context)。升级路径: duckdns 免费域名 + Let's Encrypt(或阿里云买域名), 换掉证书即可, 顺带消警告。
- 网页 JS 用相对路径(`fetch('login')` 等), 反代挂子路径(/chat)和挂根都兼容; **改回绝对路径会破坏 /chat 部署**。
- 安全组已收口为 22 + 443, 7711/7712 不再公网直连。
- 不做 E2E(服务器保留明文转发, 消息互通简单)。

升级路径: 若将来要"终端用户无 SSH 账号也能透明加密", 再上 libsodium(放弃零依赖)或协议层 TLS。当前零 C 代码改动, 服务器二进制不用重编。

## Git / Release
- remote: `git@github.com:SsumingLau/terminal-chat.git`(SSH 免密 push)。
- Release v1.0.0 已建; 更新附件 = 重新打 tar 放 `~/Downloads/terminal-chat-src.tar.gz` + macOS universal 客户端, 网页上传覆盖。
