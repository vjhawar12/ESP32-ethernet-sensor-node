#include "stdbool.h"
#include "s3.h"
// #include "eth01.c"

volatile bool check_for_ota = 0;

void app_main() {   
	// change s3_ota_update to a task scheduled by FreeRTOS later
	// sensor readings and otehr tasks should also be scheduled
	s3_ota_update();
}  