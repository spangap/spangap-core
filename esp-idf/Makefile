# spangap-core release-ceremony targets.
#
# These are NOT part of any build — they refresh checked-in sources. Keeping
# them out of the build keeps per-build images reproducible.

.PHONY: timezones

# Regenerate src/timezones_gen.c (the built-in IANA->POSIX zone table, two
# sorted rodata arrays) from the upstream mapping (network fetch). Platform-
# owned: every consumer compiles it in, so zone data rides each firmware
# update. ETag cache in build/ so a clean forces a re-download. Run ahead of a
# release; commit the result.
timezones:
	python3 scripts/update-zones.py . --cache-dir build
