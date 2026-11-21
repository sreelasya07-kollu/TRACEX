#pragma once
// Hand-rolled protobuf wire codec for capturekit.v1 messages (spec/scanbundle/messages/capturekit.proto).
// Kept dependency-free so the core builds identically on iOS, Android and desktop.
#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

#include "capturekit/bundle/types.hpp"

namespace capturekit::bundle::codec {

std::vector<std::byte> encode(const PoseSample& m);
std::vector<std::byte> encode(const ImuSample& m);
std::vector<std::byte> encode(const IntrinsicsSample& m);
std::vector<std::byte> encode(const FrameRecord& m);
std::vector<std::byte> encode(const EventRecord& m);
std::vector<std::byte> encode(const PointCloudRecord& m);
std::vector<std::byte> encode(const PlaneRecord& m);
std::vector<std::byte> encode(const DetectionsRecord& m);
std::vector<std::byte> encode(const LabelsRecord& m);  // schema 1.3.0 (ADR 0028)
std::vector<std::byte> encode(const ShellRecord& m);
std::vector<std::byte> encode(const GapsRecord& m);

// Decoders return nullopt on malformed input (never throw, never read out of bounds).
std::optional<PoseSample> decode_pose(const std::byte* p, size_t n);
std::optional<ImuSample> decode_imu(const std::byte* p, size_t n);
std::optional<IntrinsicsSample> decode_intrinsics(const std::byte* p, size_t n);
std::optional<FrameRecord> decode_frame(const std::byte* p, size_t n);
std::optional<EventRecord> decode_event(const std::byte* p, size_t n);
std::optional<PointCloudRecord> decode_points(const std::byte* p, size_t n);
std::optional<PlaneRecord> decode_plane(const std::byte* p, size_t n);
std::optional<DetectionsRecord> decode_detections(const std::byte* p, size_t n);
std::optional<LabelsRecord> decode_labels(const std::byte* p, size_t n);
std::optional<ShellRecord> decode_shell(const std::byte* p, size_t n);
std::optional<GapsRecord> decode_gaps(const std::byte* p, size_t n);

/// FileDescriptorSet for capturekit.proto (MCAP schema data), embedded at build time.
std::string_view descriptor_set();

}  // namespace capturekit::bundle::codec
