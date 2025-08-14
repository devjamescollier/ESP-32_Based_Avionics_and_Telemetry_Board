/* ESP32-Based Avionics Board (Rev A) bring-up diagnostics (ESP-IDF).
 * Safety: every actuator output is driven LOW before anything else runs and stays LOW
 * unless the operator explicitly requests a test over the serial console.
 * The ESC output (IO13) is only ever driven with minimum-throttle pulses (1000 us),
 * and only by the explicit 'e' bench arming test; nothing in this firmware can spin the motor. */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "driver/uart.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_system.h"
#include "esp_chip_info.h"

/* pin map: ESP32-Based Avionics Board Rev A schematic */
#define PIN_SERVO1   GPIO_NUM_26
#define PIN_SERVO2   GPIO_NUM_27
#define PIN_SERVO3   GPIO_NUM_23
#define PIN_SERVO4   GPIO_NUM_4
#define PIN_ESC      GPIO_NUM_13
#define PIN_BUZZER   GPIO_NUM_12
#define PIN_LED_G    GPIO_NUM_14
#define PIN_LED_W    GPIO_NUM_2
#define PIN_SDA      GPIO_NUM_21
#define PIN_SCL      GPIO_NUM_22
#define SPI1_SCK     GPIO_NUM_25   /* IMU2 ASM330LHH */
#define SPI1_MISO    GPIO_NUM_32
#define SPI1_MOSI    GPIO_NUM_33
#define IMU2_CS      GPIO_NUM_15
#define SPI2_SCK     GPIO_NUM_17   /* microSD + IMU1 LSM6DSV32X */
#define SPI2_MISO    GPIO_NUM_18
#define SPI2_MOSI    GPIO_NUM_5
#define SD_CS        GPIO_NUM_16
#define IMU1_CS      GPIO_NUM_19
#define IMU1_INT1    GPIO_NUM_34
#define IMU2_INT1    GPIO_NUM_35
#define VSENSE_CH    ADC_CHANNEL_0  /* IO36 */
#define VSENSE_RATIO ((100.0f + 39.0f) / 39.0f)

static const gpio_num_t ACTUATORS[] = {PIN_SERVO1, PIN_SERVO2, PIN_SERVO3, PIN_SERVO4, PIN_ESC, PIN_BUZZER};
static const gpio_num_t SERVOS[] = {PIN_SERVO1, PIN_SERVO2, PIN_SERVO3, PIN_SERVO4};
static spi_device_handle_t imu1, imu2;
static i2c_master_bus_handle_t i2c;

static void actuators_safe(void)
{
    for (int i = 0; i < sizeof(ACTUATORS) / sizeof(ACTUATORS[0]); i++) {
        gpio_reset_pin(ACTUATORS[i]);
        gpio_set_level(ACTUATORS[i], 0);
        gpio_set_direction(ACTUATORS[i], GPIO_MODE_OUTPUT);
        gpio_set_level(ACTUATORS[i], 0);
    }
    gpio_reset_pin(PIN_LED_G); gpio_set_direction(PIN_LED_G, GPIO_MODE_OUTPUT); gpio_set_level(PIN_LED_G, 0);
    gpio_reset_pin(PIN_LED_W); gpio_set_direction(PIN_LED_W, GPIO_MODE_OUTPUT); gpio_set_level(PIN_LED_W, 0);
}

static float read_vsense(void)
{
    static adc_oneshot_unit_handle_t adc;
    static adc_cali_handle_t cali;
    if (!adc) {
        adc_oneshot_unit_init_cfg_t u = {.unit_id = ADC_UNIT_1};
        ESP_ERROR_CHECK(adc_oneshot_new_unit(&u, &adc));
        adc_oneshot_chan_cfg_t c = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
        ESP_ERROR_CHECK(adc_oneshot_config_channel(adc, VSENSE_CH, &c));
        adc_cali_line_fitting_config_t cc = {.unit_id = ADC_UNIT_1, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
        if (adc_cali_create_scheme_line_fitting(&cc, &cali) != ESP_OK) cali = NULL;
    }
    int raw, mv = 0, sum = 0;
    for (int i = 0; i < 16; i++) {
        adc_oneshot_read(adc, VSENSE_CH, &raw);
        if (cali) adc_cali_raw_to_voltage(cali, raw, &mv); else mv = raw * 2450 / 4095;
        sum += mv;
    }
    return sum / 16.0f / 1000.0f * VSENSE_RATIO;
}

static esp_err_t i2c_read(uint8_t addr, uint8_t reg, uint8_t *buf, size_t n)
{
    i2c_device_config_t d = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addr, .scl_speed_hz = 100000};
    i2c_master_dev_handle_t h;
    esp_err_t e = i2c_master_bus_add_device(i2c, &d, &h);
    if (e != ESP_OK) return e;
    e = i2c_master_transmit_receive(h, &reg, 1, buf, n, 50);
    i2c_master_bus_rm_device(h);
    return e;
}

