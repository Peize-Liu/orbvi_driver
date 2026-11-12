#!/bin/bash
set -eo pipefail
mode=${1:?ros1 or ros2}
action=${2:-run}
seconds=${3:-10}
export LD_LIBRARY_PATH=/work/driver-install/lib:${LD_LIBRARY_PATH:-}
children=()
cleanup() { for pid in "${children[@]}"; do kill -TERM "$pid" 2>/dev/null || true; done; wait || true; }
trap cleanup EXIT
trap 'exit 0' TERM INT
if [ "$mode" = ros2 ]; then
  source /opt/ros/humble/setup.bash
  source /work/unified-ros2-install/setup.bash
  export RMW_IMPLEMENTATION=rmw_fastrtps_cpp
  export FASTRTPS_DEFAULT_PROFILES_FILE=/sources/orbvi_driver/config/fastdds.xml
  export RMW_FASTRTPS_PUBLICATION_MODE=ASYNCHRONOUS
  export ROS_DOMAIN_ID=86
  node=(/work/unified-ros2-install/orbvi_driver_ros2/lib/orbvi_driver_ros2/orbvi_node --ros-args -p config:=/capture.yaml)
  checker=/work/unified-ros2-install/orbvi_driver_ros2/lib/orbvi_driver_ros2/orbvi_ros_check
else
  export LD_LIBRARY_PATH=/work/driver-noetic-install/lib:/opt/orbvi/ffmpeg/lib:${LD_LIBRARY_PATH:-}
  source /opt/ros/noetic/setup.bash
  source /work/unified-ros1-install/setup.bash
  roscore >&2 & children+=($!)
  python3 - <<'PY'
import socket,time
end=time.monotonic()+20
while True:
    try:
        with socket.create_connection(('127.0.0.1',11311),timeout=.2):break
    except OSError:
        if time.monotonic()>end:raise
        time.sleep(.1)
PY
  node=(/work/unified-ros1-install/lib/orbvi_driver_ros1/orbvi_node _config:=/capture.yaml)
  checker=/work/unified-ros1-install/lib/orbvi_driver_ros1/orbvi_ros_check
fi
python3 -u /sources/orbvi_driver/tools/imu_tunnel.py client --tcp-port 15601 --udp-port 5600 >&2 & children+=($!)
"${node[@]}" >&2 & children+=($!)
if [ "$action" = check ]; then
  "$checker" /capture.yaml "$seconds"
elif [ "$action" = dashboard ]; then
  python3 -u /sources/orbvi_driver/tools/dashboard.py & children+=($!)
  wait -n "${children[@]}"
  exit 1
else
  wait -n "${children[@]}"
  exit 1
fi
