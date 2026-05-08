/**
 * auth — cookie-based authentication for the web server.
 *
 * Realms are named password scopes (e.g. "admin"). Each realm has its own
 * password. Passwords must be unique across realms — the login flow uses
 * the password alone to determine the realm.
 *
 * Config lives under secrets.auth.* (persisted, never sent to browser).
 * No in-memory caches — all reads/writes go through the storage API.
 */
#ifndef SECCAM_AUTH_H
#define SECCAM_AUTH_H

#include <string>

enum auth_err_t : int {
    AUTH_OK = 0,
    AUTH_NO_SUCH_REALM = 1,
    AUTH_WRONG_PASSWORD = 2,
    AUTH_SAME_AS_OTHER_REALM = 3,
    AUTH_RATE_LIMITED = 4,
};

/** Initialise auth: sweep expired cookies. Call once at startup. */
void authInit();

/** Returns true when secrets.auth.enable == 1. */
bool authEnabled();

/** Set or change a realm password.
 *  - Probe unset state: old="" + new="" → AUTH_OK if password is unset.
 *  - Set initial password: old="" + new=<pw> (realm hash must be "" or "--").
 *  - Change password: old=<current> + new=<new>.
 *  - Cannot set empty password (new="" when hash is set → AUTH_WRONG_PASSWORD).
 *  - Returns AUTH_SAME_AS_OTHER_REALM if new password matches another realm. */
auth_err_t authPasswd(const char* realm, const char* oldPw, const char* newPw);

/** Try logging in with a password.
 *  If tryRealm is non-empty, checks only that realm. Otherwise tries all.
 *  On success: outRealm = matched realm name, outCookie = raw session token.
 *  Exponential backoff on repeated failures. */
auth_err_t authLogin(const char* password, const char* tryRealm,
                     std::string& outRealm, std::string& outCookie);

/** Check a session cookie. Returns the realm name, or "" if invalid/expired. */
std::string authCheck(const char* cookie);

/** Delete a session cookie. Returns true if found and removed. */
bool authLogout(const char* cookie);

#endif
