#include "s3.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_eth_phy_w5500.h"
#include "esp_eth_mac_w5500.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_check.h"
#include "esp_tls.h"
#include "esp_http_client.h"
#include "sdkconfig.h"
#include "lwip/ip4_addr.h"
#include <string.h>
#include <sys/param.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include "nvs_flash.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "driver/gptimer.h"
#include "stdbool.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif
#include "cJSON.h"
#include "freertos/semphr.h"
#include "driver/i2c_master.h"
#include "driver/i2c_slave.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
// Exceeding around 1024 causes stack overflow
#define MAX_HTTP_OUTPUT_BUFFER 1024
#define ETH_CONNECTED_BIT BIT0
#define OTA_REQUESTED_BIT BIT1
#define HEARTBEAT_BIT BIT2
#define STREAM_BIT BIT3
#define STREAM_BIT_MANUAL BIT4
#define MEASURE_ALL_BIT BIT0
// NOTE: These constants are specific to the ESP32-S3-ETH boards. They use the following schematic: 
// https://files.waveshare.com/wiki/ESP32-S3-ETH/ESP32-S3-ETH-Schematic.pdf
#define S3_RESET_GPIO 9
#define S3_INT_GPIO 10
#define S3_MOSI_GPIO 11
#define S3_MISO_GPIO 12
#define S3_SCLK_GPIO 13
#define S3_CS_GPIO 14
// 300000000ULL = 5 minutes
#define HEARTBEAT_PERIOD_US 300000000ULL
// 5000000ULL = 5 seconds
#define STREAM_PERIOD_US 5000000ULL
// 1000000ULL = 1 second
#define DATA_PERIOD_US 1000000ULL
// not needed, so assigning it a default value
#define S3_ETH_PHY_ADDR 1
// conservative speed
#define S3_SPI_CLK_MHZ 20
#define S3_TAG "ESP_S3_ETH Client Instance"
#define TAG "HTTP_CLIENT_GET"
#define STATION3_IP "192.168.3.125"
// ip adddresses for secondaries
#define IP_ADDR_0 "192.168.3.200"
#define IP_ADDR_1 "192.168.3.201"
#define IP_ADDR_2 "192.168.3.202"
#define IP_ADDR_3 "192.168.3.203"
#define IP_ADDR_4 "192.168.3.204"
#define IP_ADDR_5 "192.168.3.205"
#define IP_ADDR_6 "192.168.3.206"
// this is not a real node, its a test one
#define DEFAULT_IP_ADDR IP_ADDR_6
#define MANIFEST "https://192.168.3.125:4443/s3_manifest.json"
#define SUBNET "255.255.255.0"
#define GATEWAY "192.168.3.1"
// port for udp server
#define PORT_UDP 5000
#define PORT_TCP 4000
// I2C port setup: change these later
#define MASTER_SCL_GPIO GPIO_NUM_0
#define MASTER_SDA_GPIO GPIO_NUM_1
#define AHT20_SCL_GPIO GPIO_NUM_0
#define AHT20_SDA_GPIO GPIO_NUM_1
// from AHT20 datasheet
#define AHT20_ADDR 0x38
// Grounding A0-2 gives address 0x20
#define PCF8585_ADDR 0x20
// standard 100 khz
#define SCL_FREQUENCY_HZ 100000
#define AHT20_DATA_LENGTH 128
#define HCSR505_GPIO 2
// connect INT to gpio pin 10
#define HCSR505_INTR_PIN GPIO_NUM_10

static char response_buffer[MAX_HTTP_OUTPUT_BUFFER + 1] = {0};
char server_version_str[64];
char firmware_url_str[128];
char target_str[16];
char flash_size_MB_str[16];
char commit_version_str[128];
extern const char server_cert_pem_start[] asm("_binary_cert_pem_start");
char ip_addr[16] = DEFAULT_IP_ADDR;
esp_eth_mac_t *mac = NULL;
esp_eth_phy_t *phy = NULL;
esp_netif_t *eth_netif = NULL;
esp_eth_handle_t eth_handle = NULL;
spi_device_handle_t spi_handle = NULL;
SemaphoreHandle_t mutex;
static EventGroupHandle_t log_group, main_group, collect_group; 
i2c_master_dev_handle_t aht20_handle, hcsr505_handle;
uint8_t aht20_data[AHT20_DATA_LENGTH];
uint8_t success;
adc_oneshot_unit_handle_t adc_oneshot_handle; 
adc_cali_handle_t adc_cali_handle; 
int manifest_overflow = 0;
// Snapshot of the latest application telemetry. These fields are formatted into
// a JSON payload and sent periodically over UDP.
typedef struct stream_data {
	char chip[16];
	char firmvers[16];
	char ip[16];
	char temperature[16];
	char humidity[16];
	char air_quality[16];
	bool motion_detected;
} stream_data; 
stream_data* sensor_data;
// Transport-side context for the UDP streaming task. Bundles the latest
// payload string, destination address, and socket handle.
typedef struct stream_payload {
	stream_data* _stream_data;
	int sock;
	struct sockaddr_in dest_addr;
	char msg[512];
} stream_payload;
stream_payload* payload;

