#!/bin/sh
# 服务器上运行(需 sudo): 自签名证书 + Apache 反代 443 -> 7712, 网页端 HTTPS。
# 用法: ./deploy-https.sh <服务器IP> [聊天路径, 默认 chat]
# 无域名方案: 自签名加密有效, 只是浏览器首次访问要点"高级 → 继续"。
set -e
IP="$1"
[ -n "$IP" ] || { echo "用法: $0 <服务器IP> [聊天路径]"; exit 1; }
P="${2:-chat}"
sudo apt-get install -y apache2 openssl >/dev/null
sudo mkdir -p /etc/apache2/ssl
# 证书已存在就不重生成: 重跑脚本不能把浏览器已接受的证书作废
if [ ! -f /etc/apache2/ssl/chat.key ] || [ ! -f /etc/apache2/ssl/chat.crt ]; then
    sudo openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
      -keyout /etc/apache2/ssl/chat.key -out /etc/apache2/ssl/chat.crt \
      -subj "/CN=$IP" -addext "subjectAltName=IP:$IP"   # SAN 必须, 否则 Chrome 拒收
fi
sudo tee /etc/apache2/sites-available/chat-https.conf >/dev/null <<EOF
<VirtualHost *:443>
    ServerName $IP
    SSLEngine on
    SSLCertificateFile /etc/apache2/ssl/chat.crt
    SSLCertificateKeyFile /etc/apache2/ssl/chat.key
    # 聊天挂在 /$P; 其余路径全部反代到 :80 的现有站点(全站 https)
    RedirectMatch ^/$P$ /$P/
    # disablereuse 防后端连接复用串流; timeout 86400s 防 SSE 长连接被掐断
    ProxyPass /$P/ http://127.0.0.1:7712/ disablereuse=On timeout=86400
    ProxyPassReverse /$P/ http://127.0.0.1:7712/
    <Location /$P/>
        SetEnv proxy-sendchunked 1
    </Location>
    ProxyPass / http://127.0.0.1:80/
    ProxyPassReverse / http://127.0.0.1:80/
</VirtualHost>
EOF
sudo a2enmod ssl proxy proxy_http >/dev/null
sudo a2ensite chat-https >/dev/null 2>&1 || true   # 已启用时 a2ensite 返回 1, 忽略
sudo apachectl configtest
sudo service apache2 reload
echo "完成: 浏览器打开 https://$IP/$P 聊天(首次点'高级 → 继续访问')"
echo "      :80 上的所有页面(含根路径)现在都可走 https://$IP/<路径>"
echo "注意: 自签名证书下浏览器通知不可用; 想要通知 + 无警告需域名真证书"
