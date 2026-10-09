#pragma once
#include "FreeRTOS.h"
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void){ SemaphoreHandle_t s=malloc(sizeof *s); pthread_mutex_init(&s->m,NULL); return s; }
static inline int xSemaphoreTake(SemaphoreHandle_t s, TickType_t t){ (void)t; pthread_mutex_lock(&s->m); return pdTRUE; }
static inline int xSemaphoreGive(SemaphoreHandle_t s){ pthread_mutex_unlock(&s->m); return pdTRUE; }
static inline void vSemaphoreDelete(SemaphoreHandle_t s){ pthread_mutex_destroy(&s->m); free(s); }