// Remove CR/LF from a received console command so string comparison works
// reliably for commands typed from nc/telnet-style clients.
void strfilter(char* buffer, int length) {
	int i = 0;
	while (i < length && buffer[i] != 0) {
		if (buffer[i] == '\r' || buffer[i] == '\n') {
			buffer[i] = 0;
			break;
		}
		i++;
	}
}

// Bring up the ESP-IDF networking foundation: TCP/IP stack, default event
// loop, and an Ethernet netif object that the W5500 driver will attach to.
esp_err_t network_init(void) {
	ESP_RETURN_ON_ERROR(esp_netif_init(), S3_TAG, "esp_netif failed"); // connects TCP/IP stack to the hardware
	ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), S3_TAG, "esp_event_loop_create_default failed"); // event loop that reports link status and network changes, not packet transmission	
	esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH(); // configuration structure for Ethernet
	eth_netif = esp_netif_new(&cfg); // create network interface for Ethernet driver
	if (!eth_netif) {
		ESP_LOGE(S3_TAG, "esp_netif_new failed");
		return ESP_FAIL;
	}
	return ESP_OK;
}

// ESP32 S3 ETH uses a SPI ethernet controller not a MAC + external
// Initializing mac and phy, the ethernet driver components
esp_err_t mac_phy_init(void) {
	eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();      // apply default common MAC configuration
	eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();      // apply default PHY configuration
	phy_config.phy_addr = S3_ETH_PHY_ADDR;           // alter the PHY address according to your board design
	phy_config.reset_gpio_num = S3_RESET_GPIO; // alter the GPIO used for PHY reset
	// Install GPIO interrupt service (as the SPI-Ethernet module is interrupt-driven)
	ESP_RETURN_ON_ERROR(gpio_install_isr_service(0), S3_TAG, "gpio_install_isr_service failed");
	// SPI bus configuration
	spi_bus_config_t buscfg = {
		.miso_io_num = S3_MISO_GPIO,
		.mosi_io_num = S3_MOSI_GPIO,
		.sclk_io_num = S3_SCLK_GPIO,
		.quadwp_io_num = -1,
		.quadhd_io_num = -1,
	};
	ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO), S3_TAG, "spi_bus_initialize failed");
	// Configure SPI device
	spi_device_interface_config_t spi_devcfg = {
		.mode = 0,
		.clock_speed_hz = S3_SPI_CLK_MHZ * 1000 * 1000,
		.spics_io_num = S3_CS_GPIO,
		.queue_size = 20
	};
	eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(SPI2_HOST, &spi_devcfg);
	w5500_config.int_gpio_num = S3_INT_GPIO;
	mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
	phy = esp_eth_phy_new_w5500(&phy_config);
	if (!mac) {
		ESP_LOGE(S3_TAG, "esp_eth_mac_new_w5500 failed");
		return ESP_FAIL;
	}
	if (!phy) {
		ESP_LOGE(S3_TAG, "esp_eth_phy_new_w5500 failed");
		return ESP_FAIL;
	}
	return ESP_OK;
}  	

// driver initialization with mac and phy
// Install the ESP-IDF Ethernet driver instance backed by the initialized
// W5500 MAC/PHY objects.
esp_err_t driver_init(void) {
	esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
	ESP_RETURN_ON_ERROR(esp_eth_driver_install(&config, &eth_handle), S3_TAG, "ETH_DEFAULT_CONFIG failed");
	return ESP_OK;
}

// IP acquisition callback. Once the interface has a valid address we set an
// event bit so the rest of the application can safely start network services.
void got_ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
    const esp_netif_ip_info_t *ip_info = &event->ip_info;
    ESP_LOGI(TAG, "Ethernet Got IP Address\n");
	ESP_LOGI(TAG, "ETH IP: " IPSTR "\n", IP2STR(&ip_info->ip));
	ESP_LOGI(TAG, "ETH MASK: " IPSTR "\n", IP2STR(&ip_info->netmask));
	ESP_LOGI(TAG, "ETH GW: " IPSTR "\n", IP2STR(&ip_info->gw));
	fflush(stdout);
	xEventGroupSetBits(main_group, ETH_CONNECTED_BIT);
}

