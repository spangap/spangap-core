#pragma once
/* Crash-safe file generations for the store's root file.
 *
 * A file has up to three generations on disk:
 *   <path>      current — the last write whose final rename completed
 *   <path>.new  in flight — complete only once its write was synced
 *   <bak>       previous — what <path> held before the last write
 *
 * Write order (every step leaves a readable generation):
 *   1. write + sync <path>.new          current and bak untouched
 *   2. rename <path> -> <bak>           .new is complete; bak replaced
 *   3. rename <path>.new -> <path>      the commit point
 * A reset between 2 and 3 leaves no current but a complete .new, which is why
 * the load order is current, then .new, then bak.
 *
 * An unreadable file is never overwritten or rotated into <bak>: setAside()
 * moves it to <path>.unreadable-1 (older ones shift up, the oldest beyond
 * `keep` is dropped) before anything is written in its place.
 *
 * Written against a minimal file-system adaptor so the ordering can be
 * host-tested with a reset injected after every step. Fs provides:
 *   bool write(const std::string& p, const void* d, size_t n);  create/truncate, write all, sync
 *   bool exists(const std::string& p);
 *   bool rename(const std::string& from, const std::string& to);  replaces `to`
 *   bool remove(const std::string& p);
 */
#include <string>
#include <cstddef>

namespace srot {

template <class Fs>
bool write(Fs& fs, const std::string& path, const std::string& bak,
           const void* data, size_t len) {
  std::string tmp = path + ".new";
  if (!fs.write(tmp, data, len)) { fs.remove(tmp); return false; }
  if (!bak.empty() && fs.exists(path)) fs.rename(path, bak);
  return fs.rename(tmp, path);
}

template <class Fs>
bool setAside(Fs& fs, const std::string& path, int keep) {
  auto slot = [&](int i) { return path + ".unreadable-" + std::to_string(i); };
  if (keep < 1) keep = 1;
  if (fs.exists(slot(keep))) fs.remove(slot(keep));
  for (int i = keep - 1; i >= 1; i--)
    if (fs.exists(slot(i))) fs.rename(slot(i), slot(i + 1));
  return fs.rename(path, slot(1));
}

}  // namespace srot
