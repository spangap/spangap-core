# LCD — on-device LVGL launcher

`lcd` is spangap's on-device UI: a phone-style launcher of program icons under an
opaque status bar, owning the display, the input devices, and the single LVGL
context. Other components register *programs* (full-screen apps reachable from
the launcher) and *settings panes*; the lcd module handles bring-up, layering,
navigation, the icon pipeline, and the clock/Wi-Fi status bar.

Gated on `CONFIG_SPANGAP_LCD`. When off, no `src/lcd_ui/*` file compiles and
LVGL / `esp_lcd` / `esp_lcd_touch` are never pulled into the build. When on,
`lcdInit()` is called automatically by `spangapInit()` (after storage + web, so
the status bar can read config and Wi-Fi state).

Public API: [lcd.h](../spangap-core/include/lcd.h). Board HAL contract:
[lcd_board.h](../spangap-core/include/lcd_board.h).

---

## Developer perspective

### Enabling it

Two things are required in the consuming buildable straddle:

1. **`spangap/spangap-lcd` in `straddle.yaml`** — under `requires:` if the
   build is unconditionally LCD-based (the natural choice for a board with
   a display), or `optional_requires:` to keep `--no-lcd` available.
   That's the on/off switch; `CONFIG_SPANGAP_LCD` is the short-form
   presence symbol `spangap-inside` emits when the straddle is staged.
