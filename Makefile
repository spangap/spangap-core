# Spangap workspace entry points. Spangap itself isn't an IDF project, so
# there's no idf.py to hang actions off — `make` plays that role here.

.PHONY: reallyclean help

help:
	@echo "Targets:"
	@echo "  reallyclean   Strip browser/{node_modules,dist,.quasar,package-lock.json} and .DS_Store"

reallyclean:
	@bash scripts/reallyclean.sh