void eth_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    switch (event_id) {
        case ETHERNET_EVENT_START:
            ESP_LOGI(S3_TAG, "Ethernet Started");
            break;
        case ETHERNET_EVENT_CONNECTED:
            ESP_LOGI(S3_TAG, "Ethernet Link Up");
            break;
        case ETHERNET_EVENT_DISCONNECTED:
            ESP_LOGI(S3_TAG, "Ethernet Link Down");
            break;
        case ETHERNET_EVENT_STOP:
            ESP_LOGI(S3_TAG, "Ethernet Stopped");
            break;
        default:
            break;
    }
}

// Replace DHCP with a known static IP. This simplifies multi-node deployment
// and makes it easier for the backend/controller to address nodes directly.
esp_err_t assign_static_ip(const char* ip_addr) {
	// switching from dhcp to static ip
	ESP_RETURN_ON_ERROR(esp_netif_dhcpc_stop(eth_netif), S3_TAG, "esp_netif_dhcpc_stop failed");
	esp_netif_ip_info_t ip;
	// clearing struct before setting
    memset(&ip, 0 , sizeof(esp_netif_ip_info_t));
	// assigning IP
	ip.ip.addr = ipaddr_addr(ip_addr);
	// assigning subnet (devices that are considered local)
    ip.netmask.addr = ipaddr_addr(SUBNET);
	// assigning gateway (home router)
    ip.gw.addr = ipaddr_addr(GATEWAY);
	ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(eth_netif, &ip), S3_TAG, "esp_netif_set_ip_info failed");
	ESP_LOGD(S3_TAG, "Success to set static ip: %s, netmask: %s, gw: %s", ip_addr, SUBNET, GATEWAY);
    return ESP_OK;
}

// connecting the drivers and the network
esp_err_t attach_driver_to_network(const char* ip_addr) {
	// starting handler to catch errors
	ESP_RETURN_ON_ERROR(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL), S3_TAG, "eth_event_handler failed");
	ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL), S3_TAG, "got_ip_event_handler failed");
	// attach Ethernet driver to TCP/IP stack
	ESP_RETURN_ON_ERROR(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle)), S3_TAG, "esp_netif_attach failed");
	// assigning static IPs
	ESP_ERROR_CHECK(assign_static_ip(ip_addr));
	// starting ethernet
	ESP_RETURN_ON_ERROR(esp_eth_start(eth_handle), S3_TAG, "esp_eth_start failed");

	return ESP_OK;
}

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

static bool IRAM_ATTR timer_heartbeat_callback(gptimer_handle_t timer, const gptimer_alarm_event_data_t* edata, void* usr_ctx) {
	BaseType_t xHigherPriorityTaskWoken, xResult;
    xHigherPriorityTaskWoken = pdFALSE; // ensures the scheduler does not preempt this ISR
	xEventGroupSetBitsFromISR(log_group, HEARTBEAT_BIT, &xHigherPriorityTaskWoken);
	if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR(); // yields only after the ISR is finished
    }
	return false;
}

static bool IRAM_ATTR timer_stream_callback(gptimer_handle_t timer, const gptimer_alarm_event_data_t* edata, void* usr_ctx) {
	BaseType_t xHigherPriorityTaskWoken, xResult;
    xHigherPriorityTaskWoken = pdFALSE; // ensures the scheduler does not preempt this ISR
	xEventGroupSetBitsFromISR(log_group, STREAM_BIT, &xHigherPriorityTaskWoken);
	if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR(); // yields only after the ISR is finished
    }
	return false;
}

static bool IRAM_ATTR timer_data_callback(gptimer_handle_t timer, const gptimer_alarm_event_data_t* edata, void* usr_ctx) {
	BaseType_t xHigherPriorityTaskWoken, xResult;
    xHigherPriorityTaskWoken = pdFALSE; // ensures the scheduler does not preempt this ISR
	xEventGroupSetBitsFromISR(collect_group, MEASURE_ALL_BIT, &xHigherPriorityTaskWoken);
	if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR(); // yields only after the ISR is finished
    }
	return false;
}

