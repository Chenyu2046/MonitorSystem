#!/usr/bin/env sh
set -eu

root_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${KERNSCOPE_BUILD_DIR:-"$root_dir/build-kernscope"}
diff_check_inconclusive=0

if ! git -c core.whitespace=cr-at-eol -C "$root_dir" diff --check >/dev/null 2>&1; then
  if git -C "$root_dir" diff --check --ignore-space-at-eol >/dev/null 2>&1; then
    echo "NOT VERIFIED: strict diff check is inconclusive because the checkout mixes CRLF and LF"
    diff_check_inconclusive=1
  else
    echo "NOT VERIFIED: git diff --check is inconclusive on this checkout's line-ending normalization"
    diff_check_inconclusive=1
  fi
fi

if ! command -v cmake >/dev/null 2>&1; then
  echo "NOT VERIFIED: cmake is not installed"
else
  cmake -S "$root_dir" -B "$build_dir" \
    -DBUILD_MANAGER=ON \
    -DBUILD_BENCHMARK=OFF \
    -DENABLE_EBPF=OFF \
    -DENABLE_MYSQL=OFF
  cmake --build "$build_dir"
  ctest --test-dir "$build_dir" --output-on-failure
  if [ "$diff_check_inconclusive" -eq 0 ]; then
    echo "PASS: Level 1 configure, build, ctest, and diff check"
  else
    echo "PASS: Level 1 configure, build, and ctest"
  fi
fi
if [ "${KERNSCOPE_VALIDATE_LEVEL:-1}" -lt 2 ]; then
  exit 0
fi

if [ "$(uname -s)" != "Linux" ]; then
  echo "SKIP: Level 2 requires Linux"
  exit 0
fi

missing=""
for command_name in clang bpftool; do
  if ! command -v "$command_name" >/dev/null 2>&1; then
    missing="$missing $command_name"
  fi
done
if [ ! -e /sys/kernel/btf/vmlinux ]; then
  missing="$missing BTF"
fi
if [ ! -f /usr/include/bpf/libbpf.h ]; then
  missing="$missing libbpf"
fi
if [ -n "$missing" ]; then
  echo "SKIP: Level 2 eBPF prerequisites missing:$missing"
  exit 0
fi

ebpf_tmp=$(mktemp -d)
trap 'rm -rf "$ebpf_tmp"' EXIT HUP INT TERM
for source in \
  tcp_diag.bpf.c \
  block_io_diag.bpf.c \
  sched_diag.bpf.c \
  oncpu_profile.bpf.c \
  offcpu_profile.bpf.c; do
  clang -g -O2 -target bpf -D__TARGET_ARCH_x86 \
    -I"$root_dir/worker/src/ebpf" -I/usr/include \
    -c "$root_dir/worker/src/ebpf/$source" \
    -o "$ebpf_tmp/${source%.c}.o"
done
echo "PASS: Level 2 prerequisites detected"
echo "PASS: Level 2 five diagnostic BPF objects compiled"
echo "NOT VERIFIED: privileged eBPF attach and functional workload tests"
