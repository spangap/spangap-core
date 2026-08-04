/** `usb cdc` / `usb jtag` — run the console on a TinyUSB composite device
 *  instead of on the USB-Serial-JTAG controller.
 *
 *  The chip has one internal USB PHY, shared between the USB-Serial-JTAG
 *  controller and the USB-OTG core; only one of them drives D+/D- at a time.
 *  tinyusb_driver_install() moves the PHY to USB-OTG, which takes the
 *  USB-Serial-JTAG console off the wire mid-command: the host sees a
 *  disconnect, then a fresh enumeration of the composite device. The reverse
 *  move needs the USB-Serial-JTAG peripheral re-armed by hand — the PHY is
 *  routed away from it, and its RX interrupt does not survive the round trip
 *  (pmUsbSerialJtagReattach).
 *
 *  Two CDC-ACM ports is the ceiling, and it is a hardware one: each port claims
 *  an interrupt IN endpoint for its notification element plus a bulk IN/OUT
 *  pair, and the OTG core has five IN endpoints besides EP0.
 *
 *  The routing survives a software reset. Its select bits live in RTC_CNTL,
 *  which esp_restart() does not clear — it resets the CPUs and a fixed list of
 *  peripherals, and neither USB controller is on that list. Only a power-on or
 *  EN-pin reset clears the RTC domain and returns the PHY to USB-Serial-JTAG.
 *  A shutdown handler therefore hands the PHY back before any restart, so a
 *  reboot issued over CDC does not come back to a console nothing is driving.
 *
 *  All of that is built only under CONFIG_SPANGAP_USB_CDC (off by default) and
 *  only where the console is on USB at all. Otherwise this file compiles to the
 *  stub branch at the bottom: the transport flags stay false, `usb cdc` reports
 *  itself unavailable, the device presents one serial port, and no TinyUSB code
 *  or .bss reaches the image — see docs/usb-console.md.
 */

#include "cli.h"
#include "log.h"
#include "pm.h"
#include "storage.h"
#include "compat.h"

#include "esp_mac.h"

#include <cstring>
#include <cstdlib>
#include <cstdio>

#include "esp_err.h"
#include "esp_system.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

/* SPANGAP_CDC_BUILT — "the CDC transport is built" — comes from cli.h, where
 * the serial-port contract it sizes lives. */
#if SPANGAP_CDC_BUILT
#include <driver/usb_serial_jtag.h>
#include "tinyusb.h"
#include "tusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_console.h"
#include "vfs_tinyusb.h"
#endif

/* True while the console runs on a TinyUSB CDC port rather than the
 * USB-Serial-JTAG controller. cli.cpp reads it to choose its console I/O path;
 * pm.cpp reads it to stop polling a controller that no longer owns the PHY.
 * Defined unconditionally so those readers need no Kconfig guard of their own. */
extern "C" { volatile bool consoleOnCdc = false; }

/* Set for the duration of a transport switch. When idle the serial task parks
 * indefinitely on the USB-Serial-JTAG RX ring — deliberately, so an idle
 * console costs no wakes — and from there it would never reconsider which
 * transport to read. That park has to be avoided across a switch, or the task
 * blocks on the controller being taken away and the new console is deaf.
 * While this is set the task polls instead. */
extern "C" { volatile bool consoleSwitchPending = false; }

/* Set while no transport owns the pads: the outgoing controller has lost them
 * and the incoming one has no host yet. Console writes in this window reach
 * nobody, and queueing them is worse than dropping them — the queue is drained
 * into whichever session attaches next, which replays seconds-old lines,
 * truncated wherever the ring wrapped, over that session's own first output.
 * Every console writer drops instead; the log's other consumers are unaffected,
 * so nothing is lost from the log file or the ring.
 *
 * Narrower than consoleSwitchPending, which starts earlier so the serial task
 * can leave its park: the warning that announces the move has to go out on the
 * transport that is about to leave, and a host watches for it. */
extern "C" { volatile bool consoleWriteDead = false; }

#if SPANGAP_CDC_BUILT

/* CDC-ACM ports currently presented; 0 while the console is on USB-Serial-JTAG. */
static int cdcPorts = 0;

/* Light sleep gates the USB clock, and TinyUSB — unlike the USB-Serial-JTAG
 * controller, which IDF keeps awake through CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION
 * — has no such arrangement: a nap drops the CDC link and the host must replug.
 * Held for as long as the console lives on CDC. */
