/**
 * service.cpp — the boot-registered platform object registry.
 *
 * A plain ordered vector of Service*. Registration order is boot order (the
 * generated spangapRegisterServices() appends in init_order()), and both phase
 * walks traverse it in that order. See service.h for the model.
 *
 * The vector is a function-local static so it is constructed on first
 * serviceRegister() — no static-init-order dependency on any other TU. Objects
 * are immortal (never removed), so the registry only ever grows during the
 * single-threaded registration pass at the top of app_main.
 *
 * Each entry carries the band the generator assigned it (service.h). The onInit
 * walk is the only phase that filters on it: a safe-mode boot stops after the
 * platform band's web entry, leaving lcd and every straddle down.
 */
#include "spangap.h"

#include <vector>

namespace {
struct entry_t { Service* svc; service_band_t band; };

std::vector<entry_t>& registry() {
    static std::vector<entry_t> reg;
    return reg;
}
}  // namespace

void serviceRegister(Service* s, service_band_t band) {
    if (s) registry().push_back({s, band});
}

void serviceRunStart(void) {
    /* Before the first onStart, because this is the last moment the hardware is
     * untouched. The board check probes buses itself, and a board's onStart is
     * exactly what claims them (hw-lilygo-tdeck creates the shared I2C0 bus
     * there) — so it goes first or it does not work at all. It never returns on
     * a wrong board. */
    spangapConfirmBoard();
    for (const entry_t& e : registry()) e.svc->onStart();
}

void serviceRunInit(void) {
    bool safe = spangapSafeMode() != SAFE_MODE_NONE;
    for (const entry_t& e : registry()) {
        if (safe && e.band != SERVICE_BAND_SAFE) continue;
        e.svc->onInit();
    }
}
