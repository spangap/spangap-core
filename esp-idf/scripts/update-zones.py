#!/usr/bin/env python3
"""
Download IANA→POSIX timezone mapping from GitHub.

Writes the result to data/factory_state/storage/external/s.time.zones.json,
which the device's storage layer mounts at the s.time.zones key on boot.

Usage: python3 scripts/update-zones.py [--force]

Checks ETag header; skips download if unchanged since last run.
Use --force to always download.
"""
import json, os, sys, urllib.request, urllib.error

ZONES_URL = "https://raw.githubusercontent.com/nayarsystems/posix_tz_db/master/zones.json"
ZONES_PATH = os.path.join(os.path.dirname(__file__), "..", "data", "factory_state",
                          "storage", "external", "s.time.zones.json")
CACHE_PATH = os.path.join(os.path.dirname(__file__), ".zones_etag")

def fetch_zones(force=False):
    """Download zones.json if modified since last check. Returns (dict, etag) or (None, None)."""
    headers = {}
    if not force and os.path.exists(CACHE_PATH):
        with open(CACHE_PATH) as f:
            headers["If-None-Match"] = f.read().strip()

    req = urllib.request.Request(ZONES_URL, headers=headers)
    try:
        resp = urllib.request.urlopen(req, timeout=15)
    except urllib.error.HTTPError as e:
        if e.code == 304:
            print("zones.json: not modified")
            return None, None
        raise
    except (urllib.error.URLError, OSError) as e:
        print(f"zones.json: skipped (no network: {e.reason})")
        return None, None

    etag = resp.headers.get("ETag", "")
    if etag:
        with open(CACHE_PATH, "w") as f:
            f.write(etag)

    data = json.loads(resp.read())
    print(f"zones.json: downloaded {len(data)} zones")
    return data, etag

def flat_to_nested(zones):
    """Convert {"Africa/Cairo": "EET-2..."} to nested {Africa: {Cairo: "EET-2..."}}."""
    nested = {}
    for iana, posix in sorted(zones.items()):
        parts = iana.split("/")
        node = nested
        for p in parts[:-1]:
            node = node.setdefault(p, {})
        node[parts[-1]] = posix
    return nested

def update_zones_file(zones_nested, etag):
    """Replace contents of s.time.zones.json (the external storage file)."""
    path = os.path.realpath(ZONES_PATH)
    os.makedirs(os.path.dirname(path), exist_ok=True)

    zones_nested["updated"] = etag
    with open(path, "w") as f:
        json.dump(zones_nested, f, indent=2, ensure_ascii=False)
        f.write("\n")

    n = sum(1 for v in zones_nested.values() if isinstance(v, dict))
    print(f"s.time.zones.json: updated ({n} continents)")

def main():
    force = "--force" in sys.argv
    zones, etag = fetch_zones(force)
    if zones is None:
        return
    nested = flat_to_nested(zones)
    update_zones_file(nested, etag)

if __name__ == "__main__":
    main()
