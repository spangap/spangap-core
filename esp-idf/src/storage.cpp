/**
 * storage — config store + file I/O service.
 *
 * Config: JSON file on /state, dot-notation keys in std::map.
 * File I/O: POSIX-like API. SD card paths → direct calls on caller's thread.
 *   LittleFS paths → proxied to a small DRAM-stack worker (SPI flash ops
 *   disable the PSRAM cache, so the call stack must be in DRAM).
 *
 * ITS server: handle 0 = browser config WS (root path "/").
 */
#include "storage.h"
#include "ipc.h"
#include "log.h"
#include "cli.h"
#include "its.h"
#include "web.h"
#include "net.h"

#include <map>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "cJSON.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

struct CfgEntry {
  cfg_type_t type;
  std::string value;
};

static std::map<std::string, CfgEntry> store;
static bool savePending = false;
static esp_timer_handle_t saveTimer = nullptr;

/* ---- Task state ---- */

static TaskHandle_t storageHandle = nullptr;
static int wsHandle = -1;

/* ---- File I/O ---- */

/*
 * All file ops run on the fs worker task (DRAM stack). Callers block on a
 * semaphore until the worker completes. This is safe for any caller stack
 * type — the worker's DRAM stack is needed because SPI flash ops disable
 * the PSRAM cache. Data buffers in PSRAM are fine (only accessed after
 * cache re-enable).
 */

#define MAX_FILE_SLOTS 6

static FILE* fileFps[MAX_FILE_SLOTS];
static bool fileActive[MAX_FILE_SLOTS];
static QueueHandle_t fsQueue = nullptr;

struct storage_file_op_t {
  enum Op { OPEN, READ, WRITE, TELL, CLOSE, STAT, RENAME, REMOVE } op;
  const char* path;
  const char* path2;        /* rename: newPath; open: mode */
  int slot;
  void* buf;
  size_t len;
  struct stat* st;
  int result;
  SemaphoreHandle_t done;
};

static void handleFileOp(storage_file_op_t* req) {
  switch (req->op) {
    case storage_file_op_t::OPEN: {
      FILE* f = fopen(req->path, req->path2);
      if (!f) { req->result = -1; break; }
      fileFps[req->slot] = f;
      req->result = req->slot;
      break;
    }
    case storage_file_op_t::READ: {
      int s = req->slot;
      if (s < 0 || s >= MAX_FILE_SLOTS || !fileFps[s]) { req->result = 0; break; }
      req->result = (int)fread(req->buf, 1, req->len, fileFps[s]);
      break;
    }
    case storage_file_op_t::WRITE: {
      int s = req->slot;
      if (s < 0 || s >= MAX_FILE_SLOTS || !fileFps[s]) { req->result = 0; break; }
      req->result = (int)fwrite(req->buf, 1, req->len, fileFps[s]);
      break;
    }
    case storage_file_op_t::TELL: {
      int s = req->slot;
      if (s < 0 || s >= MAX_FILE_SLOTS || !fileFps[s]) { req->result = -1; break; }
      req->result = (int)ftell(fileFps[s]);
      break;
    }
    case storage_file_op_t::CLOSE: {
      int s = req->slot;
      if (s >= 0 && s < MAX_FILE_SLOTS && fileFps[s]) {
        fclose(fileFps[s]);
        fileFps[s] = nullptr;
        fileActive[s] = false;
      }
      req->result = 0;
      break;
    }
    case storage_file_op_t::STAT:
      req->result = stat(req->path, req->st);
      break;
    case storage_file_op_t::RENAME:
      req->result = rename(req->path, req->path2);
      break;
    case storage_file_op_t::REMOVE:
      req->result = remove(req->path);
      break;
  }
  xSemaphoreGive(req->done);
}

static void fsWorkerFn(void*) {
  for (;;) {
    storage_file_op_t* op;
    if (xQueueReceive(fsQueue, &op, portMAX_DELAY))
      handleFileOp(op);
  }
}

