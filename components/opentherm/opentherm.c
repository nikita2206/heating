/*
 * OpenTherm Library for ESP-IDF
 * Adapted from https://github.com/ihormelnyk/opentherm_library
 */

#include "opentherm.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "hal/gpio_ll.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "OpenTherm";

// OpenTherm timing constants (in microseconds)
#define OT_TIMING_BIT_PERIOD 1000
#define OT_TIMING_BIT_HALF_PERIOD 500
#define OT_TIMING_ISR_BIT_THRESHOLD 750  // ISR threshold: edges >750µs apart = new bit
#define OT_TIMING_RESPONSE_TIMEOUT 800000  // 800ms
#define OT_TIMING_GATEWAY_REQUEST_TIMEOUT 1500000  // 1.5s wait for thermostat frame

// Get current time in microseconds
static inline unsigned long IRAM_ATTR micros(void)
{
    return (unsigned long)esp_timer_get_time();
}

static void opentherm_disable_all_inputs(OpenTherm *ot);
static void opentherm_prepare_receive(OpenTherm *ot, OpenThermRole role);

// Fast ISR debug buffer write (no formatting, just store raw values)
static inline void IRAM_ATTR opentherm_isr_debug_log(OpenTherm *ot, uint8_t event_type, 
                                                       unsigned long timestamp, uint8_t pin_level,
                                                       uint8_t status, uint8_t bit_index,
                                                       uint32_t response, uint32_t elapsed)
{
    if (!ot->isr_debug_enabled) {
        return;
    }
    
    uint32_t write_idx = ot->isr_debug_write_idx;
    uint32_t next_idx = (write_idx + 1) % OT_ISR_DEBUG_BUFFER_SIZE;
    
    // Only write if buffer not full (simple check, may lose entries if reader is slow)
    if (next_idx != ot->isr_debug_read_idx) {
        opentherm_isr_debug_entry_t *entry = &ot->isr_debug_buffer[write_idx];
        entry->timestamp = timestamp;
        entry->event_type = event_type;
        entry->pin_level = pin_level;
        entry->status = status;
        entry->bit_index = bit_index;
        entry->response = response;
        entry->elapsed = elapsed;
        
        ot->isr_debug_write_idx = next_idx;
    }
}

/**
 * GPIO interrupt handler - decodes Manchester-encoded OpenTherm frames
 * 
 * WHY: OpenTherm uses Manchester encoding at 1kHz bit rate (1ms per bit).
 * Manchester encoding guarantees a transition at mid-bit (~500µs) for each bit,
 * and creates an additional transition at bit boundaries (~1000µs) when consecutive
 * bits differ. By measuring edge-to-edge timing, we can reliably decode bits:
 *   - Edges <750µs apart = mid-bit transition (ignore)
 *   - Edges >750µs apart = new bit period (sample pin state)
 * 
 * WHAT: This ISR fires on ANY edge (rising or falling) of the selected input pin.
 * Uses a 750µs threshold to distinguish mid-bit transitions from new bit periods:
 *   1. First edge = start bit
 *   2. Each edge >750µs after previous = sample new bit (read GPIO level)
 *   3. Continue until 32 data bits captured
 * 
 * Frame structure: [start bit] [32 data bits] [stop bit]
 * Total frame time: ~34ms (34 bits × 1ms each)
 * 
 * IMPORTANT: This runs in interrupt context (IRAM_ATTR), keep it fast!
 */
