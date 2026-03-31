#!/usr/bin/env python3
"""
Download IANA→POSIX timezone mapping from GitHub and update settings.json.

Usage: python3 scripts/update-zones.py [--force]

Checks ETag header; skips download if unchanged since last run.
Use --force to always download.
"""
import json, os, sys, urllib.request, urllib.error

ZONES_URL = "https://raw.githubusercontent.com/nayarsystems/posix_tz_db/master/zones.json"
SETTINGS_PATH = os.path.join(os.path.dirname(__file__), "..", "data", "factory_state", "settings.json")
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

def update_settings(zones_nested, etag):
    """Replace s.time.zones in settings.json."""
    settings_path = os.path.realpath(SETTINGS_PATH)
    with open(settings_path) as f:
        settings = json.load(f)

    zones_nested["updated"] = etag
    s = settings.setdefault("s", {})
    t = s.setdefault("time", {})
    t["zones"] = zones_nested

    with open(settings_path, "w") as f:
        json.dump(settings, f, indent=2, ensure_ascii=False)
        f.write("\n")

    n = sum(1 for v in zones_nested.values() if isinstance(v, dict))
    print(f"settings.json: updated s.time.zones ({n} continents)")

def main():
    force = "--force" in sys.argv
    zones, etag = fetch_zones(force)
    if zones is None:
        return
    nested = flat_to_nested(zones)
    update_settings(nested, etag)

if __name__ == "__main__":
    main()