static pm_lock_handle_t cdcLock = nullptr;

/** Take one byte from the console CDC port; 1 when a byte was read, else 0.
 *  The serial task comes through here rather than reading STDIN_FILENO because
 *  freopen() need not preserve a descriptor number — fd 0 can still refer to
 *  the USB-Serial-JTAG device the console has left, which is where every
 *  keystroke went while this was missing. */
extern "C" int consoleCdcRead(char* out) {
  if (!consoleOnCdc) return 0;
  size_t n = 0;
  if (tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, (uint8_t*)out, 1, &n) != ESP_OK) return 0;
  return n == 1 ? 1 : 0;
}

/* Block read/write on a named CDC port, for the serial task's handler shuttle
 * (cli.h). Whole chunks rather than the console's single byte: a client that
 * validates its configuration a quarter second after sending it gets nowhere at
 * one byte per poll interval. Nothing here touches line endings — a handler
 * carries a framed protocol, not text. */
extern "C" int consoleCdcReadPort(int itf, uint8_t* out, size_t max) {
  if (!consoleOnCdc || itf < 0 || itf >= cdcPorts) return 0;
  size_t n = 0;
  if (tinyusb_cdcacm_read((tinyusb_cdcacm_itf_t)itf, out, max, &n) != ESP_OK) return 0;
  return (int)n;
}

extern "C" int consoleCdcWritePort(int itf, const uint8_t* data, size_t len) {
  if (!consoleOnCdc || itf < 0 || itf >= cdcPorts) return 0;
  size_t w = tinyusb_cdcacm_write_queue((tinyusb_cdcacm_itf_t)itf, data, len);
  tinyusb_cdcacm_write_flush((tinyusb_cdcacm_itf_t)itf, pdMS_TO_TICKS(50));
  return (int)w;
}

/* Discard whatever the host queued while nobody was reading. Bytes that arrived
 * before the console adopted a port are not input for the session about to
 * start on it — delivered late they land as a burst of keystrokes nobody
 * typed, which is how an unread port had accumulated twenty of them. */
static void cdcDrain(int itf) {
  uint8_t sink[64];
  size_t  n = 0;
  /* Bounded: a host that writes as fast as this reads would otherwise hold the
   * loop here indefinitely, and this runs where a stall is expensive. A few
   * passes clear any realistic backlog; anything beyond it is live traffic. */
  for (int pass = 0; pass < 16; pass++)
    if (tinyusb_cdcacm_read((tinyusb_cdcacm_itf_t)itf, sink, sizeof sink, &n) != ESP_OK || n == 0) break;
}

/* Push queued CDC bytes onto the wire. cliFlush() calls this in place of the
 * USB-Serial-JTAG FIFO flush while the console is on CDC: a short write
 * otherwise sits in the TX FIFO until something fills a packet, which strands
 * prompts (no trailing newline) indefinitely.
 *
 * Nothing here may depend on the host's DTR line. Web Serial does not define
 * whether opening a port asserts it, and Chrome need not: a console that only
 * writes or flushes once DTR is seen goes permanently mute against a host that
 * never raises it, which is far worse than the stale bytes such gating was
 * meant to suppress. */
extern "C" void consoleCdcFlush(void) {
  if (consoleOnCdc) tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(50));
}

/* String descriptors, resolved per install rather than compiled in. The serial
 * number is what a host turns into a device node name — macOS builds
 * /dev/cu.usbmodem<serial><interface> — so the Kconfig default of "123456"
 * gives every board on a desk the same two names. Indices are the standard
 * ones: 0 language, 1 manufacturer, 2 product, 3 serial, 4 CDC interface.
 * Both CDC interfaces share index 4; telling the two ports apart by name would
 * take a custom configuration descriptor giving each its own iInterface.
 * TinyUSB reads this table whenever the host asks, so it outlives the install. */
/* 31 characters is the ceiling: esp_tinyusb converts through a 32-entry buffer
 * and truncates anything longer without a word. */
static char        serialStr[32];
static char        productStr[48];
static const char* descStrings[5];

