/* Host test for storage_rotate.h: a reset injected after every file-system
 * step of a root write or a set-aside never leaves the store without a
 * readable generation, and never loses an unreadable file being set aside.
 *
 *   g++ -std=c++17 -Wall -I../../src test_storage_rotate.cpp -o /tmp/test_storage_rotate && /tmp/test_storage_rotate
 */
#include "storage_rotate.h"

#include <cstdio>
#include <map>
#include <string>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* In-memory fs. `budget` mutating steps succeed; the next one is cut short by
 * a reset (a write leaves a truncated file, a rename or remove does nothing)
 * and every later step fails. rename replaces its destination, as on
 * LittleFS and the fs layer's FAT rename. */
struct MemFs {
  std::map<std::string, std::string> files;
  int budget = 1 << 30;
  bool down = false;
  bool cutNow = false;   /* the step that just failed is the one the reset cut */
  bool step() {
    cutNow = false;
    if (down) return false;
    if (budget-- > 0) return true;
    down = cutNow = true;
    return false;
  }
  bool write(const std::string& p, const void* d, size_t n) {
    std::string s((const char*)d, n);
    if (!step()) { if (cutNow) files[p] = s.substr(0, n / 2) + "~"; return false; }
    files[p] = s;
    return true;
  }
  bool exists(const std::string& p) { return files.count(p) != 0; }
  bool rename(const std::string& a, const std::string& b) {
    if (!step() || !files.count(a)) return false;
    files[b] = files[a];
    files.erase(a);
    return true;
  }
  bool remove(const std::string& p) {
    if (!step()) return false;
    return files.erase(p) != 0;
  }
};

/* A generation is readable when it is exactly "gen<n>" (a truncated write
 * ends in '~'). Mirrors the load order: current, in-flight write, previous. */
static std::string load(MemFs& fs, const std::string& path, const std::string& bak) {
  for (const std::string& p : { path, path + ".new", bak }) {
    auto it = fs.files.find(p);
    if (it != fs.files.end() && it->second.rfind("gen", 0) == 0 &&
        it->second.back() != '~')
      return it->second;
  }
  return "";
}

int main() {
  const std::string cur = "/s/root.json.gz", bak = "/b/root.json.gz";

  /* A complete write rotates: current becomes previous, the new data current. */
  {
    MemFs fs;
    fs.files[cur] = "gen1";
    fs.files[bak] = "gen0";
    CHECK(srot::write(fs, cur, bak, "gen2", 4));
    CHECK(fs.files[cur] == "gen2");
    CHECK(fs.files[bak] == "gen1");
    CHECK(!fs.exists(cur + ".new"));
  }

  /* A reset after any step leaves gen1 or gen2 loadable — never nothing. */
  for (int k = 0; k < 6; k++) {
    MemFs fs;
    fs.files[cur] = "gen1";
    fs.files[bak] = "gen0";
    fs.budget = k;
    srot::write(fs, cur, bak, "gen2", 4);
    std::string got = load(fs, cur, bak);
    CHECK(got == "gen1" || got == "gen2");
    if (got != "gen1" && got != "gen2") std::printf("  reset after %d steps: loaded '%s'\n", k, got.c_str());
  }

  /* Without a previous-generation path the write is the plain .new + rename. */
  {
    MemFs fs;
    fs.files[cur] = "gen1";
    CHECK(srot::write(fs, cur, "", "gen2", 4));
    CHECK(fs.files[cur] == "gen2");
    CHECK(fs.files.size() == 1);
  }

  /* A failed write leaves current and previous untouched and no .new behind. */
  {
    MemFs fs;
    fs.files[cur] = "gen1";
    fs.files[bak] = "gen0";
    struct FailFs : MemFs {
      bool write(const std::string&, const void*, size_t) { return false; }
    } ffs;
    ffs.files = fs.files;
    CHECK(!srot::write(ffs, cur, bak, "gen2", 4));
    CHECK(ffs.files[cur] == "gen1" && ffs.files[bak] == "gen0");
    CHECK(!ffs.exists(cur + ".new"));
  }

  /* setAside keeps the newest `keep` unreadable files, slot 1 newest. */
  {
    MemFs fs;
    for (int i = 1; i <= 4; i++) {
      fs.files[cur] = "bad" + std::to_string(i);
      CHECK(srot::setAside(fs, cur, 3));
      CHECK(!fs.exists(cur));
    }
    CHECK(fs.files[cur + ".unreadable-1"] == "bad4");
    CHECK(fs.files[cur + ".unreadable-2"] == "bad3");
    CHECK(fs.files[cur + ".unreadable-3"] == "bad2");
    CHECK(!fs.exists(cur + ".unreadable-4"));
  }

  /* A reset at any step of a set-aside keeps the file at its path or slot 1,
   * and every older kept file survives somewhere. */
  for (int k = 0; k < 6; k++) {
    MemFs fs;
    fs.files[cur] = "badN";
    fs.files[cur + ".unreadable-1"] = "bad2";
    fs.files[cur + ".unreadable-2"] = "bad1";
    fs.budget = k;
    bool ok = srot::setAside(fs, cur, 3);
    bool atPath  = fs.exists(cur) && fs.files[cur] == "badN";
    bool inSlot1 = fs.files.count(cur + ".unreadable-1") && fs.files[cur + ".unreadable-1"] == "badN";
    CHECK(atPath != inSlot1);
    CHECK(ok == inSlot1);
    int seen = 0;
    for (auto& f : fs.files) if (f.second == "bad1" || f.second == "bad2") seen++;
    CHECK(seen == 2);
  }

  if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
  std::printf("all storage_rotate tests passed\n");
  return 0;
}
