#include "capturekit/bundle/stream_writer.hpp"

#include <mcap/writer.hpp>

#include <string>
#include <unordered_map>

#include "capturekit/bundle/codec.hpp"

namespace capturekit::bundle {

struct StreamWriter::Impl {
  mcap::McapWriter w;
  StreamWriterOptions opts;
  bool open = false;
  StreamCounts counts;
  int64_t last_flush_ns = INT64_MIN;
  struct Ch { mcap::ChannelId id = 0; int64_t last_t = INT64_MIN; uint32_t seq = 0; };
  std::unordered_map<std::string, Ch> ch;

  bool put(const char* topic, int64_t t_ns, const std::vector<std::byte>& data) {
    if (!open) return false;
    auto& c = ch[topic];
    if (t_ns <= c.last_t || t_ns < 0) { ++counts.rejected_non_monotonic; return false; }
    c.last_t = t_ns;
    mcap::Message msg;
    msg.channelId = c.id;
    msg.sequence = ++c.seq;
    msg.logTime = mcap::Timestamp(t_ns);
    msg.publishTime = msg.logTime;
    msg.data = data.data();
    msg.dataSize = data.size();
    if (!w.write(msg).ok()) return false;
    ++counts.per_topic[topic];
    if (counts.t_start_ns == 0 || t_ns < counts.t_start_ns) counts.t_start_ns = t_ns;
    if (t_ns > counts.t_end_ns) counts.t_end_ns = t_ns;
    if (last_flush_ns == INT64_MIN) last_flush_ns = t_ns;
    if (t_ns - last_flush_ns >= opts.flush_interval_ns) { flush(); last_flush_ns = t_ns; }
    return true;
  }
  void flush() {
    if (!open) return;
    w.closeLastChunk();
    if (auto* s = w.dataSink()) s->flush();
  }
};

StreamWriter::StreamWriter() : impl_(std::make_unique<Impl>()) {}
StreamWriter::~StreamWriter() { close(); }

bool StreamWriter::open(const std::string& path, const StreamWriterOptions& opts) {
  close();
  impl_ = std::make_unique<Impl>();
  impl_->opts = opts;
  mcap::McapWriterOptions o("capturekit");
  o.chunkSize = opts.chunk_size;
  o.compression = mcap::Compression::None;
  o.library = "capturekit-core";
  if (!impl_->w.open(path, o).ok()) return false;
  auto desc = codec::descriptor_set();
  auto schema = [&](const char* name) {
    mcap::Schema s(name, "protobuf", std::string(desc));
    impl_->w.addSchema(s);
    return s.id;
  };
  const std::pair<const char*, const char*> chans[] = {
      {topic::kPose, "capturekit.v1.Pose"},           {topic::kGyro, "capturekit.v1.ImuSample"},
      {topic::kAccel, "capturekit.v1.ImuSample"},     {topic::kIntrinsics, "capturekit.v1.Intrinsics"},
      {topic::kFrameIndex, "capturekit.v1.FrameIndex"}, {topic::kEvents, "capturekit.v1.Event"},
      {topic::kPoints, "capturekit.v1.PointCloud"},   {topic::kPlanes, "capturekit.v1.PlaneAnchor"},
      {topic::kDetections, "capturekit.v1.Detections"},
      {topic::kGuideLabels, "capturekit.v1.RoomLabels"}, {topic::kGuideShell, "capturekit.v1.RoomShell"},
      {topic::kGuideGaps, "capturekit.v1.RoomGaps"},
  };
  std::unordered_map<std::string, mcap::SchemaId> schemas;
  for (auto& [t, s] : chans) {
    if (!schemas.count(s)) schemas[s] = schema(s);
    mcap::Channel c(t, "protobuf", schemas[s]);
    impl_->w.addChannel(c);
    impl_->ch[t].id = c.id;
  }
  impl_->open = true;
  return true;
}

bool StreamWriter::is_open() const { return impl_ && impl_->open; }
bool StreamWriter::write(const PoseSample& m) { return impl_->put(topic::kPose, m.t_ns, codec::encode(m)); }
bool StreamWriter::write_gyro(const ImuSample& m) { return impl_->put(topic::kGyro, m.t_ns, codec::encode(m)); }
bool StreamWriter::write_accel(const ImuSample& m) { return impl_->put(topic::kAccel, m.t_ns, codec::encode(m)); }
bool StreamWriter::write(const IntrinsicsSample& m) { return impl_->put(topic::kIntrinsics, m.t_ns, codec::encode(m)); }
bool StreamWriter::write(const FrameRecord& m) {
  bool ok = impl_->put(topic::kFrameIndex, m.t_ns, codec::encode(m));
  if (ok && m.keyframe) ++impl_->counts.keyframes;
  return ok;
}
bool StreamWriter::write(const EventRecord& m) { return impl_->put(topic::kEvents, m.t_ns, codec::encode(m)); }
bool StreamWriter::write(const PointCloudRecord& m) { return impl_->put(topic::kPoints, m.t_ns, codec::encode(m)); }
bool StreamWriter::write(const PlaneRecord& m) { return impl_->put(topic::kPlanes, m.t_ns, codec::encode(m)); }
bool StreamWriter::write(const DetectionsRecord& m) { return impl_->put(topic::kDetections, m.t_ns, codec::encode(m)); }
bool StreamWriter::write(const LabelsRecord& m) { return impl_->put(topic::kGuideLabels, m.t_ns, codec::encode(m)); }
bool StreamWriter::write(const ShellRecord& m) { return impl_->put(topic::kGuideShell, m.t_ns, codec::encode(m)); }
bool StreamWriter::write(const GapsRecord& m) { return impl_->put(topic::kGuideGaps, m.t_ns, codec::encode(m)); }
void StreamWriter::flush() { impl_->flush(); }

StreamCounts StreamWriter::close() {
  if (impl_ && impl_->open) {
    impl_->w.close();
    impl_->open = false;
  }
  return impl_ ? impl_->counts : StreamCounts{};
}

const StreamCounts& StreamWriter::counts() const { return impl_->counts; }

}  // namespace capturekit::bundle
