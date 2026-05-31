# Getting started

First-time **host setup** for building and flashing a spangap device.
For the day-to-day build / flash / debug workflow (including the split
VM+host setup), see [development.md](development.md); for platform
conventions and the module map, see [`../CLAUDE.md`](../CLAUDE.md).

## Build dependencies

### ESP-IDF toolchain
ESP-IDF **v5.5.4**, installed via EIM at `~/.espressif/v5.5.4/esp-idf`.
The `~/bin/idf.py` wrapper auto-activates its venv. See
[development.md → Toolchain](development.md) for the wrapper, the `~/bin`
symlinks (`idf.py`, `flasher`, `spangap-cli`), and the `--spangap`
sibling-checkout flag.

### Browser SPA
The web interface (`<consumer>/web-interface/`) is built with
Quasar/Vite by the consumer's `deploy.sh`, invoked automatically by the
CMake build. Requires Node/npm (see the consumer's `package.json`).

### LCD icon rasterizer — only for `CONFIG_SPANGAP_LCD` builds
The `lcd` module ships launcher icons as LVGL `RGB565A8 .bin` files,
generated at build time from `.svg` / `.png` sources by
[`spangap-core/scripts/lcd-icons.py`](../spangap-core/scripts/lcd-icons.py)
(wired in via the `spangap_lcd_icons()` CMake helper). It needs:

- **libcairo** — native library, for SVG rasterization:
  - macOS: `brew install cairo`
  - Debian/Ubuntu: `sudo apt install libcairo2`
- **Python packages**, installed into the **ESP-IDF Python environment
  the build uses** (not the system Python):
  ```bash
  # $IDF_PYTHON_ENV_PATH is set after sourcing ESP-IDF's export.sh
  "$IDF_PYTHON_ENV_PATH/bin/python" -m pip install pillow cairosvg pypng lz4
  ```
  - `pillow` — resize `.png` sources.
  - `cairosvg` (needs libcairo) — rasterize `.svg` sources to PNG.
  - `pypng`, `lz4` — required by LVGL's bundled `LVGLImage.py`
    (the PNG→`.bin` converter), even when compression isn't used.

This toolchain is **optional and best-effort**: if any piece is missing,
`lcd-icons.py` prints a warning and skips, the firmware still builds, and
launcher tiles render label-only until icons are present. Pre-rendered
`.bin` files dropped directly under `<consumer>/data/lcd/icons/<WxH>/`
ship via the normal factory-image merge with no tooling at all.

### LVGL fonts — only to *regenerate* bundled fonts
The on-device UI ships its fonts as **checked-in** C arrays, so an ordinary
build needs no font tooling. You only need a toolchain to regenerate them:

- **Accented Montserrat** (`lv_font_montserrat_12_latin`) — needs
  [`lv_font_conv`](https://github.com/lvgl/lv_font_conv): `npm i -g lv_font_conv`,
  or rely on Node's `npx` (the generator falls back to `npx -y lv_font_conv`).
  Regenerate with [`spangap-core/scripts/gen-text-font.py`](../spangap-core/scripts/gen-text-font.py);
  it reuses the Montserrat/FontAwesome TTFs bundled with the lvgl component.
- **Spleen 5×8** terminal font — regenerates from its BDF via
  `gen-spleen-font.py`, no extra tooling.

See [lcd.md → Fonts](lcd.md) for what each font is for and how to widen accent
coverage.