static void IRAM_ATTR opentherm_isr_handler(void *arg)
{
    opentherm_isr_context_t *ctx = (opentherm_isr_context_t *)arg;
    if (ctx == NULL) {
        return;
    }

    OpenTherm *ot = ctx->ot;
    if (ot == NULL) {
        return;
    }

    OpenThermStatus status = ot->status;
    bool should_process = false;

    // WHY: Only process interrupts when we're actively waiting for this specific
    // pin/role. Prevents cross-talk between master and slave interfaces.
    if (status == OT_STATUS_RESPONSE_WAITING || status == OT_STATUS_RESPONSE_START_BIT || status == OT_STATUS_RESPONSE_RECEIVING) {
        // Legacy non-gateway mode
        should_process = true;
    } else if (ot->gateway_mode) {
        // Gateway mode: check if this interrupt is from the pin we're currently listening to
        if (((status == OT_STATUS_GATEWAY_REQUEST_WAITING || status == OT_STATUS_GATEWAY_REQUEST_START_BIT || status == OT_STATUS_GATEWAY_REQUEST_RECEIVING) && ctx->role == OT_ROLE_MASTER) ||
            ((status == OT_STATUS_GATEWAY_RESPONSE_WAITING || status == OT_STATUS_GATEWAY_RESPONSE_START_BIT || status == OT_STATUS_GATEWAY_RESPONSE_RECEIVING) && ctx->role == OT_ROLE_SLAVE)) {
            if (ot->current_receive_pin == ctx->pin) {
                should_process = true;
            }
        }
    }

    if (!should_process) {
        return;
    }
    
    // Increment edge counter for hardware diagnostics
    if (ctx->role == OT_ROLE_MASTER) {
        ot->isr_edge_count_master++;
    } else {
        ot->isr_edge_count_slave++;
    }

    unsigned long current_time = micros();
    int pin_level = gpio_ll_get_level(GPIO_LL_GET_HW(GPIO_PORT_0), ctx->pin);

    // State machine for Manchester decoding (adapted from ihormelnyk/opentherm_library)
    // States: WAITING -> START_BIT -> RECEIVING -> READY
    
    // State: WAITING - looking for start bit (HIGH edge)
    if (status == OT_STATUS_RESPONSE_WAITING || status == OT_STATUS_GATEWAY_REQUEST_WAITING || status == OT_STATUS_GATEWAY_RESPONSE_WAITING) {
        if (pin_level == 1) {
            // Detected HIGH - start bit beginning
            if (status == OT_STATUS_RESPONSE_WAITING) {
                ot->status = OT_STATUS_RESPONSE_START_BIT;
            } else if (status == OT_STATUS_GATEWAY_REQUEST_WAITING) {
                ot->status = OT_STATUS_GATEWAY_REQUEST_START_BIT;
            } else {
                ot->status = OT_STATUS_GATEWAY_RESPONSE_START_BIT;
            }
            ot->response_timestamp = current_time;
        } else {
            // Invalid - frame must start with HIGH
            ot->status = OT_STATUS_RESPONSE_INVALID;
            ot->response_timestamp = current_time;
        }
    }
    // State: START_BIT - looking for LOW transition <750µs after HIGH
    else if (status == OT_STATUS_RESPONSE_START_BIT || status == OT_STATUS_GATEWAY_REQUEST_START_BIT || status == OT_STATUS_GATEWAY_RESPONSE_START_BIT) {
        if ((current_time - ot->response_timestamp < OT_TIMING_ISR_BIT_THRESHOLD) && pin_level == 0) {
            // Valid start bit (HIGH->LOW within 750µs) - begin receiving data
            if (status == OT_STATUS_RESPONSE_START_BIT) {
                ot->status = OT_STATUS_RESPONSE_RECEIVING;
            } else if (status == OT_STATUS_GATEWAY_REQUEST_START_BIT) {
                ot->status = OT_STATUS_GATEWAY_REQUEST_RECEIVING;
            } else {
                ot->status = OT_STATUS_GATEWAY_RESPONSE_RECEIVING;
            }
            ot->response_timestamp = current_time;
            ot->response_bit_index = 0;
            ot->response = 0;
        } else if (current_time - ot->response_timestamp >= OT_TIMING_ISR_BIT_THRESHOLD) {
            // Timeout - didn't see LOW edge in time
            ot->status = OT_STATUS_RESPONSE_INVALID;
            ot->response_timestamp = current_time;
        }
    }
    // State: RECEIVING - collecting 32 data bits
    else if (status == OT_STATUS_RESPONSE_RECEIVING || status == OT_STATUS_GATEWAY_REQUEST_RECEIVING || status == OT_STATUS_GATEWAY_RESPONSE_RECEIVING) {
        unsigned long elapsed = current_time - ot->response_timestamp;
        
        // Debug: Log edge arrival (event_type=0) - log ALL bits to diagnose issue
        opentherm_isr_debug_log(ot, 0, current_time, pin_level, status, 
                                ot->response_bit_index, ot->response, elapsed);
        
        if (elapsed > OT_TIMING_ISR_BIT_THRESHOLD) {
            if (ot->response_bit_index < 32) {
                // Sample data bit: Invert pin reading to match working library
                // Pin LOW → Bit 1, Pin HIGH → Bit 0
                bool bit_value = (pin_level == 0);
                ot->response = (ot->response << 1) | (bit_value ? 1 : 0);
                
                // Debug: Log sample (event_type=1) - log ALL bits
                opentherm_isr_debug_log(ot, 1, current_time, pin_level, status,
                                        ot->response_bit_index, ot->response, elapsed);
                
                ot->response_timestamp = current_time;
                ot->response_bit_index++;
            } else {
                // 32 bits collected - frame complete (current edge is stop bit)
                if (status == OT_STATUS_RESPONSE_RECEIVING) {
                    ot->status = OT_STATUS_RESPONSE_READY;
                } else if (status == OT_STATUS_GATEWAY_REQUEST_RECEIVING) {
                    ot->status = OT_STATUS_GATEWAY_REQUEST_READY;
                } else {
                    ot->status = OT_STATUS_GATEWAY_RESPONSE_READY;
                }
                ot->response_timestamp = current_time;
                
                // Debug: Log completion (event_type=2)
                opentherm_isr_debug_log(ot, 2, current_time, pin_level, ot->status,
                                        32, ot->response, elapsed);
            }
        }
    }
}

