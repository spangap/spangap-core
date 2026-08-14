/**
 * detect_probe — the peripheral reads a board's detect_hw() is built out of.
 *
 * A board straddle answers exactly one question about itself: `detect_hw()`,
 * which returns its own `hw-<straddle>` string when the hardware under it is
 * that board, and NULL when it is not. This header is the vocabulary that
 * answer is written in — bus opens, ACK checks, ID-register reads, a radio
 * probe — so a board states its identity as a few readable lines and never as
 * bus boilerplate.
 *
 * Everything here is a bus READ or a scratch write the real firmware overwrites
 * at init. Nothing writes flash. Every helper leaves the bus it opened closed
 * and every pin it drove back at its reset default, because the caller may well
 * be a different board's probe running next.
 *
 * TWO COPIES, KEPT IDENTICAL BY HAND. This file and each board's detect_hw()
 * are copied verbatim into flashmon's standalone detector
 * (flashmon/esp-idf/main/), which runs the same probes from RAM on a chip whose
 * firmware is unknown. There is deliberately no generator: the copy is a handful
 * of self-contained functions, and a build-time mechanism to move them would
 * cost more than it saves. Change one, change the other.
 *
 * I2C is the `i2c_master` driver, never the legacy `driver/i2c.h`. The legacy
 * driver latches a process-wide flag that makes every later `i2c_new_master_bus`
 * fail, so one probe on the old API would take down the keyboard, touch, RTC and
 * codec of the firmware that ran it.
 *
 * A probe opens an I2C bus and the SPI host itself, so detect_hw() must run
 * before anything else claims either. In the firmware that means
 * serviceRunStart(), ahead of the first onStart() — a board's own onStart is
 * exactly what takes them (hw-lilygo-tdeck creates the shared I2C0 bus there),
 * and anything later fails with "I2C bus id(0) has already been acquired" and
 * reads as a board nothing recognises. Nothing needs to be up first: a probe
 * drives the power rail it needs.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── the contract ────────────────────────────────────────────────────────────
 * Implemented by the staged board straddle, and by nobody else. spangap-core
 * declares it weak, so a build with no board straddle (the generic image) links
 * a null and skips the check rather than failing to link. */
const char* detect_hw(void);

/* ── logging ─────────────────────────────────────────────────────────────────
 * One tag for the whole business, so `log tag detect debug` turns the entire
 * probe trace on and nothing else. Everything a probe learns on the way is
 * debug; the single line that says which board this is, is info. */
#define DETECT_TAG "detect"
#define detect_dbg(fmt, ...)  ESP_LOGD(DETECT_TAG, fmt, ##__VA_ARGS__)

/** The one line a successful probe prints. `name` is the straddle name with
 *  underscores (hw_lilygo_tdeck), matching the function that found it. */
#define detect_found(name)    ESP_LOGI(DETECT_TAG, "%s found", name)

/* ── the extras ──────────────────────────────────────────────────────────────
 * Some parts a probe can read identify nothing: the T-Deck's touch, RTC and
 * GNSS are fitted or not on the same straddle, the Nibble Zero's environmental
 * sensor is a fitting choice. They exist for the trace a person reads.
 *
 * Off unless the build asks for them, because they are not free: detect_gps()
 * listens 1.2 s per baud rate, and the firmware runs this on EVERY boot, before
 * anything else has started, to answer a question the anchor and the radio have
 * already settled. flashmon's standalone detector defines DETECT_EXTRAS=1 —
 * there the trace is the whole product and the run ends in a reset anyway. */
#ifndef DETECT_EXTRAS
#define DETECT_EXTRAS 0
#endif

/* ── flash size ──────────────────────────────────────────────────────────────
 * Physical flash size (SFDP) is board-specific and readable without touching a
 * single pin, so it is the cheapest way for a probe to rule its board out
 * (16 MB → tdeck/heltec, 8 MB → xiao, 4 MB → t3s3/nibble). PSRAM size would
 * split the 16 MB pair further, but a RAM-loaded detector cannot init PSRAM
 * (octal vs quad is build-fixed), so the peripheral anchor settles same-flash
 * boards instead. A read error fails open — an unreadable size rules nothing
 * out. */
