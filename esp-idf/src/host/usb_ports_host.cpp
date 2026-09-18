/**
 * usb_ports_host.cpp — the console-transport flags on a host.
 *
 * The console is the process's own stdin and stdout: there is one serial port,
 * it never moves, and the CDC transport does not exist. The three transport
 * flags stay false for the life of the process, so every reader in cli.cpp and
 * log.cpp takes its non-CDC path; the calls below are the ones those paths
 * make from the branch that is never taken.
 */
#include "cli.h"
#include "storage.h"

extern "C" { volatile bool consoleOnCdc = false; }
extern "C" { volatile bool consoleSwitchPending = false; }
extern "C" { volatile bool consoleWriteDead = false; }

extern "C" int  consoleCdcRead(char*) { return 0; }
extern "C" int  consoleCdcReadPort(int, uint8_t*, size_t) { return 0; }
extern "C" int  consoleCdcWritePort(int, const uint8_t*, size_t) { return 0; }
extern "C" void consoleCdcFlush(void) {}
extern "C" int  consoleCdcPortCount(void) { return 0; }
extern "C" void consoleCdcPortStats(int, uint32_t* rx, uint32_t* tx,
                                    uint32_t* rxEvt, uint32_t* dtrEvt) {
    if (rx) *rx = 0;
    if (tx) *tx = 0;
    if (rxEvt) *rxEvt = 0;
    if (dtrEvt) *dtrEvt = 0;
}
extern "C" const char* consoleModeName(void) { return "stdio"; }
extern "C" const char* consoleLastSwitchError(void) { return ""; }
extern "C" void consoleForceJtag(void) {}

void usbPortsRegisterCmds() {
    /* One port, always: stdin and stdout are it. Claimants watch this key to
     * size themselves — see cli.h. */
    storageSet("sys.usb.serial_ports", 1);
}