// Send bit using Manchester encoding (matched to working library)
static void opentherm_send_bit(OpenTherm *ot, bool high, gpio_num_t pin)
{
    if (high) {
        // Manchester 1: active (LOW) then idle (HIGH)
        gpio_set_level(pin, 0);  // Active = LOW
        esp_rom_delay_us(OT_TIMING_BIT_HALF_PERIOD);
        gpio_set_level(pin, 1);  // Idle = HIGH
        esp_rom_delay_us(OT_TIMING_BIT_HALF_PERIOD);
    } else {
        // Manchester 0: idle (HIGH) then active (LOW)
        gpio_set_level(pin, 1);  // Idle = HIGH
        esp_rom_delay_us(OT_TIMING_BIT_HALF_PERIOD);
        gpio_set_level(pin, 0);  // Active = LOW
        esp_rom_delay_us(OT_TIMING_BIT_HALF_PERIOD);
    }
}

// Initialize OpenTherm instance
void opentherm_init(OpenTherm *ot, gpio_num_t in_pin, gpio_num_t out_pin, OpenThermRole role)
{
    memset(ot, 0, sizeof(OpenTherm));
    
    ot->in_pin = in_pin;
    ot->out_pin = out_pin;
    ot->role = role;
    ot->status = OT_STATUS_NONE;
    ot->gateway_mode = false;
    ot->current_receive_pin = GPIO_NUM_NC;
    ot->current_receive_role = role;
    ot->gateway_state = OT_GATEWAY_STATE_IDLE;
    ot->gateway_timer_start = 0;
    ot->gateway_timeout_flag = false;
    ot->gateway_request.data = 0;
    ot->gateway_response.data = 0;
    ot->gateway_timeout_flag = false;
    ot->isr_primary_ctx.ot = ot;
    ot->isr_primary_ctx.pin = in_pin;
    ot->isr_primary_ctx.role = role;
    ot->isr_secondary_ctx.ot = ot;
    ot->isr_secondary_ctx.pin = GPIO_NUM_NC;
    ot->isr_secondary_ctx.role = OT_ROLE_SLAVE;
    
    // Configure input pin with strong pull-up for fast edges
    gpio_config_t in_conf = {
        .pin_bit_mask = (1ULL << in_pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE
    };
    gpio_config(&in_conf);
    
    // Configure output pin
    gpio_config_t out_conf = {
        .pin_bit_mask = (1ULL << out_pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&out_conf);
    gpio_set_level(out_pin, 1);  // Idle state is high
    
    ESP_LOGI(TAG, "OpenTherm initialized (role: %s, in: %d, out: %d)", 
             role == OT_ROLE_MASTER ? "MASTER" : "SLAVE", in_pin, out_pin);
}

/**
 * Initialize gateway mode (proxy between master and slave)
 * 
 * WHY: To act as a man-in-the-middle, we need TWO separate OpenTherm interfaces:
 *   1. Master interface: connects to thermostat (we act as SLAVE to receive requests)
 *   2. Slave interface: connects to boiler (we act as MASTER to forward requests)
 * 
 * Each interface has an input pin (for receiving) and output pin (for transmitting).
 * Both interfaces use GPIO interrupts for Manchester decoding.
 * 
 * WHAT: Sets up dual GPIO pairs and configures ISR contexts so the interrupt
 * handler can distinguish which interface generated each edge event.
 * 
 * @param in_pin_master   GPIO to receive from thermostat
 * @param out_pin_master  GPIO to transmit to thermostat
 * @param in_pin_slave    GPIO to receive from boiler
 * @param out_pin_slave   GPIO to transmit to boiler
 */
void opentherm_init_gateway(OpenTherm *ot,
                            gpio_num_t in_pin_master, gpio_num_t out_pin_master,
                            gpio_num_t in_pin_slave, gpio_num_t out_pin_slave)
{
    // Initialize primary interface (master side - receives from thermostat)
    // We configure as SLAVE because we're receiving from the thermostat (master)
    opentherm_init(ot, in_pin_master, out_pin_master, OT_ROLE_SLAVE);
    
    // Configure secondary interface (slave side - receives from boiler)
    ot->in_pin_secondary = in_pin_slave;
    ot->out_pin_secondary = out_pin_slave;
    ot->gateway_mode = true;
    
    // Configure secondary input pin with interrupt capability
    gpio_config_t in_conf = {
        .pin_bit_mask = (1ULL << in_pin_slave),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE  // Capture both rising and falling edges
    };
    gpio_config(&in_conf);
    
    // Configure secondary output pin for transmitting to boiler
    gpio_config_t out_conf = {
        .pin_bit_mask = (1ULL << out_pin_slave),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&out_conf);
    gpio_set_level(out_pin_slave, 1);  // OpenTherm idle state is high (no transmission)
    
    ESP_LOGI(TAG, "OpenTherm gateway initialized");
    ESP_LOGI(TAG, "  Master side (thermostat): in=%d, out=%d", in_pin_master, out_pin_master);
    ESP_LOGI(TAG, "  Slave side (boiler): in=%d, out=%d", in_pin_slave, out_pin_slave);

    // Initialize gateway state machine
    ot->current_receive_pin = GPIO_NUM_NC;  // No active listening initially
    ot->current_receive_role = OT_ROLE_MASTER;  // Will listen for thermostat first
    ot->gateway_state = OT_GATEWAY_STATE_IDLE;
    ot->gateway_timer_start = 0;
    
    // Initialize debug counters for hardware diagnostics
    ot->isr_edge_count_master = 0;
    ot->isr_edge_count_slave = 0;
    
    // Set up ISR contexts so interrupt handler knows which interface fired
    ot->isr_primary_ctx.role = OT_ROLE_MASTER;  // Primary receives from thermostat
    ot->isr_secondary_ctx.pin = in_pin_slave;
    ot->isr_secondary_ctx.role = OT_ROLE_SLAVE;  // Secondary receives from boiler
}

// Set message callback
void opentherm_set_message_callback(OpenTherm *ot, opentherm_message_callback_t callback, void *user_data)
{
    ot->message_callback = callback;
    ot->user_data = user_data;
}

// Start OpenTherm communication
void opentherm_start(OpenTherm *ot)
{
    // Install GPIO ISR handler
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to install GPIO ISR service: %s", esp_err_to_name(err));
        return;
    }

    gpio_isr_handler_add(ot->in_pin, opentherm_isr_handler, (void *)&ot->isr_primary_ctx);
    gpio_intr_disable(ot->in_pin);

    if (ot->gateway_mode && ot->in_pin_secondary != GPIO_NUM_NC) {
        gpio_isr_handler_add(ot->in_pin_secondary, opentherm_isr_handler, (void *)&ot->isr_secondary_ctx);
        gpio_intr_disable(ot->in_pin_secondary);
    }

    ot->status = OT_STATUS_READY;
    ESP_LOGI(TAG, "OpenTherm started");
}

// Send message frame
static void opentherm_send_frame(OpenTherm *ot, uint32_t data, gpio_num_t pin)
{
    // Send start bit
    opentherm_send_bit(ot, 1, pin);
    
    // Send 32 data bits (MSB first)
    for (int i = 31; i >= 0; i--) {
        opentherm_send_bit(ot, (data >> i) & 1, pin);
    }
    
    // Send stop bit
    opentherm_send_bit(ot, 1, pin);
    
    // Return to idle (high)
    gpio_set_level(pin, 1);
}

/**
 * Disable all GPIO interrupts for both interfaces
 * 
 * WHY: When transitioning between states or processing a captured frame,
 * we must ensure no new interrupts fire that could corrupt the current
 * frame data or cause race conditions. Always disable before reading
 * captured data or changing receive direction.
 */
static void opentherm_disable_all_inputs(OpenTherm *ot)
{
    if (ot->in_pin != GPIO_NUM_NC) {
        gpio_intr_disable(ot->in_pin);
    }

    if (ot->gateway_mode && ot->in_pin_secondary != GPIO_NUM_NC) {
        gpio_intr_disable(ot->in_pin_secondary);
    }
}

/**
 * Prepare to receive a frame from specified role (thermostat or boiler)
 * 
 * WHY: OpenTherm uses Manchester encoding on a single bidirectional wire per interface.
 * We can't listen to both sides simultaneously - we must explicitly choose which
 * device we're listening to and enable only that GPIO interrupt.
 * 
 * WHAT: 
 *   1. Disable all interrupts (prevent race conditions)
 *   2. Clear frame reception state (prepare for new 32-bit frame)
 *   3. Select correct input pin (master=thermostat, slave=boiler)
 *   4. Enable interrupt on that pin only
 *   5. Start timeout timer
 * 
 * The ISR (opentherm_isr_handler) will capture Manchester edges and reconstruct
 * the 32-bit frame, then set status to GATEWAY_REQUEST_READY or GATEWAY_RESPONSE_READY.
 * 
 * @param role OT_ROLE_MASTER to listen for thermostat, OT_ROLE_SLAVE for boiler
 */
static void opentherm_prepare_receive(OpenTherm *ot, OpenThermRole role)
{
    opentherm_disable_all_inputs(ot);

    // Clear frame reception state for new capture
    ot->response = 0;
    ot->response_bit_index = 0;
    ot->response_timestamp = 0;
    ot->current_receive_role = role;
    ot->current_receive_pin = (role == OT_ROLE_MASTER) ? ot->in_pin : ot->in_pin_secondary;

    if (ot->current_receive_pin == GPIO_NUM_NC) {
        ESP_LOGE(TAG, "Gateway receive pin not configured for role %d", role);
        ot->status = OT_STATUS_TIMEOUT;
        return;
    }

    // Set status so ISR knows what type of frame it's capturing
    if (role == OT_ROLE_MASTER) {
        ot->status = OT_STATUS_GATEWAY_REQUEST_WAITING;
    } else {
        ot->status = OT_STATUS_GATEWAY_RESPONSE_WAITING;
    }

    // Enable interrupt on selected pin - ISR will handle Manchester decoding
    gpio_set_intr_type(ot->current_receive_pin, GPIO_INTR_ANYEDGE);
    gpio_intr_enable(ot->current_receive_pin);
    ot->gateway_timer_start = micros();
}

// Send request (master role)
bool opentherm_send_request(OpenTherm *ot, OpenThermMessage *request)
{
    if (ot->status != OT_STATUS_READY) {
        return false;
    }
    
    ot->status = OT_STATUS_REQUEST_SENDING;
    opentherm_send_frame(ot, request->data, ot->out_pin);
    ot->status = OT_STATUS_RESPONSE_WAITING;
    ot->response = 0;
    ot->response_bit_index = 0;
    
    // Wait for response with timeout
    unsigned long start_time = micros();
    while (ot->status == OT_STATUS_RESPONSE_WAITING) {
        if (micros() - start_time > OT_TIMING_RESPONSE_TIMEOUT) {
            ot->status = OT_STATUS_TIMEOUT;
            ESP_LOGW(TAG, "Response timeout");
            return false;
        }
        vTaskDelay(1);
    }
    
    if (ot->status == OT_STATUS_RESPONSE_READY) {
        ot->status = OT_STATUS_READY;
        return true;
    }
    
    return false;
}

// Send response (slave role)
bool opentherm_send_response(OpenTherm *ot, OpenThermMessage *response)
{
    if (ot->status != OT_STATUS_READY) {
        return false;
    }
    
    opentherm_send_frame(ot, response->data, ot->out_pin);
    return true;
}

/**
 * Reset gateway to a clean idle state
 * 
 * WHY: When starting the gateway or recovering from errors, we need a known
 * clean state with all interrupts disabled. This prevents spurious edges or
 * partially-received frames from corrupting the state machine.
 * 
 * WHAT: Disables all GPIO interrupts, clears state flags, and prepares for
 * the next thermostat request by returning to IDLE state.
 */
void opentherm_gateway_reset(OpenTherm *ot)
{
    if (ot == NULL || !ot->gateway_mode) {
        return;
    }

    // Disable all input interrupts to prevent race conditions during reset
    opentherm_disable_all_inputs(ot);

    ot->gateway_state = OT_GATEWAY_STATE_IDLE;
    ot->status = OT_STATUS_READY;
    ot->current_receive_pin = GPIO_NUM_NC;
    ot->current_receive_role = OT_ROLE_MASTER;
    ot->gateway_timer_start = micros();
    ot->gateway_timeout_flag = false;
}

/**
 * Process gateway state machine - the heart of OpenTherm MITM proxying
 * 
 * WHY: OpenTherm is a master-slave protocol where the thermostat (master) sends
 * requests and expects timely responses. To act as a transparent proxy (MITM),
 * we must:
 *   1. Listen for thermostat requests on the master interface
 *   2. Forward each request to the boiler on the slave interface
 *   3. Capture the boiler's response
 *   4. Forward the response back to the thermostat
 * All within strict timing constraints (800ms response timeout per OpenTherm spec).
 * 
 * WHAT: State machine with three states:
 *   - IDLE: Initial state, transitions immediately to WAITING_REQUEST
 *   - WAITING_REQUEST: GPIO interrupt enabled on master input, waiting for thermostat
 *   - WAITING_RESPONSE: After forwarding request, waiting for boiler response
 * 
 * Call this repeatedly (e.g. every 1ms) from main loop. Returns true when a complete
 * request/response pair has been proxied, allowing caller to log or process the transaction.
 * 
 * @return true if a complete request->response cycle completed this iteration
 */
bool opentherm_gateway_process(OpenTherm *ot, OpenThermMessage *request, OpenThermMessage *response)
{
    if (ot == NULL || !ot->gateway_mode) {
        return false;
    }

    unsigned long now = micros();

    switch (ot->gateway_state) {
        case OT_GATEWAY_STATE_IDLE:
            // Start by listening for thermostat requests
            opentherm_prepare_receive(ot, OT_ROLE_MASTER);
            ot->gateway_state = OT_GATEWAY_STATE_WAITING_REQUEST;
            break;

        case OT_GATEWAY_STATE_WAITING_REQUEST:
            // WHY: Thermostat sends requests periodically (typically every 1 second).
            // We must capture the entire 32-bit OpenTherm frame via ISR edge detection.
            if (ot->status == OT_STATUS_GATEWAY_REQUEST_READY) {
                // Complete frame captured from thermostat
                opentherm_disable_all_inputs(ot);
                ot->gateway_request.data = ot->response;  // ISR stores frame in 'response' field
                ot->gateway_timeout_flag = false;

                // Log the request (direction: from thermostat/master)
                if (ot->message_callback) {
                    ot->message_callback(ot, &ot->gateway_request, OT_ROLE_MASTER);
                }

                // Forward request to boiler immediately to meet OpenTherm timing requirements
                if (ot->out_pin_secondary != GPIO_NUM_NC) {
                    opentherm_send_frame(ot, ot->gateway_request.data, ot->out_pin_secondary);
                } else {
                    ESP_LOGE(TAG, "Gateway secondary output pin not configured");
                }

                // Now listen for boiler response
                opentherm_prepare_receive(ot, OT_ROLE_SLAVE);
                ot->gateway_state = OT_GATEWAY_STATE_WAITING_RESPONSE;

            } else if (now - ot->gateway_timer_start > OT_TIMING_GATEWAY_REQUEST_TIMEOUT) {
                // WHY: Re-arm if we've been idle too long. This handles spurious edges
                // or cases where the thermostat hasn't sent anything recently.
                // Keeps the state machine responsive and prevents stuck states.
                opentherm_prepare_receive(ot, OT_ROLE_MASTER);
            }
            break;

        case OT_GATEWAY_STATE_WAITING_RESPONSE:
            // WHY: Boiler must respond within 800ms per OpenTherm spec, or thermostat
            // will time out and may enter error state. We forward the response as soon
            // as it's complete.
            if (ot->status == OT_STATUS_GATEWAY_RESPONSE_READY) {
                // Complete response captured from boiler
                opentherm_disable_all_inputs(ot);
                ot->gateway_response.data = ot->response;
                ot->gateway_timeout_flag = false;

                // Log the response (direction: from boiler/slave)
                if (ot->message_callback) {
                    ot->message_callback(ot, &ot->gateway_response, OT_ROLE_SLAVE);
                }

                // Forward response back to thermostat to complete the transaction
                if (ot->out_pin != GPIO_NUM_NC) {
                    opentherm_send_frame(ot, ot->gateway_response.data, ot->out_pin);
                }

                // Return captured messages to caller for additional logging/validation
                if (request) {
                    *request = ot->gateway_request;
                }
                if (response) {
                    *response = ot->gateway_response;
                }

                // Transaction complete - return to waiting for next thermostat request
                opentherm_prepare_receive(ot, OT_ROLE_MASTER);
                ot->gateway_state = OT_GATEWAY_STATE_WAITING_REQUEST;
                return true;  // Signal: complete transaction proxied

            } else if (now - ot->gateway_timer_start > OT_TIMING_RESPONSE_TIMEOUT) {
                // WHY: Boiler failed to respond in time. We must recover gracefully
                // and not leave the thermostat waiting forever. Set timeout flag for
                // telemetry/diagnostics, then return to listening for next request.
                opentherm_disable_all_inputs(ot);
                ESP_LOGW(TAG, "Gateway timeout waiting for boiler response");
                ot->gateway_timeout_flag = true;
                opentherm_prepare_receive(ot, OT_ROLE_MASTER);
                ot->gateway_state = OT_GATEWAY_STATE_WAITING_REQUEST;
            }
            break;

        default:
            break;
    }

    return false;  // No complete transaction this iteration
}

// Get last response
bool opentherm_get_last_response(OpenTherm *ot, OpenThermMessage *response)
{
    if (response) {
        response->data = ot->response;
    }
    return true;
}

// Get status
OpenThermStatus opentherm_get_status(OpenTherm *ot)
{
    return ot->status;
}

// Build request message
uint32_t opentherm_build_request(OpenThermMessageType type, uint8_t id, uint16_t data)
{
    uint32_t request = ((uint32_t)type << 28) | ((uint32_t)id << 16) | data;
    
    // Calculate parity bit
    uint8_t parity = 0;
    for (int i = 0; i < 32; i++) {
        if (request & ((uint32_t)1 << i)) {
            parity ^= 1;
        }
    }
    request |= ((uint32_t)parity << 31);
    
    return request;
}

// Build response message
uint32_t opentherm_build_response(OpenThermMessageType type, uint8_t id, uint16_t data)
{
    return opentherm_build_request(type, id, data);
}

// Get message type
OpenThermMessageType opentherm_get_message_type(uint32_t message)
{
    return (OpenThermMessageType)((message >> 28) & 0x7);
}

// Get data ID
uint8_t opentherm_get_data_id(uint32_t message)
{
    return (uint8_t)((message >> 16) & 0xFF);
}

// Get uint16 value
uint16_t opentherm_get_uint16(uint32_t message)
{
    return (uint16_t)(message & 0xFFFF);
}

// Get float value (f8.8 format)
float opentherm_get_float(uint32_t message)
{
    uint16_t data = opentherm_get_uint16(message);
    return (float)((int16_t)data) / 256.0f;
}

// Get uint8 value (HB)
uint8_t opentherm_get_uint8(uint32_t message)
{
    return (uint8_t)((message >> 8) & 0xFF);
}

// Get int8 value (LB)
int8_t opentherm_get_int8(uint32_t message)
{
    return (int8_t)(message & 0xFF);
}

// Validate response
bool opentherm_is_valid_response(uint32_t request, uint32_t response)
{
    uint8_t request_id = opentherm_get_data_id(request);
    uint8_t response_id = opentherm_get_data_id(response);
    
    return request_id == response_id;
}

// Message type to string
const char* opentherm_message_type_to_string(OpenThermMessageType type)
{
    switch (type) {
        case OT_MSGTYPE_READ_DATA: return "READ_DATA";
        case OT_MSGTYPE_WRITE_DATA: return "WRITE_DATA";
        case OT_MSGTYPE_INVALID_DATA: return "INVALID_DATA";
        case OT_MSGTYPE_RESERVED: return "RESERVED";
        case OT_MSGTYPE_READ_ACK: return "READ_ACK";
        case OT_MSGTYPE_WRITE_ACK: return "WRITE_ACK";
        case OT_MSGTYPE_DATA_INVALID: return "DATA_INVALID";
        case OT_MSGTYPE_UNKNOWN_DATAID: return "UNKNOWN_DATAID";
        default: return "UNKNOWN";
    }
}

// Status to string
const char* opentherm_status_to_string(OpenThermStatus status)
{
    switch (status) {
        case OT_STATUS_NONE: return "NONE";
        case OT_STATUS_READY: return "READY";
        case OT_STATUS_REQUEST_SENDING: return "REQUEST_SENDING";
        case OT_STATUS_RESPONSE_WAITING: return "RESPONSE_WAITING";
        case OT_STATUS_RESPONSE_START_BIT: return "RESPONSE_START_BIT";
        case OT_STATUS_RESPONSE_RECEIVING: return "RESPONSE_RECEIVING";
        case OT_STATUS_RESPONSE_READY: return "RESPONSE_READY";
        case OT_STATUS_RESPONSE_INVALID: return "RESPONSE_INVALID";
        case OT_STATUS_TIMEOUT: return "TIMEOUT";
        case OT_STATUS_GATEWAY_REQUEST_WAITING: return "GATEWAY_REQUEST_WAITING";
        case OT_STATUS_GATEWAY_REQUEST_START_BIT: return "GATEWAY_REQUEST_START_BIT";
        case OT_STATUS_GATEWAY_REQUEST_RECEIVING: return "GATEWAY_REQUEST_RECEIVING";
        case OT_STATUS_GATEWAY_REQUEST_READY: return "GATEWAY_REQUEST_READY";
        case OT_STATUS_GATEWAY_RESPONSE_WAITING: return "GATEWAY_RESPONSE_WAITING";
        case OT_STATUS_GATEWAY_RESPONSE_START_BIT: return "GATEWAY_RESPONSE_START_BIT";
        case OT_STATUS_GATEWAY_RESPONSE_RECEIVING: return "GATEWAY_RESPONSE_RECEIVING";
        case OT_STATUS_GATEWAY_RESPONSE_READY: return "GATEWAY_RESPONSE_READY";
        default: return "UNKNOWN";
    }
}

// Enable/disable ISR debug logging to ring buffer
void opentherm_isr_debug_enable(OpenTherm *ot, bool enable)
{
    ot->isr_debug_enabled = enable;
    if (enable) {
        // Clear buffer on enable
        ot->isr_debug_write_idx = 0;
        ot->isr_debug_read_idx = 0;
    }
}

// Flush ISR debug buffer to ESP_LOGI (call from main loop, not ISR!)
void opentherm_isr_debug_flush(OpenTherm *ot)
{
    while (ot->isr_debug_read_idx != ot->isr_debug_write_idx) {
        uint32_t read_idx = ot->isr_debug_read_idx;
        opentherm_isr_debug_entry_t *entry = &ot->isr_debug_buffer[read_idx];
        
        const char *event_str;
        switch (entry->event_type) {
            case 0: event_str = "Edge"; break;
            case 1: event_str = "SAMPLE"; break;
            case 2: event_str = "COMPLETE"; break;
            default: event_str = "?"; break;
        }
        
        ESP_LOGI(TAG, "ISR[%lu]: %s bit=%d pin=%d elapsed=%luµs resp=0x%lX status=%d",
                 entry->timestamp, event_str, entry->bit_index, entry->pin_level,
                 entry->elapsed, entry->response, entry->status);
        
        ot->isr_debug_read_idx = (read_idx + 1) % OT_ISR_DEBUG_BUFFER_SIZE;
    }
}