static int fsOp(storage_file_op_t& req) {
  req.done = xSemaphoreCreateBinary();
  storage_file_op_t* ptr = &req;
  xQueueSend(fsQueue, &ptr, portMAX_DELAY);
  xSemaphoreTake(req.done, portMAX_DELAY);
  vSemaphoreDelete(req.done);
  return req.result;
}

static int allocSlot() {
  for (int i = 0; i < MAX_FILE_SLOTS; i++) {
    if (!fileActive[i]) { fileActive[i] = true; return i; }
  }
  return -1;
}

/* ---- JSON helpers ---- */

static std::vector<std::string> splitKey(const std::string& key) {
  std::vector<std::string> parts;
  size_t start = 0;
  for (size_t i = 0; i <= key.size(); i++) {
    if (i == key.size() || key[i] == '.') {
      if (i > start)
        parts.emplace_back(key, start, i - start);
      start = i + 1;
    }
  }
  return parts;
}

static void flattenJson(cJSON* obj, const std::string& prefix) {
  cJSON* item;
  cJSON_ArrayForEach(item, obj) {
    std::string key = prefix.empty() ? item->string : prefix + "." + item->string;
    if (cJSON_IsObject(item)) {
      flattenJson(item, key);
    } else if (cJSON_IsNumber(item)) {
      char buf[16];
      snprintf(buf, sizeof(buf), "%d", item->valueint);
      store[key] = { CFG_INT, std::string(buf) };
    } else if (cJSON_IsString(item)) {
      store[key] = { CFG_STR, std::string(item->valuestring) };
    }
  }
}

static cJSON* buildNestedJson(bool (*pred)(const std::string& key)) {
  cJSON* root = cJSON_CreateObject();
  for (auto& [key, entry] : store) {
    if (pred && !pred(key)) continue;
    auto parts = splitKey(key);
    if (parts.empty()) continue;
    cJSON* current = root;
    for (size_t i = 0; i < parts.size() - 1; i++) {
      cJSON* child = cJSON_GetObjectItem(current, parts[i].c_str());
      if (!child) {
        child = cJSON_CreateObject();
        cJSON_AddItemToObject(current, parts[i].c_str(), child);
      }
      current = child;
    }
    const char* leaf = parts.back().c_str();
    if (entry.type == CFG_INT)
      cJSON_AddNumberToObject(current, leaf, atoi(entry.value.c_str()));
    else
      cJSON_AddStringToObject(current, leaf, entry.value.c_str());
  }
  return root;
}

static cJSON* buildKeyJson(const char* key, const CfgEntry& entry) {
  auto parts = splitKey(key);
  if (parts.empty()) return nullptr;
  cJSON* root = cJSON_CreateObject();
  cJSON* current = root;
  for (size_t i = 0; i < parts.size() - 1; i++) {
    cJSON* child = cJSON_CreateObject();
    cJSON_AddItemToObject(current, parts[i].c_str(), child);
    current = child;
  }
  const char* leaf = parts.back().c_str();
  if (entry.type == CFG_INT)
    cJSON_AddNumberToObject(current, leaf, atoi(entry.value.c_str()));
  else
    cJSON_AddStringToObject(current, leaf, entry.value.c_str());
  return root;
}

/* readFileLocal: only called from storageLoad() which runs on main's DRAM stack at boot */
static char* readFileLocal(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) return nullptr;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz <= 0) { fclose(f); return nullptr; }
  char* buf = (char*)malloc(sz + 1);
  if (!buf) { fclose(f); return nullptr; }
  fread(buf, 1, sz, f);
  buf[sz] = '\0';
  fclose(f);
  return buf;
}

static bool loadJsonFile(const char* path) {
  char* text = readFileLocal(path);
  if (!text) return false;
  cJSON* root = cJSON_Parse(text);
  free(text);
  if (!root) return false;
  flattenJson(root, "");
  cJSON_Delete(root);
  return true;
}

/* ---- Settings file write ---- */

