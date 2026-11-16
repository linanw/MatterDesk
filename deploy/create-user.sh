#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

deploy_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
username="${1:-matterdesk}"
output="${2:-$deploy_dir/secrets/users.caddy}"
image='caddy:2.11.9-alpine@sha256:003a97808e781aa9f7e75cc2f43d34d461099031faf5e20faefb9f61126836f0'
if (( $# > 2 )) || [[ ! "$username" =~ ^[a-zA-Z0-9_.@-]{1,64}$ ]]; then
    echo '用法：./create-user.sh [用户名：字母/数字/_.@-] [新凭据文件路径]' >&2
    exit 2
fi
if [[ -e "$output" || -L "$output" ]]; then
    echo '凭据文件已存在；请指定新文件，避免覆盖。' >&2
    exit 1
fi

IFS= read -r -s -p "给 $username 设置一个强密码（16–72 字节）：" password </dev/tty
printf '\n' >&2
IFS= read -r -s -p '再次输入密码：' confirmation </dev/tty
printf '\n' >&2
if [[ "$password" != "$confirmation" ]] || (( ${#password} < 16 || ${#password} > 72 )) || [[ "$password" =~ [[:cntrl:]] || "$password" == [[:space:]]* || "$password" == *[[:space:]] ]]; then
    echo '密码需相同，长度 16–72 字节，且无控制字符和首尾空格。' >&2
    exit 1
fi
unset confirmation

# Pass via stdin, without placing the password in process arguments or files.
hash="$(printf '%s' "$password" | docker run --rm -i --network none "$image" caddy hash-password --algorithm bcrypt)"
unset password
if [[ ! "$hash" =~ ^\$2[aby]\$[0-9]{2}\$[A-Za-z0-9./]{53}$ ]]; then
    echo '没有取得可用的 bcrypt 哈希。' >&2
    exit 1
fi

umask 077
mkdir -p -- "$(dirname -- "$output")"
( set -o noclobber; printf '%s %s\n' "$username" "$hash" >"$output" )
printf '已写入 %s。可将该文件挂载为 Caddy 的凭据文件。\n' "$output"
