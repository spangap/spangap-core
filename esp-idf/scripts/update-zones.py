#!/usr/bin/env python3
"""
Download IANA→POSIX timezone mapping from GitHub, writing it as a nested
JSON tree shipped as a plain user-state file the device parses on demand.

Output goes to <out_dir>/factory_state/timezones.json — the root of the
device's state store, NOT under storage/external/. (It used to be an external
storage blob mounted into the config tree at `s.time.zones`, which kept the
whole ~15 KB map resident in RAM for the entire runtime. It now lives outside
storage so ntpApplyTimezone() can parse it transiently and free it.)
ETag cache (skips the download when GitHub says "not modified") goes to
<cache_dir>/.zones_etag, which defaults to <out_dir>.

Usage:
    python3 update-zones.py <out_dir> [--cache-dir <dir>] [--force]

A release-time step, NOT part of any build (so per-build images stay
reproducible). Run via spangap-core's `make timezones`, which points out_dir
at spangap-core/data — the platform-owned copy that ships to every consumer
through spangap_create_factory_image's data merge. The generated file is
checked in; re-run only to refresh the IANA data.
"""
import argparse
import json
import os
import sys
import urllib.error
import urllib.request

ZONES_URL = "https://raw.githubusercontent.com/nayarsystems/posix_tz_db/master/zones.json"


def fetch_zones(cache_path, force):
    """Download zones.json if modified since last check.
    Returns (dict, etag) or (None, None)."""
    headers = {}
    if not force and os.path.exists(cache_path):
        with open(cache_path) as f:
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
        os.makedirs(os.path.dirname(cache_path) or ".", exist_ok=True)
        with open(cache_path, "w") as f:
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


def write_zones_file(out_path, zones_nested, etag):
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    zones_nested["updated"] = etag
    with open(out_path, "w") as f:
        json.dump(zones_nested, f, indent=2, ensure_ascii=False)
        f.write("\n")
    n = sum(1 for v in zones_nested.values() if isinstance(v, dict))
    print(f"timezones.json: updated ({n} continents)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out_dir", help="output staging dir (writes "
                                    "factory_state/timezones.json under here)")
    ap.add_argument("--cache-dir", default=None,
                    help=".zones_etag location (default: out_dir)")
    ap.add_argument("--force", action="store_true",
                    help="ignore ETag cache and always re-download")
    args = ap.parse_args()

    out_dir = os.path.abspath(args.out_dir)
    cache_dir = os.path.abspath(args.cache_dir) if args.cache_dir else out_dir
    cache_path = os.path.join(cache_dir, ".zones_etag")
    out_path = os.path.join(out_dir, "factory_state", "timezones.json")

    zones, etag = fetch_zones(cache_path, args.force)
    if zones is None:
        return
    write_zones_file(out_path, flat_to_nested(zones), etag)


if __name__ == "__main__":
    main()
