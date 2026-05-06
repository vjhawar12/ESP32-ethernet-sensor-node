#ifndef SENSORS_H
#define SENSORS_H

// Periodic application task that will eventually gather all sensor values and
// refresh the outgoing telemetry JSON. Right now the payload structure and
// synchronization are in place even though sensor collection is still being
// expanded.
void measure_sensor_values(void* pv_params);
void motion_detected_handler(void* pvParams);

#endif