// Create three independent periodic timers:
// 1) heartbeat logging
// 2) UDP streaming cadence
// 3) sensor collection cadence
//
// The callbacks only set event bits from ISR context; the real work is done in
// tasks so the ISR path stays short and deterministic.
void timer_setup(void) {
	gptimer_handle_t timer_heartbeat; 
	gptimer_config_t timer_heartbeat_config = {
		.clk_src = GPTIMER_CLK_SRC_DEFAULT,
		.direction = GPTIMER_COUNT_UP, 
		.resolution_hz = 1000000, // one tick = 1 us	
	}; 
	gptimer_alarm_config_t alarm_heartbeat_config = {
		.reload_count = 0,
		.alarm_count = HEARTBEAT_PERIOD_US,
		.flags.auto_reload_on_alarm = true
	};
	gptimer_event_callbacks_t cbs_heartbeat = {
		.on_alarm = timer_heartbeat_callback,
	}; 
	ESP_ERROR_CHECK(gptimer_new_timer(&timer_heartbeat_config, &timer_heartbeat));
	ESP_ERROR_CHECK(gptimer_set_alarm_action(timer_heartbeat, &alarm_heartbeat_config));
	ESP_ERROR_CHECK(gptimer_register_event_callbacks(timer_heartbeat, &cbs_heartbeat, NULL));
	ESP_ERROR_CHECK(gptimer_enable(timer_heartbeat));
	ESP_ERROR_CHECK(gptimer_start(timer_heartbeat));

	gptimer_handle_t timer_stream; 
	gptimer_config_t timer_stream_config = {
		.clk_src = GPTIMER_CLK_SRC_DEFAULT,
		.direction = GPTIMER_COUNT_UP, 
		.resolution_hz = 1000000, // one tick = 1 us	
	}; 
	gptimer_alarm_config_t alarm_stream_config = {
		.reload_count = 0,
		.alarm_count = STREAM_PERIOD_US,
		.flags.auto_reload_on_alarm = true
	};
	gptimer_event_callbacks_t cbs_stream = {
		.on_alarm = timer_stream_callback,
	}; 
	ESP_ERROR_CHECK(gptimer_new_timer(&timer_stream_config, &timer_stream));
	ESP_ERROR_CHECK(gptimer_set_alarm_action(timer_stream, &alarm_stream_config));
	ESP_ERROR_CHECK(gptimer_register_event_callbacks(timer_stream, &cbs_stream, NULL));
	ESP_ERROR_CHECK(gptimer_enable(timer_stream));
	ESP_ERROR_CHECK(gptimer_start(timer_stream));

	gptimer_handle_t timer_data; 
	gptimer_config_t timer_data_config = {
		.clk_src = GPTIMER_CLK_SRC_DEFAULT,
		.direction = GPTIMER_COUNT_UP, 
		.resolution_hz = 1000000, // one tick = 1 us	
	}; 
	gptimer_alarm_config_t alarm_data_config = {
		.reload_count = 0,
		.alarm_count = DATA_PERIOD_US,
		.flags.auto_reload_on_alarm = true
	};
	gptimer_event_callbacks_t cbs_data = {
		.on_alarm = timer_data_callback,
	}; 
	ESP_ERROR_CHECK(gptimer_new_timer(&timer_data_config, &timer_data));
	ESP_ERROR_CHECK(gptimer_set_alarm_action(timer_data, &alarm_data_config));
	ESP_ERROR_CHECK(gptimer_register_event_callbacks(timer_data, &cbs_data, NULL));
	ESP_ERROR_CHECK(gptimer_enable(timer_data));
	ESP_ERROR_CHECK(gptimer_start(timer_data));
}

// Periodic liveness task. Useful during bring-up and field debugging to prove
// that the node is still running even when no client is connected.
void heartbeat(void* pvParams) {
	while (1) {
		xEventGroupWaitBits(
			log_group,
			HEARTBEAT_BIT, 
			pdTRUE,
			pdTRUE,
			portMAX_DELAY
		);
		ESP_LOGW(S3_TAG, "Heartbeat from %s", ip_addr);
	}
}