static void test_i2c(void)
{
    printf("I2C scan (SDA IO21, SCL IO22):");
    int found = 0;
    for (uint8_t a = 0x08; a < 0x78; a++)
        if (i2c_master_probe(i2c, a, 20) == ESP_OK) { printf(" 0x%02X", a); found++; }
    printf("%s\n", found ? "" : " none");
    uint8_t who = 0;
    for (uint8_t a = 0x1C; a <= 0x1E; a += 2)
        if (i2c_read(a, 0x0F, &who, 1) == ESP_OK)
            printf("  LIS3MDL @0x%02X WHO_AM_I=0x%02X (%s)\n", a, who, who == 0x3D ? "PASS" : "FAIL, expect 0x3D");
    /* MS5611: reset, then PROM words 0..7 with CRC-4 (AN520) */
    i2c_device_config_t d = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x77, .scl_speed_hz = 100000};
    i2c_master_dev_handle_t ms;
    if (i2c_master_bus_add_device(i2c, &d, &ms) == ESP_OK) {
        uint8_t cmd = 0x1E; uint16_t prom[8] = {0};
        if (i2c_master_transmit(ms, &cmd, 1, 50) == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(5));
            for (int i = 0; i < 8; i++) {
                uint8_t c = 0xA0 + 2 * i, b[2];
                if (i2c_master_transmit_receive(ms, &c, 1, b, 2, 50) == ESP_OK) prom[i] = (b[0] << 8) | b[1];
            }
            uint16_t crc_read = prom[7] & 0xF, rem = 0, p7 = prom[7];
            prom[7] &= 0xFF00;
            for (int cnt = 0; cnt < 16; cnt++) {
                rem ^= (cnt & 1) ? (prom[cnt >> 1] & 0x00FF) : (prom[cnt >> 1] >> 8);
                for (int bit = 8; bit > 0; bit--) rem = (rem & 0x8000) ? (rem << 1) ^ 0x3000 : (rem << 1);
            }
            rem = (rem >> 12) & 0xF; prom[7] = p7;
            printf("  MS5611 @0x77 PROM C1..C6 = %u %u %u %u %u %u  CRC %s\n", prom[1], prom[2], prom[3], prom[4], prom[5], prom[6],
                   rem == crc_read ? "PASS" : "FAIL");
        } else printf("  MS5611 @0x77 no response (FAIL)\n");
        i2c_master_bus_rm_device(ms);
    }
}

static uint8_t spi_reg(spi_device_handle_t dev, uint8_t reg)
{
    spi_transaction_t t = {.flags = SPI_TRANS_USE_RXDATA | SPI_TRANS_USE_TXDATA, .length = 16};
    t.tx_data[0] = 0x80 | reg; t.tx_data[1] = 0;
    if (spi_device_polling_transmit(dev, &t) != ESP_OK) return 0;
    return t.rx_data[1];
}

static void test_imus(void)
{
    uint8_t w2 = spi_reg(imu2, 0x0F), w1 = spi_reg(imu1, 0x0F);
    printf("IMU2 ASM330LHH  (SPI1, CS IO15) WHO_AM_I=0x%02X %s\n", w2, w2 == 0x6B ? "PASS" : "FAIL, expect 0x6B");
    printf("IMU1 LSM6DSV32X (SPI2, CS IO19) WHO_AM_I=0x%02X %s\n", w1, w1 == 0x70 ? "PASS" : "FAIL, expect 0x70");
    printf("INT1 levels: IMU1 IO34=%d IMU2 IO35=%d\n", gpio_get_level(IMU1_INT1), gpio_get_level(IMU2_INT1));
}

