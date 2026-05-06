#include "rtos_objects.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

EventGroupHandle_t log_group;
EventGroupHandle_t main_group;
EventGroupHandle_t collect_group; 
SemaphoreHandle_t mutex; 
