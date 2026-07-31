#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/adc.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "font.h"

// ─── Screen Configuration ────────────────────────────────────────────────────
#define I2C_SCL          GPIO_NUM_22
#define I2C_SDA          GPIO_NUM_21
#define LCD_WIDTH        128
#define LCD_HEIGHT       64
#define I2C_ADDRESS      0x3C

// ─── PWM & Potentiometer Peripherals ─────────────────────────────────────────
#define PWM_PIN          GPIO_NUM_25
#define POT_PIN          GPIO_NUM_33          
#define POT_ADC_CH       ADC1_CHANNEL_5       
#define PWM_FREQ         25000                // 25kHz standard for PC fans
#define PWM_RESOLUTION   LEDC_TIMER_10_BIT    // 10-bit = 0 to 1023
#define PWM_CHANNEL      LEDC_CHANNEL_0
#define PWM_TIMER        LEDC_TIMER_0

// ─── Tachometer Configuration ────────────────────────────────────────────────
#define TACH_PIN         GPIO_NUM_14          
#define DEBOUNCE_US      3000                 // 3ms safety window
#define ESP_INTR_FLAG_DEFAULT 0

static const char *TAG = "fan_system";

static uint8_t buf[LCD_WIDTH * LCD_HEIGHT / 8];
static esp_lcd_panel_handle_t panel;

// Volatile variables used within ISR context
static volatile uint32_t pulse_count = 0;
static volatile int global_rpm = 0;

// Spinlock initialization for safe cross-core variable sharing in v5.x
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

// ─── Display Helpers ─────────────────────────────────────────────────────────
void draw_pixel(int x, int y, int on) {
    if (x < 0 || x >= LCD_WIDTH || y < 0 || y >= LCD_HEIGHT) return;
    int byte_idx = x + (y / 8) * LCD_WIDTH;
    int bit      = y % 8;
    if (on) buf[byte_idx] |=  (1 << bit);
    else    buf[byte_idx] &= ~(1 << bit);
}

void draw_char(int x, int y, char c) {
    int idx = font_index(c);
    for (int col = 0; col < 5; col++) {
        uint8_t col_data = font5x7[idx][col];
        for (int row = 0; row < 7; row++) {
            draw_pixel(x + col, y + row, (col_data >> row) & 1);
        }
    }
}

void draw_string(int x, int y, const char *str) {
    while (*str) {
        draw_char(x, y, *str);
        x += 6;
        str++;
    }
}

void display_update(void) {
    esp_lcd_panel_draw_bitmap(panel, 0, 0, LCD_WIDTH, LCD_HEIGHT, buf);
}

void display_clear(void) {
    memset(buf, 0x00, sizeof(buf));
}

// ─── PWM Configuration ────────────────────────────────────────────────────────
void pwm_init(void) {
    ledc_timer_config_t timer_cfg = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .timer_num       = PWM_TIMER,
        .duty_resolution = PWM_RESOLUTION,
        .freq_hz         = PWM_FREQ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer_cfg);

    ledc_channel_config_t channel_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = PWM_CHANNEL,
        .timer_sel  = PWM_TIMER,
        .intr_type  = LEDC_INTR_DISABLE,
        .gpio_num   = PWM_PIN,
        .duty       = 0,
        .hpoint     = 0,
    };
    ledc_channel_config(&channel_cfg);
}

// ─── Potentiometer Configuration ─────────────────────────────────────────────
void pot_init(void) {
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(POT_ADC_CH, ADC_ATTEN_DB_12); 
}

// ─── Tachometer ISR & Initialization ─────────────────────────────────────────
static void IRAM_ATTR tach_isr_handler(void* arg) {
    static int64_t last_interrupt_time = 0;
    int64_t current_time = esp_timer_get_time();

    // Verify time gap to cleanly separate real pulses from idle line ripple
    if ((current_time - last_interrupt_time) > DEBOUNCE_US) {
        portENTER_CRITICAL_ISR(&mux);
        pulse_count++;
        portEXIT_CRITICAL_ISR(&mux);
        last_interrupt_time = current_time;
    }
}

void tach_init(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << TACH_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,     
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_POSEDGE         // Positive edge helps ignore low-state ground noise
    };
    gpio_config(&io_conf);

    // Install GPIO ISR service and allocate the interrupt handler
    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    gpio_isr_handler_add(TACH_PIN, tach_isr_handler, (void*) TACH_PIN);
}

// ─── FreeRTOS Tachometer Processing Task ─────────────────────────────────────
void tach_processing_task(void *pvParameters) {
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xPeriod = pdMS_TO_TICKS(1000); 

    while (1) {
        vTaskDelayUntil(&xLastWakeTime, xPeriod);

        portENTER_CRITICAL(&mux);
        uint32_t pulses = pulse_count;
        pulse_count = 0;
        portEXIT_CRITICAL(&mux);

        // Calculate RPM. If no pulses registered, it hits a clean 0 RPM
        global_rpm = (pulses * 60) / 2;
    }
}

// ─── Dynamic PWM Setting ──────────────────────────────────────────────────────
void set_pwm_duty(int percent) {
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;
    
    uint32_t duty = (1023 * percent) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, PWM_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, PWM_CHANNEL);
}

// ─── Main ─────────────────────────────────────────────────────────────────────
void app_main(void) {
    // 1. Initialize modern I2C Master Bus
    i2c_master_bus_handle_t i2c_bus;
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port             = I2C_NUM_0,
        .sda_io_num           = I2C_SDA,
        .scl_io_num           = I2C_SCL,
        .clk_source           = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt    = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &i2c_bus));

    // 2. Initialize LCD Intel-style IO interface
    esp_lcd_panel_io_handle_t io_handle;
    esp_lcd_panel_io_i2c_config_t io_cfg = {
        .dev_addr            = I2C_ADDRESS,
        .control_phase_bytes = 1,
        .dc_bit_offset       = 6,
        .lcd_cmd_bits        = 8,
        .lcd_param_bits      = 8,
        .scl_speed_hz        = 100000,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus, &io_cfg, &io_handle));

    // 3. Initialize physical SSD1306 Panel allocation
    esp_lcd_panel_dev_config_t panel_cfg = {
        .bits_per_pixel = 1,
        .reset_gpio_num = -1,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(io_handle, &panel_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
    
    // 4. Initialize Fan Control & Measurement Peripherals
    pwm_init();
    pot_init();
    tach_init();
    
    // 5. Spin up the dedicated asynchronous RPM computation engine
    xTaskCreate(tach_processing_task, "tach_task", 2048, NULL, 5, NULL);

    ESP_LOGI(TAG, "System running! Managing fan PWM and RPM telemetry loops.");

    int last_percent = -1;
    int last_rpm = -1;
    char duty_str[32];
    char rpm_str[32];

    while (1) {
        int adc_raw = adc1_get_raw(POT_ADC_CH);
        int percent = (adc_raw * 100) / 4095;
        
        set_pwm_duty(percent);

        int current_rpm = global_rpm;

        if (percent != last_percent || current_rpm != last_rpm) {
            display_clear();
            
            snprintf(duty_str, sizeof(duty_str), "POWER: %d%%", percent);
            snprintf(rpm_str, sizeof(rpm_str),   "SPEED: %d RPM", current_rpm);
            
            draw_string(0, 0,  "FAN CONTROLLER");
            draw_string(0, 20, duty_str);
            draw_string(0, 40, rpm_str);
            
            display_update();
            
            last_percent = percent;
            last_rpm = current_rpm;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