static void buildDescStrings(void) {
  storageGetStr("s.net.hostname", productStr, sizeof productStr, CONFIG_SPANGAP_FW_HOSTNAME);
  if (!productStr[0]) safeStrncpy(productStr, CONFIG_SPANGAP_FW_HOSTNAME, sizeof productStr);

  /* The low three MAC bytes are the chip's index within its OUI block — a
   * sequential counter, so two chips from one block never share them and the
   * serial needs no other source of uniqueness. They lead so the hostname,
   * which is the part a person reads, ends the name; a host appending an
   * interface number then lands after the trailing underscore rather than
   * against the hex. The `hostname` command bounds its input to 20 characters
   * of [A-Za-z0-9_]; the precision bounds the compile-time default too, and
   * keeps the hostname from pushing anything off the 31-character end. */
  uint8_t mac[6] = {};
  esp_efuse_mac_get_default(mac);
  snprintf(serialStr, sizeof serialStr, "%02x%02x%02x_%.23s_",
           mac[3], mac[4], mac[5], productStr);

  /* Index 0 is a language ID pair, not text — two raw bytes for English. */
  static const char langid[] = { 0x09, 0x04 };
  descStrings[0] = langid;
  descStrings[1] = CONFIG_TINYUSB_DESC_MANUFACTURER_STRING;
  descStrings[2] = productStr;
  descStrings[3] = serialStr;
  descStrings[4] = CONFIG_TINYUSB_DESC_CDC_STRING;
}

/* esptool's reset convention. The USB-Serial-JTAG controller implements this in
 * hardware; a CDC port only receives the line states and has to act on them
 * itself. A falling edge on RTS means reset, and DTR held at that moment asks
 * for ROM download mode rather than an ordinary boot. */
static volatile bool resetToLoader = false;
static volatile bool resetPending  = false;
/* Per port: the two ports need separate edge state, or the host settling the
 * second port's lines reads as a phantom edge on the first. */
static bool          prevRts[TINYUSB_CDC_ACM_MAX];
static bool          prevDtr[TINYUSB_CDC_ACM_MAX];

/* Serial-handler registry hooks (cli.cpp). A claimed port's DTR edges are
 * attach/detach for its handler, and its reset arming is suppressed. */
extern "C" bool serialPortIsClaimed(int port);
extern "C" void serialPortHostAttached(int port);
extern "C" void serialPortHostDetached(int port);
extern "C" void serialPortWake(void);

static void cdcResetTask(void*) {
  err("USB reset signal received, port will change\n");
  /* The log reaches its other consumers — the browser's WebRTC channel among
   * them — by way of the log task's fan-out, so give that a beat to leave the
   * device before the restart takes every transport down with it. */
  delay(400);
  if (resetToLoader) REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
  esp_restart();
}

static void cdcLineStateCb(int itf, cdcacm_event_t* event) {
  if (itf < 0 || itf >= TINYUSB_CDC_ACM_MAX) return;

  bool rts = event->line_state_changed_data.rts;
  bool dtr = event->line_state_changed_data.dtr;
  bool claimed = serialPortIsClaimed(itf);

  if (dtr && !prevDtr[itf]) {
    /* Drop anything queued while no host was reading. A CDC TX FIFO holds its
     * contents until someone opens the port, so without this the first thing a
     * new session receives is whatever the last one left behind. The host
     * identifies the console port by sending a CR, which the console answers by
     * naming the transport it is on — no unprompted greeting is needed here. */
    tud_cdc_n_write_clear(itf);
  }

  if (claimed) {
    /* DTR is the attach signal on CDC: a pyserial-class client raises it on
     * open and drops it on close. It is also why the esptool arming below is
     * skipped — a clean close drops DTR before RTS, which is the reset
     * sequence's own shape, and would reboot the device on every client exit. */
    if (dtr && !prevDtr[itf])       serialPortHostAttached(itf);
    else if (!dtr && prevDtr[itf])  serialPortHostDetached(itf);
  } else if (itf == TINYUSB_CDC_ACM_0) {
    /* Console port only. The other port carries data, and a host driver there
     * raises and drops these same lines as a matter of course — opening and
     * closing an interface is routine — which must never restart a running
     * device.
     *
     * The full two-step esptool sequence — RTS alone (reset asserted), then DTR
     * alone (released into the bootloader) — rather than any falling RTS. A
     * bare edge is not distinctive enough: closing a port drops both lines, and
     * treating that as a reset request restarts the device every time a monitor
     * goes away.
     *
     * Require the lines to have been in the "reset asserted" state — RTS raised
     * with DTR low — and to be leaving it now. DTR high at that moment asks for
     * the bootloader, low for an ordinary restart. Insisting on that prior
     * state is what separates a reset request from a port merely closing: a
     * host with the port open holds DTR asserted, so a close never passes
     * through it. */
    bool armed = prevRts[itf] && !prevDtr[itf];
    if (armed && !rts && !resetPending) {
      resetPending  = true;
      resetToLoader = dtr;
      /* Off the TinyUSB task: the work below logs and sleeps, and that task has
       * a device to keep answering until the restart actually lands. */
      spawnTask(cdcResetTask, "usbrst", 3072, nullptr, 5, 1);
    }
  }
  prevRts[itf] = rts;
  prevDtr[itf] = dtr;
}