static bool isSaved(const std::string& key) {
  return key.size() > 2 && key[0] == 's' && key[1] == '.';
}

static void writeSettingsFile() {
  cJSON* root = buildNestedJson(isSaved);
  char* text = cJSON_Print(root);
  cJSON_Delete(root);
  if (!text) return;

  /* Write via proxied file I/O (fs worker's DRAM stack) */
  int f = storageFopen("/state/settings.new", "w");
  if (f >= 0) {
    storageFwrite(text, strlen(text), f);
    storageFclose(f);
    storageRename("/state/settings.new", "/state/settings.json");
  }
  cJSON_free(text);
  savePending = false;
}

static void saveTimerCb(void* arg) {
  if (!storageHandle) return;
  ipc_msg_t msg = {};
  msg.type = MSG_STORAGE_SAVE;
  ipcSendMsg(storageHandle, msg);
}

static void startSaveTimer() {
  if (!saveTimer) {
    esp_timer_create_args_t args = {};
    args.callback = saveTimerCb;
    args.name = "storage_save";
    esp_timer_create(&args, &saveTimer);
  }
  esp_timer_stop(saveTimer);
  esp_timer_start_once(saveTimer, 500000);
  savePending = true;
}

/* ---- Config change subscriptions ---- */

#define STORAGE_MAX_SUBS     96
#define STORAGE_CHANGE_PORT  42

struct storage_sub_t {
  TaskHandle_t        task;
  storage_change_cb_t cb;
  char                scope[40];
};

static storage_sub_t subs[STORAGE_MAX_SUBS];
static int           subCount = 0;

/* Payload for aux message on STORAGE_CHANGE_PORT */
struct storage_change_msg_t {
  storage_change_cb_t cb;
  char                key[36];
  char                val[24];
};

/* Aux handler installed on each subscribing task — unpacks and calls */
static void storageChangeDispatch(TaskHandle_t sender, uint16_t port,
                                  const void* data, size_t len) {
  if (len < sizeof(storage_change_msg_t)) return;
  auto* msg = (const storage_change_msg_t*)data;
  if (msg->cb) msg->cb(msg->key, msg->val);
}

void storageSubscribeChanges(const char* scope, storage_change_cb_t cb) {
  if (subCount >= STORAGE_MAX_SUBS) return;

  TaskHandle_t me = xTaskGetCurrentTaskHandle();

  /* Register aux handler on this task if first subscription */
  bool needsHandler = true;
  for (int i = 0; i < subCount; i++) {
    if (subs[i].task == me) { needsHandler = false; break; }
  }
  if (needsHandler)
    itsOnAux(storageChangeDispatch, STORAGE_CHANGE_PORT);

  auto& s = subs[subCount++];
  s.task = me;
  s.cb = cb;
  strncpy(s.scope, scope, sizeof(s.scope) - 1);
  s.scope[sizeof(s.scope) - 1] = '\0';
}

static void fireSubscriptions(const char* key, const char* val) {
  storage_change_msg_t msg = {};
  strncpy(msg.key, key, sizeof(msg.key) - 1);
  strncpy(msg.val, val, sizeof(msg.val) - 1);

  for (int i = 0; i < subCount; i++) {
    size_t scopeLen = strlen(subs[i].scope);
    if (scopeLen == 0 || strncmp(key, subs[i].scope, scopeLen) == 0) {
      msg.cb = subs[i].cb;
      itsSendAuxByHandle(subs[i].task, &msg, sizeof(msg),
                         pdMS_TO_TICKS(10), STORAGE_CHANGE_PORT);
    }
  }
}

static cfg_type_t inferType(const char* val) {
  if (!val || !*val) return CFG_STR;
  const char* p = val;
  if (*p == '-') p++;
  if (!*p) return CFG_STR;
  while (*p) {
    if (*p < '0' || *p > '9') return CFG_STR;
    p++;
  }
  return CFG_INT;
}

/* ---- Public Config API ---- */

