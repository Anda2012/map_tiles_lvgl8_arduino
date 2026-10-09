#pragma once
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
typedef uint32_t TickType_t; typedef int BaseType_t; typedef uint32_t UBaseType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY 0xFFFFFFFFu
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
typedef struct { pthread_mutex_t m; } shim_mutex_t;
typedef shim_mutex_t *SemaphoreHandle_t;
typedef struct { pthread_mutex_t m; pthread_cond_t c; size_t item, len, count, head; uint8_t *buf; } shim_queue_t;
typedef shim_queue_t *QueueHandle_t;
typedef pthread_t *TaskHandle_t;