static inline bool detect_flash_mb(int mb)
{
    uint32_t bytes = 0;
    if (esp_flash_get_physical_size(NULL, &bytes) != ESP_OK || bytes == 0) {
        detect_dbg("flash size unreadable — not ruling out %d MB", mb);
        return true;
    }
    bool ok = bytes == (uint32_t)mb * 1024u * 1024u;
    detect_dbg("flash %u MB, want %d MB: %s",
               (unsigned)(bytes / (1024u * 1024u)), mb, ok ? "yes" : "no");
    return ok;
}

/* ── I2C ─────────────────────────────────────────────────────────────────────
 * One bus at a time on port 0, opened for a probe and closed before it returns.
 * Internal pull-ups are on: a probe runs before the firmware configures the
 * board's real bus policy, and every board here has externals that simply win. */
typedef struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
} detect_i2c_t;

static inline bool detect_i2c_open(detect_i2c_t* h, int sda, int scl)
{
    i2c_master_bus_config_t bc;
    memset(&bc, 0, sizeof bc);
    bc.i2c_port = I2C_NUM_0;
    bc.sda_io_num = (gpio_num_t)sda;
    bc.scl_io_num = (gpio_num_t)scl;
    bc.clk_source = I2C_CLK_SRC_DEFAULT;
    bc.glitch_ignore_cnt = 7;
    bc.flags.enable_internal_pullup = true;
    h->bus = NULL;
    h->dev = NULL;
    if (i2c_new_master_bus(&bc, &h->bus) != ESP_OK) {
        detect_dbg("i2c(%d/%d): bus would not open", sda, scl);
        return false;
    }
    return true;
}

static inline void detect_i2c_close(detect_i2c_t* h)
{
    if (h->dev) { i2c_master_bus_rm_device(h->dev); h->dev = NULL; }
    if (h->bus) { i2c_del_master_bus(h->bus); h->bus = NULL; }
}

/** Does anything answer to `addr`? The whole of what an OLED, a keyboard or an
 *  RTC will tell us — none of them has an ID register — so a bare ACK is a
 *  fact about the address, never about the part. Its meaning comes from the
 *  pins the caller probed it on. */
static inline bool detect_i2c_ack(detect_i2c_t* h, uint8_t addr)
{
    bool ok = i2c_master_probe(h->bus, addr, 50) == ESP_OK;
    detect_dbg("i2c: 0x%02X %s", addr, ok ? "acks" : "silent");
    return ok;
}

/* Attach (or re-attach) the device handle every transfer below goes through. */
static inline bool detect_i2c_dev(detect_i2c_t* h, uint8_t addr)
{
    if (h->dev) { i2c_master_bus_rm_device(h->dev); h->dev = NULL; }
    i2c_device_config_t dc;
    memset(&dc, 0, sizeof dc);
    dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dc.device_address = addr;
    dc.scl_speed_hz = 100000;
    return i2c_master_bus_add_device(h->bus, &dc, &h->dev) == ESP_OK;
}

/** Classic register read: write the register address, repeated START, read `n`.
 *  `reg`/`rlen` carry the address so an 8-bit and a 16-bit register look the
 *  same to a caller. */
static inline bool detect_i2c_rd_n(detect_i2c_t* h, uint8_t addr,
                                   const uint8_t* reg, size_t rlen,
                                   uint8_t* buf, size_t n)
{
    if (!detect_i2c_dev(h, addr)) return false;
    return i2c_master_transmit_receive(h->dev, reg, rlen, buf, n, 100) == ESP_OK;
}

static inline bool detect_i2c_rd(detect_i2c_t* h, uint8_t addr, uint8_t reg,
                                 uint8_t* buf, size_t n)
{
    return detect_i2c_rd_n(h, addr, &reg, 1, buf, n);
}

