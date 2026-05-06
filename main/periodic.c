#include "s3.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include <sys/param.h>
#include "driver/gptimer.h"
#include "stdbool.h"
#include <lwip/netdb.h>
#include "freertos/semphr.h"
#include "app_config.h"
#include "manifest.h"
#include "periodic.h"
#include "rtos_objects.h"


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