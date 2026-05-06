#ifndef SENSOR_CONTEXT_H
#define SENSOR_CONTEXT_H

#include "lwip/sockets.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/i2c_master.h"
#include "freertos/semphr.h"

typedef struct stream_data {
	char chip[16];
	char firmvers[16];
	char ip[16];
	char temperature[16];
	char humidity[16];
	char air_quality[16];
	bool motion_detected;
} stream_data; 

// Transport-side context for the UDP streaming task. Bundles the latest
// payload string, destination address, and socket handle.
typedef struct stream_payload {
	stream_data* _stream_data;
	int sock;
	struct sockaddr_in dest_addr;
	char msg[512];
} stream_payload;


extern stream_data* sensor_data;
extern stream_payload* payload;
extern adc_oneshot_unit_handle_t adc_oneshot_handle; 
extern adc_cali_handle_t adc_cali_handle; 
extern i2c_master_dev_handle_t aht20_handle, hcsr505_handle;

#endif