#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

// Driver for the infrared add-on connected to the CATT port: an infrared transmitter and an infrared receiver,
// driven directly by the RMT peripheral

/// @brief Set up the transmitter and receiver of the add-on
/// @return ESP_OK on success, ESP_ERR_NOT_SUPPORTED if the device has no CATT port, or another error code
esp_err_t catt_ir_init(void);

/// @brief Release the transmitter and receiver of the add-on
void catt_ir_deinit(void);

/// @brief Set the carrier of the transmitter
esp_err_t catt_ir_set_carrier(uint32_t frequency_hz, uint8_t duty_percent);

/// @brief Transmit an NEC code, address and command are sent low byte first
esp_err_t catt_ir_send_nec(uint16_t address, uint16_t command);

/// @brief Start listening for NEC codes
esp_err_t catt_ir_receive_start(void);

/// @brief Stop listening for NEC codes
void catt_ir_receive_stop(void);

/// @brief Wait for an NEC code, catt_ir_receive_start() must be called first
/// @return ESP_OK if a code was received, ESP_ERR_TIMEOUT if no code was received in time (NEC repeat codes and
///         glitches are ignored), ESP_ERR_INVALID_RESPONSE if a signal was received that is not an NEC code
esp_err_t catt_ir_receive(uint16_t* out_address, uint16_t* out_command, TickType_t timeout);
