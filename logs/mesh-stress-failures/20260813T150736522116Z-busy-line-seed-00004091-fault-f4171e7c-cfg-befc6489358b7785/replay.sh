#!/bin/sh
set -eu
expected_sha256=9f4428a4cd5f4c866aa809c4d8e784162a8cab2a8dd1bd0d1783859534f14cc4
actual_sha256=$(sha256sum /home/tommie/Projects/IMEC2/logs/mesh-stress-failures/20260813T150736522116Z-busy-line-seed-00004091-fault-f4171e7c-cfg-befc6489358b7785/mesh_stress | awk '{print $1}')
if [ "$actual_sha256" != "$expected_sha256" ]; then
  echo 'mesh_stress snapshot hash mismatch' >&2
  exit 125
fi
unset ASAN_OPTIONS UBSAN_OPTIONS LSAN_OPTIONS
export ASAN_OPTIONS=detect_leaks=0:halt_on_error=1
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
timeout --signal=TERM --kill-after=1s 30s /home/tommie/Projects/IMEC2/logs/mesh-stress-failures/20260813T150736522116Z-busy-line-seed-00004091-fault-f4171e7c-cfg-befc6489358b7785/mesh_stress --scenario busy-line --seed 0x00004091 --fault-seed 0xf4171e7c --packets 2 --loss 50 --ack-loss 300 --duplicate 300 --delay 500 --max-delay-us 4000 --max-steps 800 --reset-step 160 --trace /home/tommie/Projects/IMEC2/logs/mesh-stress-failures/20260813T150736522116Z-busy-line-seed-00004091-fault-f4171e7c-cfg-befc6489358b7785/trace.jsonl