static inline bool detect_i2c_rd16(detect_i2c_t* h, uint8_t addr, uint16_t reg,
                                   uint8_t* buf, size_t n)
{
    uint8_t r[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    return detect_i2c_rd_n(h, addr, r, 2, buf, n);
}

static inline bool detect_i2c_wr(detect_i2c_t* h, uint8_t addr, uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    if (!detect_i2c_dev(h, addr)) return false;
    return i2c_master_transmit(h->dev, b, 2, 100) == ESP_OK;
}

/** SCCB register read — the camera variant of the above. SCCB is not
 *  repeated-START-capable: the sub-address write must end in a STOP and the
 *  read start fresh, which is a transmit and a receive rather than the combined
 *  transfer detect_i2c_rd uses. */
static inline bool detect_sccb_rd(detect_i2c_t* h, uint8_t addr,
                                  const uint8_t* reg, size_t rlen, uint8_t* val)
{
    if (!detect_i2c_dev(h, addr)) return false;
    if (i2c_master_transmit(h->dev, reg, rlen, 200) != ESP_OK) return false;
    return i2c_master_receive(h->dev, val, 1, 200) == ESP_OK;
}

static inline bool detect_sccb_rd8(detect_i2c_t* h, uint8_t addr, uint8_t reg, uint8_t* val)
{
    return detect_sccb_rd(h, addr, &reg, 1, val);
}

static inline bool detect_sccb_rd16(detect_i2c_t* h, uint8_t addr, uint16_t reg, uint8_t* val)
{
    uint8_t r[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    return detect_sccb_rd(h, addr, r, 2, val);
}

/* ── one-shot I2C conveniences — open, ask, close ────────────────────────────
 * A probe's anchor is usually a single question on a single bus, and these say
 * it in one line. */

static inline bool detect_ack(int sda, int scl, uint8_t addr)
{
    detect_i2c_t h;
    if (!detect_i2c_open(&h, sda, scl)) return false;
    bool ok = detect_i2c_ack(&h, addr);
    detect_i2c_close(&h);
    return ok;
}

/** Either of two addresses answering — the usual shape for an OLED, which sits
 *  at 0x3C or 0x3D depending on one strap. */
static inline bool detect_ack2(int sda, int scl, uint8_t a, uint8_t b)
{
    detect_i2c_t h;
    if (!detect_i2c_open(&h, sda, scl)) return false;
    bool ok = detect_i2c_ack(&h, a) || detect_i2c_ack(&h, b);
    detect_i2c_close(&h);
    return ok;
}

/** GT911 capacitive touch: ACK at 0x5D or 0x14, confirmed by the "911" product
 *  ID string at register 0x8140. */
static inline bool detect_gt911(int sda, int scl)
{
    detect_i2c_t h;
    if (!detect_i2c_open(&h, sda, scl)) return false;
    uint8_t addr = 0;
    if (detect_i2c_ack(&h, 0x5D)) addr = 0x5D;
    else if (detect_i2c_ack(&h, 0x14)) addr = 0x14;
    bool ok = false;
    if (addr) {
        uint8_t id[4] = {0};
        ok = detect_i2c_rd16(&h, addr, 0x8140, id, 4) &&
             id[0] == '9' && id[1] == '1' && id[2] == '1';
    }
    detect_i2c_close(&h);
    detect_dbg("i2c(%d/%d): GT911 touch %s", sda, scl, ok ? "present" : "absent");
    return ok;
}

/** ES7210 quad-mic ADC: chip ID 0x72/0x10 in registers 0xFD/0xFE at 0x40. */
static inline bool detect_es7210(int sda, int scl)
{
    detect_i2c_t h;
    if (!detect_i2c_open(&h, sda, scl)) return false;
    uint8_t hi = 0, lo = 0;
    bool ok = detect_i2c_ack(&h, 0x40) &&
              detect_i2c_rd(&h, 0x40, 0xFD, &hi, 1) &&
              detect_i2c_rd(&h, 0x40, 0xFE, &lo, 1) &&
              hi == 0x72 && lo == 0x10;
    detect_i2c_close(&h);
    detect_dbg("i2c(%d/%d): ES7210 audio ADC %s", sda, scl, ok ? "present" : "absent");
    return ok;
}

/** BME280 and its relatives: chip ID register 0xD0 at 0x76 or 0x77. Fills
 *  `model` (>= 8 bytes) with the part name when one answers. */
static inline bool detect_bme280(int sda, int scl, char* model)
{
    detect_i2c_t h;
    if (!detect_i2c_open(&h, sda, scl)) return false;
    uint8_t addr = 0;
    if (detect_i2c_ack(&h, 0x76)) addr = 0x76;
    else if (detect_i2c_ack(&h, 0x77)) addr = 0x77;
    const char* name = NULL;
    if (addr) {
        uint8_t id = 0;
        if (detect_i2c_rd(&h, addr, 0xD0, &id, 1))
            name = id == 0x60 ? "BME280" : id == 0x58 ? "BMP280" : id == 0x61 ? "BME680" : NULL;
    }
    detect_i2c_close(&h);
    detect_dbg("i2c(%d/%d): environmental sensor %s", sda, scl, name ? name : "absent");
    if (!name) return false;
    if (model) strcpy(model, name);
    return true;
}

/* ── camera (DVP over SCCB) ──────────────────────────────────────────────────
 * A DVP sensor keeps its SCCB block clocked off XCLK, so with no XCLK it does
 * not ACK at all. Generate ~20 MHz on the XCLK pin for the length of the probe.
 * OV2640 cannot recover from XCLK gating without a power cycle, which is fine
 * both places this runs: the detector is reset into real firmware straight
 * after, and the firmware cold-inits its camera later in boot. */
static inline void detect_xclk_start(int pin)
{
    ledc_timer_config_t t;
    memset(&t, 0, sizeof t);
    t.speed_mode = LEDC_LOW_SPEED_MODE;
    t.duty_resolution = LEDC_TIMER_1_BIT;   /* 1-bit @ 20 MHz: 20M*2 = 40M <= 80M APB */
    t.timer_num = LEDC_TIMER_0;
    t.freq_hz = 20000000;
    t.clk_cfg = LEDC_AUTO_CLK;
    if (ledc_timer_config(&t) != ESP_OK) return;
    ledc_channel_config_t c;
    memset(&c, 0, sizeof c);
    c.gpio_num = pin;
    c.speed_mode = LEDC_LOW_SPEED_MODE;
    c.channel = LEDC_CHANNEL_0;
    c.timer_sel = LEDC_TIMER_0;
    c.duty = 1;                              /* 50% of 1-bit */
    c.hpoint = 0;
    ledc_channel_config(&c);
}

static inline void detect_xclk_stop(int pin)
{
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    gpio_reset_pin((gpio_num_t)pin);
}

/** The camera sensors that fit a DVP socket, by their chip-ID registers — the
 *  same scheme esp32-camera uses. OV parts at 0x3C use 16-bit register
 *  addressing; OV2640 needs a bank select first; the GC parts and the OV7 series
 *  use 8-bit registers. Fills `model` (>= 8 bytes). */
static inline bool detect_camera(int sda, int scl, int xclk, char* model)
{
    if (xclk >= 0) { detect_xclk_start(xclk); vTaskDelay(pdMS_TO_TICKS(20)); }
    detect_i2c_t h;
    if (!detect_i2c_open(&h, sda, scl)) {
        if (xclk >= 0) detect_xclk_stop(xclk);
        return false;
    }
    const char* name = NULL;
    uint8_t at = 0, hi = 0, lo = 0;

    /* OV2640 — 0x30, 8-bit regs: bank 0xFF=1, then PID 0x0A/0x0B = 0x26 / 0x41|0x42. */
    detect_i2c_wr(&h, 0x30, 0xFF, 0x01);
    if (detect_sccb_rd8(&h, 0x30, 0x0A, &hi) && detect_sccb_rd8(&h, 0x30, 0x0B, &lo) &&
        hi == 0x26 && (lo == 0x41 || lo == 0x42)) { name = "OV2640"; at = 0x30; }

    /* OV5640 / OV3660 — 0x3C, 16-bit regs: chip ID at 0x300A/0x300B. */
    if (!name && detect_sccb_rd16(&h, 0x3C, 0x300A, &hi) &&
        detect_sccb_rd16(&h, 0x3C, 0x300B, &lo)) {
        at = 0x3C;
        if (hi == 0x56 && lo == 0x40) name = "OV5640";
        else if (hi == 0x36 && lo == 0x60) name = "OV3660";
    }
    /* GC2145 — same 0x3C but 8-bit regs: ID 0xF0/0xF1 = 0x21/0x45. */
    if (!name && detect_sccb_rd8(&h, 0x3C, 0xF0, &hi) &&
        detect_sccb_rd8(&h, 0x3C, 0xF1, &lo) && hi == 0x21 && lo == 0x45) {
        name = "GC2145"; at = 0x3C;
    }
    /* OV7670 / OV7725 — 0x21, 8-bit regs: PID 0x0A/0x0B. */
    if (!name && detect_sccb_rd8(&h, 0x21, 0x0A, &hi) &&
        detect_sccb_rd8(&h, 0x21, 0x0B, &lo)) {
        at = 0x21;
        if (hi == 0x76 && lo == 0x73) name = "OV7670";
        else if (hi == 0x77 && lo == 0x21) name = "OV7725";
    }
    /* GC0308 — 0x21, 8-bit: reg 0x00 = 0x9B. */
    if (!name && detect_sccb_rd8(&h, 0x21, 0x00, &hi) && hi == 0x9B) {
        name = "GC0308"; at = 0x21;
    }

    detect_i2c_close(&h);
    if (xclk >= 0) detect_xclk_stop(xclk);
    detect_dbg("i2c(%d/%d) xclk%d: camera %s at 0x%02X", sda, scl, xclk,
               name ? name : "absent", at);
    if (!name) return false;
    if (model) strcpy(model, name);
    return true;
}

/* ── LoRa radio over SPI ─────────────────────────────────────────────────────
 * The radio is what confirms a board once its anchor peripheral has answered,
 * and on the boards whose line splits by modem it is what names the straddle.
 * The host is opened and freed here, so nothing else may hold it. */
typedef struct { int sck, mosi, miso, cs, rst, busy; } detect_spi_pins_t;

static inline spi_device_handle_t detect_spi_open(const detect_spi_pins_t* p)
{
    spi_bus_config_t bus;
    memset(&bus, 0, sizeof bus);
    bus.sclk_io_num = p->sck;
    bus.mosi_io_num = p->mosi;
    bus.miso_io_num = p->miso;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 64;
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_DISABLED) != ESP_OK) {
        detect_dbg("spi: host busy — cannot probe a radio");
        return NULL;
    }
    spi_device_interface_config_t dev;
    memset(&dev, 0, sizeof dev);
    dev.clock_speed_hz = 1000000;
    dev.mode = 0;
    dev.spics_io_num = p->cs;
    dev.queue_size = 1;
    spi_device_handle_t h = NULL;
    if (spi_bus_add_device(SPI2_HOST, &dev, &h) != ESP_OK) {
        spi_bus_free(SPI2_HOST);
        return NULL;
    }
    return h;
}

