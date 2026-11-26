/*
 * File    : board.c
 * Purpose : Board-specific GPIO and LED control implementation.
 * Does    : Status indicator LED initialization and state-tracked GPIO control.
 * Related : board.h
 */

/*
 * SPDX-FileCopyrightText: 2017 Intel Corporation
 * SPDX-FileContributor: 2018-2021 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "board.h"

#define TAG "BOARD"

struct _led_state led_state[3] = {
    { LED_OFF, LED_OFF, LED_R, "red"   },
    { LED_OFF, LED_OFF, LED_G, "green" },
    { LED_OFF, LED_OFF, LED_B, "blue"  },
};

/*
 * Purpose : Finds the LED matching pin and writes to GPIO only if state has changed.
 * Called  : mesh_init() (LED_G ON) and prov_complete() (LED_G OFF).
 */
void board_led_operation(uint8_t pin, uint8_t onoff)
{
    for (int i = 0; i < 3; i++) {
        if (led_state[i].pin != pin) {
            continue;
        }
        if (onoff == led_state[i].previous) {
            ESP_LOGW(TAG, "led %s is already %s",
                     led_state[i].name, (onoff ? "on" : "off"));
            return;
        }
        gpio_set_level(pin, onoff);
        led_state[i].previous = onoff;
        return;
    }
    ESP_LOGE(TAG, "LED is not found!");
}

/*
 * Purpose : Resets Red, Green, and Blue pins to output mode and drives them LOW.
 * Called  : board_init() during startup.
 */
static void board_led_init(void)
{
    for (int i = 0; i < 3; i++) {
        gpio_reset_pin(led_state[i].pin);
        gpio_set_direction(led_state[i].pin, GPIO_MODE_OUTPUT);
        gpio_set_level(led_state[i].pin, LED_OFF);
        led_state[i].previous = LED_OFF;
    }
}

/*
 * Purpose : Entry point for board peripheral initialization.
 * Called  : app_main() Step 2.
 */
void board_init(void)
{
    board_led_init();
}
