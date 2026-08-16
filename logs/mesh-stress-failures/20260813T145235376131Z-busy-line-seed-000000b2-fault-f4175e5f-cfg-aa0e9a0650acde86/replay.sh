#!/bin/sh
set -eu
expected_sha256=0063f1a777984574efa9b8dab6c8b5ebb8fdaf2deefdc667dfc1615dac3f8163
actual_sha256=$(sha256sum /home/tommie/Projects/IMEC2/logs/mesh-stress-failures/20260813T145235376131Z-busy-line-seed-000000b2-fault-f4175e5f-cfg-aa0e9a0650acde86/mesh_stress | awk '{print $1}')
if [ "$actual_sha256" != "$expected_sha256" ]; then
  echo 'mesh_stress snapshot hash mismatch' >&2
  exit 125
fi
unset ASAN_OPTIONS UBSAN_OPTIONS LSAN_OPTIONS
export ASAN_OPTIONS=detect_leaks=0:halt_on_error=1
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
timeout --signal=TERM --kill-after=1s 30s /home/tommie/Projects/IMEC2/logs/mesh-stress-failures/20260813T145235376131Z-busy-line-seed-000000b2-fault-f4175e5f-cfg-aa0e9a0650acde86/mesh_stress --scenario busy-line --seed 0x000000b2 --fault-seed 0xf4175e5f --packets 2 --loss 50 --ack-loss 300 --duplicate 300 --delay 500 --max-delay-us 4000 --max-steps 300 --trace /home/tommie/Projects/IMEC2/logs/mesh-stress-failures/20260813T145235376131Z-busy-line-seed-000000b2-fault-f4175e5f-cfg-aa0e9a0650acde86/trace.jsonl