// Wait for either the periodic stream event or a manual stream-enable flag,
// then send the latest formatted payload to the backend over UDP.
void udp_stream(void* pvParams) {
	stream_payload* payload = (stream_payload*) pvParams;
	while (1) {
		xEventGroupWaitBits(
			log_group,
			STREAM_BIT | STREAM_BIT_MANUAL,
			pdFALSE, 
			pdTRUE,
			portMAX_DELAY
		);
		if (mutex && xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) {
			snprintf(payload->msg, sizeof(payload->msg), "{\n\t\"chip\"\t:\t\"%s\",\n\t\"firmvers\"\t:\t\"%s\",\n\t\"ip\"\t:\t\"%s\",\n\t\"temperature\"\t:\t\"%s\",\n\t\"humidity\"\t:\t\"%s\",\n\t\"air_quality\"\t:\t\"%s\",\n\t\"motion_detected\"\t:\t%s\n}", 
				sensor_data->chip, sensor_data->firmvers, sensor_data->ip, sensor_data->temperature, sensor_data->humidity, sensor_data->air_quality, sensor_data->motion_detected? "true" : "false" 
			);
			int err = sendto(payload->sock, payload->msg, strlen(payload->msg), 0, (struct sockaddr *)&payload->dest_addr, sizeof(payload->dest_addr));
			if (err < 0) {
				ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
			} else {
				ESP_LOGI(TAG, "Message sent");
			}
			xSemaphoreGive(mutex);
		}
		xEventGroupClearBits(log_group, STREAM_BIT);
	}
}

// Create and bind the UDP socket used for telemetry, then spawn the task that
// transmits payloads on stream events.
void udp_socket_create(void* pv_params) {
	int addr_family = (int)pv_params; // ipv4
	struct sockaddr_in dest_addr_ip4 = {0};
	struct sockaddr_in dest_addr = {0};
	int ip_protocol = IPPROTO_IP;
	int sock; 
	sock = socket(addr_family, SOCK_DGRAM, ip_protocol);
	if (sock < 0) {
		ESP_LOGI(S3_TAG, "Failed to create socket");  
		return;
	}
	dest_addr_ip4.sin_addr.s_addr = htonl(INADDR_ANY);
	dest_addr_ip4.sin_family = AF_INET;
	dest_addr_ip4.sin_port = htons(PORT_UDP);
	int res = inet_pton(AF_INET, STATION3_IP, &dest_addr.sin_addr);
	if (res != 1) {
		ESP_LOGI(S3_TAG, "Invalid Station3 IP");
		close(sock);
		return;  
	}
	dest_addr.sin_family = AF_INET;
	dest_addr.sin_port = htons(PORT_UDP);
	payload->sock = sock;
	payload->dest_addr = dest_addr;
	ESP_LOGI(S3_TAG, "Created socket"); 
	int err = bind(sock,  (struct sockaddr *)&dest_addr_ip4, sizeof(dest_addr_ip4));
	if (err < 0) {
		ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
		close(sock);
		return;
	}
	ESP_LOGI(TAG, "UDP Socket bound, port %d", PORT_UDP);
	xTaskCreate(udp_stream, "UDP Stream", 4096, payload, 2, NULL);
}

// Receive one command from the TCP console client
int rx(char* message, int sock) {
	int len = recv(sock, message, 127, 0);
	if (len < 0) {
		ESP_LOGE(TAG, "Error occurred during receiving: errno %d", errno);
		return -1;
	} else if (len > 0) {
		message[len] = 0; 
		ESP_LOGI(TAG, "Received %d bytes: %s", len, message);
		return 1;
	} else {
		ESP_LOGI(TAG, "Client closed connection.");
		return 0;
	}
}

// Send the full message, retrying until all bytes are written or the socket
// reports an error.
void tx(const char* message, int sock) {
	int to_write = strlen(message);
	int len = strlen(message);
	while (to_write > 0) {
		int written = send(sock, message + (len - to_write), to_write, 0);
		if (written < 0) {
			ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
			return;
		}
		to_write -= written;
	}
}

void handle_exit(char* tx_buffer, int sock) {
	tx("Session terminated\n", sock);
}

void handle_help(char* tx_buffer, int sock) {
	tx("Options:\nreboot\nstream on\nstream off\nota flash\n ota status\n", sock);
}

void handle_reboot(char* tx_buffer, int sock) {
	tx("Restarting...\n", sock);
	esp_restart();
}

void handle_stream_on(char* tx_buffer, int sock) {
	tx("Resuming live streaming of sensor data...\n", sock);
	xEventGroupSetBits(log_group, STREAM_BIT_MANUAL); 
}

void handle_stream_off(char* tx_buffer, int sock) {
	tx("Pausing live streaming of sensor data...\n", sock);
	xEventGroupClearBits(log_group, STREAM_BIT_MANUAL); 
}

void handle_ota_check(char* tx_buffer, int sock) {
	snprintf(tx_buffer, 256, "Latest Version: %s\nRunning Version: %s\nLast time ota triggered: %s\n", server_version_str, esp_app_get_description()->version, "NULL");
	tx(tx_buffer, sock);
}