/* Inbound bytes on a CDC port. The serial task reads the ports itself; this
 * only has to wake it, so a handler's stream is not paced by a poll interval. */
static void cdcRxCb(int itf, cdcacm_event_t*) {
  if (serialPortIsClaimed(itf)) serialPortWake();
}

static bool cdcClose(void);

/* Runs from esp_restart(), before the scheduler stops, so the full teardown is
 * available here. Covers every restart path — the `reboot` command, an OTA
 * swap, anything else that restarts cleanly — rather than just the one. A panic
 * still bypasses it, and needs the power cycle. */
static void cdcShutdownHook(void) {
  if (consoleOnCdc) cdcClose();
}

/* Move stdin/stdout/stderr off any CDC device and back onto the
 * USB-Serial-JTAG one, and drop the CDC filesystem with them.
 *
 * Not tinyusb_console_deinit(): that restores by reopening
 * /dev/uart/CONFIG_ESP_CONSOLE_UART_NUM, and a board whose console is USB has
 * no UART, so the number is -1 and every freopen fails — leaving the standard
 * streams NULL and the device with no console at all.
 *
 * Separate from the PHY hand-back because the two belong at opposite ends of a
 * teardown: the streams must leave the CDC device before it is dismantled, the
 * PHY only after. A bring-up that fails partway still needs both, in that
 * order, or it strands the console on a device it has just destroyed. */
/* Why the last bring-up failed. The per-step errors are logged where they
 * happen, which on this path is while the console is mid-switch: the log's
 * route out is gated on a connection flag that reads false until a tick has
 * updated it, so those writes are discarded whole and the reason is lost
 * exactly when it is needed. Kept here and reported once a console is back. */
static char cdcFailReason[112];

/* Why the last switch failed, in a form fit to show a person, kept until one has
 * seen it. Every step of a failed switch reports where it happens, and every one
 * of those moments is a moment with no console: the CDC device is gone or never
 * came up, and the USB-Serial-JTAG controller has only just been handed the pads
 * back, so the host is still re-enumerating and will not read for another second
 * or two. Writing then puts the reason in a queue whose next reader is a host
 * that, having just opened the port, has every reason to treat what it finds
 * there as stale and drop it. Held here instead, reported once the link is up,
 * and kept afterwards so `usb` can repeat it. */
static char cdcLastError[192];

/* Wait for a host on the USB-Serial-JTAG link, then report. Bounded, because a
 * device with nothing plugged in must not hold the CLI task waiting for a reader
 * that is not coming; the log call runs either way, so the reason still reaches
 * the log file and every other consumer. */
static void reportSwitchFailure(void) {
  if (!cdcLastError[0]) return;
  for (int i = 0; i < 30 && !usb_serial_jtag_is_connected(); i++) delay(100);
  /* Straight to the console as well as through the log: this is the answer to a
   * command someone just typed, and it must land on the console they typed it
   * on whether or not the log's own routing has caught up with the switch. */
  char msg[224];
  int n = snprintf(msg, sizeof msg, "\r\nusb: %s\r\n", cdcLastError);
  if (n > 0) consoleWriteRaw(msg, (size_t)(n < (int)sizeof msg ? n : (int)sizeof msg - 1));
  err("usb: %s\n", cdcLastError);
}

