#include "driver/gpio.h"

#ifndef APP_CONFIG_H
#define APP_CONFIG_H
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

#endif