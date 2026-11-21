#include "capturekit/guide/point_store.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace capturekit::guide {

SparsePointStore::SparsePointStore(double voxel_m, size_t capacity)
    : vm_(voxel_m > 0 ? voxel_m : 0.05), cap_(std::max<size_t>(1, capacity)) {}

int64_t SparsePointStore::key(const Vec3& p) const {
  // 21 bits per axis (+-52 km at 5 cm), offset to non-negative.
  auto q = [&](double x) {
    int64_t i = int64_t(std::floor(x / vm_)) + (int64_t(1) << 20);
    return std::clamp<int64_t>(i, 0, (int64_t(1) << 21) - 1);
  };
  return (q(p.x()) << 42) | (q(p.y()) << 21) | q(p.z());
}

bool SparsePointStore::add(const Vec3& p) {
  if (!p.allFinite()) return false;
  const int64_t k = key(p);
  auto it = vox_.find(k);
  if (it == vox_.end()) {
    if (vox_.size() >= cap_) {  // evict the least recently touched voxel
      auto old = age_.begin();
      vox_.erase(old->second);
      age_.erase(old);
      ++evicted_;
    }
    it = vox_.emplace(k, Voxel{}).first;
  } else {
    age_.erase(it->second.seq);
  }
  Voxel& v = it->second;
  v.sum += p;
  ++v.n;
  v.seq = ++seq_;
  age_.emplace(v.seq, k);
  return true;
}

void SparsePointStore::for_each(const std::function<void(const Voxel&)>& f) const {
  std::vector<int64_t> keys;
  keys.reserve(vox_.size());
  for (auto& kv : vox_) keys.push_back(kv.first);
  std::sort(keys.begin(), keys.end());
  for (int64_t k : keys) f(vox_.at(k));
}

}  // namespace capturekit::guide
