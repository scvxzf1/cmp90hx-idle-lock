#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "install.sh must run as root" >&2
    exit 1
fi

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
command -v nvidia-smi >/dev/null 2>&1 || {
    echo "nvidia-smi is required" >&2
    exit 1
}

matches=$(nvidia-smi \
    --query-gpu=name,uuid,pci.bus_id,pci.device_id,pci.sub_device_id,memory.total \
    --format=csv,noheader,nounits | \
    awk -F', ' '$1 == "NVIDIA CMP 90HX" && $4 == "0x220D10DE" && $6 == "10240" { print $2 "|" $3 "|" $5 }')
count=$(printf '%s\n' "$matches" | awk 'NF { count++ } END { print count + 0 }')
if [ "$count" -lt 1 ]; then
    echo "no compatible CMP 90HX found; refusing to install" >&2
    exit 1
fi
echo "found $count compatible CMP 90HX device(s); runtime will enumerate all of them"

install -o root -g root -m 0755 "$script_dir/cmp90hx-idle-lock" /usr/local/sbin/cmp90hx-idle-lock
install -o root -g root -m 0644 "$script_dir/cmp90hx-idle-lock.service" /etc/systemd/system/cmp90hx-idle-lock.service
systemctl daemon-reload
systemctl enable cmp90hx-idle-lock.service
systemctl restart cmp90hx-idle-lock.service