static inline void detect_spi_close(spi_device_handle_t h)
{
    if (h) { spi_bus_remove_device(h); spi_bus_free(SPI2_HOST); }
}

static inline bool detect_spi_xfer(spi_device_handle_t h, const uint8_t* tx,
                                   uint8_t* rx, size_t n)
{
    spi_transaction_t t;
    memset(&t, 0, sizeof t);
    t.length = n * 8;
    t.tx_buffer = tx;
    t.rx_buffer = rx;
    return spi_device_transmit(h, &t) == ESP_OK;
}

static inline void detect_radio_reset(const detect_spi_pins_t* p)
{
    if (p->rst < 0) return;
    gpio_set_direction((gpio_num_t)p->rst, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)p->rst, 0);
    esp_rom_delay_us(2000);
    gpio_set_level((gpio_num_t)p->rst, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
}

static inline bool detect_busy_low(int busy)
{
    if (busy < 0) return true;
    gpio_set_direction((gpio_num_t)busy, GPIO_MODE_INPUT);
    for (int i = 0; i < 1000; i++) {          /* up to ~100 ms */
        if (gpio_get_level((gpio_num_t)busy) == 0) return true;
        esp_rom_delay_us(100);
    }
    return false;
}

/** Which LoRa modem is on this header, as a short slug ("sx1262", "sx1276",
 *  "sx1280", "lr1121"), or NULL if none answers. Fills `slug` (>= 8 bytes) when
 *  given. Order is most-specific first: SX127x has a real version register,
 *  the rest are identified by a command exchange over BUSY. */
