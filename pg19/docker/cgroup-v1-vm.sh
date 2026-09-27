#!/bin/bash
#
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
#
# cgroup-v1-vm.sh IMAGE [SUITE...]: the port's suites on cgroup v1, in a
# KVM guest.
#
# Cloudberry's resource groups on cgroup v1 (gp_resource_manager = group)
# need the v1 hierarchies of cpu, cpuacct, cpuset and memory, which a kernel
# with CONFIG_CPUSETS_V1 and CONFIG_MEMCG_V1 off -- these hosts', 6.12 and
# after by default -- cannot mount, in a container or out of one.  So the
# suite runs in a guest booted with Debian 12's kernel, 6.1, which has them:
# its root file system the image's own (docker export), the port as the
# image has it, and its init mounts the v1 hierarchies and gives the parent
# gpdb of each to the image's postgres, runs the suites as postgres -- by
# default resgroup_v1, Cloudberry's isolation2_resgroup_v1_schedule -- and
# powers the guest off.  What the suites print comes out on the guest's
# console, here; RESULTS_DIR, where it is set, gets what they keep, copied
# out of the guest's disk.
#
# Needs /dev/kvm, which this user can open, and Docker: the kernel comes
# from Debian's package in a debian:bookworm container, qemu from a small
# image made here, and the guest's files are kept in a Docker volume,
# cb-cgroup-v1-vm (CB_VM_VOLUME), which a later run uses again.  CB_VM_CPUS
# and CB_VM_MEM (MB) size the guest: 8 and 12288 by default.

set -eu

IMAGE="${1:?usage: cgroup-v1-vm.sh IMAGE [SUITE...]}"
shift
SUITES="${*:-resgroup_v1}"
VOL="${CB_VM_VOLUME:-cb-cgroup-v1-vm}"
CPUS="${CB_VM_CPUS:-8}"
MEM="${CB_VM_MEM:-12288}"
QEMU_IMAGE=cb-cgroup-v1-qemu:1

[ -r /dev/kvm ] && [ -w /dev/kvm ] || { echo "cgroup-v1-vm: /dev/kvm is not there for this user" >&2; exit 77; }
docker volume create "$VOL" > /dev/null

# qemu, and e2fsprogs for the guest's disk
if ! docker image inspect "$QEMU_IMAGE" > /dev/null 2>&1; then
	docker build -q -t "$QEMU_IMAGE" - > /dev/null <<'EOF'
FROM debian:trixie-slim
RUN apt-get update -qq \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
        qemu-system-x86 e2fsprogs \
 && rm -rf /var/lib/apt/lists/*
EOF
fi

# Debian 12's kernel and the initramfs its package makes, with virtio's and
# ext4's modules
docker run --rm -v "$VOL":/vm debian:bookworm-slim bash -c '
	[ -f /vm/vmlinuz ] && [ -f /vm/initrd.img ] && exit 0
	apt-get update -qq > /dev/null &&
	DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
		linux-image-amd64 initramfs-tools > /dev/null 2>&1 &&
	cp /boot/vmlinuz-* /vm/vmlinuz && cp /boot/initrd.img-* /vm/initrd.img'

# The guest's init: the v1 hierarchies, as Cloudberry's hosts mount them,
# with a parent gpdb in each for postgres; then the suites, as postgres.
init="$(mktemp)"
cat > "$init" <<EOF
#!/bin/bash
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
mkdir -p /dev/pts /dev/shm
mount -t devpts devpts /dev/pts
mount -t tmpfs tmpfs /dev/shm
mount -t tmpfs tmpfs /tmp
mount -t tmpfs tmpfs /run
chmod 1777 /tmp /dev/shm
ip link set lo up
hostname cbvm
mount -t tmpfs cgroup /sys/fs/cgroup
for c in cpu,cpuacct cpuset memory; do
	mkdir -p /sys/fs/cgroup/\$c
	mount -t cgroup -o \$c cgroup /sys/fs/cgroup/\$c
	mkdir -p /sys/fs/cgroup/\$c/gpdb
done
ln -s cpu,cpuacct /sys/fs/cgroup/cpu
ln -s cpu,cpuacct /sys/fs/cgroup/cpuacct
cat /sys/fs/cgroup/cpuset/cpuset.cpus > /sys/fs/cgroup/cpuset/gpdb/cpuset.cpus
cat /sys/fs/cgroup/cpuset/cpuset.mems > /sys/fs/cgroup/cpuset/gpdb/cpuset.mems
chown -R postgres:postgres /sys/fs/cgroup/cpu,cpuacct/gpdb /sys/fs/cgroup/cpuset/gpdb /sys/fs/cgroup/memory/gpdb
echo "cbvm: \$(uname -r), \$(nproc) cpus, cgroup v1: \$(ls /sys/fs/cgroup | tr '\n' ' ')"
rm -rf /cbvm-results; mkdir -p /cbvm-results; chown postgres:postgres /cbvm-results
cd /cb
runuser -u postgres -- env HOME=/var/lib/postgresql PATH=/usr/local/pgsql/bin:\$PATH \
	LD_LIBRARY_PATH=/usr/local/pgsql/lib:/usr/local/pgsql/lib/x86_64-linux-gnu \
	RESULTS_DIR=/cbvm-results bash /cb/pg19/test/run.sh $SUITES
echo "cbvm: exit \$?"
sync
echo o > /proc/sysrq-trigger
sleep 30
EOF
chmod +x "$init"

# The guest's disk: the image's files, and the init
cid=$(docker create "$IMAGE")
size_mb=$(( $(docker image inspect --format '{{.Size}}' "$IMAGE") / 1048576 + 4096 ))
docker export "$cid" |
	docker run --rm -i -v "$VOL":/vm -v "$init":/cbvm-init:ro "$QEMU_IMAGE" bash -c "
		mkdir /rootfs && tar -x -C /rootfs &&
		install -m 755 /cbvm-init /rootfs/cbvm-init &&
		rm -f /vm/root.img &&
		mkfs.ext4 -q -F -L cbroot -d /rootfs /vm/root.img ${size_mb}M"
docker rm "$cid" > /dev/null
rm -f "$init"

# The guest, its console here
docker run --rm --device /dev/kvm -v "$VOL":/vm "$QEMU_IMAGE" \
	qemu-system-x86_64 -enable-kvm -cpu host -smp "$CPUS" -m "$MEM" \
		-kernel /vm/vmlinuz -initrd /vm/initrd.img \
		-drive file=/vm/root.img,if=virtio,format=raw \
		-append "root=/dev/vda rw console=ttyS0 init=/cbvm-init panic=-1 quiet" \
		-nographic -no-reboot | tee /dev/stderr | grep -q "^cbvm: exit 0" && rc=0 || rc=1

# What the suites kept
if [ -n "${RESULTS_DIR:-}" ]; then
	mkdir -p "$RESULTS_DIR"
	docker run --rm -v "$VOL":/vm -v "$RESULTS_DIR":/out "$QEMU_IMAGE" \
		debugfs -R "rdump /cbvm-results /out" /vm/root.img > /dev/null 2>&1 || true
fi
exit "$rc"
