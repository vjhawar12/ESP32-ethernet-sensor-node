#ifndef SENSORS_H
#define SENSORS_H

#include "stdint.h"
#include "driver/i2c_master.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"


uint64_t read_aht20(i2c_master_dev_handle_t aht20_handle);
int read_hcsr505(i2c_master_dev_handle_t hcsr505_handle);

// calculate the ppm from the voltage
// returns V
float read_mq135(adc_oneshot_unit_handle_t adc_oneshot_handle, adc_cali_handle_t adc_cali_handle);


// Periodic application task that will eventually gather all sensor values and
// refresh the outgoing telemetry JSON. Right now the payload structure and
// synchronization are in place even though sensor collection is still being
// expanded.
void measure_sensor_values(void* pv_params);
void motion_detected_handler(void* pvParams);

#endif