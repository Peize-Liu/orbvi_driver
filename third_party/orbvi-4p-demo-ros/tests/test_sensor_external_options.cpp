#include "sensor_options.hpp"
#include <cassert>

int main() {
    orbvi::sensor::Options options;
    const char *normal[]{"sensor_demo"};
    assert(orbvi::sensor::parse_options(1, normal, "/dev/null", options) == 0);
    assert(!options.camera.external_sync);
    const char *external[]{"sensor_demo", "--external-sync", "--udp-dest", "127.0.0.1:5600"};
    assert(orbvi::sensor::parse_options(4, external, "/dev/null", options) == 0);
    assert(options.camera.external_sync && options.camera.fps == 30 && options.camera.camera_mask == 0xf);
    assert(options.imu_udp_dest == "127.0.0.1:5600");
    const char *external60[]{"sensor_demo", "--external-sync", "--fps", "60", "--sample-rate-hz", "1000"};
    assert(orbvi::sensor::parse_options(6, external60, "/dev/null", options) == 0);
    assert(options.camera.external_sync && options.camera.fps == 60 && options.imu_sample_rate_hz == 1000);
    const char *bad_rate[]{"sensor_demo", "--external-sync", "--fps", "31"};
    assert(orbvi::sensor::parse_options(4, bad_rate, "/dev/null", options) == 2);
    const char *bad_codec[]{"sensor_demo", "--external-sync", "--codec", "h265"};
    assert(orbvi::sensor::parse_options(4, bad_codec, "/dev/null", options) == 2);
}