static void restoreJtagStreams(void) {
  fflush(stdout);
  /* /dev/console, not the USB-Serial-JTAG device node: the console VFS is what
   * these streams were opened on at boot, and it is the layer that knows which
   * transport the console is configured for. The device node is a different
   * path with its own write gating, which drops whole chunks whenever its
   * SOF-derived connected flag reads false — as it does right after a
   * reattach, before any tick has run to update it. */
  freopen("/dev/console", "r", stdin);
  freopen("/dev/console", "w", stdout);
  freopen("/dev/console", "w", stderr);
  setvbuf(stdout, nullptr, _IOLBF, 0);   /* reopening dropped it again */
  esp_vfs_tusb_cdc_unregister(NULL);
}

static bool cdcOpen(int n) {
  cdcFailReason[0] = '\0';
  /* Empty the stream before the transport moves. Anything still buffered would
   * otherwise be flushed by the reopen much later, replaying minutes-old lines
   * onto whichever console is attached by then — and truncated mid-line, so a
   * colour sequence can lose its reset and tint everything that follows. */
  fflush(stdout);

  /* Start each bring-up from a known line state. These persist across a close,
   * and stale values make the first edge a host produces read as the middle of
   * a reset sequence — or, with the latch still set from a previous session,
   * make a genuine one read as a repeat and be dropped. */
  for (int i = 0; i < TINYUSB_CDC_ACM_MAX; i++) prevRts[i] = prevDtr[i] = false;
  resetPending = false;

  buildDescStrings();

  tinyusb_config_t cfg = TINYUSB_DEFAULT_CONFIG();
  cfg.descriptor.string       = descStrings;
  cfg.descriptor.string_count = sizeof descStrings / sizeof descStrings[0];

  esp_err_t e = tinyusb_driver_install(&cfg);
  if (e != ESP_OK) {
    snprintf(cdcFailReason, sizeof cdcFailReason, "tinyusb install failed (%s)%s",
             esp_err_to_name(e),
             e == ESP_ERR_INVALID_STATE ? "; the usb phy is still held, reboot to clear" : "");
    restoreJtagStreams();
    pmUsbSerialJtagReattach();
    return false;
  }

  for (int i = 0; i < n; i++) {
    tinyusb_config_cdcacm_t acm = {};
    acm.cdc_port = (tinyusb_cdcacm_itf_t)i;
    /* Both ports: the console port needs the esptool reset convention, and
     * either port may be claimed by a serial handler, whose attach and detach
     * are DTR edges. The callbacks themselves discriminate. */
    acm.callback_line_state_changed = cdcLineStateCb;
    acm.callback_rx                 = cdcRxCb;
    e = tinyusb_cdcacm_init(&acm);
    if (e == ESP_OK) cdcDrain(i);
    if (e != ESP_OK) {
      snprintf(cdcFailReason, sizeof cdcFailReason, "cdc %d init failed (%s)", i, esp_err_to_name(e));
      while (--i >= 0) tinyusb_cdcacm_deinit(i);
      tinyusb_driver_uninstall();
      restoreJtagStreams();
      pmUsbSerialJtagReattach();
      return false;
    }
  }
  cdcPorts = n;

  /* Points stdin/stdout/stderr at CDC 0. That one call carries the whole
   * console: logVprintf's direct stdout echo, the serial task's prompts and
   * banners, and its STDIN_FILENO reads all follow it across. */
  e = tinyusb_console_init(TINYUSB_CDC_ACM_0);
  if (e != ESP_OK) {
    /* Name what ESP_ERR_NO_MEM means here, because it is not what it says: the
     * redirect registers /dev/tusbcdc as a filesystem, and the error is the VFS
     * table refusing another one, not the heap. */
    snprintf(cdcFailReason, sizeof cdcFailReason, "console redirect failed (%s)%s",
             esp_err_to_name(e),
             e == ESP_ERR_NO_MEM ? "; the vfs table is full, raise CONFIG_VFS_MAX_COUNT" : "");
    for (int i = cdcPorts - 1; i >= 0; i--) tinyusb_cdcacm_deinit(i);
    cdcPorts = 0;
    tinyusb_driver_uninstall();
    restoreJtagStreams();
    pmUsbSerialJtagReattach();
    return false;
  }

  if (!cdcLock) pmLockCreate(PM_NO_LIGHT_SLEEP, "usbcdc", &cdcLock);
  pmLockAcquire(cdcLock);

  static bool hooked = false;
  if (!hooked) hooked = (esp_register_shutdown_handler(cdcShutdownHook) == ESP_OK);

  /* freopen() reopens the stream and with it discards the line buffering set at
   * boot, leaving stdout fully buffered — log lines would then sit in the FILE
   * buffer until something filled it, which on an idle device is never. */
  setvbuf(stdout, nullptr, _IOLBF, 0);

  consoleOnCdc = true;
  return true;
}

