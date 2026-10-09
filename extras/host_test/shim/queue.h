#pragma once
#include "FreeRTOS.h"
static inline QueueHandle_t xQueueCreate(size_t len, size_t item){ QueueHandle_t q=calloc(1,sizeof *q); pthread_mutex_init(&q->m,NULL); pthread_cond_init(&q->c,NULL); q->item=item; q->len=len; q->buf=malloc(len*item); return q; }
static inline int xQueueSend(QueueHandle_t q,const void*it,TickType_t t){ (void)t; pthread_mutex_lock(&q->m); if(q->count==q->len){pthread_mutex_unlock(&q->m);return pdFALSE;} memcpy(q->buf+((q->head+q->count)%q->len)*q->item,it,q->item); q->count++; pthread_cond_signal(&q->c); pthread_mutex_unlock(&q->m); return pdTRUE; }
static inline int xQueueReceive(QueueHandle_t q,void*out,TickType_t ms){ struct timespec ts; clock_gettime(CLOCK_REALTIME,&ts); ts.tv_nsec+=(long)(ms%1000)*1000000L; ts.tv_sec+=ms/1000+ts.tv_nsec/1000000000L; ts.tv_nsec%=1000000000L; pthread_mutex_lock(&q->m); while(q->count==0){ if(pthread_cond_timedwait(&q->c,&q->m,&ts)!=0){pthread_mutex_unlock(&q->m);return pdFALSE;} } memcpy(out,q->buf+q->head*q->item,q->item); q->head=(q->head+1)%q->len; q->count--; pthread_mutex_unlock(&q->m); return pdTRUE; }
static inline void vQueueDelete(QueueHandle_t q){ free(q->buf); free(q); }