static inline const char* detect_radio(int sck, int mosi, int miso, int cs,
                                       int rst, int busy, char* slug)
{
    detect_spi_pins_t p;
    p.sck = sck; p.mosi = mosi; p.miso = miso; p.cs = cs; p.rst = rst; p.busy = busy;
    spi_device_handle_t h = detect_spi_open(&p);
    if (!h) return NULL;
    detect_radio_reset(&p);

    const char* found = NULL;

    /* SX127x — RegVersion 0x42 (read: addr bit 7 = 0). */
    {
        uint8_t tx[2] = { 0x42, 0x00 }, rx[2] = {0};
        if (detect_spi_xfer(h, tx, rx, 2)) {
            if (rx[1] == 0x12 || rx[1] == 0x22) found = "sx1276";
        }
    }

    /* LR11xx — GetVersion (0x01 0x01); reply device byte 0xDF/0xDA/0xDB. */
    if (!found && busy >= 0 && detect_busy_low(busy)) {
        uint8_t cmd[2] = { 0x01, 0x01 };
        detect_spi_xfer(h, cmd, NULL, 2);
        detect_busy_low(busy);
        uint8_t rtx[5] = {0}, rrx[5] = {0};   /* stat, hw, device, fw_maj, fw_min */
        if (detect_spi_xfer(h, rtx, rrx, 5)) {
            uint8_t dev = rrx[2];
            if (dev == 0xDF || dev == 0xDA || dev == 0xDB) found = "lr1121";
        }
    }

    /* SX126x — no version register: write a scratch byte to the sync-word
     * register 0x0740 and read it back; GetStatus (0xC0) must return a live
     * status byte too. */
    if (!found && busy >= 0 && detect_busy_low(busy)) {
        uint8_t w[4] = { 0x0D, 0x07, 0x40, 0xA5 };                         /* WriteRegister */
        detect_spi_xfer(h, w, NULL, 4);
        detect_busy_low(busy);
        uint8_t rtx[5] = { 0x1D, 0x07, 0x40, 0x00, 0x00 }, rrx[5] = {0};   /* ReadRegister */
        bool ok = detect_spi_xfer(h, rtx, rrx, 5);
        detect_busy_low(busy);
        uint8_t stx[2] = { 0xC0, 0x00 }, srx[2] = {0};                     /* GetStatus */
        detect_spi_xfer(h, stx, srx, 2);
        if (ok && rrx[4] == 0xA5 && srx[1] != 0x00 && srx[1] != 0xFF) found = "sx1262";
    }

    /* SX128x (2.4 GHz) — command/BUSY like SX126x but distinct opcodes (write
     * 0x18 / read 0x19), so the sync-word probe above cannot catch it. Read the
     * firmware-version register at 0x0153: opcode + 2 addr + 1 status byte,
     * then the two data bytes. */
    if (!found && busy >= 0 && detect_busy_low(busy)) {
        uint8_t rtx[6] = { 0x19, 0x01, 0x53, 0x00, 0x00, 0x00 }, rrx[6] = {0};
        bool ok = detect_spi_xfer(h, rtx, rrx, 6);
        detect_busy_low(busy);
        uint16_t fw = (uint16_t)((rrx[4] << 8) | rrx[5]);
        if (ok && fw != 0x0000 && fw != 0xFFFF) found = "sx1280";
    }

    detect_spi_close(h);
    detect_dbg("spi(sck%d/mosi%d/miso%d/cs%d): radio %s",
               sck, mosi, miso, cs, found ? found : "absent");
    if (!found) return NULL;
    if (slug) strcpy(slug, found);
    return found;
}