void storageLoad() {
  loadJsonFile("/fixed/factory_state/settings.json");
  loadJsonFile("/state/settings.json");
  remove("/state/settings.new");
}

bool storageExists(const char* key) { return store.count(key) > 0; }

int storageGetInt(const char* key, int def) {
  auto it = store.find(key);
  if (it == store.end()) return def;
  return atoi(it->second.value.c_str());
}

void storageGetStr(const char* key, char* out, size_t outLen, const char* def) {
  if (outLen == 0) return;
  auto it = store.find(key);
  const char* src = (it != store.end()) ? it->second.value.c_str() : def;
  strncpy(out, src, outLen - 1);
  out[outLen - 1] = '\0';
}

void storageSet(const char* key, int val) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%d", val);
  store[key] = { CFG_INT, std::string(buf) };
  fireSubscriptions(key, buf);
  if (isSaved(key)) startSaveTimer();
}

void storageSet(const char* key, const char* val) {
  store[key] = { inferType(val), std::string(val) };
  fireSubscriptions(key, val);
  if (isSaved(key)) startSaveTimer();
}

void storageSetQuiet(const char* key, int val) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%d", val);
  store[key] = { CFG_INT, std::string(buf) };
  fireSubscriptions(key, buf);
}

void storageSetQuiet(const char* key, const char* val) {
  store[key] = { inferType(val), std::string(val) };
  fireSubscriptions(key, val);
}

void storageUnset(const char* key) {
  store.erase(key);
  fireSubscriptions(key, "");
  if (isSaved(key)) startSaveTimer();
}

void storageSave() {
  if (saveTimer) esp_timer_stop(saveTimer);
  writeSettingsFile();
}

cfg_type_t storageGetType(const char* key) {
  auto it = store.find(key);
  if (it == store.end()) return CFG_STR;
  return it->second.type;
}

void storageForEach(const char* prefix, void (*cb)(const char* key, const char* val)) {
  size_t plen = strlen(prefix);
  for (auto it = store.lower_bound(prefix); it != store.end(); ++it) {
    if (strncmp(it->first.c_str(), prefix, plen) != 0) break;
    cb(it->first.c_str(), it->second.value.c_str());
  }
}

void storageList(cli_write_fn write) {
  char buf[192];
  for (auto& [key, entry] : store) {
    int n = snprintf(buf, sizeof(buf), "  %s = %s\n", key.c_str(), entry.value.c_str());
    if (n > 0) write(buf, (size_t)n);
  }
  if (store.empty()) write("(empty)\n", 8);
}

/* ---- Public File I/O API ---- */

int storageFopen(const char* path, const char* mode) {
  int slot = allocSlot();
  if (slot < 0) return -1;
  storage_file_op_t req = {};
  req.op = storage_file_op_t::OPEN;
  req.path = path;
  req.path2 = mode;
  req.slot = slot;
  if (fsOp(req) < 0) { fileActive[slot] = false; return -1; }
  return slot;
}

size_t storageFread(void* buf, size_t maxLen, int f) {
  if (f < 0 || f >= MAX_FILE_SLOTS || !fileActive[f]) return 0;
  storage_file_op_t req = {};
  req.op = storage_file_op_t::READ;
  req.slot = f;
  req.buf = buf;
  req.len = maxLen;
  int r = fsOp(req);
  return r > 0 ? (size_t)r : 0;
}

size_t storageFwrite(const void* buf, size_t len, int f) {
  if (f < 0 || f >= MAX_FILE_SLOTS || !fileActive[f]) return 0;
  storage_file_op_t req = {};
  req.op = storage_file_op_t::WRITE;
  req.slot = f;
  req.buf = (void*)buf;
  req.len = len;
  int r = fsOp(req);
  return r > 0 ? (size_t)r : 0;
}

long storageFtell(int f) {
  if (f < 0 || f >= MAX_FILE_SLOTS || !fileActive[f]) return -1;
  storage_file_op_t req = {};
  req.op = storage_file_op_t::TELL;
  req.slot = f;
  return (long)fsOp(req);
}

