# Clock notes

How to measure: open the **Clock check** screen in the sample app, hold the phone still for 10 s, then move it for 10 s. It logs `clockcheck.jsonl` lines `{"src": "ar"|"gyro"|"accel", "raw_ns", "mono_ns"}`, where `mono_ns` is the platform monotonic clock read in the callback. Run `ckit inspect-raw <session> --clock` to get the median `raw−mono` offset and its spread per source.

Expected behaviour (from platform docs, to be confirmed per device):
- **iOS:** `ARFrame.timestamp`, `CMLogItem.timestamp` and `ProcessInfo.systemUptime` share one base, so the offset should be about 0 and the spread should be a few ms of callback latency.
- **Android:** `SensorEvent.timestamp` has used the `elapsedRealtimeNanos` base on modern devices, but some OEMs use `uptimeNanos`. ARCore `Frame.getTimestamp()` follows the camera `SENSOR_INFO_TIMESTAMP_SOURCE` (REALTIME or UNKNOWN). If the offset is greater than 1 ms, record it below and apply it at the edge.

## Measurements
| device-slug | OS | src | median offset (ns) | p95 abs spread (ns) | date | notes |
|---|---|---|---|---|---|---|
| (pending: needs physical devices) | | | | | | |
