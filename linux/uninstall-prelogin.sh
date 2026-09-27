#!/bin/sh
set -eu

[ "$(id -u)" -eq 0 ] || { echo '请通过 pkexec 运行此卸载程序' >&2; exit 77; }
uid=
while [ "$#" -gt 0 ]; do
    case "$1" in
        --uid) uid=${2-}; shift 2 ;;
        *) echo "Usage: $0 --uid UID" >&2; exit 64 ;;
    esac
done
case "$uid" in ''|*[!0-9]*) echo 'invalid uid' >&2; exit 64;; esac

systemctl disable --now bananaDesk-prelogin.service >/dev/null 2>&1 || true
rm -f /etc/systemd/system/bananaDesk-prelogin.service
rm -f /usr/local/libexec/bananaDesk-prelogin-xauth /usr/local/libexec/bananaDesk-prelogin-launch
rm -rf "/run/bananaDesk-${uid}"
systemctl daemon-reload
echo 'bananaDesk Linux 登录前共享服务已关闭'
