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
 */
#include "service.h"

#include <vector>

namespace {
std::vector<Service*>& registry() {
    static std::vector<Service*> reg;
    return reg;
}
}  // namespace

void serviceRegister(Service* s) {
    if (s) registry().push_back(s);
}

void serviceRunStart(void) {
    for (Service* s : registry()) s->onStart();
}

void serviceRunInit(void) {
    for (Service* s : registry()) s->onInit();
}