void handle_ota_start(char* tx_buffer, int sock) {
	tx("Initiating OTA...\n", sock);
	xEventGroupSetBits(main_group, OTA_REQUESTED_BIT);
}

void handle_invalid(char* tx_buffer, int sock) {
	tx("Invalid command. Use 'help' to see valid commands\n", sock);
}

// Simple line-oriented TCP console for remote interaction with the node. This
// is intentionally small but useful for remote bring-up and OTA control.
void communicate(int sock) {
	char rx_buffer[256];
	char tx_buffer[256];
	int status;
	int exit = 0;
	strncpy(tx_buffer, "ESP32 Node Console\nType 'help' for commands\nType 'exit' to terminate the session\n", sizeof(tx_buffer) - 1);
	tx_buffer[sizeof(tx_buffer) - 1] = 0;
	tx(tx_buffer, sock);
	while (!exit) {
		strncpy(tx_buffer, "esp32$\t", sizeof(tx_buffer) - 1);
		tx_buffer[sizeof(tx_buffer) - 1] = 0;
		tx(tx_buffer, sock);
		status = rx(rx_buffer, sock);	
		if (status < 1) {
			return;
		} 
		strfilter(rx_buffer, 128);
		// returns 0 if equal
		if (!strcmp(rx_buffer, "exit")) {
			handle_exit(tx_buffer, sock);
			exit = 1;
		} else if (!strcmp(rx_buffer, "help")) {
			handle_help(tx_buffer, sock);
		} else if (!strcmp(rx_buffer, "reboot")) {
			handle_reboot(tx_buffer, sock);
		} else if (!strcmp(rx_buffer, "stream on")) {
			handle_stream_on(tx_buffer, sock);
		} else if (!strcmp(rx_buffer, "stream off")) {
			handle_stream_off(tx_buffer, sock);
		} else if (!strcmp(rx_buffer, "ota status")) {
			handle_ota_check(tx_buffer, sock);
		} else if (!strcmp(rx_buffer, "ota flash")) {
			handle_ota_start(tx_buffer, sock);
		} else {
			handle_invalid(tx_buffer, sock);
		}
	}
}

// Start the TCP console server. Each accepted connection is handled in-place
// and closed before the server returns to listen for the next client.
void tcp_server_create(void* pv_params) {
	const int buffer_length = 128;
	char addr_str[buffer_length]; // client ip address 
	int ipv4 = (int)pv_params; // using ipv4 strictly
	int ip_protocol = IPPROTO_IP; // default protocol
	struct sockaddr_storage dest_addr; // address the server will bind to
	struct sockaddr_in *dest_addr_ip4 = (struct sockaddr_in*) &dest_addr;
	dest_addr_ip4->sin_addr.s_addr = htonl(INADDR_ANY); // bind to all possible IPs on the ESP
	dest_addr_ip4->sin_family = AF_INET; // using ipv4
	dest_addr_ip4->sin_port = htons(PORT_TCP); // assign port PORT_TCP
	ip_protocol = IPPROTO_IP;
	// socket used to wait for clients
	int listen_socket = socket(ipv4, SOCK_STREAM, ip_protocol);
	if (listen_socket < 0) {
		ESP_LOGE(TAG, "Unable to create TCP socket");
		return; 
	}
	int keep_alive = 1;
	int disable_delay = 1;
	int idle_seconds = 30;
	int keepalive_intvl = 5;
	int probe_count = 3;
	int reuse = 1;
	// letting server bind to same address/port after reset
	setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(int));
	// attaching socket to ip address at a specific port
	int err = bind(listen_socket, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
	if (err != 0) {
        ESP_LOGE(TAG, "Error occurred during bind: errno %d", errno);
		return;
    }
	// wait for connection requests
	err = listen(listen_socket, 1); 
	if (err != 0) {
        ESP_LOGE(TAG, "Error occurred during listen: errno %d", errno);
		return;
    }
	while (1) {
		ESP_LOGI(TAG, "TCP Socket bound and listening on port %d", PORT_TCP);
		struct sockaddr_storage source_addr; // Large enough for both IPv4 or IPv6
        socklen_t addr_len = sizeof(source_addr);
		// accept incoming connection on a new socket used for communication
        int sock = accept(listen_socket, (struct sockaddr *)&source_addr, &addr_len);
        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
            break;
        }
		// enabling keep alive probes on socket
		setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keep_alive, sizeof(int));
		// disable delay during transmission 
		setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &disable_delay, sizeof(int));
		// how long to idle for before first keep alive probe 
		setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &idle_seconds, sizeof(int));	
		// time between keepalive probes 
		setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepalive_intvl, sizeof(int));	
		// number of keepalive probes 
		setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &probe_count, sizeof(int));	
		if (source_addr.ss_family == PF_INET) {
			// converting client ip to stringan
            inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr, addr_str, sizeof(addr_str) - 1);
			ESP_LOGI(TAG, "Listening to client at ip %s", addr_str);
			communicate(sock);
        }
		shutdown(sock, 0);
		close(sock);
	}
	shutdown(listen_socket, 0);
	close(listen_socket);
}

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