/** The radio on this header is `want` ("sx1262", …). The form a board straddle
 *  whose line splits by modem asks in: an SX1262 T3-S3 is not an LR1121 one. */
static inline bool detect_radio_is(int sck, int mosi, int miso, int cs,
                                   int rst, int busy, const char* want)
{
    const char* got = detect_radio(sck, mosi, miso, cs, rst, busy, NULL);
    return got && strcmp(got, want) == 0;
}

/* ── GNSS — passive autobaud on the receiver's UART RX ───────────────────────
 * The receivers only differ by default baud (u-blox 38400, Quectel 9600). Both
 * stream NMEA continuously when powered, so listen (RX only — the physical TX
 * pin is the console UART on other boards) and take the baud that first yields a
 * checksum-valid "$…*hh" sentence. */
static inline bool detect_nmea_valid(const char* buf, int len)
{
    for (int i = 0; i < len; i++) {
        if (buf[i] != '$') continue;
        uint8_t sum = 0;
        int j = i + 1;
        for (; j < len && buf[j] != '*' && buf[j] != '$'; j++) sum ^= (uint8_t)buf[j];
        if (j + 2 >= len || buf[j] != '*') continue;
        int hi = buf[j + 1], lo = buf[j + 2];
        hi = (hi <= '9') ? hi - '0' : (hi | 0x20) - 'a' + 10;
        lo = (lo <= '9') ? lo - '0' : (lo | 0x20) - 'a' + 10;
        if (hi >= 0 && hi < 16 && lo >= 0 && lo < 16 && sum == (uint8_t)((hi << 4) | lo))
            return true;
    }
    return false;
}

