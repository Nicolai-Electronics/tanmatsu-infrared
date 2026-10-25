#include "catt_ir.h"
#include "bsp/catt.h"
#include "driver/rmt_rx.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "freertos/queue.h"
#include "ir_nec_encoder.h"

static char const TAG[] = "catt_ir";

// The transmitter of the add-on is connected to CATT pin D3 and the receiver to CATT pin D2,
// on Tanmatsu these are GPIO 3 and GPIO 5
#define CATT_IR_TX_PIN CATT_PIN_D3
#define CATT_IR_RX_PIN CATT_PIN_D2

#define CATT_IR_RESOLUTION_HZ 1000000  // 1MHz, 1 tick = 1us

// NEC timing in microseconds
#define NEC_DECODE_MARGIN           200
#define NEC_LEADING_CODE_DURATION_0 9000
#define NEC_LEADING_CODE_DURATION_1 4500
#define NEC_PAYLOAD_ZERO_DURATION_0 560
#define NEC_PAYLOAD_ZERO_DURATION_1 560
#define NEC_PAYLOAD_ONE_DURATION_0  560
#define NEC_PAYLOAD_ONE_DURATION_1  1690

// Shorter receptions (e.g. NEC repeat codes, which consist of 2 symbols) are ignored
#define MIN_FRAME_SYMBOLS 8

static rmt_channel_handle_t tx_channel  = NULL;
static rmt_encoder_handle_t nec_encoder = NULL;
static bool                 tx_enabled  = false;
static rmt_channel_handle_t rx_channel  = NULL;
static QueueHandle_t        rx_queue    = NULL;
static bool                 rx_active   = false;
static rmt_symbol_word_t    rx_symbols[64];  // An NEC frame consists of 34 symbols

static rmt_receive_config_t const receive_config = {
    .signal_range_min_ns = 1250,      // Shorter pulses are glitches, the shortest NEC pulse is 560us
    .signal_range_max_ns = 12000000,  // A pulse this long ends the reception, the longest NEC pulse is 9000us
};

static bool rx_done_callback(rmt_channel_handle_t channel, rmt_rx_done_event_data_t const* edata, void* user_data) {
    BaseType_t high_task_wakeup = pdFALSE;
    xQueueSendFromISR((QueueHandle_t)user_data, edata, &high_task_wakeup);
    return high_task_wakeup == pdTRUE;
}

esp_err_t catt_ir_init(void) {
    if (tx_channel != NULL) {
        return ESP_OK;
    }
    gpio_num_t tx_gpio = bsp_catt_get_gpio(CATT_IR_TX_PIN);
    gpio_num_t rx_gpio = bsp_catt_get_gpio(CATT_IR_RX_PIN);
    if (tx_gpio == GPIO_NUM_NC || rx_gpio == GPIO_NUM_NC) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    rmt_tx_channel_config_t const tx_config = {
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .resolution_hz     = CATT_IR_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
        .gpio_num          = tx_gpio,
    };
    ir_nec_encoder_config_t const encoder_config = {
        .resolution = CATT_IR_RESOLUTION_HZ,
    };
    esp_err_t res = rmt_new_tx_channel(&tx_config, &tx_channel);
    if (res == ESP_OK) {
        res = catt_ir_set_carrier(38000, 33);
    }
    if (res == ESP_OK) {
        res = rmt_new_ir_nec_encoder(&encoder_config, &nec_encoder);
    }
    if (res == ESP_OK) {
        res        = rmt_enable(tx_channel);
        tx_enabled = res == ESP_OK;
    }

    rmt_rx_channel_config_t const rx_config = {
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .resolution_hz     = CATT_IR_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .gpio_num          = rx_gpio,
        .flags.invert_in   = true,  // The output of the receiver module is active low
    };
    rmt_rx_event_callbacks_t const callbacks = {
        .on_recv_done = rx_done_callback,
    };
    if (res == ESP_OK) {
        res = rmt_new_rx_channel(&rx_config, &rx_channel);
    }
    if (res == ESP_OK) {
        rx_queue = xQueueCreate(1, sizeof(rmt_rx_done_event_data_t));
        res      = rx_queue != NULL ? ESP_OK : ESP_ERR_NO_MEM;
    }
    if (res == ESP_OK) {
        res = rmt_rx_register_event_callbacks(rx_channel, &callbacks, rx_queue);
    }

    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize the infrared add-on: %s", esp_err_to_name(res));
        catt_ir_deinit();
    }
    return res;
}

