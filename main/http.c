#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_tls.h"
#include "esp_http_client.h"
#include <string.h>
#include <sys/param.h>
#include "stdbool.h"
#include <lwip/netdb.h>
#include "freertos/semphr.h"
#include "app_config.h"
#include "http.h"
#include "manifest.h"

extern const char server_cert_pem_start[] asm("_binary_cert_pem_start");

// HTTP client callback used during manifest fetch. The response may arrive in
// multiple chunks, so data is appended incrementally into response_buffer.
esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    static int output_len;       // Stores number of bytes read
    switch(evt->event_id) {
		// error on GET request
        case HTTP_EVENT_ERROR:
            ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
            break;
		// TCP connection formed 
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
			// must reset output_len to accept new data later
			output_len = 0;
            break;
		// HTTP request sent
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
		// receiving response headers
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            break;
		// receiving response data
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            // Clean the buffer in case of a new request
            if (output_len == 0) {
                memset(response_buffer, 0, MAX_HTTP_OUTPUT_BUFFER + 1);
            }
			// if new data
			if (evt->data_len > MAX_HTTP_OUTPUT_BUFFER - output_len) {
				ESP_LOGE(TAG, "Incoming data buffer length greater than buffer size");
				manifest_overflow = 1;
				return ESP_FAIL;
			} else {
				manifest_overflow = 0;
				// copy output data into buffer
				memcpy((char *)response_buffer + output_len, evt->data, evt->data_len);
				output_len += evt->data_len;
				// dont forget to null-terminate text
            	((char *)response_buffer)[output_len] = '\0';
			}
            break;
		// response finished
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
            int mbedtls_err = 0;
            esp_err_t err = esp_tls_get_and_clear_last_error((esp_tls_error_handle_t)evt->data, &mbedtls_err, NULL);
            if (err != 0) {
                ESP_LOGI(TAG, "Last esp error code: 0x%x", err);
                ESP_LOGI(TAG, "Last mbedtls failure: 0x%x", mbedtls_err);
            }
            output_len = 0;
            break;
        default:
            break;
    }
    return ESP_OK;
}

// Fetch the OTA manifest over HTTPS. The event handler fills response_buffer,
// which is then parsed by parse_manifest().
esp_err_t http_get_request(void) {
    esp_http_client_config_t config = {
        .url = MANIFEST,	
        .method = HTTP_METHOD_GET,
		.event_handler = _http_event_handler,
		.cert_pem = server_cert_pem_start,
        .user_data = response_buffer, // Pass buffer to the event handler
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);
	if (manifest_overflow) {
		return ESP_FAIL;
	}
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "HTTP GET Status = %d, content_length = %" PRId64,
                 esp_http_client_get_status_code(client),
                 esp_http_client_get_content_length(client));
        // Response data will be in response_buffer if handled via user_data
    } else {
        ESP_LOGE(TAG, "HTTP GET request failed: %s", esp_err_to_name(err));
		return ESP_FAIL; 
    } 	
	ESP_LOGI(TAG, "Response body: \n %s", response_buffer); 
    esp_http_client_cleanup(client);
	return ESP_OK;
}