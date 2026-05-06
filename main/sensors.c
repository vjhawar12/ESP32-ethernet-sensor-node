#include "freertos/FreeRTOS.h"
#include "esp_err.h"
#include "esp_log.h"
#include "sensors.h"
#include "app_config.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "sensor_context.h"
#include "rtos_objects.h"

uint64_t read_aht20(i2c_master_dev_handle_t aht20_handle) {
    uint8_t success;
	uint8_t measure[3] = {0xAC, 0x33, 0x00}; 
	esp_err_t err; 
	err = i2c_master_transmit(aht20_handle, measure, 3, -1);
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "Error on calibration!");
		return 0x0;
	}
	vTaskDelay(pdMS_TO_TICKS(80));
	uint8_t init = 0x71;
	err = i2c_master_transmit_receive(aht20_handle, &init, 1, &success, 1, -1);
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "Error measuring temperature/humidity!");
		return 0x0;
	}
	if (!(success & (1 << 7))) {
		// measurement completed
		uint8_t data[6];
		err = i2c_master_receive(aht20_handle, data, 6, -1);
		if (err != ESP_OK) {
			ESP_LOGW(TAG, "Error measuring temperature/humidity!");
			return 0x0;
		}
		// data[0] = status
		// data[1], data[2], upper 4 bits of data[3] = humidity
		// lower 4 bits of data[3], data[4], data[5] = temperature
		// remaining: CRC
		uint32_t humidity = ((uint32_t)data[1] << 12) | ((uint32_t)data[2] << 4) | ((uint32_t)(data[3] >> 4) & 0x0F);
		uint32_t temperature = ((uint32_t)(data[3] & 0x0F) << 16) | (uint32_t)data[4] << 8 | (uint32_t)data[5];
		return ((uint64_t)humidity << 32) | (uint64_t)temperature;
	}
	return 0x0;
}

int read_hcsr505(i2c_master_dev_handle_t hcsr505_handle) {
	uint8_t init[2] = {0xFF, 0xFF};  
	uint8_t raw_data[2];
	i2c_master_transmit(hcsr505_handle, init, 2, -1);
	i2c_master_receive(hcsr505_handle, raw_data, 2, -1);
	uint16_t data = ((uint16_t)(raw_data[1] << 8)) | raw_data[0];
	return (data & (uint16_t)(1 << HCSR505_GPIO)) != 0;
}

// calculate the ppm from the voltage
// returns V
float read_mq135(adc_oneshot_unit_handle_t adc_oneshot_handle, adc_cali_handle_t adc_cali_handle) {
	int adc_raw, adc_cali; 
	ESP_ERROR_CHECK(adc_oneshot_read(adc_oneshot_handle, ADC_CHANNEL_1, &adc_raw));
	ESP_ERROR_CHECK(adc_cali_raw_to_voltage(adc_cali_handle, adc_raw, &adc_cali));
	return adc_cali / 1000.0f;
}


void motion_detected_handler(void* pvParams) {
	// log the date and time here too
	ESP_LOGW(TAG, "Motion detected!");
}



// Periodic application task that will eventually gather all sensor values and
// refresh the outgoing telemetry JSON. Right now the payload structure and
// synchronization are in place even though sensor collection is still being
// expanded.
void measure_sensor_values(void* pv_params) {
	while (1) {
		xEventGroupWaitBits(
			collect_group,
			MEASURE_ALL_BIT,
			pdTRUE, 
			pdTRUE,
			portMAX_DELAY
		);
		// AHT20
		uint64_t data = read_aht20(aht20_handle);
		if (data == 0) continue;
		uint32_t humidity = (uint32_t)((data & 0xFFFFFFFF00000000) >> 32);
		uint32_t temperature = (uint32_t)(data & 0x00000000FFFFFFFF);
		float humidity_fl = 100.0f * humidity / (1048576.0f);
		float temp_celcius_fl = (200.0f * temperature / (1048576.0f)) - 50.0f;
		// MQ135
		float air_quality = read_mq135(adc_oneshot_handle, adc_cali_handle);	
		 // HCSR505
		int motion_detected = read_hcsr505(hcsr505_handle);
		if (mutex && xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) {
			snprintf(sensor_data->temperature, sizeof(sensor_data->temperature), "%f", temp_celcius_fl);
			snprintf(sensor_data->humidity, sizeof(sensor_data->humidity), "%f", humidity_fl);
			snprintf(sensor_data->air_quality, sizeof(sensor_data->air_quality), "%f", air_quality); 
			sensor_data->motion_detected = motion_detected;
			xSemaphoreGive(mutex);
		}
	}	
}