void catt_ir_deinit(void) {
    catt_ir_receive_stop();
    if (rx_channel != NULL) {
        rmt_del_channel(rx_channel);
        rx_channel = NULL;
    }
    if (rx_queue != NULL) {
        vQueueDelete(rx_queue);
        rx_queue = NULL;
    }
    if (tx_enabled) {
        rmt_disable(tx_channel);
        tx_enabled = false;
    }
    if (tx_channel != NULL) {
        rmt_del_channel(tx_channel);
        tx_channel = NULL;
    }
    if (nec_encoder != NULL) {
        rmt_del_encoder(nec_encoder);
        nec_encoder = NULL;
    }
}

esp_err_t catt_ir_set_carrier(uint32_t frequency_hz, uint8_t duty_percent) {
    if (tx_channel == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    rmt_carrier_config_t const carrier_config = {
        .frequency_hz = frequency_hz,
        .duty_cycle   = duty_percent / 100.0f,
    };
    return rmt_apply_carrier(tx_channel, &carrier_config);
}

esp_err_t catt_ir_send_nec(uint16_t address, uint16_t command) {
    if (!tx_enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    // The encoder reads the scan code during the transmission, so it has to outlive this function call
    static ir_nec_scan_code_t   scan_code;
    rmt_transmit_config_t const transmit_config = {
        .loop_count = 0,
    };
    scan_code.address = address;
    scan_code.command = command;
    esp_err_t res     = rmt_transmit(tx_channel, nec_encoder, &scan_code, sizeof(scan_code), &transmit_config);
    if (res != ESP_OK) {
        return res;
    }
    return rmt_tx_wait_all_done(tx_channel, 1000);  // An NEC frame takes about 70ms
}

esp_err_t catt_ir_receive_start(void) {
    if (rx_channel == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (rx_active) {
        return ESP_OK;
    }
    xQueueReset(rx_queue);
    esp_err_t res = rmt_enable(rx_channel);
    if (res != ESP_OK) {
        return res;
    }
    res = rmt_receive(rx_channel, rx_symbols, sizeof(rx_symbols), &receive_config);
    if (res != ESP_OK) {
        rmt_disable(rx_channel);
        return res;
    }
    rx_active = true;
    return ESP_OK;
}

void catt_ir_receive_stop(void) {
    if (!rx_active) {
        return;
    }
    rmt_disable(rx_channel);
    rx_active = false;
}

static bool nec_in_range(uint32_t duration, uint32_t spec_duration) {
    return duration < spec_duration + NEC_DECODE_MARGIN && duration > spec_duration - NEC_DECODE_MARGIN;
}

// Decode 16 bits, least significant bit first
static bool nec_decode_bits(rmt_symbol_word_t const* symbols, uint16_t* out_value) {
    uint16_t value = 0;
    for (int i = 0; i < 16; i++) {
        if (nec_in_range(symbols[i].duration0, NEC_PAYLOAD_ONE_DURATION_0) &&
            nec_in_range(symbols[i].duration1, NEC_PAYLOAD_ONE_DURATION_1)) {
            value |= 1 << i;
        } else if (!nec_in_range(symbols[i].duration0, NEC_PAYLOAD_ZERO_DURATION_0) ||
                   !nec_in_range(symbols[i].duration1, NEC_PAYLOAD_ZERO_DURATION_1)) {
            return false;
        }
    }
    *out_value = value;
    return true;
}

// An NEC frame consists of a leading code, 16 address bits, 16 command bits and a stop bit
static esp_err_t nec_decode_frame(rmt_symbol_word_t const* symbols, size_t count, uint16_t* out_address,
                                  uint16_t* out_command) {
    if (count < MIN_FRAME_SYMBOLS) {
        return ESP_ERR_TIMEOUT;
    }
    uint16_t address = 0;
    uint16_t command = 0;
    if (count < 33 || count > 34 || !nec_in_range(symbols[0].duration0, NEC_LEADING_CODE_DURATION_0) ||
        !nec_in_range(symbols[0].duration1, NEC_LEADING_CODE_DURATION_1) || !nec_decode_bits(&symbols[1], &address) ||
        !nec_decode_bits(&symbols[17], &command)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *out_address = address;
    *out_command = command;
    return ESP_OK;
}

esp_err_t catt_ir_receive(uint16_t* out_address, uint16_t* out_command, TickType_t timeout) {
    if (!rx_active) {
        return ESP_ERR_INVALID_STATE;
    }
    rmt_rx_done_event_data_t event;
    if (xQueueReceive(rx_queue, &event, timeout) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t result = nec_decode_frame(event.received_symbols, event.num_symbols, out_address, out_command);
    ESP_LOGD(TAG, "Received %zu symbols: %s", event.num_symbols, esp_err_to_name(result));

    // Listen for the next code, the symbol buffer is reused so this has to happen after decoding
    esp_err_t res = rmt_receive(rx_channel, rx_symbols, sizeof(rx_symbols), &receive_config);
    if (res != ESP_OK) {
        catt_ir_receive_stop();
        return res;
    }
    return result;
}
