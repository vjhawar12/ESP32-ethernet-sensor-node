#include "s3.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_http_client.h"
#include <string.h>
#include <sys/param.h>
#include <stdio.h>
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "stdbool.h"
#include <lwip/netdb.h>
#include "cJSON.h"
#include "freertos/semphr.h"
#include "app_config.h"
#include "ota.h"
#include "manifest.h"


extern const char server_cert_pem_start[] asm("_binary_cert_pem_start");

// Compare semantic versions in MAJOR.MINOR.PATCH form. Returns non-zero when
// the current running image is older than the manifest version.
int less_than(const char *current, const char *latest)
{
    int c_major, c_minor, c_patch;
    int l_major, l_minor, l_patch;
    sscanf(current, "%d.%d.%d", &c_major, &c_minor, &c_patch);
    sscanf(latest,  "%d.%d.%d", &l_major, &l_minor, &l_patch);
    if (c_major != l_major)
        return c_major < l_major;
    if (c_minor != l_minor)
        return c_minor < l_minor;
    return c_patch < l_patch;
}

// Parse the manifest JSON, capture useful metadata, and trigger OTA when a
// newer firmware image is available.
void parse_manifest(bool can_trigger_ota) {
	ESP_LOGI(TAG, "Raw manifest response: %s", response_buffer);
	cJSON *json = cJSON_Parse(response_buffer);
	if (json == NULL) {
		ESP_LOGI(TAG, "NULL json\n");
		return;
	}
	cJSON* flash_size_MB = cJSON_GetObjectItemCaseSensitive(json, "flash_size_MB");
	if (cJSON_IsString(flash_size_MB) && flash_size_MB->valuestring != NULL) {
		strncpy(flash_size_MB_str, flash_size_MB->valuestring, sizeof(flash_size_MB_str) - 1);
		flash_size_MB_str[sizeof(flash_size_MB_str) - 1] = 0;
	}
	cJSON* target = cJSON_GetObjectItemCaseSensitive(json, "target");
	if (cJSON_IsString(target) && target->valuestring != NULL) {
		strncpy(target_str, target->valuestring, sizeof(target_str) - 1);
		target_str[sizeof(target_str) - 1] = 0;
	}
	cJSON* commit_version = cJSON_GetObjectItemCaseSensitive(json, "commit_version");
	if (cJSON_IsString(commit_version) && commit_version->valuestring != NULL) {
		strncpy(commit_version_str, commit_version->valuestring, sizeof(commit_version_str) - 1);
		commit_version_str[sizeof(commit_version_str) - 1] = 0;
	}
	cJSON* server_version = cJSON_GetObjectItemCaseSensitive(json, "version");
    if (cJSON_IsString(server_version) && (server_version->valuestring != NULL)) {
		strncpy(server_version_str, server_version->valuestring, sizeof(server_version_str) - 1);
		server_version_str[sizeof(server_version_str) - 1] = 0;
        ESP_LOGI(TAG, "Server Version: %s\n", server_version->valuestring);
		cJSON *firmware_url = cJSON_GetObjectItemCaseSensitive(json, "firmware_url");
		if (cJSON_IsString(firmware_url) && (firmware_url->valuestring != NULL)) {
			strncpy(firmware_url_str, firmware_url->valuestring, sizeof(firmware_url_str) - 1);
			firmware_url_str[sizeof(firmware_url_str) - 1] = 0;
			ESP_LOGI(TAG, "Firmware URL: %s\n", firmware_url->valuestring);
		} else {
			ESP_LOGI(TAG, "Missing firmware URL\n");
			cJSON_Delete(json);
			return;
		}
		ESP_LOGI(TAG, "Current app version: %s", esp_app_get_description()->version);
		ESP_LOGI(TAG, "Manifest version: %s", server_version->valuestring);
		if (!can_trigger_ota) {
			cJSON_Delete(json);
			return;
		}
		// comparing current version (app_desc->version) with latest version (version->valuestring)
		if (less_than(esp_app_get_description()->version, server_version->valuestring)) {
			// trigger OTA
				 esp_http_client_config_t http_config = {
					.url = firmware_url->valuestring,
					.cert_pem = server_cert_pem_start,
					.keep_alive_enable = true,
				};
				esp_https_ota_config_t ota_config = {
					.http_config = &http_config,
				};
				ESP_LOGI(TAG, "Newer firmware detected, starting OTA...");
				ESP_LOGI(TAG, "OTA URL: %s", firmware_url->valuestring);
				esp_err_t err = esp_https_ota(&ota_config);
				if (err == ESP_OK) {
					ESP_LOGI(TAG, "OTA triggered succesfully! Flashing new code now ...");
					esp_restart();
				} else {
					ESP_LOGI(TAG, "Raw manifest response: %s", response_buffer);
					ESP_LOGI(TAG, "Current app version: %s", esp_app_get_description()->version);
					ESP_LOGI(TAG, "Manifest version: %s", server_version->valuestring);
					ESP_LOGI(TAG, "OTA URL: %s", firmware_url->valuestring);
					ESP_LOGE(TAG, "OTA Failed! %s (0x%x)", esp_err_to_name(err), err);
				}
		} else {
			ESP_LOGI(TAG, "Already up-to-date with firmware"); 
		}	
    } else {
		ESP_LOGI(TAG, "Invalid JSON \n");
	}
	cJSON_Delete(json);
}

// If we successfully booted an OTA slot, mark it valid so ESP-IDF cancels any
// pending rollback to the previous image.
void validate_ota(void) {
	const esp_partition_t *running = esp_ota_get_running_partition();
	if (running != NULL) {
		if (running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 || running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1) {
			esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
			if (err == ESP_OK) {
				ESP_LOGI(S3_TAG, "Marked OTA app valid");
			} else {
				ESP_LOGW(S3_TAG, "Failed to mark OTA app valid: %s", esp_err_to_name(err));
			}
		} else {
			ESP_LOGI(S3_TAG, "Running from factory partition, not marking OTA valid");
		}
	}	
}