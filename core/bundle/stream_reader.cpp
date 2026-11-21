#include "capturekit/bundle/stream_reader.hpp"

#include <mcap/reader.hpp>

#include <cstdio>
#include <cstring>
#include <unordered_map>

#include "capturekit/bundle/codec.hpp"

namespace capturekit::bundle {
namespace {

void decode_into(StreamContents& out, const std::string& t, const std::byte* p, size_t n) {
  bool ok = true;
  auto take = [&](auto opt, auto& vec) { if (opt) vec.push_back(std::move(*opt)); else ok = false; };
  if (t == topic::kPose) take(codec::decode_pose(p, n), out.poses);
  else if (t == topic::kGyro) take(codec::decode_imu(p, n), out.gyro);
  else if (t == topic::kAccel) take(codec::decode_imu(p, n), out.accel);
  else if (t == topic::kIntrinsics) take(codec::decode_intrinsics(p, n), out.intrinsics);
  else if (t == topic::kFrameIndex) take(codec::decode_frame(p, n), out.frames);
  else if (t == topic::kEvents) take(codec::decode_event(p, n), out.events);
  else if (t == topic::kPoints) take(codec::decode_points(p, n), out.points);
  else if (t == topic::kPlanes) take(codec::decode_plane(p, n), out.planes);
  else if (t == topic::kDetections) take(codec::decode_detections(p, n), out.detections);
  else if (t == topic::kGuideLabels) take(codec::decode_labels(p, n), out.labels);
  else if (t == topic::kGuideShell) take(codec::decode_shell(p, n), out.shell);
  else if (t == topic::kGuideGaps) take(codec::decode_gaps(p, n), out.gaps);
  if (!ok) ++out.undecodable;
}

}  // namespace

StreamContents read_streams(const std::string& path) {
  StreamContents out;
  {
    // Finalized = footer + summary parse cleanly. Only the summary is read here.
    mcap::McapReader r;
    if (!r.open(path).ok()) {
      out.problems.push_back("open/footer: not a finalized MCAP");
    } else {
      auto st = r.readSummary(mcap::ReadSummaryMethod::NoFallbackScan);
      out.finalized = st.ok();
      if (!st.ok()) out.problems.push_back("summary: " + st.message);
      r.close();
    }
  }
  // Messages always come from one sequential pass over the data section, which works the
  // same for finalized, unfinalized (crash) and truncated files.
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) { out.problems.push_back("cannot open"); return out; }
  mcap::FileReader fr(f);
  if (fr.size() < sizeof(mcap::Magic)) { std::fclose(f); out.problems.push_back("too short"); return out; }
  std::byte* magic = nullptr;
  if (fr.read(&magic, 0, sizeof(mcap::Magic)) != sizeof(mcap::Magic) ||
      std::memcmp(magic, mcap::Magic, sizeof(mcap::Magic)) != 0) {
    std::fclose(f);
    out.problems.push_back("bad magic");
    return out;
  }
  out.opened = true;
  std::unordered_map<mcap::ChannelId, std::string> topics;
  mcap::TypedRecordReader rr(fr, sizeof(mcap::Magic));
  bool done = false;
  rr.onChannel = [&](const mcap::ChannelPtr c, mcap::ByteOffset, std::optional<mcap::ByteOffset>) { topics[c->id] = c->topic; };
  rr.onMessage = [&](const mcap::Message& m, mcap::ByteOffset, std::optional<mcap::ByteOffset>) {
    auto it = topics.find(m.channelId);
    if (it == topics.end()) { ++out.undecodable; return; }
    decode_into(out, it->second, m.data, size_t(m.dataSize));
  };
  rr.onDataEnd = [&](const mcap::DataEnd&, mcap::ByteOffset) { done = true; };
  mcap::ByteOffset last = 0;
  size_t stalls = 0;
  while (!done && rr.next()) {
    if (!rr.status().ok()) { out.problems.push_back("scan: " + rr.status().message); break; }
    if (rr.offset() == last && ++stalls > 4) { out.problems.push_back("scan: no progress"); break; }
    last = rr.offset();
  }
  if (!rr.status().ok() && (out.problems.empty() || out.problems.back().rfind("scan:", 0) != 0))
    out.problems.push_back("scan: " + rr.status().message);
  std::fclose(f);
  return out;
}

}  // namespace capturekit::bundle
