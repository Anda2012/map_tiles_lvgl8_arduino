#pragma once
/* FreeRTOS headers, as spelled on the supported cores. Stock FreeRTOS, arduino-pico
 * and most others use the plain names; STM32duino wants "STM32FreeRTOS.h" instead
 * (define MAP_TILES_RTOS_HEADER to override). */
#if defined(MAP_TILES_RTOS_HEADER)
#include MAP_TILES_RTOS_HEADER
#elif defined(ESP_PLATFORM) || defined(ARDUINO_ARCH_ESP32)
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#else
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#endif
