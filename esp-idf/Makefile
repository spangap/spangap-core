# spangap-core release-ceremony targets.
#
# These are NOT part of any build — they refresh checked-in factory data that
# ships to EVERY consumer through the data merge (spangap_create_factory_image
# merges spangap-core/data/ into each consumer's /fixed image). Keeping them out
# of the build keeps per-build images reproducible.

.PHONY: timezones

# Regenerate data/factory_state/timezones.json from the
# IANA->POSIX mapping (network fetch). Platform-owned: consumers inherit it via
# the data merge, so it lives here, not per-consumer. ETag cache in build/ so a
# clean forces a re-download. Run ahead of a release; commit the result.
timezones:
	python3 scripts/update-zones.py data --cache-dir build
