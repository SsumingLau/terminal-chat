#!/bin/sh
# 加密终端: SSH 隧道连服务器 7711, 再启动本地客户端。
# 用法: ./tunnel.sh <ssh 目标>   (如 ./tunnel.sh lsm 或 user@服务器IP)
set -e
[ -n "$1" ] || { echo "用法: ./tunnel.sh <ssh 目标>"; exit 1; }
ssh -fN -o ExitOnForwardFailure=yes -L 7711:127.0.0.1:7711 "$1"
exec ./smallchat-client 127.0.0.1 7711