static void test_sd(void)
{
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI3_HOST; host.max_freq_khz = 4000;
    sdspi_device_config_t sc = SDSPI_DEVICE_CONFIG_DEFAULT();
    sc.gpio_cs = SD_CS; sc.host_id = SPI3_HOST;
    sdspi_dev_handle_t h;
    if (sdspi_host_init() != ESP_OK || sdspi_host_init_device(&sc, &h) != ESP_OK) { printf("microSD: host init FAIL\n"); return; }
    host.slot = h;
    sdmmc_card_t card;
    esp_err_t e = sdmmc_card_init(&host, &card);
    if (e == ESP_OK) { printf("microSD: PASS\n"); sdmmc_card_print_info(stdout, &card); }
    else printf("microSD: no card / FAIL (%s)\n", esp_err_to_name(e));
    sdspi_host_remove_device(h);
}

static void servo_neutral_test(void)
{
    printf("Servo test: 1500 us pulses at 50 Hz on S1..S4 for 3 s each. ESC is NOT driven.\n"
           "Remove propellers. Type YES and Enter to continue: ");
    fflush(stdout);
    char line[8] = {0}; int n = 0;
    while (n < 7) {
        uint8_t c;
        if (uart_read_bytes(UART_NUM_0, &c, 1, pdMS_TO_TICKS(15000)) <= 0) break;
        if (c == '\r' || c == '\n') break;
        line[n++] = c;
    }
    printf("\n");
    if (strcmp(line, "YES")) { printf("aborted\n"); return; }
    printf("SERVO_PWR = %.2f V\n", read_vsense());
    ledc_timer_config_t tc = {.speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_14_BIT, .timer_num = LEDC_TIMER_0, .freq_hz = 50, .clk_cfg = LEDC_AUTO_CLK};
    ESP_ERROR_CHECK(ledc_timer_config(&tc));
    for (int i = 0; i < 4; i++) {
        ledc_channel_config_t cc = {.gpio_num = SERVOS[i], .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0,
                                    .duty = (uint32_t)(1500.0 / 20000.0 * (1 << 14))};
        printf("  S%d ...\n", i + 1);
        ESP_ERROR_CHECK(ledc_channel_config(&cc));
        vTaskDelay(pdMS_TO_TICKS(3000));
        ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
        gpio_reset_pin(SERVOS[i]); gpio_set_direction(SERVOS[i], GPIO_MODE_OUTPUT); gpio_set_level(SERVOS[i], 0);
    }
    printf("servo test done, outputs LOW\n");
}

static bool confirm(const char *word)
{
    char line[8] = {0}; int n = 0;
    while (n < 7) {
        uint8_t c;
        if (uart_read_bytes(UART_NUM_0, &c, 1, pdMS_TO_TICKS(15000)) <= 0) break;
        if (c == '\r' || c == '\n') break;
        line[n++] = c;
    }
    printf("\n");
    return strcmp(line, word) == 0;
}

static void esc_arm_test(void)
{
    printf("ESC arming test: 1000 us (minimum throttle) at 50 Hz for 5 s on IO13, then output LOW.\n"
           "PROPELLER REMOVED? ESC powered from the battery via the Y-harness? Type ARM and Enter: ");
    fflush(stdout);
    if (!confirm("ARM")) { printf("aborted\n"); return; }
    ledc_timer_config_t tc = {.speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_14_BIT, .timer_num = LEDC_TIMER_1, .freq_hz = 50, .clk_cfg = LEDC_AUTO_CLK};
    ESP_ERROR_CHECK(ledc_timer_config(&tc));
    ledc_channel_config_t cc = {.gpio_num = PIN_ESC, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_1, .timer_sel = LEDC_TIMER_1,
                                .duty = (uint32_t)(1000.0 / 20000.0 * (1 << 14))};
    ESP_ERROR_CHECK(ledc_channel_config(&cc));
    printf("  sending 1000 us ... listen for the ESC arming tones\n");
    vTaskDelay(pdMS_TO_TICKS(5000));
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 0);
    gpio_reset_pin(PIN_ESC); gpio_set_direction(PIN_ESC, GPIO_MODE_OUTPUT); gpio_set_level(PIN_ESC, 0);
    printf("ESC test done, IO13 LOW\n");
}

