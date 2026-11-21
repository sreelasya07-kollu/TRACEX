#pragma once
// SparsePointStore (Phase 9, stage 2): the sparse AR points of a scan, deduplicated into 5 cm voxels.
// Bounded: at capacity the least recently touched voxel is evicted (ties impossible: every touch gets a
// fresh sequence number), so the content depends only on the input order. No clock, no randomness.
#include <cstdint>
#include <functional>
#include <map>
#include <unordered_map>

#include "capturekit/guide/types.hpp"

namespace capturekit::guide {

class SparsePointStore {
 public:
  struct Voxel {
    Vec3 sum = Vec3::Zero();
    uint32_t n = 0;
    uint64_t seq = 0;  // last touch
    Vec3 mean() const { return n ? Vec3(sum / double(n)) : sum; }
  };

  explicit SparsePointStore(double voxel_m = 0.05, size_t capacity = 60000);

  /// Adds one point (non-finite points are ignored). Returns false when it was ignored.
  bool add(const Vec3& p);
  size_t size() const { return vox_.size(); }
  size_t capacity() const { return cap_; }
  double voxel_m() const { return vm_; }
  uint64_t evicted() const { return evicted_; }
  uint64_t version() const { return seq_; }  // bumps on every accepted point
  /// Visits every voxel in ascending key order (deterministic regardless of hash-table layout).
  void for_each(const std::function<void(const Voxel&)>& f) const;

 private:
  int64_t key(const Vec3& p) const;
  double vm_;
  size_t cap_;
  uint64_t seq_ = 0, evicted_ = 0;
  std::unordered_map<int64_t, Voxel> vox_;
  std::map<uint64_t, int64_t> age_;  // seq -> key, oldest first
};

}  // namespace capturekit::guide
