/**
 * auth — cookie-based authentication.
 *
 * All state lives in secrets.auth.* config keys (persisted, never sent to
 * browser via the storage DC). No in-memory caches — reads/writes go through storage.
 *
 * Password hashing: SHA-256 with 16-byte random salt.
 * Stored as "salt_hex:hash_hex" in secrets.auth.realms.N.hash.
 *
 * Cookies: 128-bit random tokens (32 hex chars), 60-day expiry.
 */
#include "auth.h"
#include "storage.h"
#include "log.h"
#include "compat.h"
#include "its.h"
#include "web.h"
#include "cJSON.h"
#include "mbedtls/sha256.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include <cstdio>
#include <cstring>
#include <string>

#define AUTH_TAG "auth"

#define MAX_COOKIES       16
#define COOKIE_EXPIRY_S   (60 * 24 * 3600)   /* 60 days */
#define MAX_BACKOFF_S     300                 /* 5 minutes */

/* ---- Helpers ---- */

static void toHex(const uint8_t* data, size_t len, char* out) {
    for (size_t i = 0; i < len; i++)
        snprintf(out + i * 2, 3, "%02x", data[i]);
}

static bool fromHex(const char* hex, uint8_t* out, size_t outLen) {
    for (size_t i = 0; i < outLen; i++) {
        unsigned v;
        if (sscanf(hex + i * 2, "%2x", &v) != 1) return false;
        out[i] = (uint8_t)v;
    }
    return true;
}

/** Compute SHA-256 of salt+password, return "salt_hex:hash_hex". */
static std::string hashPassword(const char* password, const uint8_t* salt, size_t saltLen) {
    uint8_t digest[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, salt, saltLen);
    mbedtls_sha256_update(&ctx, (const uint8_t*)password, strlen(password));
    mbedtls_sha256_finish(&ctx, digest);
    mbedtls_sha256_free(&ctx);

    char saltHex[33], hashHex[65];
    toHex(salt, saltLen, saltHex);
    toHex(digest, 32, hashHex);

    return std::string(saltHex) + ":" + hashHex;
}

/** Hash password with a fresh random salt. */
static std::string hashPasswordNew(const char* password) {
    uint8_t salt[16];
    uint32_t r[4];
    for (int i = 0; i < 4; i++) r[i] = esp_random();
    memcpy(salt, r, 16);
    return hashPassword(password, salt, 16);
}

/** Verify password against stored "salt_hex:hash_hex". */
static bool verifyPassword(const char* password, const char* stored) {
    const char* colon = strchr(stored, ':');
    if (!colon || colon - stored != 32) return false;

    uint8_t salt[16];
    if (!fromHex(stored, salt, 16)) return false;

    std::string computed = hashPassword(password, salt, 16);
    return computed == stored;
}

/* ---- Realm helpers ---- */

static int realmCount() {
    return storageArrayCount("secrets.auth.realms.");
}

static bool realmGet(int idx, char* name, size_t nameLen, char* hash, size_t hashLen) {
    char key[64];
    snprintf(key, sizeof(key), "secrets.auth.realms.%d.name", idx);
    storageGetStr(key, name, nameLen);
    if (!name[0]) return false;
    snprintf(key, sizeof(key), "secrets.auth.realms.%d.hash", idx);
    storageGetStr(key, hash, hashLen);
    return true;
}

static int realmFind(const char* realm) {
    int count = realmCount();
    char name[32];
    char hash[128];
    for (int i = 0; i < count; i++) {
        if (realmGet(i, name, sizeof(name), hash, sizeof(hash)) &&
            strcmp(name, realm) == 0)
            return i;
    }
    return -1;
}

/* ---- Cookie helpers ---- */

static int cookieCount() {
    return storageArrayCount("secrets.auth.cookies.");
}

