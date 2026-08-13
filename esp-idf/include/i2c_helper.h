/**
 * i2c_helper — shared I2C bus policy for the platform.
 *
 * An I2C bus is a pair of wires with several chips hanging off it: on one
 * board the touch controller, the keyboard and the RTC; on another the PMU;
 * on a third an OLED. Each driver opens its own `i2c_master_bus_config_t`,
 * but the electrical properties of the lines are the board's, not any one
 * driver's, so the settings that describe the wiring belong in one place
 * where every bus creator reads the same answer.
 *
 * Use it when filling a bus config:
 *
 *     i2c_master_bus_config_t bus = {};
 *     bus.sda_io_num = ...;
 *     bus.scl_io_num = ...;
 *     bus.flags.enable_internal_pullup = SPANGAP_I2C_PULLUP;
 *     i2c_new_master_bus(&bus, &handle);
 *
 * This file does not own bus creation — each driver still calls
 * `i2c_new_master_bus` and `i2c_master_bus_add_device` itself.
 */
#pragma once

#include "sdkconfig.h"

/** Whether to ask the SoC to pull SDA/SCL up, from
 *  CONFIG_SPANGAP_I2C_INTERNAL_PULLUP. A Kconfig bool is undefined rather
 *  than 0 when unset, so it is resolved to a usable value here instead of at
 *  each bus. See the symbol's help text for which way a board wants it. */
#ifdef CONFIG_SPANGAP_I2C_INTERNAL_PULLUP
#define SPANGAP_I2C_PULLUP true
#else
#define SPANGAP_I2C_PULLUP false
#endif
