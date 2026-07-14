/**
 * auth — realm/password store + CLI. Pure core: no transport.
 *
 * State lives in secrets.auth.* config keys (persisted, never sent to
 * browser via the storage DC). No in-memory caches — reads/writes go
 * through storage.
 *
 * Password hashing: SHA-256 with 16-byte random salt.
 * Stored as "salt_hex:hash_hex" in secrets.auth.realms.N.hash.
 *
 * Cookies: 128-bit random tokens (32 hex chars), 60-day expiry.
 *
 * The HTTP /auth/{login,passwd,logout} endpoints live in
 * spangap-web/src/auth_web.cpp on top of this API — see authWebInit.
 */
#include "auth.h"
#include "storage.h"
#include "log.h"
#include "compat.h"
#include "cli.h"
#include "mbedtls/sha256.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include <cstdio>
#include <cstring>
#include <string>

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

static std::string hashPasswordNew(const char* password) {
    uint8_t salt[16];
    uint32_t r[4];
    for (int i = 0; i < 4; i++) r[i] = esp_random();
    memcpy(salt, r, 16);
    return hashPassword(password, salt, 16);
}

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
    for (const char* field : {"cookie", "expires", "realm"}) {
        char key[80];
        snprintf(key, sizeof(key), "secrets.auth.cookies.%d.%s", count - 1, field);
        storageUnset(key);
    }
    storageEnd();
}