2. **A board HAL**, registered with `lcdSetBoard()` **before** `spangapInit()`
   (which calls `lcdInit()`). Without one, `lcdInit()` logs and bails — no
   display is brought up. See [The board HAL](#the-board-hal-lcd_boardh).
   The board-specific touch driver (e.g. `espressif/esp_lcd_touch_gt911`)
   stays in the consumer's `main/idf_component.yml` since it's board-pinned.
   `lvgl` itself is pinned by `spangap-lcd` — the consumer doesn't list it.

The T-Deck Plus reference implementation is
[tdeck.cpp](../../reticulous/main/tdeck.cpp); its `app_main` calls
`tdeckPreInit()` (which registers the board HAL) then `spangapInit()`.

### The lcd task contract

**LVGL is not thread-safe, and everything that touches it runs on the lcd
task.** That is the one rule. To run code there from any other task, hop on with
`lcdRun()`:

```cpp
lcdRun(ON_LCD {
    lv_obj_t* layer = (lv_obj_t*)arg;   // arg is whatever you passed (default null)
    /* ...LVGL calls, safe here only... */
});
```

`ON_LCD` is sugar for a captureless `[](void* arg)` lambda (it mirrors storage's
`ON_CHANGE`). `lcdRun()` queues the call as an ITS aux message and returns
immediately; `fn` runs shortly after on the lcd task. Safe from any task.

The lcd task **never does flash I/O** — it runs an `itsPoll` loop whose
notification accounting must stay intact, so a blocking FS round-trip would
desync it. Icon files are read by a separate loader task (see
[Icon cache + loader](#icon-cache--loader-lcd_iconscpp)). If you need a file's
bytes on the lcd task, read them elsewhere and hand them over via `lcdRun`.

### Registering a program

A program is a full-screen app reachable from the launcher. Register it once,
from any task, any time:

```cpp
lcdRegister("Reticulum", "rns", ON_LCD {
    lv_obj_t* layer = (lv_obj_t*)arg;        // your program's own layer
    lv_obj_t* lbl = lv_label_create(layer);
    lv_label_set_text(lbl, "hello from rns");
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_center(lbl);
});
```

- `name` is the icon's label.
- `iconBasename` names a `.bin` icon under `/fixed/lcd/icons/<res>/<basename>.bin`
  — no path, no size, no extension. `<res>` is fixed to the launcher tile size
  (`LAUNCHER_ICON_RES`, `36x36`); icons render at that native size, no scaling.
  Until the bytes load, the tile shows label-only; missing icons just stay
  label-only. See [Icons](#icons).
- `fn` runs **once**, on the lcd task, with the program's layer the first time
  its icon is opened (or again if the layer was reclaimed). **The fn must touch
  nothing outside that layer.**

The layer persists after `fn` returns: opening another program hides yours;
re-opening reveals it exactly as left (your widgets are still there — `fn` does
not run again). Build your whole UI into `layer` in the one call.

### Navigation

- A **swipe up from the bottom edge** (the home-bar strip) hides the current
  program and returns to the launcher.
- A **hardware Home button**, if the board provides one (`button_read`, see
  [The board HAL](#the-board-hal-lcd_boardh)): a **≥1 s hold** returns to the
  launcher; a **short press** is a normal click on the focused widget, so the
  same button doubles as the trackball/centre click.
- Either way the current program **slides up off the top** to reveal the
  launcher (a short LVGL animation in `lcdGoHomeInternal`).
- `lcdGoHome()` does the same programmatically — safe to call from inside a
  registered fn (e.g. wire it to a Back button). It hops onto the lcd task for
  you.

### Input devices

The lcd task owns all input and routes it through one LVGL **focus group**,
`lcdInputGroup()`:

- **Touch** (pointer indev) — the primary pointer when the board has a panel.
- **Hardware button** (`button_read`) — a keypad indev: short press = `ENTER` on
  the focused widget, ≥1 s hold = Home. *(Skipped when a cursor device is present
  — see below — which claims the button as its click instead.)*
- **Hardware keyboard** — *not* owned by lcd. A keyboard's wiring/quirks are
  device-specific, so a **consumer** creates its own `LV_INDEV_TYPE_KEYPAD` indev,
  joins it to `lcdInputGroup()`, and calls `lcdSetHasKeyboard(true)` so text
  fields edit in place instead of popping the on-screen keyboard. lcd never reads
  a keyboard itself. (T-Deck example: [reticulous/main/tdeck.cpp](../../reticulous/main/tdeck.cpp).)
- **Cursor device** (`pointer_read`) — a trackball/mouse: a second pointer indev
  with a visible cursor. The board owns the absolute position (it integrates its
  own device's motion); lcd draws the cursor and uses `button_read` as the click
  (short press = click at the cursor, ≥1 s hold = Home). The cursor **shows on
  motion and auto-hides** via a one-shot `lv_timer` that only runs during the
  visible window. lcd owns the cursor but not the *dwell policy*: the consumer
  that owns the pointing device sets it with `lcdPointerSetVisibleMs(ms)`
  (`<0` = always visible; lcd defaults to 2 s until told otherwise). Because this
  pointer owns the centre button, the keypad button indev is not created when
  `pointer_read` is set.

**The lcd-owned indevs are interrupt-driven, never polled.** Touch, button and
cursor indevs are all `LV_INDEV_MODE_EVENT`, so LVGL runs no read timer for them —
the hardware is read only on a real edge. The board attaches the lcd component's
exported `lcdInputISR` (see [lcd_board.h](../spangap-core/include/lcd_board.h)) to
each input INT line; the ISR does nothing but flag + `vTaskNotifyGiveFromISR` the
lcd task, which then calls each indev's `read_cb` once via `lcdInputPoll()`.
Time-based behaviour stays event-driven too: touch tracking while a finger is
down is a 10 ms `lv_timer` that exists only during a touch, and the button's ≥1 s
hold is a one-shot `lv_timer` armed on press. With nothing held, the lcd task is
idle — and to keep it that way it pauses any released indev's LVGL read timer each
loop (`lcdPauseIdleInputTimers`), since LVGL resumes a pointer's read timer on
press for its own long-press timing and can miss the release-pause.

> A **consumer keyboard** may not be able to follow this model — e.g. the T-Deck's
> ESP32-C3 keyboard never drives its INT and its read is destructive — so the
> consumer polls it on its own task, off the lcd task, and bumps lcd with
> `lcdRun()` only when a real key arrives (see
> [tdeck.cpp](../../reticulous/main/tdeck.cpp) and
> [tdeck.md](../../reticulous/docs/tdeck.md) §1.2). lcd stays unaware.

To build your own focusable widget in a program (e.g. a `lv_textarea`), add it to
the group so the keyboard/button can reach it:

```cpp
lv_obj_t* ta = lv_textarea_create(layer);
lv_group_add_obj(lcdInputGroup(), ta);   // the HW keyboard now types into it
```

### Backlight

```cpp
lcdSetBacklight(200);   // 0..255, 0 = off; any task
```

Persists `s.lcd.backlight`. The value is applied through a storage
subscription (single source of truth), so writing the key directly with
`storageSet("s.lcd.backlight", n)` — e.g. from the browser or a settings pane —
has the identical effect.

### Settings panes

The module provides a built-in **Settings** program (a gear tile, registered by
`lcdSettingsInit()` from the lcd task) with a nested menu, and `lcdSetting*`
helpers that build uniform storage-bound rows — the on-device analogue of the
browser's `Setting*` components. The platform itself registers one pane out of
the box: **net** adds a `Net/Wifi` pane (see `wifiSettingsPane` in
[net.cpp](../spangap-core/src/net.cpp)).

Register a leaf item with a slash-path; intermediate segments auto-become
submenus:

```cpp
lcdRegisterSettings("Net/Wifi", "Wifi", ON_LCD {
    lv_obj_t* pane = (lv_obj_t*)arg;         // empty scrollable flex-column
    lcdSettingSection (pane, "WiFi");
    lcdSettingSwitch  (pane, "Enable", "s.net.wifi.enable");
    lcdSettingValue   (pane, "Status", "wifi.sta.state");     // read-only, live
    lcdSettingValue   (pane, "IP",     "wifi.sta.ip");
    lcdSettingSection (pane, "Access Point");
    lcdSettingText    (pane, "Name",     "s.net.wifi.ap.ssid");
    lcdSettingText    (pane, "Password", "s.net.wifi.ap.pass");
});
```

`"Net/Wifi"` creates a **Net** submenu containing a **Wifi** item. Call at init,
before Settings is first opened; it populates an in-RAM registry, so it works
even before `lcdInit()` and from any init task.

> **Path casing.** Submenu segments are matched **case-sensitively by their raw
> id**, so two components that want to share a submenu must spell the segment
> identically. Use first-letter-uppercase segments (`Net/...`, not `net/...`) —
> that's the platform convention, so everything lands in the same `Net` submenu.

Helpers (all run on the lcd task, inside a settings fn; each returns the row):

| Helper | Control | Bound to |
|---|---|---|
| `lcdSettingSection(parent, title)` | bold divider, no control | — |
| `lcdSettingSwitch(parent, label, key)` | toggle | int key (0/1) |
| `lcdSettingSlider(parent, label, key, min, max)` | slider, clamped | int key |
| `lcdSettingText(parent, label, key, secret=false)` | text field → inline edit (HW keyboard) or on-screen keyboard | string key |
| `lcdSettingDropdown(parent, label, key, optionsCsv)` | dropdown | string key |
| `lcdSettingValue(parent, label, key)` | read-only, live (event-driven) | string key |
| `lcdSettingButton(parent, label, onClick)` | action button | — (`onClick` runs on lcd task) |

> **Key lifetime:** storage keys passed to the helpers are stored **by pointer**,
> not copied — panes are rebuilt on every navigation. Pass string literals /
> static storage, never a temporary `std::string`'s `.c_str()`.

Writes apply immediately (there is no "save" concept) — a switch flips the
config key the instant it's toggled.

**Two-way bound.** Every control is also *subscribed* to its key, so an external
write (browser, CLI, another task) flows back into the control — flip a switch in
the browser and the on-device switch follows, and vice versa. `lcdSettingValue`
is purely event-driven off this subscription (no polling). Bindings tear down on
widget delete and the key is unsubscribed once its last binding goes.

**Text entry.** When a consumer has reported a hardware keyboard
(`lcdSetHasKeyboard(true)`), `lcdSettingText` edits in place — the value becomes
an inline `lv_textarea`; focus it, type, Enter commits. Otherwise it falls back to
a full-screen `lv_keyboard` overlay. The on-screen keyboard appears only when
`lcdHasKeyboard()` is false.

For a custom focusable control outside the helpers, expose it to the keyboard via
`lcdInputGroup()` (`lv_group_add_obj`).

### Built-in programs: Log + CLI

The platform registers two programs out of the box (alongside the gear),
implemented in [lcd_apps.cpp](../spangap-core/src/lcd_ui/lcd_apps.cpp) and
auto-added at lcd init — the on-device counterparts of the browser's Log and CLI
windows. Both are monospace terminals in the **Spleen 5×8** bitmap font
(`lv_font_spleen_5x8`, ~63 cols × 24 rows on a 320×240 panel): a scrollable
wrapping label for output, capped to **`s.log.file.paste` kB** of scrollback
(the same knob that sizes the log paste-back) with oldest whole lines trimmed
off the top. The body is the virtualized `lcdTextView` — it keeps the full
scrollback in a string but only lays out the on-screen window. Because a panel
flush paints the whole text region at only a few Hz, rendering is **debounced**:
append/scroll mark the view dirty and (re)arm a paused lv_timer that fires once
activity has settled, then does one reflow + repaint of the *latest* state
(capped so a non-stop stream still updates ~1 Hz). So a boot-time log flood
paints once when it quiets instead of churning through every intermediate scroll
position, and a drag redraws when the finger stops. The CLI program adds a one-line command box (added to
`lcdInputGroup()`, so the hardware keyboard types into it); Enter local-echoes
the line, sends it, and clears.

**The lcd task is itself an ITS *client* here** (`itsClientInit()` in lcd.cpp).
Each program dials the log / CLI task's packet-mode DC port (`log:1` / `cli:1`)
when its layer is first built; recv + disconnect callbacks are dispatched by the
lcd task's own `itsPoll` loop, so they touch LVGL directly. Log connects with
the connect-payload `{"ansi":0}` so it gets paste-back + live lines **without**
ANSI colour escapes (LVGL can't render them); CLI uses the DC port's LINE mode
(the device echoes nothing — the program echoes locally). The log/CLI DC ports
each accept **2** handles now (browser xterm + on-device viewer).

**Lifecycle — the connection is bound to the layer's existence, not its
visibility.** A program hidden behind another keeps its connection open and
keeps appending to its (offscreen) widgets, so re-opening shows current state.
The binding is an `LV_EVENT_DELETE` handler on the program layer: when the
launcher evicts a layer (`lv_obj_del`), LVGL fires DELETE, the handler closes
the ITS connection, and the next open rebuilds + reconnects from scratch.
Callbacks additionally guard with `lv_obj_is_valid()`. (Eviction is a launcher
*capability* — see [Launcher](#launcher-lcd_launchercpp) — not yet triggered by
any policy; wiring DELETE now means the apps are already correct when one lands.)

### Config keys

| Key | Default | Meaning |
|---|---|---|
| `s.lcd.backlight` | `200` | backlight 0..255; applied live |
| `s.lcd.date_format` | `"%d %b %Y, %H:%M"` | `strftime` format for the status-bar clock |

(`s.lcd.icon_res` is no longer used — the icon resolution is fixed to the
launcher tile size in code, `LAUNCHER_ICON_RES`; see [Icons](#icons).)

Both are live: the lcd task subscribes and reacts (backlight immediately; the
clock format on its next tick). They're `s.*`, so they sync to the browser. The
clock re-renders only as often as the shown value changes — once a minute, aligned
to the minute boundary, for the default format, or every second only if the format
includes seconds.

### Icons

Icons are LVGL `RGB565A8` (16-bit colour + 8-bit alpha) `.bin` files, shipped
read-only under `/fixed/lcd/icons/<WxH>/<name>.bin` and loaded by basename.

**Build pipeline.** Drop `*.svg` / `*.png` *sources* somewhere outside `data/`
(e.g. `assets/lcd-icons/`) and rasterize them at build time with the
`spangap_lcd_icons()` CMake helper:

```cmake
spangap_lcd_icons(SRC_DIR "${CMAKE_SOURCE_DIR}/assets/lcd-icons"
                  SIZES "36x36")   # 36x36 = the launcher's LAUNCHER_ICON_RES
```

It renders each source to each size bucket and converts via LVGL's bundled
`LVGLImage.py` into `data_merged/lcd/icons/<WxH>/<name>.bin`, which ships to
`/fixed`. It must run after `spangap_create_factory_image()`.

**Platform defaults are merged in.** The helper *always* also rasterizes
spangap-core's own [`assets/lcd-icons/`](../spangap-core/assets/lcd-icons/)
(`gear`, `log`, `cli` — the icons for the built-in programs), so every LCD
consumer gets them with no per-app work. The two source dirs are merged by
basename with the **consumer winning** on a collision — the build-time analogue
of the `data/` merge. `SRC_DIR` is therefore optional: a consumer with no icons
of its own can call `spangap_lcd_icons()` bare and still inherit the platform
set, or pass `SRC_DIR` to add/override its own.

**Best-effort by design:** if the host lacks Pillow / cairosvg / `LVGLImage.py`,
the script warns and skips — the firmware still builds and tiles render
label-only until icons are present. You can also drop **pre-rendered** `.bin`
files directly under `<consumer>/data/lcd/icons/<WxH>/`; they ship via the normal
data merge with no tooling. Host setup for the rasterizer (libcairo + Pillow +
cairosvg + pypng + lz4) is in [getting-started.md](getting-started.md).

### Fonts

Two bundled fonts beyond LVGL's stock set, both checked-in C arrays under
`src/lcd_ui/`:

- **`lv_font_spleen_5x8`** — 1-bpp monospace bitmap (ASCII), for the Log / CLI
  terminals (above). Built by [`scripts/gen-spleen-font.py`](../spangap-core/scripts/gen-spleen-font.py)
  from `scripts/spleen-5x8.bdf` — no extra tooling.
- **`lv_font_montserrat_12_latin`** — 4-bpp proportional, declared in `lcd.h`.
  A drop-in accented **superset** of LVGL's `lv_font_montserrat_12`: the stock
  Montserrat fonts carry only ASCII + a sparse symbol set, so accented user text
  (`ä`, `é`, `ñ`, …) renders as placeholder boxes. This one adds Latin-1
  Supplement + Latin Extended-A (U+00A0–U+017F: Western & Central European) while
  keeping the full `LV_SYMBOL_*` set. Use it for any widget showing user text:
  `lv_obj_set_style_text_font(label, &lv_font_montserrat_12_latin, 0)`.

  Text is UTF-8 end to end — storage and `lv_label_set_text` both take UTF-8, and
  LVGL decodes each sequence to a codepoint and looks the glyph up by codepoint.
  Nothing converts; the font just has to *carry* that codepoint (which the range
  above ensures). `ä` is the two wire bytes `0xC3 0xA4` → codepoint U+00E4.

**Regenerate** the accented font with
[`scripts/gen-text-font.py`](../spangap-core/scripts/gen-text-font.py) — it wraps
[`lv_font_conv`](https://github.com/lvgl/lv_font_conv) over the Montserrat +
FontAwesome TTFs that ship inside the lvgl component (run it from a consumer
checkout so it finds them, or pass `--lvgl-fonts`), then rewrites the include /
header to match the tree so a re-run reproduces the committed `.c` verbatim. The
checked-in `.c` means an ordinary build never needs the tool — see
[getting-started.md](getting-started.md) for installing it. Both source fonts are
OFL-1.1 (Font Awesome Free also CC-BY-4.0); widen coverage by editing
`TEXT_RANGE` in the script and regenerating.

### The board HAL (`lcd_board.h`)

All hardware wiring — SPI pins, panel controller, backlight GPIO, touch bus —
lives behind the [lcd_board.h](../spangap-core/include/lcd_board.h) contract, so
there is no link-time dependency from spangap-core back onto consumer symbols.
Fill an `lcd_board_t` (a static, must outlive the process) and register it
before `spangapInit()`:

```cpp
typedef struct {
    esp_lcd_panel_handle_t (*init)(esp_lcd_panel_io_handle_t* ioOut,
                                   int* wOut, int* hOut);   // required
    void                   (*shutdown)(void);               // may be NULL
    void                   (*backlight)(uint8_t level);     // may be NULL
    esp_lcd_touch_handle_t (*touch_init)(void);             // may be NULL
    bool                   (*button_read)(void);            // may be NULL
    bool                   (*pointer_read)(int* x, int* y); // may be NULL
} lcd_board_t;
void lcdSetBoard(const lcd_board_t* board);

// And the shared input ISR the board attaches to its INT lines:
void lcdInputISR(void* arg);   // flag + wake the lcd task; IRAM-safe
```

- `init` brings up the (shared) SPI bus, panel IO, and controller; sets
  orientation; leaves the panel on. It returns the panel handle and, via out
  params, the panel-IO handle (lcd registers its DMA-done callback on it) and
  the resolution in the chosen orientation. Return `NULL` on failure.
- `backlight` receives 0..255 (0 = off); driven by the `s.lcd.backlight`
  subscription.
- `touch_init` returns a touch handle, or `NULL` if the board has no touch — lcd
  then runs without a pointer indev (the focus group still exists so a
  button/keyboard-only board can drive the same UI). The field itself may also be
  `NULL`.
- `button_read` returns `true` while the centre/Home button is held. `NULL` = no
  button. lcd wires it to a keypad indev: short press clicks the focused widget,
  ≥1 s hold returns to the launcher.
- There is **no `key_read`**: a hardware keyboard is not part of the HAL. lcd owns
  no keyboard — a consumer that has one creates its own keypad indev on
  `lcdInputGroup()` and calls `lcdSetHasKeyboard(true)` (see the
  [Input devices](#input-devices) note).
- `pointer_read` writes the current absolute cursor position (screen px) to
  `*x,*y` and returns `true` iff it moved since the last call. `NULL` = no cursor
  device. The board owns the position — it integrates its device's motion, applies
  sensitivity + axis orientation, and clamps to the panel. lcd turns it into a
  pointer indev with an auto-hiding cursor and uses `button_read` as the click, so
  it skips the keypad button indev when this is set. The cursor's visible dwell is
  set by the consumer via `lcdPointerSetVisibleMs()` — lcd owns no pointer config
  key, keeping it ignorant of *what* the device is (a trackball is the consumer's
  business).

`lcdInputISR` is the one ISR all input INT lines share — the board attaches it
(via `gpio_isr_handler_add`, and as `esp_lcd_touch`'s callback) to each line. It
only flags + `vTaskNotifyGiveFromISR`s the lcd task, so there is no input polling;
it is `IRAM_ATTR` and touches only DRAM, so install the GPIO ISR service with
`ESP_INTR_FLAG_IRAM`.

See [tdeck.cpp](../../reticulous/main/tdeck.cpp) for a complete
ST7789V + GT911 + trackball example on a bus shared with SD and LoRa.

---

## Internals

Source lives in [spangap-core/src/lcd_ui/](../spangap-core/src/lcd_ui/); cross-file
glue is [lcd_internal.h](../spangap-core/src/lcd_ui/lcd_internal.h). Six files
(plus the generated `lv_font_spleen_5x8.c`), each owning one concern; everything
runs on the lcd task unless noted. The built-in Log + CLI programs
([lcd_apps.cpp](../spangap-core/src/lcd_ui/lcd_apps.cpp)) are documented under
[Built-in programs: Log + CLI](#built-in-programs-log--cli) above.

### Task model & threading

[lcd.cpp](../spangap-core/src/lcd_ui/lcd.cpp) spawns the **lcd task** ("lcd",
prio 2, core 1 — core 0 hosts Wi-Fi — 16 KB **PSRAM** stack; PSRAM is fine
because the task never does flash I/O). Its body is the canonical `itsPoll` loop
married to LVGL's timer. Input is event-mode (see [Input devices](#input-devices)),
so `lv_timer_handler`'s idle return is honest — the loop sleeps until the next
LVGL timer or a wake (an input ISR or an ITS message), with no fixed-interval
input poll:

```cpp
for (;;) {
    while (itsPoll(0)) {}                  // drain aux + storage callbacks
    if (s_inputPending) {                  // an input ISR fired (touch/button/key edge)
        s_inputPending = false;
        while (lcdInputPoll()) {}          // read indevs; drain click/keystroke synthesis
    }
    uint32_t idle = lv_timer_handler();
    itsPoll(idle == LV_NO_TIMER_READY ? portMAX_DELAY : pdMS_TO_TICKS(idle));
}
```

`itsServerInit()` gives the task an ITS inbox. Two aux ports carry work onto it
(`LCD_RUN_PORT = 10`, `LCD_REG_PORT = 11` — chosen not to clash with storage's
1/42/43):

- `lcdRun(fn, arg)` → `lcd_run_msg_t{fn, arg}` on port 10 → `onRunMsg` calls
  `fn(arg)`.
- `lcdRegister(name, basename, fn)` → `lcd_reg_msg_t` on port 11 → `onRegMsg`
  calls `lcdLauncherAdd`.

Both payloads `static_assert` to `<= ITS_MAX_MSG_DATA`. `lcdGoHome()` is just
`lcdRun([]{ lcdGoHomeInternal(); })`. `lcdSetBacklight()` is just
`storageSet("s.lcd.backlight", level)` — the subscription does the rest.
`lcdInit()` writes the three config defaults and spawns the task; it's
idempotent (returns early if the task already exists).

Live config is wired in the task after bring-up: `NOW_AND_ON_CHANGE` on
`s.lcd.backlight` applies it via the board's `backlight` op. (An `ON_CHANGE` on
`s.lcd.icon_res` remains, but `lcdIconResRefresh()` now always returns false —
the icon resolution is fixed to the tile size, `LAUNCHER_ICON_RES` — so it never
reloads.)

### Layering & z-order

The active screen holds two kinds of children: the **launcher** (icon grid) and
each **program layer**. The **status bar** and the **home-bar** live on
`lv_layer_top()`, which always draws above screen children. So the z-order falls
out for free, with no manual ordering:

```
lv_layer_top():   status bar (top, 24px)  +  home-bar (bottom swipe strip, 18px)
screen children:  program layers  (shown one at a time, between the two)
                  launcher        (revealed when no program is shown)
```

`LCD_STATUSBAR_H = 24` is reserved at the top; the launcher and every program
layer are positioned at `y = LCD_STATUSBAR_H` and sized
`screenH - LCD_STATUSBAR_H`. The top layer is kept click-through (not clickable,
not scrollable) so taps fall through to program content; only the small home-bar
on it is clickable.

### LVGL bring-up (`lcd_lvgl.cpp`)

[lcd_lvgl.cpp](../spangap-core/src/lcd_ui/lcd_lvgl.cpp) is LVGL v9 over `esp_lcd`:

- Calls `board->init()`, then `lv_init()` and `lv_display_create(w, h)`.
- **Draw buffers:** two strips, double-buffered, in **internal DMA-capable**
  RAM (`MALLOC_CAP_DMA`), `LV_DISPLAY_RENDER_MODE_PARTIAL`. Strip line count is
  derived from `LCD_DRAW_BUDGET = 4000` and the panel width, so one strip fits
  the shared-bus `max_transfer_sz` (the SD driver brings the bus up at 4096
  first; `spiHelperInitBus` is first-caller-wins).
- **Flush** (`flushCb`): swaps RGB565 to big-endian (ST7789 wants BE; LVGL
  renders LE), then **holds the shared-bus lock across the whole transfer
  including the async DMA drain**. This is load-bearing: `esp_lcd` releases the
  SPI driver's own bus lock the moment the colour DMA is *queued*, so without
  `spiHelperBusLock()` a co-resident polling driver (LoRa on the same SPI2 bus)
  can grab the bus mid-DMA and panic. The flush blocks on a binary semaphore
  (`s_dmaDone`) given from the `on_color_trans_done` ISR, then unlocks and calls
  `lv_display_flush_ready`.
- **Tick:** a periodic `esp_timer` (`LCD_TICK_MS = 2`) calls `lv_tick_inc(2)`.
- **Touch:** if `board->touch_init()` returns a handle, a `LV_INDEV_TYPE_POINTER`
  indev reads it via `esp_lcd_touch_read_data` / `_get_data` in `touchReadCb`,
  which **rotates the raw GT911 coordinates** to the display orientation itself
  (esp_lcd_touch is left identity — its swap/mirror mis-pairs the maxes for this
  panel) and clamps to the panel. The read timer runs at **10 ms while a finger
  is down**, relaxing to ~30 ms when up.
- **Button:** if `board->button_read` is set (and no cursor device claimed it), a
  `LV_INDEV_TYPE_KEYPAD` indev bound to the focus group. `buttonReadCb` emits
  `ENTER` on a short press and calls `lcdGoHomeInternal()` on a ≥1 s hold (and does
  *not* also emit the click). There is no keyboard indev here — a consumer keyboard
  creates its own (see [Input devices](#input-devices)).
- A focus group (`lcdInputGroup()`) is created regardless and shared by the
  launcher tiles, the button, a consumer's keyboard, and any program/settings
  widget added to it.

Exposes `lcdScreenW()` / `lcdScreenH()` / `lcdInputGroup()` to the other files;
`lcdInputGroup()` is also part of the public API ([lcd.h](../spangap-core/include/lcd.h)).

### Icon cache + loader (`lcd_icons.cpp`)

[lcd_icons.cpp](../spangap-core/src/lcd_ui/lcd_icons.cpp) solves "the lcd task
can't do flash I/O" with a three-stage hand-off and zero locks:

1. **Loader task** ("lcd_load", prio 1, core 1, 4 KB PSRAM stack) blocks on a
   FreeRTOS request queue (`LCD_ICON_QUEUE_DEPTH = 16`). It has *no* `itsPoll`
   loop, so the fs proxy's pickup-wait can't desync anything. On a request it
   `fs_stat`s, rejects bad sizes (`> LCD_ICON_MAX_BYTES = 256 KB`), `malloc`s a
   **PSRAM** buffer, reads the file, and hands it back via `lcdRun(onLoaded, …)`.
2. **`onLoaded`** runs on the lcd task: it drops the bytes into an in-RAM cache
   (`std::unordered_map<abs-path, Blob>`, lcd-task-only) and calls
   `lcdLauncherIconLoaded(basename)`.
3. A tiny **`lv_fs` driver** registered on letter **`'D'`** serves LVGL's image
   decoder straight from that cache — pure memory, zero flash on the lcd task.
   So `lv_image_set_src("D:/fixed/lcd/icons/36x36/rns.bin")` just works once the
   bytes are cached.

`lcdIconRequest(basename)` is the entry point: it returns immediately if already
cached (calling `lcdLauncherIconLoaded` directly), else enqueues a `LoadReq`. The
resolution string is fixed to `LAUNCHER_ICON_RES` (`36x36`, the tile size), so
`lcdIconResRefresh()` is a no-op returning false. The cache is touched only on the
lcd task.

### Launcher (`lcd_launcher.cpp`)

[lcd_launcher.cpp](../spangap-core/src/lcd_ui/lcd_launcher.cpp) owns the icon
grid and the program-layer lifecycle. It keeps a `std::vector<Entry>` (name,
basename, fn, the tile's `img`).

- `lcdLauncherInit` builds the launcher container (a `ROW_WRAP` flex grid laying
  out 4 columns × 3 rows of 72×64 tiles on the 320×216 area below the status bar;
  non-scrollable until icons exceed the 12 that fit — re-enable for paging then) and
  the home-bar on `lv_layer_top()`. The home-bar listens for `LV_EVENT_GESTURE`;
  on `LV_DIR_TOP` it calls `lcdGoHomeInternal()`. (Neither the top layer nor the
  strip is scrollable — a scrollable object would eat the drag as a scroll and
  never emit the gesture.)
- `lcdLauncherAdd` makes a tile (button + image + label), joins it to the input
  group, pushes the `Entry`, and fires `lcdIconRequest` for its icon. The image
  renders at its native size — the icon bucket is fixed to the tile size
  (`LAUNCHER_ICON_RES` = `36x36`), so there's no runtime scaling and the 36px icon
  sits clear of the label on the compact 3-row grid.
- `openEntry` is the lifecycle core. The layer is **not** stored on the Entry;
  each program layer is tagged with its entry index in `user_data`, so reuse is a
  scan of the screen's children (`findProgramLayer`), not a stored pointer. First
  open creates the layer (`makeProgramLayer` — opaque black, below the status bar)
  and calls `e.fn(layer)` once; thereafter it reveals the kept layer. Because the
  LVGL tree is the source of truth, **eviction is just `lv_obj_del(layer)`** — the
  next open finds nothing and rebuilds, no dangling pointer to clear; an
  `LV_EVENT_DELETE` guard keeps `s_current` valid if the shown layer is deleted.
- `lcdGoHomeInternal` **slides the current layer up off the top** (a short LVGL
  animation) to reveal the launcher, then hides + re-parks it; `openEntry`
  cancels any in-flight slide before revealing a layer.
- `lcdLauncherIconLoaded` sets the real image src on every tile matching the
  basename; `lcdLauncherReload` re-resolves cached icons and re-requests the rest
  (kept for completeness — the icon resolution is now fixed, so nothing triggers it).

### Status bar (`lcd_statusbar.cpp`)

[lcd_statusbar.cpp](../spangap-core/src/lcd_ui/lcd_statusbar.cpp) is an opaque
bar on `lv_layer_top()`:

- **Clock** (left): `strftime(s.lcd.date_format)` against `localtime_r`. The
  `lv_timer` reschedules itself to fire only when the shown value changes —
  aligned to the next minute boundary for a minute-resolution format, or every
  second only if the format includes seconds (`%S`/`%T`/`%r`/`%c`/`%X`).
- **Wi-Fi glyph** (right): `LV_SYMBOL_WIFI` whose opacity tracks RSSI. Purely
  **event-driven** — it subscribes to `wifi.sta` storage changes (which `net`
  publishes via `storageSet` on `wifi.sta.state` / `wifi.sta.rssi`) and maps
  state/RSSI to an opacity bucket. No net call, no polling.
- **Fullscreen.** A program can hide the bar and reclaim its 24px for an
  immersive screen via `lcdProgramFullscreen(true)` (public, `lcd.h`). The bar
  isn't owned by the program, so the *launcher* coordinates this: it grows the
  current layer to the full height, remembers which layer asked, and restores the
  bar when that program goes Home or another is opened — re-hiding it when the
  program is re-opened. Toggle it as your own view changes (e.g. a chat thread on,
  the conversation list off); percentage-sized children reflow automatically.

### Settings (`lcd_settings.cpp`)

[lcd_settings.cpp](../spangap-core/src/lcd_ui/lcd_settings.cpp) holds the menu
registry, the nav UI, the `lcdSetting*` helpers, and the gear program.

- **Registry:** `lcdRegisterSettings` splits the slash-path and walks/creates a
  tree of `Node`s (auto-titled submenus; a leaf gets `fn` + the explicit label).
  It's a plain in-RAM tree, so registration works before `lcdInit()`.
- **Nav UI** (`settingsOpen`): builds a header (back chevron + **breadcrumb**
  title, e.g. `Settings/Net/Wifi`, on a bar visually distinct from the page) over
  a scrollable flex-column content area, then `renderMenu()`. A nav stack tracks
  the current submenu; tapping a submenu row descends, tapping an item clears the
  content and calls the item's `fn(content)`. Back pops a pane → submenu → parent
  submenu, and at the root exits Settings via `lcdGoHomeInternal()`.
- **Helpers** wire an LVGL control's `LV_EVENT_VALUE_CHANGED` to a `storageSet` on
  the bound key (held by raw pointer — see the lifetime warning above) **and**
  subscribe that key so external writes flow back into the control (`bindAttach` /
  `bindDispatch`; two-way). The binding is removed on widget `LV_EVENT_DELETE`,
  unsubscribing the key when its last binding goes.
- `lcdSettingValue` is a label bound `BK_VALUE` — event-driven off the
  subscription, no timer/poll. `lcdSettingText` edits **in place** when a hardware
  keyboard is present (an inline `lv_textarea` added to the focus group, committing
  on `LV_EVENT_READY` / defocus); otherwise it opens the full-screen `lv_keyboard`
  overlay (a `TextRef` freed on `LV_EVENT_DELETE`).
- **Gear program:** `lcdSettingsInit()` (called from `lcdTaskFn` right after the
  launcher is built) registers `lcdLauncherAdd("Settings", "gear", settingsOpen)`,
  so the gear is the launcher's first tile. Its icon is label-only unless a
  `gear` source is supplied to the rasterizer.

### Build wiring

- The whole `src/lcd_ui/` tree lives in the `spangap-lcd` straddle. Whether
  it compiles at all is determined by whether `spangap-lcd` is in the
  staged set — which `spangap-inside` controls via the buildable's
  `requires:` / `optional_requires:` and the `--no-lcd` flag. When staged,
  `CONFIG_SPANGAP_LCD` is auto-emitted as a presence symbol; when not,
  the symbol is undefined and every consumer's `#if CONFIG_SPANGAP_LCD`
  block compiles away.
- The display stack (`lvgl__lvgl`, `esp_lcd`, `espressif__esp_lcd_touch`)
  is deliberately **not** in `REQUIRES` — `REQUIRES` is captured during
  early CMake expansion, before LVGL needs to be linked. Instead the libs
  are `target_link_libraries(... PUBLIC/PRIVATE)`'d after
  `idf_component_register`, in `spangap-lcd/esp-idf/CMakeLists.txt`. The
  `lvgl` version is pinned in `spangap-lcd`'s own `idf_component.yml`, so
  the consumer doesn't have to think about it. (The board-specific touch
  driver, e.g. `espressif/esp_lcd_touch_gt911`, stays in the consumer's
  `main/idf_component.yml` since it's hardware-pinned.)
- `spangap_lcd_icons()` ([project_include.cmake](../spangap-core/project_include.cmake))
  is the icon rasterizer helper described above; it shells out to
  [scripts/lcd-icons.py](../spangap-core/scripts/lcd-icons.py), which drives
  LVGL's `LVGLImage.py`. It hangs off the factory-image / LittleFS targets so
  icons land in `/fixed` before the image is built.
