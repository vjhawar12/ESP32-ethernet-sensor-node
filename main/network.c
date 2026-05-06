#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_eth_phy_w5500.h"
#include "esp_eth_mac_w5500.h"
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif
#include "app_config.h"
#include "network.h"
#include "app_config.h"
#include "esp_err.h"
#include "esp_check.h"
#include "lwip/ip4_addr.h"
#include "manifest.h"
#include "rtos_objects.h"

esp_netif_t *eth_netif = NULL;
esp_eth_mac_t *mac = NULL;
esp_eth_phy_t *phy = NULL;
esp_eth_handle_t eth_handle = NULL;

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
    EventGroupHandle_t *main_group = (EventGroupHandle_t*)arg;
    ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
    const esp_netif_ip_info_t *ip_info = &event->ip_info;
    ESP_LOGI(TAG, "Ethernet Got IP Address\n");
	ESP_LOGI(TAG, "ETH IP: " IPSTR "\n", IP2STR(&ip_info->ip));
	ESP_LOGI(TAG, "ETH MASK: " IPSTR "\n", IP2STR(&ip_info->netmask));
	ESP_LOGI(TAG, "ETH GW: " IPSTR "\n", IP2STR(&ip_info->gw));
	fflush(stdout);
	xEventGroupSetBits(*main_group, ETH_CONNECTED_BIT);
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
esp_err_t attach_driver_to_network(const char* ip_addr, EventGroupHandle_t* main_group) {
	// starting handler to catch errors
	ESP_RETURN_ON_ERROR(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL), S3_TAG, "eth_event_handler failed");
	ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, main_group), S3_TAG, "got_ip_event_handler failed");
	// attach Ethernet driver to TCP/IP stack
	ESP_RETURN_ON_ERROR(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle)), S3_TAG, "esp_netif_attach failed");
	// assigning static IPs
	ESP_ERROR_CHECK(assign_static_ip(ip_addr));
	// starting ethernet
	ESP_RETURN_ON_ERROR(esp_eth_start(eth_handle), S3_TAG, "esp_eth_start failed");

	return ESP_OK;
}

// Bring up the full Ethernet path in dependency order.
void network_start(void) {
	ESP_ERROR_CHECK(network_init());
	ESP_ERROR_CHECK(mac_phy_init());
	ESP_ERROR_CHECK(driver_init());
	ESP_ERROR_CHECK(attach_driver_to_network(ip_addr, &main_group));
}
