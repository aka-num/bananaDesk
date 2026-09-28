#!/bin/sh
set -eu

usage() {
    echo "Usage: $0 --user USER --uid UID --gid GID --home HOME --exec PATH --bind IPV4 --port PORT --invite-file PATH [--display :0] [--defer-start]" >&2
    exit 64
}
die() { echo "bananaDesk 登录前服务：$*" >&2; exit 64; }

[ "$(id -u)" -eq 0 ] || { echo '请通过 pkexec 运行此安装程序' >&2; exit 77; }
command -v systemctl >/dev/null 2>&1 || die '当前系统没有 systemctl'

user= uid= gid= home= app= bind= port= invite= display=:0 defer_start=0
while [ "$#" -gt 0 ]; do
    if [ "$1" = --defer-start ]; then
        defer_start=1
        shift
        continue
    fi
    [ "$#" -ge 2 ] || usage
    case "$1" in
        --user) user=$2; shift 2 ;;
        --uid) uid=$2; shift 2 ;;
        --gid) gid=$2; shift 2 ;;
        --home) home=$2; shift 2 ;;
        --exec) app=$2; shift 2 ;;
        --bind) bind=$2; shift 2 ;;
        --port) port=$2; shift 2 ;;
        --invite-file) invite=$2; shift 2 ;;
        --display) display=$2; shift 2 ;;
        *) usage ;;
    esac
done

[ -n "$user" ] && [ -n "$uid" ] && [ -n "$gid" ] && [ -n "$home" ] && [ -n "$app" ] && [ -n "$bind" ] && [ -n "$port" ] && [ -n "$invite" ] || usage
case "$user" in ''|*[!A-Za-z0-9_.-]*) die '用户名无效' ;; esac
case "$uid" in ''|*[!0-9]*) die 'uid 无效' ;; esac
case "$gid" in ''|*[!0-9]*) die 'gid 无效' ;; esac
case "$port" in ''|*[!0-9]*) die '端口无效' ;; esac
[ "$port" -ge 1024 ] && [ "$port" -le 65535 ] || die '端口必须在 1024 到 65535 之间'
case "$display" in
    :*) display_number=${display#:}; case "$display_number" in ''|*[!0-9]*) die '显示器必须是 :0、:1 等 X11 地址' ;; esac ;;
    *) die '显示器必须是 :0、:1 等 X11 地址' ;;
esac
newline=$(printf '\012x'); newline=${newline%x}
carriage=$(printf '\015x'); carriage=${carriage%x}
for value in "$home" "$app" "$invite"; do
    case "$value" in *"$newline"*|*"$carriage"*) die '路径不能包含换行符' ;; esac
done
[ -d "$home" ] || die '用户 home 目录不存在'
[ -x "$app" ] || die 'bananaDesk 可执行文件不存在或不可执行'
[ "$(id -u "$user" 2>/dev/null || echo -1)" = "$uid" ] || die '用户名和 uid 不匹配'
[ "$(id -g "$user" 2>/dev/null || echo -1)" = "$gid" ] || die '用户主组和 gid 不匹配'
group=$(id -gn "$user")
case "$group" in ''|*[!A-Za-z0-9_.-]*) die '用户主组名称无效' ;; esac
valid_ipv4() {
    awk -F. 'NF == 4 { for (i = 1; i <= 4; ++i) if ($i !~ /^[0-9]+$/ || $i > 255) exit 1; exit 0 } { exit 1 }' <<EOF_IPV4
$1
EOF_IPV4
}
valid_ipv4 "$bind" || die '绑定地址必须是本机 IPv4 地址'

unit=/etc/systemd/system/bananaDesk-prelogin.service
libexec=/usr/local/libexec
runtime="/run/bananaDesk-${uid}"
source_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
[ -x "$source_dir/bananaDesk-prelogin-xauth" ] || die '缺少 Xauthority 辅助脚本'
[ -x "$source_dir/bananaDesk-prelogin-launch" ] || die '缺少服务启动脚本'

unit_quote() {
    value=$1
    value=$(printf '%s' "$value" | sed 's/\\/\\\\/g; s/"/\\"/g; s/%/%%/g')
    printf '"%s"' "$value"
}

systemctl disable --now bananaDesk-prelogin.service >/dev/null 2>&1 || true
install -d -m 0755 "$libexec"
install -m 0755 "$source_dir/bananaDesk-prelogin-xauth" "$libexec/bananaDesk-prelogin-xauth"
install -m 0755 "$source_dir/bananaDesk-prelogin-launch" "$libexec/bananaDesk-prelogin-launch"

tmp_unit=$(mktemp /etc/systemd/system/.bananaDesk-prelogin.service.XXXXXX)
trap 'rm -f "$tmp_unit"' EXIT HUP INT TERM
cat > "$tmp_unit" <<EOF
[Unit]
Description=bananaDesk Linux pre-login remote desktop
After=display-manager.service network-online.target
Wants=network-online.target

[Service]
Type=simple
User=$user
Group=$group
WorkingDirectory=$home
Environment=HOME=$(unit_quote "$home")
Environment=USER=$(unit_quote "$user")
Environment=DISPLAY=$(unit_quote "$display")
Environment=XAUTHORITY=$(unit_quote "$runtime/Xauthority")
Environment=XDG_SESSION_TYPE=x11
Environment=QT_QPA_PLATFORM=xcb
Environment=BANANADESK_EXEC=$(unit_quote "$app")
Environment=BANANADESK_BIND=$(unit_quote "$bind")
Environment=BANANADESK_PORT=$(unit_quote "$port")
Environment=BANANADESK_INVITE=$(unit_quote "$invite")
PermissionsStartOnly=true
ExecStartPre=$libexec/bananaDesk-prelogin-xauth --uid $uid --gid $gid --display $display --output $runtime/Xauthority
ExecStart=$libexec/bananaDesk-prelogin-launch
Restart=on-failure
RestartSec=5s
KillMode=control-group
NoNewPrivileges=true
# X11's UNIX socket is in /tmp/.X11-unix; PrivateTmp would hide it.
PrivateTmp=false
RuntimeDirectory=bananaDesk-$uid
RuntimeDirectoryMode=0700
LimitNOFILE=4096

[Install]
WantedBy=multi-user.target
EOF
chmod 0644 "$tmp_unit"
chown root:root "$tmp_unit"
mv -f "$tmp_unit" "$unit"
trap - EXIT HUP INT TERM
systemctl daemon-reload
systemctl enable bananaDesk-prelogin.service
if [ "$defer_start" -eq 0 ]; then
    systemctl start bananaDesk-prelogin.service
fi
echo 'bananaDesk Linux 登录前共享服务已启用'