// Bring up the full Ethernet path in dependency order.
void network_start(void) {
	ESP_ERROR_CHECK(network_init());
	ESP_ERROR_CHECK(mac_phy_init());
	ESP_ERROR_CHECK(driver_init());
	ESP_ERROR_CHECK(attach_driver_to_network(ip_addr));
}


uint64_t read_aht20() {
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


int read_hcsr505() {
	uint8_t init[2] = {0xFF, 0xFF};  
	uint8_t raw_data[2];
	i2c_master_transmit(hcsr505_handle, init, 2, -1);
	i2c_master_receive(hcsr505_handle, raw_data, 2, -1);
	uint16_t data = ((uint16_t)(raw_data[1] << 8)) | raw_data[0];
	return (data & (uint16_t)(1 << HCSR505_GPIO)) != 0;
}

// calculate the ppm from the voltage
// returns V
float read_mq135() {
	int adc_raw, adc_cali; 
	ESP_ERROR_CHECK(adc_oneshot_read(adc_oneshot_handle, ADC_CHANNEL_1, &adc_raw));
	ESP_ERROR_CHECK(adc_cali_raw_to_voltage(adc_cali_handle, adc_raw, &adc_cali));
	return adc_cali / 1000.0f;
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
		uint64_t data = read_aht20();
		if (data == 0) continue;
		uint32_t humidity = (uint32_t)((data & 0xFFFFFFFF00000000) >> 32);
		uint32_t temperature = (uint32_t)(data & 0x00000000FFFFFFFF);
		float humidity_fl = 100.0f * humidity / (1048576.0f);
		float temp_celcius_fl = (200.0f * temperature / (1048576.0f)) - 50.0f;
		// MQ135
		float air_quality = read_mq135();	
		 // HCSR505
		int motion_detected = read_hcsr505();
		if (mutex && xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) {
			snprintf(sensor_data->temperature, sizeof(sensor_data->temperature), "%f", temp_celcius_fl);
			snprintf(sensor_data->humidity, sizeof(sensor_data->humidity), "%f", humidity_fl);
			snprintf(sensor_data->air_quality, sizeof(sensor_data->air_quality), "%f", air_quality); 
			sensor_data->motion_detected = motion_detected;
			xSemaphoreGive(mutex);
		}
	}	
}

static void IRAM_ATTR motion_detected_handler(void* pvParams) {
	// log the date and time here too
	ESP_LOGW(TAG, "Motion detected!");
}


void gpio_init(void) {
	gpio_config_t io_intr_conf = {
		.pin_bit_mask = (1ULL << HCSR505_INTR_PIN),
		.mode = GPIO_MODE_INPUT,
		.pull_up_en = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_HIGH_LEVEL,
	}; 
	gpio_config(&io_intr_conf);
	// mac_phy_init() calls gpio_install_isr_service(0) which is needed for gpio_isr_handler_add
	// so ethernet_init must happen before gpio_init
	gpio_isr_handler_add(HCSR505_INTR_PIN, motion_detected_handler, NULL);
}; 


