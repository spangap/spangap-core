/**
 * hosted.cpp — platform glue for a chip whose radio is an ESP-Hosted
 * co-processor's.
 *
 * ESP-Hosted logs every RPC to the co-processor, both ways, at info — several
 * lines for each Wi-Fi call, all day — and its SDIO transport prints the
 * co-processor's card description straight to stdout, past the logger,
 * whenever its tag is at info. Those tags default to warnings here; `log <tag>
 * info` brings any of them back. The RPC lines come from the rpc_tx / rpc_rx
 * tasks under the rpc_* tags. ESP-Hosted starts from a constructor, before
 * app_main, so the defaults are registered from one that runs ahead of it.
 */
#include "sdkconfig.h"

#if CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE
#include "log.h"

extern "C" __attribute__((constructor(101))) void hostedQuietCardDump(void) {
    static const char* const kTags[] = {
        "sdio_wrapper", "rpc_core", "rpc_req", "rpc_rsp", "rpc_evt",
    };
    for (const char* t : kTags) logTagDefault(t, "warn");
}
#endif
