#pragma once
#include "FreeRTOS.h"
typedef void (*TaskFunction_t)(void*);
typedef struct { TaskFunction_t fn; void *arg; } shim_task_arg_t;
static void *shim_tramp(void *a){ shim_task_arg_t t=*(shim_task_arg_t*)a; free(a); t.fn(t.arg); return NULL; }
static inline int xTaskCreate(TaskFunction_t fn,const char*n,uint32_t st,void*arg,UBaseType_t p,TaskHandle_t*h){ (void)n;(void)st;(void)p; pthread_t *th=malloc(sizeof *th); if(h)*h=th; shim_task_arg_t *a=malloc(sizeof *a); a->fn=fn;a->arg=arg; pthread_create(th,NULL,shim_tramp,a); pthread_detach(*th); return pdPASS; }
static inline void vTaskDelete(TaskHandle_t h){ if(h==NULL) pthread_exit(NULL); else pthread_cancel(*h); }
static inline void vTaskDelay(TickType_t ms){ usleep(ms*1000); }