/** Fills `model` (>= 16 bytes) with the receiver's name when one is streaming. */
static inline bool detect_gps(int rx, char* model)
{
    static const int   bauds[] = { 38400, 9600 };
    static const char* names[] = { "u-blox MIA-M10Q", "Quectel L76K" };
    for (int b = 0; b < 2; b++) {
        uart_config_t cfg;
        memset(&cfg, 0, sizeof cfg);
        cfg.baud_rate = bauds[b];
        cfg.data_bits = UART_DATA_8_BITS;
        cfg.parity = UART_PARITY_DISABLE;
        cfg.stop_bits = UART_STOP_BITS_1;
        cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
        cfg.source_clk = UART_SCLK_DEFAULT;
        if (uart_driver_install(UART_NUM_1, 2048, 0, 0, NULL, 0) != ESP_OK) return false;
        uart_param_config(UART_NUM_1, &cfg);
        uart_set_pin(UART_NUM_1, UART_PIN_NO_CHANGE, rx,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

        char buf[512];
        int got = uart_read_bytes(UART_NUM_1, (uint8_t*)buf, sizeof buf, pdMS_TO_TICKS(1200));
        uart_driver_delete(UART_NUM_1);

        if (got > 0 && detect_nmea_valid(buf, got)) {
            detect_dbg("uart(rx%d): %s GPS at %d", rx, names[b], bauds[b]);
            if (model) strcpy(model, names[b]);
            return true;
        }
    }
    detect_dbg("uart(rx%d): no GPS streaming", rx);
    return false;
}

/* ── power rails ─────────────────────────────────────────────────────────────
 * Drive on entry, hand back to hi-Z on exit. A probe that drives a rail MUST
 * release it before returning: on the board it was not looking at, that pin
 * means something else entirely, and the next probe is about to read it. */
static inline void detect_rail_drive(int pin, int level)
{
    gpio_reset_pin((gpio_num_t)pin);
    gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)pin, level);
}

static inline void detect_rail_release(int pin)
{
    gpio_reset_pin((gpio_num_t)pin);
}

#ifdef __cplusplus
}
#endif