void storageFclose(int f) {
  if (f < 0 || f >= MAX_FILE_SLOTS || !fileActive[f]) return;
  storage_file_op_t req = {};
  req.op = storage_file_op_t::CLOSE;
  req.slot = f;
  fsOp(req);
}

int storageStat(const char* path, struct stat* st) {
  storage_file_op_t req = {};
  req.op = storage_file_op_t::STAT;
  req.path = path;
  req.st = st;
  return fsOp(req);
}

int storageRename(const char* oldPath, const char* newPath) {
  storage_file_op_t req = {};
  req.op = storage_file_op_t::RENAME;
  req.path = oldPath;
  req.path2 = newPath;
  return fsOp(req);
}

int storageRemove(const char* path) {
  storage_file_op_t req = {};
  req.op = storage_file_op_t::REMOVE;
  req.path = path;
  return fsOp(req);
}

/* ---- CLI commands ---- */

static void cmdSet(const char* a) {
    if (strcmp(a, "help") == 0) { cliPrintf("  %-*s set config variable\n", CLI_HELP_COL, "set <key>=<value>"); return; }
    const char* eq = strchr(a, '=');
    if (!eq || eq == a) { cliPrintf("usage: set <key>=<value>\n"); return; }
    char key[48];
    size_t klen = eq - a;
    while (klen > 0 && a[klen - 1] == ' ') klen--;
    if (klen == 0 || klen >= sizeof(key)) { cliPrintf("err: key empty or too long\n"); return; }
    memcpy(key, a, klen); key[klen] = '\0';
    const char* val = eq + 1;
    while (*val == ' ') val++;
    storageSet(key, val);
    if (strncmp(key, "s.log", 5) == 0)
        logApplyLevels();
}

static void cmdUnset(const char* a) {
    if (strcmp(a, "help") == 0) { cliPrintf("  %-*s delete config variable\n", CLI_HELP_COL, "unset <key>"); return; }
    if (!*a) { cliPrintf("usage: unset <key>\n"); return; }
    storageUnset(a);
}

static void cmdShow(const char* a) {
    if (strcmp(a, "help") == 0) { cliPrintf("  %-*s show config variables\n", CLI_HELP_COL, "show [<prefix>]"); return; }
    if (*a) {
        bool found = false;
        for (auto& [key, entry] : store) {
            if (strncmp(key.c_str(), a, strlen(a)) == 0) {
                cliPrintf("  %s = %s\n", key.c_str(), entry.value.c_str());
                found = true;
            }
        }
        if (!found) cliPrintf("  (no matches)\n");
    } else {
        storageList([](const char* d, size_t l) { cliPrintf("%.*s", (int)l, d); });
    }
}

static void cmdSave(const char* a) {
    if (strcmp(a, "help") == 0) { cliPrintf("  %-*s write settings to flash now\n", CLI_HELP_COL, "save"); return; }
    storageSave();
}

void storageRegisterCmds() {
    cliRegisterCmd("set", cmdSet);
    cliRegisterCmd("unset", cmdUnset);
    cliRegisterCmd("show", cmdShow);
    cliRegisterCmd("save", cmdSave);
}

/* ---- Config WebSocket handling ---- */

static void wsSendFullDump() {
    if (wsHandle < 0) return;
    cJSON* root = buildNestedJson(nullptr);
    char* text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) return;
    wsSendText(wsHandle, text, strlen(text));
    cJSON_free(text);
}

static void wsSendKeyChange(const char* key) {
    if (wsHandle < 0) return;
    auto it = store.find(key);
    if (it == store.end()) return;
    cJSON* json = buildKeyJson(key, it->second);
    if (!json) return;
    char* text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!text) return;
    wsSendText(wsHandle, text, strlen(text));
    cJSON_free(text);
}