static void cookieAdd(const char* token, const char* realm, time_t expires) {
    int count = cookieCount();
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

/* ---- Backoff (single shared by all callers — auth task model is one
 * client at a time across the device, so a global counter is enough). */

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

/* ---- CLI ---- */

/** Set a realm's password unconditionally — bypasses the old-password
 *  check authPasswd enforces. Admin-only entry point (CLI). Returns
 *  AUTH_NO_SUCH_REALM, AUTH_SAME_AS_OTHER_REALM, AUTH_WRONG_PASSWORD
 *  (empty new), or AUTH_OK. */
static auth_err_t authForceSetPassword(const char* realm, const char* newPw) {
    if (!newPw || newPw[0] == '\0') return AUTH_WRONG_PASSWORD;
    int idx = realmFind(realm);
    if (idx < 0) return AUTH_NO_SUCH_REALM;
    int count = realmCount();
    for (int i = 0; i < count; i++) {
        if (i == idx) continue;
        char otherHash[128], otherKey[64];
        snprintf(otherKey, sizeof(otherKey), "secrets.auth.realms.%d.hash", i);
        storageGetStr(otherKey, otherHash, sizeof(otherHash));
        if (otherHash[0] == '\0' || strcmp(otherHash, "--") == 0) continue;
        if (verifyPassword(newPw, otherHash)) return AUTH_SAME_AS_OTHER_REALM;
    }
    std::string newHash = hashPasswordNew(newPw);
    char nameKey[64], hashKey[64];
    snprintf(nameKey, sizeof(nameKey), "secrets.auth.realms.%d.name", idx);
    snprintf(hashKey, sizeof(hashKey), "secrets.auth.realms.%d.hash", idx);
    storageBegin();
    storageSet(nameKey, realm);
    storageSet(hashKey, newHash.c_str());
    storageEnd();
    return AUTH_OK;
}

static void authCliCmd(const char* args) {
    if (strcmp(args, "help") == 0) { cliPrintf("%-*s auth status; realms; passwd\n", CLI_HELP_COL, "auth [...]"); return; }
    if (strcmp(args, "-h") == 0 || strcmp(args, "--help") == 0) {
        cliPrintf("%-*s show enabled + realm state\n", CLI_HELP_COL, "auth");
        cliPrintf("%-*s list realms with set/locked/unset state\n", CLI_HELP_COL, "auth realms");
        cliPrintf("%-*s set (overwrite) a realm password\n", CLI_HELP_COL, "auth passwd <realm> <newpw>");
        return;
    }
    if (args[0] == '\0') {
        cliPrintf("enabled: %s\n", authEnabled() ? "yes" : "no");
        cliPrintf("realms:  %d\n", realmCount());
        cliPrintf("cookies: %d / %d\n", cookieCount(), MAX_COOKIES);
        return;
    }
    if (strcmp(args, "realms") == 0) {
        int n = realmCount();
        if (n == 0) { cliPrintf("(none)\n"); return; }
        for (int i = 0; i < n; i++) {
            char name[32], hash[128];
            if (!realmGet(i, name, sizeof(name), hash, sizeof(hash))) continue;
            const char* state = hash[0] == '\0' ? "unset"
                              : strcmp(hash, "--") == 0 ? "locked"
                              : "set";
            cliPrintf("[%d] %-20s %s\n", i, name, state);
        }
        return;
    }
    if (strncmp(args, "passwd ", 7) == 0) {
        const char* p = args + 7;
        while (*p == ' ') p++;
        const char* sp = strchr(p, ' ');
        if (!sp || sp == p) { cliPrintf("usage: auth passwd <realm> <newpw>\n"); return; }
        char realm[32];
        size_t n = (size_t)(sp - p);
        if (n >= sizeof(realm)) { cliPrintf("realm name too long\n"); return; }
        memcpy(realm, p, n); realm[n] = '\0';
        const char* pw = sp + 1;
        while (*pw == ' ') pw++;
        if (!*pw) { cliPrintf("usage: auth passwd <realm> <newpw>\n"); return; }
        auth_err_t e = authForceSetPassword(realm, pw);
        switch (e) {
            case AUTH_OK:                  cliPrintf("ok\n"); break;
            case AUTH_NO_SUCH_REALM:       cliPrintf("no such realm: '%s'\n", realm); break;
            case AUTH_SAME_AS_OTHER_REALM: cliPrintf("password matches another realm\n"); break;
            case AUTH_WRONG_PASSWORD:      cliPrintf("empty password not allowed\n"); break;
            default:                       cliPrintf("error %d\n", (int)e); break;
        }
        return;
    }
    cliPrintf("unknown subcommand. try `auth -h`\n");
}

/* `passwd` — interactive password set for the "admin" realm. Prompts twice
 * (echoed as stars) and only commits when both entries match. This is the
 * convenient front door to `auth passwd admin <pw>`; the realm is fixed to
 * "admin" by design (the realm every device ships with). */
static void passwdCliCmd(const char* args) {
    if (cliWantsHelp(args)) {
        cliPrintf("%-*s set the admin password (prompts twice)\n", CLI_HELP_COL, "passwd");
        return;
    }
    char pw1[128], pw2[128];
    cliPrintf("New password: ");
    int n1 = cliReadLine(pw1, sizeof(pw1), CLI_ECHO_STARS);
    if (n1 < 0) { cliPrintf("passwd: cancelled\n"); return; }
    if (n1 == 0) {
        cliPrintf("passwd: empty password not allowed\n");
        memset(pw1, 0, sizeof(pw1));
        return;
    }
    cliPrintf("Retype new password: ");
    int n2 = cliReadLine(pw2, sizeof(pw2), CLI_ECHO_STARS);
    if (n2 < 0) {
        cliPrintf("passwd: cancelled\n");
        memset(pw1, 0, sizeof(pw1));
        return;
    }
    if (strcmp(pw1, pw2) != 0) {
        cliPrintf("passwd: passwords do not match\n");
    } else {
        auth_err_t e = authForceSetPassword("admin", pw1);
        switch (e) {
            case AUTH_OK:                  cliPrintf("password updated\n"); break;
            case AUTH_NO_SUCH_REALM:       cliPrintf("passwd: no 'admin' realm\n"); break;
            case AUTH_SAME_AS_OTHER_REALM: cliPrintf("passwd: matches another realm's password\n"); break;
            case AUTH_WRONG_PASSWORD:      cliPrintf("passwd: empty password not allowed\n"); break;
            default:                       cliPrintf("passwd: error %d\n", (int)e); break;
        }
    }
    memset(pw1, 0, sizeof(pw1));
    memset(pw2, 0, sizeof(pw2));
}

/* ---- Public API ---- */

#define AUTH_VERSION 1

void authInit() {
    int v = storageGetInt("secrets.auth.version", 0);
    if (v < AUTH_VERSION) {
        storageBegin();
        storageDefaultTree("secrets.auth", R"({
            "enable": 1,
            "realms": [{"name":"admin", "hash":""}],
            "cookies": []
        })");
        storageSet("secrets.auth.version", AUTH_VERSION);
        storageEnd();
    }

    /* Sweep expired cookies (skip when wall clock isn't valid yet). */
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    if (tv.tv_sec >= 1700000000) {
        int count = cookieCount();
        for (int i = count - 1; i >= 0; i--) {
            char key[80], val[32];
            snprintf(key, sizeof(key), "secrets.auth.cookies.%d.expires", i);
            storageGetStr(key, val, sizeof(val));
            time_t exp = (time_t)strtoll(val, nullptr, 10);
            if (exp > 1700000000 && exp < tv.tv_sec) cookieRemove(i);
        }
    }

    cliRegisterCmd("auth", authCliCmd);
    cliRegisterCmd("passwd", passwdCliCmd);
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
        if (oldPw[0] != '\0') return AUTH_WRONG_PASSWORD;
        if (newPw[0] == '\0') return AUTH_OK;
    } else if (locked) {
        if (oldPw[0] != '\0') return AUTH_WRONG_PASSWORD;
        if (newPw[0] == '\0') return AUTH_OK;
    } else {
        if (!verifyPassword(oldPw, hash)) return AUTH_WRONG_PASSWORD;
        if (newPw[0] == '\0') return AUTH_WRONG_PASSWORD;
    }

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

    std::string newHash = hashPasswordNew(newPw);
    char nameKey[64];
    snprintf(nameKey, sizeof(nameKey), "secrets.auth.realms.%d.name", idx);
    storageBegin();
    storageSet(nameKey, realm);
    storageSet(key, newHash.c_str());
    storageEnd();
    return AUTH_OK;
}

auth_err_t authLogin(const char* password, const char* tryRealm,
                     std::string& outRealm, std::string& outCookie) {
    if (isRateLimited()) return AUTH_RATE_LIMITED;

    int count = realmCount();
    for (int i = 0; i < count; i++) {
        char name[32], hash[128];
        if (!realmGet(i, name, sizeof(name), hash, sizeof(hash))) continue;
        if (tryRealm[0] != '\0' && strcmp(name, tryRealm) != 0) continue;
        if (hash[0] == '\0' || strcmp(hash, "--") == 0) continue;
        if (verifyPassword(password, hash)) {
            failCount = 0;
            uint32_t rnd[4];
            for (int j = 0; j < 4; j++) rnd[j] = esp_random();
            char token[33];
            toHex((const uint8_t*)rnd, 16, token);
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

        char expStr[20];
        snprintf(key, sizeof(key), "secrets.auth.cookies.%d.expires", i);
        storageGetStr(key, expStr, sizeof(expStr));
        time_t exp = (time_t)strtoll(expStr, nullptr, 10);

        if (now > 1700000000 && exp > 1700000000 && exp < now) {
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
