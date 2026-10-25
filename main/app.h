#pragma once

#include <stdbool.h>

/// @brief Run the infrared remote application, requires the display and user interface to be initialized
/// @param radio_started Whether the radio co-processor, which drives the infrared transmitter, was started
void app_run(bool radio_started) __attribute__((noreturn));