static bool cdcClose(void) {
  /* Clear the flag first: it gates every other task's console writes, and they
   * must be back on the USB-Serial-JTAG path before the CDC VFS goes away. */
  consoleOnCdc = false;
  delay(20);

  restoreJtagStreams();

  /* Drop off the bus before the endpoints go, and give the host a moment to
   * process the detach. The USB-Serial-JTAG controller asserts its own pull-up
   * within a few hundred microseconds of the reattach below; without a gap the
   * host can coalesce detach and attach into no event at all and never
   * re-enumerate. */
  tud_disconnect();
  /* Long enough for the TinyUSB task to carry the detach through before the
   * teardown below deletes it. tusb_deinit() refuses while the stack still
   * believes it is attached, and that refusal is not recoverable — see the
   * uninstall check. */
  delay(250);

  for (int i = cdcPorts - 1; i >= 0; i--) tinyusb_cdcacm_deinit(i);
  cdcPorts = 0;

  esp_err_t e = tinyusb_driver_uninstall();
  if (e != ESP_OK) {
    /* It gives up before releasing the PHY: stopping its task is the first
     * step and it returns on failure, so usb_del_phy() never runs. The internal
     * PHY stays marked in use and every later install fails in usb_new_phy()
     * with "selected PHY is in use" — the console works, but this direction is
     * shut until a reboot clears it. Say so rather than let the next attempt
     * look like a fresh mystery.
     *
     * Recorded rather than only logged: this fires with the CDC device already
     * torn down and the PHY not yet handed back, so the log has nowhere to put
     * it. Reported by switchConsole once a console exists. */
    snprintf(cdcLastError, sizeof cdcLastError,
             "tinyusb uninstall failed (%s); the usb phy stays held, so `usb cdc` "
             "cannot work again until a reboot", esp_err_to_name(e));
  }

  if (cdcLock) pmLockRelease(cdcLock);

  /* Unconditional, and the only thing that moves the PHY mux back:
   * usb_del_phy() drops the pull override but leaves the internal FSLS PHY
   * routed to the OTG wrap, and tinyusb_driver_uninstall() returns early
   * without even getting that far if stopping its task fails. Making this
   * contingent on the result above leaves no console and no way to ask for one
   * short of a reset. */
  pmUsbSerialJtagReattach();
  return e == ESP_OK;
}

/* Console transport, for the `usb` status line. */
extern "C" const char* consoleModeName(void) {
  return consoleOnCdc ? "tinyusb cdc" : "usb-serial-jtag";
}

extern "C" int consoleCdcPortCount(void) { return consoleOnCdc ? cdcPorts : 0; }

/* Why the last switch failed, "" if none has. Kept past the report so `usb` can
 * repeat it: the report goes out during a re-enumeration, which is the one
 * moment a host is least likely to be showing anything. */
extern "C" const char* consoleLastSwitchError(void) { return cdcLastError; }

/* How many serial ports the console currently presents — one on
 * USB-Serial-JTAG, two on the composite device. A serial-handler claimant reads
 * this to know when port 1 exists and re-applies its claim across a switch.
 * consoleCdcPortCount() reports 0 rather than 1 while the console is not on
 * CDC, so the mapping is done here rather than taken from it. */
static void publishSerialPorts(void) {
  storageSet("sys.usb.serial_ports", consoleOnCdc ? cdcPorts : 1);
}

static void switchConsole(bool wantCdc) {
  if (wantCdc == consoleOnCdc) return;
  cdcLastError[0] = '\0';

  /* Hand the serial console back to the live log before the switch. The CLI
   * session cannot outlive the port it is running on, and a watching host needs
   * the notice as a log line — the CLI's own output is addressed to one session
   * and stops at the wire, while the log fans out to every consumer. */
  consoleSwitchPending = true;
  cliSerialResumeLog();
  delay(150);
  if (wantCdc) warn("USB JTAG serial port going away\n");
  else         warn("USB CDC ports going away\n");
  delay(250);

  /* The announcement above is out; from here to the far side of the move there
   * is no wire to write to. */
  consoleWriteDead = true;
  if (consoleOnCdc) cdcClose();
  if (wantCdc && !cdcOpen(TINYUSB_CDC_ACM_MAX))
    snprintf(cdcLastError, sizeof cdcLastError,
             "switch to cdc failed (%s); console stays on usb-serial-jtag",
             cdcFailReason[0] ? cdcFailReason : "no reason recorded");
  consoleWriteDead = false;
  consoleSwitchPending = false;
  publishSerialPorts();
  /* Last, and after the pending flag is cleared: the serial task must be back on
   * its normal path before this spends up to three seconds waiting for a host. */
  reportSwitchFailure();
}

