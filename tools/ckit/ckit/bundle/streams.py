"""Reads streams.mcap with the standard `mcap` + `protobuf` libraries (independent of the C++
reader, so round-trip tests prove interop with downstream tools)."""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path

from google.protobuf import descriptor_pb2, descriptor_pool, message_factory
from mcap.reader import SeekingReader
from mcap.records import Channel, Message, Schema
from mcap.stream_reader import StreamReader

from ckit.bundle import DESC_PATH

TOPICS = {
    "/pose": "capturekit.v1.Pose",
    "/imu/gyro": "capturekit.v1.ImuSample",
    "/imu/accel": "capturekit.v1.ImuSample",
    "/intrinsics": "capturekit.v1.Intrinsics",
    "/frame_index": "capturekit.v1.FrameIndex",
    "/events": "capturekit.v1.Event",
    "/ar/points": "capturekit.v1.PointCloud",
    "/ar/planes": "capturekit.v1.PlaneAnchor",
    "/ar/detections": "capturekit.v1.Detections",
    "/guide/labels": "capturekit.v1.RoomLabels",
    "/guide/shell": "capturekit.v1.RoomShell",
    "/guide/gaps": "capturekit.v1.RoomGaps",
}
REQUIRED_TOPICS = ["/pose", "/imu/gyro", "/imu/accel", "/intrinsics", "/frame_index", "/events"]
# Optional from schema 1.4.0 (ADR 0034): the lean bundle does not record the IMU.
IMU_TOPICS = frozenset({"/imu/gyro", "/imu/accel"})

_classes: dict[tuple[bytes, str], type] = {}


def message_class(schema_data: bytes, name: str):
    key = (schema_data, name)
    if key not in _classes:
        fds = descriptor_pb2.FileDescriptorSet.FromString(schema_data)
        pool = descriptor_pool.DescriptorPool()
        for fd in fds.file:
            pool.Add(fd)
        desc = pool.FindMessageTypeByName(name)
        get = getattr(message_factory, "GetMessageClass", None)
        _classes[key] = (
            get(desc) if get else message_factory.MessageFactory(pool).GetPrototype(desc)
        )
    return _classes[key]


def spec_class(name: str):
    return message_class(DESC_PATH.read_bytes(), name)


@dataclass
class Streams:
    messages: dict[str, list] = field(default_factory=dict)  # topic -> decoded protobuf messages
    log_times: dict[str, list[int]] = field(default_factory=dict)
    finalized: bool = False
    readable: bool = False
    problems: list[str] = field(default_factory=list)
    topics_present: set[str] = field(default_factory=set)

    def get(self, topic: str) -> list:
        return self.messages.get(topic, [])


def read_streams(path: Path) -> Streams:
    s = Streams()
    try:
        with open(path, "rb") as f:
            summary = SeekingReader(f).get_summary()
            s.finalized = summary is not None
    except Exception as e:  # noqa: BLE001 - any parse failure means "not finalized"
        s.problems.append(f"summary: {type(e).__name__}: {e}")
    schemas: dict[int, Schema] = {}
    channels: dict[int, Channel] = {}
    try:
        with open(path, "rb") as f:
            for rec in StreamReader(f, skip_magic=False, validate_crcs=True).records:
                s.readable = True
                if isinstance(rec, Schema):
                    schemas[rec.id] = rec
                elif isinstance(rec, Channel):
                    channels[rec.id] = rec
                    s.topics_present.add(rec.topic)
                elif isinstance(rec, Message):
                    ch = channels.get(rec.channel_id)
                    sc = schemas.get(ch.schema_id) if ch else None
                    if ch is None or sc is None or ch.topic not in TOPICS:
                        continue
                    cls = message_class(sc.data, sc.name)
                    s.messages.setdefault(ch.topic, []).append(cls.FromString(rec.data))
                    s.log_times.setdefault(ch.topic, []).append(rec.log_time)
    except Exception as e:  # noqa: BLE001 - truncated/corrupt data ends the scan
        s.problems.append(f"scan: {type(e).__name__}: {e}")
    return s