static void cookieRemove(int idx) {
    /* Shift entries down to compact the array */
    int count = cookieCount();
    storageBegin();
    for (int i = idx; i < count - 1; i++) {
        char srcKey[80], dstKey[80], val[128];
        for (const char* field : {"cookie", "expires", "realm"}) {
            snprintf(srcKey, sizeof(srcKey), "secrets.auth.cookies.%d.%s", i + 1, field);
            snprintf(dstKey, sizeof(dstKey), "secrets.auth.cookies.%d.%s", i, field);
            storageGetStr(srcKey, val, sizeof(val));
            storageSet(dstKey, val);
        }
    }
    /* Delete the last entry */
    for (const char* field : {"cookie", "expires", "realm"}) {
        char key[80];
        snprintf(key, sizeof(key), "secrets.auth.cookies.%d.%s", count - 1, field);
        storageUnset(key);
    }
    storageEnd();
}

static void cookieAdd(const char* token, const char* realm, time_t expires) {
    int count = cookieCount();

    /* Evict oldest if at capacity */
    if (count >= MAX_COOKIES) {
        time_t oldest = 0;
        int oldestIdx = 0;
        for (int i = 0; i < count; i++) {
            char key[80], val[32];
            snprintf(key, sizeof(key), "secrets.auth.cookies.%d.expires", i);
            storageGetStr(key, val, sizeof(val));
            time_t exp = (time_t)strtoll(val, nullptr, 10);
            if (oldest == 0 || exp < oldest) {
                oldest = exp;
                oldestIdx = i;
            }
        }
        cookieRemove(oldestIdx);
        count--;
    }

    storageBegin();
    char key[80];
    snprintf(key, sizeof(key), "secrets.auth.cookies.%d.cookie", count);
    storageSet(key, token);
    snprintf(key, sizeof(key), "secrets.auth.cookies.%d.realm", count);
    storageSet(key, realm);
    snprintf(key, sizeof(key), "secrets.auth.cookies.%d.expires", count);
    char expStr[20];
    snprintf(expStr, sizeof(expStr), "%lld", (long long)expires);
    storageSet(key, expStr);
    storageEnd();
}

/* ---- Backoff state (global statics, web task is single-threaded) ---- */

static uint32_t lastFailMs = 0;
static int failCount = 0;

static bool isRateLimited() {
    if (failCount == 0) return false;
    uint32_t delaySec = 1;
    for (int i = 1; i < failCount && delaySec < MAX_BACKOFF_S; i++)
        delaySec *= 2;
    if (delaySec > MAX_BACKOFF_S) delaySec = MAX_BACKOFF_S;
    return (millis() - lastFailMs) < (delaySec * 1000);
}

/* ---- /auth/{login,passwd,logout} REST endpoints ----
 * auth registers the URL prefix "auth" with web. web forwards matching
 * HTTP requests here; we read the request, dispatch by path, send the
 * JSON response, and disconnect. Each request is short and synchronous,
 * so handlers run inline on the auth task — no temp-task hand-off. */

static constexpr uint16_t AUTH_URL_PORT = 1;
static constexpr int      AUTH_BODY_MAX = 1024;

static void authReplyJson(int h, const char* json) {
    webSendResponse(h, 200, "application/json", json, strlen(json));
}

static void authReplyResultCode(int h, int result) {
    char json[40];
    snprintf(json, sizeof(json), "{\"result\":%d}", result);
    authReplyJson(h, json);
}

