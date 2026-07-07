/**
 * service.h — the boot-registered platform object model.
 *
 * A Service is a platform object with a boot lifecycle. It supersedes the
 * free-function `start:` / `init:` hook convention: instead of a straddle
 * declaring a bare `void xInit()` symbol for the generated dispatcher to call,
 * it declares a `services:` class the dispatcher constructs and registers, and
 * the object participates in a boot phase purely by overriding that phase's
 * virtual.
 *
 * The generated staging/spangap_init_dispatch.gen.cpp constructs every service
 * (via per-straddle trampolines and adapters for legacy hooks) at the very top
 * of app_main and appends each to one ordered registry with serviceRegister().
 * Registration order IS boot order — the generator emits the registrations in
 * init_order() (platform band core/net/web/lcd, then dependency-topo straddle
 * band), so dependencies are alive before dependents, exactly as the old
 * per-phase dispatchers ordered them. app_main then walks the registry twice:
 * serviceRunStart() (pre-spangapInit, bare hardware) and serviceRunInit()
 * (post-spangapInit, ecosystem up).
 *
 * ECOSYSTEM-FREE CTOR. Every registered service is constructed at the top of
 * app_main, BEFORE any bring-up (before serviceRunStart, before spangapInit).
 * Heap/PSRAM are up; storage/fs/log/cli/ITS are NOT. A ctor must do member init
 * only — no storage/fs/log/cli/ITS calls. Heavy work belongs in onStart/onInit.
 *
 * Services are immortal: construct-once, no teardown phase. Stopping/starting a
 * service's worker task is a runtime concern the service owns internally
 * (driven by an `s.<x>.enable` subscription or a CLI verb), not a lifecycle the
 * registry manages.
 */
#ifndef SPANGAP_SERVICE_H
#define SPANGAP_SERVICE_H

#ifdef __cplusplus
/* The whole Service model is C++ (a class + virtuals). A C TU that pulls in
 * spangap.h simply sees nothing here — no C consumer registers services. */

/** A boot-registered platform object. Constructed by generated code at the very
 *  top of app_main, before ANY bring-up: ctors must be ecosystem-free (member
 *  init only — no storage/fs/log/cli/ITS). Override the phase(s) this service
 *  participates in; the default no-op means "sit this phase out". */
class Service {
public:
    virtual ~Service() {}

    /** Pre-spangapInit, on bare hardware: fs/storage/log/cli do not exist yet.
     *  For board bring-up the platform itself depends on (power a rail before
     *  fs_mount_sd() touches the SD bus, register a display/touch HAL before
     *  lcdInit()). Raw peripherals only — no info(), storage, fs_ helpers,
     *  or ITS. */
    virtual void onStart() {}

    /** Post-spangapInit: the whole ecosystem (storage/cron, net, web, lcd) is
     *  up. The ordinary bring-up band — the Service equivalent of a legacy
     *  init: hook. */
    virtual void onInit() {}
};

/** Append `s` to the ordered service registry. Called by generated code only,
 *  from spangapRegisterServices(), once per service, in boot order. Boot order
 *  IS call order. Not thread-safe (runs single-threaded at the top of
 *  app_main). */
void serviceRegister(Service* s);

/** Walk the registry in registration order, calling onStart() on each. Called
 *  once from the generated app_main, before spangapInit(). */
void serviceRunStart(void);

/** Walk the registry in registration order, calling onInit() on each. Called
 *  once from the generated app_main, after spangapInit(). */
void serviceRunInit(void);

#endif /* __cplusplus */

#endif /* SPANGAP_SERVICE_H */
