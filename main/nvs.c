#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include <string.h>
#include <sys/param.h>
#include <stdlib.h>
#include <stdio.h>
#include "nvs_flash.h"
#include "stdbool.h"
#include <lwip/netdb.h>
#include "freertos/semphr.h"
#include "app_config.h"
#include "nvs.h"
#include "manifest.h"
#include "sensor_context.h"

extern stream_data* sensor_data;

// stores the ip address (found from nvs) of the node in ip_addr. If not found in NVS, it stores DEFAULT_IP_ADDR
// Load a persistent static IP from NVS. On first boot, write the default node
// IP so future boots reuse a stable address.
void load_ip(void) {
	nvs_handle_t nvs_handle; 
	esp_err_t err;
	ESP_LOGI(S3_TAG, "Opening NVS partition ...");
	err = nvs_open("netcfg", NVS_READWRITE, &nvs_handle); 
	if (err != ESP_OK) {
		ESP_LOGE(S3_TAG, "Could not open NVS partition");
		return;
	} else {
		size_t ip_addr_len = sizeof(ip_addr); 
		err = nvs_get_str(nvs_handle, "ip_addr", ip_addr, &ip_addr_len); 
		if (err == ESP_OK) {
			ESP_LOGI(S3_TAG, "Retrieved ip address %s", ip_addr); 
			nvs_close(nvs_handle);
		} else {
			ESP_LOGI(S3_TAG, "IP Address not found, writing %s", ip_addr);
			err = nvs_set_str(nvs_handle, "ip_addr", ip_addr);
			ESP_ERROR_CHECK(err);
			ESP_LOGI(S3_TAG, "Successfully wrote %s to ip_addr", ip_addr); 
			nvs_close(nvs_handle);
		}
		snprintf(sensor_data->ip, sizeof(sensor_data->ip), "%s", ip_addr);
	}
}

void load_chip(void) {
	nvs_handle_t nvs_handle; 
	esp_err_t err;
	const char chip[] = "esp32s3"; 
	char chip_name[16];
	size_t chip_len = sizeof(chip_name); 
	ESP_LOGI(S3_TAG, "Opening NVS partition ...");
	err = nvs_open("netcfg", NVS_READWRITE, &nvs_handle); 
	if (err != ESP_OK) {
		ESP_LOGE(S3_TAG, "Could not open NVS partition");
		return;
	} else {
		err = nvs_get_str(nvs_handle, "chip", chip_name, &chip_len); 
		if (err == ESP_OK) {
			ESP_LOGI(S3_TAG, "Retrieved chip name %s", chip_name); 
			nvs_close(nvs_handle);
			snprintf(sensor_data->chip, sizeof(sensor_data->chip), "%s", chip_name);
		} else {
			ESP_LOGI(S3_TAG, "Chip name not found, writing %s", chip);
			err = nvs_set_str(nvs_handle, "chip", chip);
			ESP_ERROR_CHECK(err);
			ESP_LOGI(S3_TAG, "Successfully wrote %s to chip", chip); 
			nvs_close(nvs_handle);
			snprintf(sensor_data->chip, sizeof(sensor_data->chip), "%s", chip);
		}
	}
} 

void nvs_init() {
    esp_err_t err = nvs_flash_init(); 
	if (err != ESP_OK) {
		ESP_LOGE(S3_TAG, "Could not initialize NVS partition");
	} else {
		load_ip(); 
		load_chip();
	}
}