void i2c_init(void) {
	// master
	// hardware controller, pins, timing, etc
	i2c_master_bus_config_t i2c_mst_config = {
		.clk_source = I2C_CLK_SRC_DEFAULT,
		.i2c_port = I2C_NUM_0,
		.scl_io_num = MASTER_SCL_GPIO,
		.sda_io_num = MASTER_SDA_GPIO,
		.glitch_ignore_cnt = 7,
		.flags.enable_internal_pullup = false, // going to rely on external pullup ~10 kohms instead 
	};
	i2c_master_bus_handle_t i2c_bus_handle;
	ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_mst_config, &i2c_bus_handle)); 
	// slave 1: aht20
	i2c_device_config_t aht20_cfg = {
		.dev_addr_length = I2C_ADDR_BIT_LEN_7,
		.device_address = AHT20_ADDR,
		.scl_speed_hz = SCL_FREQUENCY_HZ,	
	};
	// slave 1: pcf8575
	i2c_device_config_t pcf8575_cfg = {
		.dev_addr_length = I2C_ADDR_BIT_LEN_7,
		.device_address = PCF8585_ADDR,
		.scl_speed_hz = SCL_FREQUENCY_HZ,	
	};
	ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus_handle, &aht20_cfg, &aht20_handle)); 
	ESP_ERROR_CHECK(i2c_master_probe(i2c_bus_handle, AHT20_ADDR, -1));
	ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus_handle, &pcf8575_cfg, &hcsr505_handle)); 
	ESP_ERROR_CHECK(i2c_master_probe(i2c_bus_handle, PCF8585_ADDR, -1));
	uint8_t init = 0x71;
	esp_err_t err; 
	vTaskDelay(pdMS_TO_TICKS(40));
	err = i2c_master_transmit_receive(aht20_handle, &init, 1, &success, 1, -1);
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "Error on calibration!");
		return;
	}
	if (!(success & 0x08)) {
		// init required
		uint8_t calibrate[3] = {0xBE, 0x08, 0x00};
		success = 0;
		err = i2c_master_transmit(aht20_handle, calibrate, 3, -1);
		vTaskDelay(pdMS_TO_TICKS(10));
		if (err != ESP_OK) {
			ESP_LOGW(TAG, "Error on calibration!");
			return;
		}
	}
}	

void adc_init() {
	adc_oneshot_unit_init_cfg_t oneshot_config = {
		.unit_id = ADC_UNIT_1, 
		.ulp_mode = ADC_ULP_MODE_DISABLE,
	};
	adc_oneshot_chan_cfg_t chan_config = {
		.bitwidth = ADC_BITWIDTH_13, 
		.atten = ADC_ATTEN_DB_12,
	};
	int adc_raw, adc_cali;
	ESP_ERROR_CHECK(adc_oneshot_new_unit(&oneshot_config, &adc_oneshot_handle));
	adc_oneshot_config_channel(adc_oneshot_handle, ADC_CHANNEL_1, &chan_config); 
	adc_cali_curve_fitting_config_t cali_config = {
		.unit_id = ADC_UNIT_1,
		.atten = ADC_ATTEN_DB_12,
		.bitwidth = ADC_BITWIDTH_13,
	};
	ESP_ERROR_CHECK(adc_cali_create_scheme_curve_fitting(&cali_config, &adc_cali_handle)); 
}

// Main firmware control path. After initialization it waits for OTA request
// events while the rest of the system runs in FreeRTOS tasks
void s3_main(void) {   
	ESP_LOGW(S3_TAG, "s3_main started");
	main_group = xEventGroupCreate();
	log_group = xEventGroupCreate();
	collect_group = xEventGroupCreate();
	sensor_data = (stream_data*)calloc(1, sizeof(stream_data));
	payload = (stream_payload*)calloc(1, sizeof(stream_payload)); 
	payload->_stream_data = sensor_data;
	esp_err_t err = nvs_flash_init(); 
	if (err != ESP_OK) {
		ESP_LOGE(S3_TAG, "Could not initialize NVS partition");
	} else {
		load_ip(); 
		load_chip();
	}
	validate_ota(); 
	network_start();
	xEventGroupWaitBits(
		main_group,
		ETH_CONNECTED_BIT,
		pdFALSE,
		pdTRUE,
		portMAX_DELAY
	); 
	i2c_init();
	adc_init();
	gpio_init();
	if (http_get_request() == ESP_OK) {
		parse_manifest(false);
	}
	mutex = xSemaphoreCreateMutex();
	timer_setup();
	xTaskCreate(measure_sensor_values, "Measure sensor values", 4096, NULL, 3, NULL);
	xTaskCreate(heartbeat, "Heartbeat Monitor Task", 4096, NULL, 3, NULL); 
	xTaskCreate(udp_socket_create, "UDP Server Task", 4096, (void *)AF_INET, 5, NULL); 	
	xTaskCreate(tcp_server_create, "TCP Server Task", 4096, (void *)AF_INET, 5, NULL); 	
	while (1) {
		xEventGroupWaitBits(
			main_group,
			OTA_REQUESTED_BIT, 
			pdTRUE,
			pdTRUE,
			portMAX_DELAY
		);
		ESP_LOGW(S3_TAG, "OTA request bit received in main loop");
		if (http_get_request() != ESP_OK) {
			continue;
		}		
		parse_manifest(true);
	}
}  	 