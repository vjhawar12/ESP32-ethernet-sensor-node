#ifndef PERIODIC_H
#define PERIODIC_H


// Create three independent periodic timers:
// 1) heartbeat logging
// 2) UDP streaming cadence
// 3) sensor collection cadence
//
// The callbacks only set event bits from ISR context; the real work is done in
// tasks so the ISR path stays short and deterministic.
void timer_setup(void);
// Periodic liveness task. Useful during bring-up and field debugging to prove
// that the node is still running even when no client is connected.
void heartbeat(void* pvParams);

#endif