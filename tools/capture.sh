#!/bin/bash
set -eo pipefail
mode=$(python3 -c "import yaml; print(yaml.safe_load(open('/capture.yaml'))['transport'].get('mode','adb'))")
if [ "$mode" = adb ]; then
  python3 -u /sources/orbvi_driver/tools/imu_tunnel.py client --tcp-port 15601 --udp-port 5600 >&2 &
  relay=$!
  trap 'kill "$relay" 2>/dev/null || true' EXIT
fi
/work/driver-install/bin/orbvi_capture /capture.yaml "${1:-10}" 3