static void report(void)
{
    esp_chip_info_t ci; esp_chip_info(&ci);
    printf("\n=== ESP32-Based Avionics Board Rev A diagnostics ===\nchip rev %d, %d cores, reset reason %d\n", ci.revision, ci.cores, esp_reset_reason());
    float v = read_vsense();
    printf("SERVO_PWR (V_SENSE x %.3f) = %.2f V %s\n", VSENSE_RATIO, v, v < 1.0f ? "(switch off / USB only)" : v < 6.0f ? "LOW BATTERY" : "");
    test_i2c(); test_imus(); test_sd();
    printf("commands: r=report  b=beep 200ms  l=LED test  s=servo neutral test  e=ESC arming test (min throttle only)\n");
}

void app_main(void)
{
    actuators_safe();
    uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);

    i2c_master_bus_config_t bc = {.i2c_port = I2C_NUM_0, .sda_io_num = PIN_SDA, .scl_io_num = PIN_SCL, .clk_source = I2C_CLK_SRC_DEFAULT,
                                  .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = false};
    ESP_ERROR_CHECK(i2c_new_master_bus(&bc, &i2c));

    spi_bus_config_t b1 = {.sclk_io_num = SPI1_SCK, .miso_io_num = SPI1_MISO, .mosi_io_num = SPI1_MOSI, .quadwp_io_num = -1, .quadhd_io_num = -1};
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &b1, SPI_DMA_DISABLED));
    spi_device_interface_config_t d2 = {.clock_speed_hz = 1000000, .mode = 3, .spics_io_num = IMU2_CS, .queue_size = 1};
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &d2, &imu2));

    /* microSD and IMU1 share this bus; IMU1 CS is held high by R4 during SD init */
    spi_bus_config_t b2 = {.sclk_io_num = SPI2_SCK, .miso_io_num = SPI2_MISO, .mosi_io_num = SPI2_MOSI, .quadwp_io_num = -1, .quadhd_io_num = -1,
                           .max_transfer_sz = 4096};
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &b2, SPI_DMA_CH_AUTO));
    spi_device_interface_config_t d1 = {.clock_speed_hz = 1000000, .mode = 3, .spics_io_num = IMU1_CS, .queue_size = 1};
    ESP_ERROR_CHECK(spi_bus_add_device(SPI3_HOST, &d1, &imu1));
    /* LSM6DSV32X: disable its I2C/I3C interface (IF_CFG bit0 I2C_I3C_disable) so SD traffic on the shared bus cannot confuse it */
    spi_transaction_t t = {.flags = SPI_TRANS_USE_TXDATA, .length = 16};
    t.tx_data[0] = 0x03; t.tx_data[1] = 0x01;
    spi_device_polling_transmit(imu1, &t);

    gpio_set_direction(IMU1_INT1, GPIO_MODE_INPUT);
    gpio_set_direction(IMU2_INT1, GPIO_MODE_INPUT);
    report();
    for (;;) {
        uint8_t c;
        if (uart_read_bytes(UART_NUM_0, &c, 1, pdMS_TO_TICKS(1000)) <= 0) continue;
        switch (c) {
        case 'r': report(); break;
        case 'b': gpio_set_level(PIN_BUZZER, 1); vTaskDelay(pdMS_TO_TICKS(200)); gpio_set_level(PIN_BUZZER, 0); printf("beep\n"); break;
        case 'l':
            gpio_set_level(PIN_LED_G, 1); vTaskDelay(pdMS_TO_TICKS(500)); gpio_set_level(PIN_LED_G, 0);
            gpio_set_level(PIN_LED_W, 1); vTaskDelay(pdMS_TO_TICKS(500)); gpio_set_level(PIN_LED_W, 0);
            printf("LEDs: green then white\n"); break;
        case 's': servo_neutral_test(); break;
        case 'e': esc_arm_test(); break;
        default: break;
        }
    }
}
