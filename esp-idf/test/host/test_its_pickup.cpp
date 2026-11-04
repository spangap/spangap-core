/* Host test for ITS_WAIT_PICKUP: a pickup send whose wait timed out is
 * dispatched later, and the give it leaves must not end the same task's next
 * pickup wait before that next send has been dispatched. Runs its.cpp on the
 * ESP-IDF FreeRTOS Linux port, linked against the component archives of a
 * linux-target build tree (BUILD) of any app that includes spangap-core.
 *
 *   BUILD=<linux build dir> sh test_its_pickup.sh [its.cpp]
 *
 * The build directory must match the host architecture, e.g.
 * reticulous/esp-idf/build.dev.hw-sim-mesh-aarch64 on an aarch64 host.
 */
#include "its.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static const uint16_t PORT = 7;
static const TickType_t SLOW_TICKS = 50;      /* receiver's handling time for a slow message */
static const TickType_t SHORT_TIMEOUT = 10;   /* sender's wait on the slow message */
static const TickType_t LONG_TIMEOUT = 500;   /* sender's wait on the follow-up */

static TaskHandle_t receiver;
static TaskHandle_t mainTask;
static volatile int handled[4];               /* per message id: dispatch finished */

/* Message body: one byte id; id 0 and 2 are slow. */
static void onAux(TaskHandle_t, const void* data, size_t len) {
  if (len < 1) return;
  uint8_t id = *(const uint8_t*)data;
  if (id == 0 || id == 2) vTaskDelay(SLOW_TICKS);
  if (id < 4) handled[id] = 1;
}

static void receiverTask(void*) {
  itsServerInit();
  itsOnAux(PORT, onAux);
  xTaskNotifyGive(mainTask);
  for (;;) itsPoll(portMAX_DELAY);
}

/* Runs at a higher priority than the receiver, so the receiver's give after
 * dispatching the slow message wakes it at once, mid-wait on the follow-up. */
static void senderTask(void*) {
  uint8_t id = 0;
  /* Round 1: copy send times out, copy follow-up. */
  bool okA = itsSendAuxByTaskHandle(receiver, PORT, &id, 1, SHORT_TIMEOUT, ITS_WAIT_PICKUP);
  CHECK(!okA);
  CHECK(!handled[0]);
  id = 1;
  bool okB = itsSendAuxByTaskHandle(receiver, PORT, &id, 1, LONG_TIMEOUT, ITS_WAIT_PICKUP);
  CHECK(okB);
  CHECK(handled[0]);
  CHECK(handled[1]);               /* returned only after its own dispatch */

  /* Round 2: copy send times out, owned follow-up. Let round 1 settle first
   * so a failure there does not cascade into this round. */
  vTaskDelay(2 * SLOW_TICKS);
  CHECK(handled[1]);
  id = 2;
  okA = itsSendAuxByTaskHandle(receiver, PORT, &id, 1, SHORT_TIMEOUT, ITS_WAIT_PICKUP);
  CHECK(!okA);
  CHECK(!handled[2]);
  uint8_t* own = (uint8_t*)malloc(1);
  *own = 3;
  okB = itsSendAuxOwnedByTaskHandle(receiver, PORT, own, 1, LONG_TIMEOUT, ITS_WAIT_PICKUP);
  CHECK(okB);
  CHECK(handled[2]);
  CHECK(handled[3]);

  std::printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
  std::fflush(stdout);
  std::exit(failures ? 1 : 0);
}

extern "C" void app_main(void) {
  mainTask = xTaskGetCurrentTaskHandle();
  xTaskCreate(receiverTask, "recv", 8192, nullptr, 5, &receiver);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  xTaskCreate(senderTask, "send", 8192, nullptr, 6, nullptr);
  vTaskDelay(portMAX_DELAY);
}
