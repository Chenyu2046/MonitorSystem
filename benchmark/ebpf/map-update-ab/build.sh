#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
OUT_DIR="${OUT_DIR:-$ROOT/benchmark/ebpf/map-update-ab/.output}"
mkdir -p "$OUT_DIR"

clang -target bpf -D__TARGET_ARCH_x86 -O2 -g \
  -I "$ROOT/worker/src/ebpf" \
  -c "$ROOT/benchmark/ebpf/map-update-ab/global_update.bpf.c" \
  -o "$OUT_DIR/global_update.bpf.o"
clang -target bpf -D__TARGET_ARCH_x86 -O2 -g \
  -I "$ROOT/worker/src/ebpf" \
  -c "$ROOT/benchmark/ebpf/map-update-ab/percpu_update.bpf.c" \
  -o "$OUT_DIR/percpu_update.bpf.o"
clang++ -std=c++17 -O2 -Wall -Wextra \
  "$ROOT/benchmark/ebpf/map-update-ab/map_update_runner.cpp" \
  -lbpf -lelf -lz -o "$OUT_DIR/map_update_runner"

echo "built $OUT_DIR"
