#ifndef RTOS_OBJECTS_H
#define RTOS_OBJECTS_H

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

extern EventGroupHandle_t log_group;
extern EventGroupHandle_t main_group;
extern EventGroupHandle_t collect_group; 
extern SemaphoreHandle_t mutex; 

#endif