static void cmdUsbCdc(const char* a) {
  if (cliWantsHelp(a)) {
    cliPrintf("%-*s console onto a %d-port TinyUSB device\n",
              CLI_HELP_COL, "usb cdc", TINYUSB_CDC_ACM_MAX);
    return;
  }
  switchConsole(true);
}

static void cmdUsbJtag(const char* a) {
  if (cliWantsHelp(a)) {
    cliPrintf("%-*s console back onto the USB-Serial-JTAG controller\n",
              CLI_HELP_COL, "usb jtag");
    return;
  }
  switchConsole(false);
}

/* Same verbs spelled the way the transports are usually written. Silent for
 * `help` so the listing carries one line per action, not two. */
static void cmdUsbCdcAlias(const char* a)  { if (cliWantsHelp(a)) return; switchConsole(true); }
static void cmdUsbJtagAlias(const char* a) { if (cliWantsHelp(a)) return; switchConsole(false); }

#else /* the CDC transport is not built */

/* The three transport flags above stay false for the life of the image, so
 * every reader in cli.cpp / pm.cpp / log.cpp takes its USB-Serial-JTAG path and
 * needs no guard of its own. These are the calls those paths make from the
 * branch that is never taken; they exist so the branch links, and they must
 * behave as "no CDC port has anything" rather than assert. */
extern "C" int  consoleCdcRead(char*) { return 0; }
extern "C" int  consoleCdcReadPort(int, uint8_t*, size_t) { return 0; }
extern "C" int  consoleCdcWritePort(int, const uint8_t*, size_t) { return 0; }
extern "C" void consoleCdcFlush(void) {}

extern "C" const char* consoleModeName(void) {
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
  return "usb-serial-jtag";
#else
  return "n/a";
#endif
}
extern "C" int consoleCdcPortCount(void) { return 0; }
extern "C" const char* consoleLastSwitchError(void) { return ""; }

/* Why the switch is unavailable: the console lives somewhere the OTG core
 * cannot take over, or the transport was left out of this build. */
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#define CDC_UNAVAIL "not built in — CONFIG_SPANGAP_USB_CDC is off"
#else
#define CDC_UNAVAIL "console is not on USB"
#endif

static void cmdUsbCdc(const char* a) {
  if (cliWantsHelp(a)) { cliPrintf("%-*s console onto a TinyUSB CDC device\n", CLI_HELP_COL, "usb cdc"); return; }
  cliPrintf("usb cdc: n/a (" CDC_UNAVAIL ")\n");
}
static void cmdUsbJtag(const char* a) {
  if (cliWantsHelp(a)) { cliPrintf("%-*s console onto the USB-Serial-JTAG controller\n", CLI_HELP_COL, "usb jtag"); return; }
  cliPrintf("usb jtag: n/a (" CDC_UNAVAIL ")\n");
}
static void cmdUsbCdcAlias(const char* a)  { if (!cliWantsHelp(a)) cmdUsbCdc(a); }
static void cmdUsbJtagAlias(const char* a) { if (!cliWantsHelp(a)) cmdUsbJtag(a); }

#endif

void usbPortsRegisterCmds() {
#if SPANGAP_CDC_BUILT
  publishSerialPorts();
#else
  /* One port, always: the USB-Serial-JTAG controller presents exactly one, and
   * a UART console is a single port too. Claimants watch this key to size
   * themselves — see cli.h. */
  storageSet("sys.usb.serial_ports", 1);
#endif
  cliRegisterCmd("usb cdc",  cmdUsbCdc);
  cliRegisterCmd("usb jtag", cmdUsbJtag);
  cliRegisterCmd("usb CDC",  cmdUsbCdcAlias);
  cliRegisterCmd("usb JTAG", cmdUsbJtagAlias);
}