static void authHandleLogin(int h, const char* hdr, int hlen) {
    char* body = webReadBody(h, hdr, hlen, AUTH_BODY_MAX, nullptr);
    if (!body) { webSendStatus(h, 400); return; }

    cJSON* req = cJSON_Parse(body);
    heap_caps_free(body);
    if (!req) { webSendStatus(h, 400); return; }

    const char* password = cJSON_GetStringValue(cJSON_GetObjectItem(req, "password"));
    const char* tryRealm = "";
    cJSON* realmItem = cJSON_GetObjectItem(req, "realm");
    if (cJSON_IsString(realmItem)) tryRealm = realmItem->valuestring;

    if (!password) {
        authReplyResultCode(h, AUTH_WRONG_PASSWORD);
    } else {
        std::string outRealm, outCookie;
        auth_err_t e = authLogin(password, tryRealm, outRealm, outCookie);
        if (e == AUTH_OK) {
            char json[256];
            snprintf(json, sizeof(json),
                "{\"result\":0,\"realm\":\"%s\",\"cookie\":\"%s\"}",
                outRealm.c_str(), outCookie.c_str());
            authReplyJson(h, json);
        } else {
            authReplyResultCode(h, (int)e);
        }
    }
    cJSON_Delete(req);
}

static void authHandlePasswd(int h, const char* hdr, int hlen) {
    char* body = webReadBody(h, hdr, hlen, AUTH_BODY_MAX, nullptr);
    if (!body) { webSendStatus(h, 400); return; }

    cJSON* req = cJSON_Parse(body);
    heap_caps_free(body);
    if (!req) { webSendStatus(h, 400); return; }

    const char* realm = cJSON_GetStringValue(cJSON_GetObjectItem(req, "realm"));
    const char* oldPw = cJSON_GetStringValue(cJSON_GetObjectItem(req, "old"));
    const char* newPw = cJSON_GetStringValue(cJSON_GetObjectItem(req, "new"));

    if (!realm || !oldPw || !newPw) {
        authReplyResultCode(h, AUTH_WRONG_PASSWORD);
    } else {
        auth_err_t e = authPasswd(realm, oldPw, newPw);
        authReplyResultCode(h, (int)e);
    }
    cJSON_Delete(req);
}

static void authHandleLogout(int h, const char* hdr, int hlen) {
    char cookie[64] = {};
    webExtractCookie(hdr, hlen, "session", cookie, sizeof(cookie));
    authLogout(cookie);
    /* Note: the previous web-task implementation also force-closed every other
     * web connection from the same client IP. Skipped here — the cookie is
     * already deleted, so any subsequent request authenticating with it will
     * fail authCheck and be rejected; cross-connection eviction is purely a
     * UX nicety and not worth the cross-task plumbing. */
    authReplyJson(h, "{\"result\":0}");
}

static void onAuthRecv(int h, size_t /*bytesAvail*/) {
    char hdr[2048];
    int hlen = webGetHeader(h, hdr, sizeof(hdr));
    if (hlen <= 0) { itsDisconnect(h); return; }

    char method[8];
    char path[64];
    webGetMethod(hdr, hlen, method, sizeof(method));
    webGetPath(hdr, hlen, path, sizeof(path));

    if (strcmp(path, "auth/logout") == 0) {
        authHandleLogout(h, hdr, hlen);
    } else if (strcmp(method, "POST") != 0) {
        webSendStatus(h, 405);
    } else if (strcmp(path, "auth/login") == 0) {
        authHandleLogin(h, hdr, hlen);
    } else if (strcmp(path, "auth/passwd") == 0) {
        authHandlePasswd(h, hdr, hlen);
    } else {
        webSendStatus(h, 404);
    }

    /* Wait for the response we just queued to drain through net's TLS write
     * before tearing the connection down — itsDisconnect → connFree frees
     * the from-buffer immediately, so without this the response bytes never
     * reach the wire and the client sees an "empty reply" with TLS close. */
    itsSendDrain(h, 1000);
    itsDisconnect(h);
}

static int onAuthConnect(int /*handle*/, const void* /*data*/, size_t /*len*/) {
    return 0;  /* accept; onRecv fires when the request bytes arrive */
}