static void mergeJsonIntoStore(cJSON* obj, const std::string& prefix) {
  cJSON* item;
  cJSON_ArrayForEach(item, obj) {
    std::string key = prefix.empty() ? item->string : prefix + "." + item->string;
    if (cJSON_IsObject(item)) {
      mergeJsonIntoStore(item, key);
    } else if (cJSON_IsNumber(item)) {
      storageSet(key.c_str(), item->valueint);
    } else if (cJSON_IsString(item)) {
      storageSet(key.c_str(), item->valuestring);
    }
  }
}

static void wsHandleMessage(const std::string& text) {
    if (text == "{\"ping\":1}") {
        if (wsHandle >= 0)
            wsSendText(wsHandle, "{\"pong\":1}", 10);
        return;
    }
    if (text == "{\"save\":1}") {
        storageSave();
        return;
    }
    cJSON* root = cJSON_Parse(text.c_str());
    if (!root) return;
    mergeJsonIntoStore(root, "");
    cJSON_Delete(root);
}

static void wsPollConfig() {
    if (wsHandle < 0) return;
    uint8_t buf[1024];
    size_t outLen = 0;
    int op = wsReadFrame(wsHandle, buf, sizeof(buf), &outLen);
    if (op == 1) {
        std::string payload((char*)buf, outLen);
        wsHandleMessage(payload);
    } else if (op < 0) {
        wsHandle = -1;
    }
}

/* ---- ITS server callbacks ---- */

static bool storageItsConnect(int handle, int itsPort, const void* data, size_t len) {
    if (len < sizeof(net_connect_t)) return false;
    auto* cd = (const net_connect_t*)data;
    if (!cd->ws) return false;

    if (wsHandle >= 0) {
        wsSendClose(wsHandle);
        itsServerKick(wsHandle);
    }
    wsHandle = handle;

    if (!wsUpgrade(handle)) {
        wsHandle = -1;
        return false;
    }
    wsSendFullDump();
    return true;
}

static void storageItsDisconnect(int handle) {
    if (handle == wsHandle) wsHandle = -1;
}

/* ---- Task function ---- */

static void storageTaskFn(void* arg) {
    itsServerInit(1, 2048, 4096);
    itsServerOnConnect(storageItsConnect);
    itsServerOnDisconnect(storageItsDisconnect);

    /* Subscribe to all config changes for browser WS sync */
    storageSubscribeChanges("", ON_CHANGE {
        wsSendKeyChange(key);
    });

    web_path_msg_t reg = {};
    reg.itsPort = 0;
    reg.path[0] = '\0';
    while (!itsSendAux("web", &reg, sizeof(reg), pdMS_TO_TICKS(500)))
        vTaskDelay(pdMS_TO_TICKS(100));

    info("ready\n");

    for (;;) {
        while (itsPoll()) {}

        ipc_msg_t msg;
        while (ipcReceive(&msg, 0)) {
            if (msg.type == MSG_NETWORK_DOWN) {
                if (wsHandle >= 0) {
                    itsServerKick(wsHandle);
                    wsHandle = -1;
                }
            }
            if (msg.type == MSG_STORAGE_SAVE)
                writeSettingsFile();
        }

        wsPollConfig();
        ulTaskNotifyTake(pdTRUE, wsHandle >= 0 ? pdMS_TO_TICKS(10) : pdMS_TO_TICKS(200));
    }
}

void storageInit() {
    /* fs worker: small DRAM-stack task for LittleFS file I/O */
    fsQueue = xQueueCreate(4, sizeof(storage_file_op_t*));
    xTaskCreatePinnedToCore(fsWorkerFn, "fs", 3072, nullptr, 1, nullptr, 1);

    /* storage task: PSRAM stack (config WS + IPC, no direct file I/O) */
    xTaskCreatePinnedToCoreWithCaps(storageTaskFn, "storage", 4096, NULL, 1, &storageHandle, 1, MALLOC_CAP_SPIRAM);
    QueueHandle_t q = xQueueCreate(16, sizeof(ipc_msg_t));
    ipcRegister(storageHandle, q, "storage");
}
