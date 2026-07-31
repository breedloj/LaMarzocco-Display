#pragma once

#include <Arduino.h>
#include <LilyGo_AMOLED.h>

// Initializes display brightness management and starts in the active state.
// A zero idle timeout disables automatic dimming.
bool display_power_init(LilyGo_Class *display,
                        uint8_t active_brightness,
                        uint8_t dim_brightness,
                        uint32_t idle_timeout_ms);

// Activity markers are safe to call from tasks running on either ESP32 core.
// They do not perform display I/O; display_power_loop() applies any resulting
// brightness transition.
void display_power_mark_user_activity();
void display_power_mark_machine_activity();

// Call periodically from a task. The display dims only after both user and
// machine activity have been idle for the configured timeout.
void display_power_loop();

// Restores active brightness immediately.
void display_power_wake();

// Returns true only when this call woke a dimmed display. Touch handlers can
// use the result to consume the first touch instead of sending a command.
bool display_power_wake_if_dimmed();

bool display_power_is_dimmed();