static void authTaskFn(void*) {
    itsServerInit(0, 16);
    itsServerPortOpen(AUTH_URL_PORT, /*packetBased=*/false, /*maxHandles=*/4,
                      /*toSize=*/2048, /*fromSize=*/2048);
    itsServerOnConnect(AUTH_URL_PORT, onAuthConnect);
    itsServerOnRecv(AUTH_URL_PORT, onAuthRecv);

    web_path_msg_t reg = {};
    reg.itsPort = AUTH_URL_PORT;
    safeStrncpy(reg.path, "auth", sizeof(reg.path));
    while (!itsSendAux("web", WEB_PATH_REG_PORT, &reg, sizeof(reg), pdMS_TO_TICKS(500)))
        vTaskDelay(pdMS_TO_TICKS(100));

    for (;;) itsPoll();
}

/* ---- Public API ---- */

/* Module config version. Bump when adding/changing defaults. See duckdns.cpp. */
#define AUTH_VERSION 1

void authInit() {
    int v = storageGetInt("secrets.auth.version", 0);
    if (v < AUTH_VERSION) {
        storageDefaultTree("secrets.auth", R"({
            "enable": 1,
            "realms": [{"name":"admin", "hash":""}],
            "cookies": []
        })");
        storageSet("secrets.auth.version", AUTH_VERSION);
    }

    /* Sweep expired cookies */
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    if (tv.tv_sec >= 1700000000) {
        int count = cookieCount();
        for (int i = count - 1; i >= 0; i--) {
            char key[80], val[32];
            snprintf(key, sizeof(key), "secrets.auth.cookies.%d.expires", i);
            storageGetStr(key, val, sizeof(val));
            time_t exp = (time_t)strtoll(val, nullptr, 10);
            if (exp > 1700000000 && exp < tv.tv_sec) {
                cookieRemove(i);
            }
        }
    }

    /* Spawn the auth task that owns the /auth URL prefix. PSRAM stack —
     * cJSON parsing for login bodies allocates from PSRAM via storage code. */
    spawnTask(authTaskFn, "auth", 8192, nullptr, 1, 1, STACK_PSRAM);
}

bool authEnabled() {
    return storageGetInt("secrets.auth.enable", 0) == 1;
}

auth_err_t authPasswd(const char* realm, const char* oldPw, const char* newPw) {
    int idx = realmFind(realm);
    if (idx < 0) return AUTH_NO_SUCH_REALM;

    char hash[128];
    char key[64];
    snprintf(key, sizeof(key), "secrets.auth.realms.%d.hash", idx);
    storageGetStr(key, hash, sizeof(hash));

    bool unset = (hash[0] == '\0');
    bool locked = (strcmp(hash, "--") == 0);

    if (unset) {
        /* Unset realm: old must be empty */
        if (oldPw[0] != '\0') return AUTH_WRONG_PASSWORD;
        /* Probe: old="" + new="" → AUTH_OK */
        if (newPw[0] == '\0') return AUTH_OK;
    } else if (locked) {
        /* Locked realm: old must be empty (admin is setting it) */
        if (oldPw[0] != '\0') return AUTH_WRONG_PASSWORD;
        if (newPw[0] == '\0') return AUTH_OK;  /* probe: locked */
    } else {
        /* Has a real password: verify old */
        if (!verifyPassword(oldPw, hash)) return AUTH_WRONG_PASSWORD;
        /* Can't clear to empty */
        if (newPw[0] == '\0') return AUTH_WRONG_PASSWORD;
    }

    /* Check new password doesn't collide with another realm */
    int count = realmCount();
    for (int i = 0; i < count; i++) {
        if (i == idx) continue;
        char otherHash[128];
        char otherKey[64];
        snprintf(otherKey, sizeof(otherKey), "secrets.auth.realms.%d.hash", i);
        storageGetStr(otherKey, otherHash, sizeof(otherHash));
        if (otherHash[0] == '\0' || strcmp(otherHash, "--") == 0) continue;
        if (verifyPassword(newPw, otherHash)) return AUTH_SAME_AS_OTHER_REALM;
    }

    /* Store new hash — must write the complete realm entry (name + hash)
     * because storageSet on array paths converts the array to an object
     * via deepMerge, which would lose unmentioned fields. */
    std::string newHash = hashPasswordNew(newPw);
    char nameKey[64];
    snprintf(nameKey, sizeof(nameKey), "secrets.auth.realms.%d.name", idx);
    storageBegin();
    storageSet(nameKey, realm);
    storageSet(key, newHash.c_str());
    storageEnd();
    /* No storageSave() here — storageEnd() starts the save timer which flushes
       asynchronously on the storage task's stack (avoids cJSON_Print recursion
       on the 6KB web task stack). */
    return AUTH_OK;
}

auth_err_t authLogin(const char* password, const char* tryRealm,
                     std::string& outRealm, std::string& outCookie) {
    if (isRateLimited()) return AUTH_RATE_LIMITED;

    int count = realmCount();
    for (int i = 0; i < count; i++) {
        char name[32], hash[128];
        if (!realmGet(i, name, sizeof(name), hash, sizeof(hash))) continue;

        /* Skip if targeting a specific realm and this isn't it */
        if (tryRealm[0] != '\0' && strcmp(name, tryRealm) != 0) continue;

        /* Skip unset or locked realms */
        if (hash[0] == '\0' || strcmp(hash, "--") == 0) continue;

        if (verifyPassword(password, hash)) {
            /* Success — reset backoff */
            failCount = 0;

            /* Generate cookie token */
            uint32_t rnd[4];
            for (int j = 0; j < 4; j++) rnd[j] = esp_random();
            char token[33];
            toHex((const uint8_t*)rnd, 16, token);

            /* Store cookie */
            struct timeval tv;
            gettimeofday(&tv, nullptr);
            time_t expires = tv.tv_sec + COOKIE_EXPIRY_S;
            cookieAdd(token, name, expires);

            outRealm = name;
            outCookie = token;
            info("login: realm=%s\n", name);
            return AUTH_OK;
        }
    }

    /* Failed */
    failCount++;
    lastFailMs = millis();
    info("login failed (attempt %d)\n", failCount);
    return AUTH_WRONG_PASSWORD;
}

bool authLogout(const char* cookie) {
    if (!cookie || !cookie[0]) return false;
    int count = cookieCount();
    for (int i = 0; i < count; i++) {
        char key[80], stored[64];
        snprintf(key, sizeof(key), "secrets.auth.cookies.%d.cookie", i);
        storageGetStr(key, stored, sizeof(stored));
        if (strcmp(stored, cookie) == 0) {
            cookieRemove(i);
            info("logout\n");
            return true;
        }
    }
    return false;
}

std::string authCheck(const char* cookie) {
    if (!cookie || !cookie[0]) return "";

    struct timeval tv;
    gettimeofday(&tv, nullptr);
    time_t now = tv.tv_sec;

    int count = cookieCount();
    for (int i = 0; i < count; i++) {
        char key[80], stored[64];
        snprintf(key, sizeof(key), "secrets.auth.cookies.%d.cookie", i);
        storageGetStr(key, stored, sizeof(stored));
        if (strcmp(stored, cookie) != 0) continue;

        /* Check expiry */
        char expStr[20];
        snprintf(key, sizeof(key), "secrets.auth.cookies.%d.expires", i);
        storageGetStr(key, expStr, sizeof(expStr));
        time_t exp = (time_t)strtoll(expStr, nullptr, 10);

        if (now > 1700000000 && exp > 1700000000 && exp < now) {
            /* Expired — remove it (skip check for cookies created before valid time) */
            cookieRemove(i);
            return "";
        }

        char realm[32];
        snprintf(key, sizeof(key), "secrets.auth.cookies.%d.realm", i);
        storageGetStr(key, realm, sizeof(realm));
        return realm;
    }
    return "";
}
