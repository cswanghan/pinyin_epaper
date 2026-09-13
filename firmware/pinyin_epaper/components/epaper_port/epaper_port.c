#include <stdio.h>
#include <string.h>
#include "epaper_port.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#define EPD_DC_PIN 10
#define EPD_CS_PIN 11
#define EPD_SCK_PIN 12
#define EPD_MOSI_PIN 13
#define EPD_RST_PIN 9
#define EPD_BUSY_PIN 8

#define epaper_rst_1 gpio_set_level(EPD_RST_PIN, 1)
#define epaper_rst_0 gpio_set_level(EPD_RST_PIN, 0)
#define epaper_cs_1 gpio_set_level(EPD_CS_PIN, 1)
#define epaper_cs_0 gpio_set_level(EPD_CS_PIN, 0)
#define epaper_dc_1 gpio_set_level(EPD_DC_PIN, 1)
#define epaper_dc_0 gpio_set_level(EPD_DC_PIN, 0)
#define ReadBusy gpio_get_level(EPD_BUSY_PIN)

static spi_device_handle_t spi;
static void                spi_send_byte(uint8_t cmd);

static void epaper_gpio_init(void) {
    gpio_config_t gpio_conf = {};
    gpio_conf.intr_type     = GPIO_INTR_DISABLE;
    gpio_conf.mode          = GPIO_MODE_OUTPUT;
    gpio_conf.pin_bit_mask  = ((uint64_t) 0x01 << EPD_RST_PIN) | ((uint64_t) 0x01 << EPD_DC_PIN) | ((uint64_t) 0x01 << EPD_CS_PIN);
    gpio_conf.pull_down_en  = GPIO_PULLDOWN_DISABLE;
    gpio_conf.pull_up_en    = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio_conf));

    gpio_conf.mode         = GPIO_MODE_INPUT;
    gpio_conf.pin_bit_mask = ((uint64_t) 0x01 << EPD_BUSY_PIN);
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio_conf));

    epaper_rst_1;
}

/*Pinout of the 1.54-inch e-Paper (G) module by Waveshare Electronics*/

/*
  Ink screen reset
*/
static void epaper_reset(void) {
    epaper_rst_1;
    vTaskDelay(pdMS_TO_TICKS(200));
    epaper_rst_0;
    vTaskDelay(pdMS_TO_TICKS(20));
    epaper_rst_1;
    vTaskDelay(pdMS_TO_TICKS(200));
}

static void epaper_spi_init(void) {
    esp_err_t ret;
    spi_bus_config_t buscfg = 
    {
        .miso_io_num = -1,
        .mosi_io_num = EPD_MOSI_PIN,
        .sclk_io_num = EPD_SCK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 65536,  
    };
    spi_device_interface_config_t devcfg = 
    {
        .spics_io_num = -1,
        .clock_speed_hz = 20 * 1000 * 1000, 
        .mode = 0,
        .queue_size = 1, 
    };
    
    ret = spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO);
    ESP_ERROR_CHECK(ret);
    ret = spi_bus_add_device(SPI3_HOST, &devcfg, &spi);
    ESP_ERROR_CHECK(ret);

    epaper_gpio_init();
}

/*
  Waiting for the idle signal
*/
static void epaper_readbusyh(void) {
    ESP_LOGI("EPD","e-Paper busy");
    vTaskDelay(pdMS_TO_TICKS(100));
    while (1) {
        if (ReadBusy) {
            ESP_LOGI("EPD","e-Paper busy release");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/*
Send byte command
*/
static void epaper_SendCommand(uint8_t Reg) {
    epaper_dc_0;
    epaper_cs_0;
    spi_send_byte(Reg);
    epaper_cs_1;
}

/*
Send byte data
*/
void epaper_SendData(uint8_t Data) {
    epaper_dc_1;
    epaper_cs_0;
    spi_send_byte(Data);
    epaper_cs_1;
}

/*send bytes data*/
void epaper_Sendbuffera(uint8_t *Data, int len) {
    epaper_dc_1;
    epaper_cs_0;
    esp_err_t         ret;
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    int      len_scl = len / 5000; //Send 5,000 bytes
    int      len_dcl = len % 5000;
    uint8_t *ptr     = Data;
    while (len_scl) {
        t.length    = 8 * 5000;
        t.tx_buffer = ptr;
        ret         = spi_device_polling_transmit(spi, &t); //Transmit!
        assert(ret == ESP_OK);                              //Should have had no issues
        len_scl--;
        ptr += 5000;
    }
    if (len_dcl > 0) {
        t.length    = 8 * len_dcl;
        t.tx_buffer = ptr;
        ret         = spi_device_polling_transmit(spi, &t);
        assert(ret == ESP_OK);
    }

    epaper_cs_1;
}

/*
  The data has been uploaded to Buff
*/
static void epaper_TurnOnDisplay(void) {

    epaper_SendCommand(0x12); // DISPLAY_REFRESH
    epaper_SendData(0x00);
    epaper_readbusyh();
}

/*
epaper init
*/
void epaper_port_init(void) {
    epaper_spi_init();  //spi init
    epaper_gpio_init(); //ws gpio init
    epaper_reset();     //reset

    epaper_SendCommand(0x4D);
    epaper_SendData(0x78);

    epaper_SendCommand(0x00);  //PSR
    epaper_SendData(0x0F);
    epaper_SendData(0x29);

    epaper_SendCommand(0X06); //BTST_P
    epaper_SendData(0x0D);
    epaper_SendData(0x12);
    epaper_SendData(0x30);
    epaper_SendData(0x20);
    epaper_SendData(0x19);
    epaper_SendData(0x2A);
    epaper_SendData(0x22);

    epaper_SendCommand(0x50); //CDI
    epaper_SendData(0x37);

    epaper_SendCommand(0x61);
    epaper_SendData(EXAMPLE_LCD_WIDTH/256);
    epaper_SendData(EXAMPLE_LCD_WIDTH%256);
    epaper_SendData(EXAMPLE_LCD_HEIGHT/256);
    epaper_SendData(EXAMPLE_LCD_HEIGHT%256);

    epaper_SendCommand(0xE9);
    epaper_SendData(0x01);

    epaper_SendCommand(0x30);
    epaper_SendData(0x08);

    epaper_SendCommand(0x04);
    epaper_readbusyh();       //waiting for the electronic paper IC to release the idle signal
}

/*
spi send byte data
*/

static void spi_send_byte(uint8_t cmd) {
    esp_err_t         ret;
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length    = 8;
    t.tx_buffer = &cmd;
    ret         = spi_device_polling_transmit(spi, &t); //Transmit!
    assert(ret == ESP_OK);                              //Should have had no issues.
}

/*Shared function API*/

/*
clear screen
*/
void epaper_port_clear(uint8_t color) {
    uint16_t Width, Height;
    Width  = (EXAMPLE_LCD_WIDTH % 4 == 0) ? (EXAMPLE_LCD_WIDTH / 4) : (EXAMPLE_LCD_WIDTH / 4 + 1);
    Height = EXAMPLE_LCD_HEIGHT;

    epaper_SendCommand(0x10);
    for (int j = 0; j < Height; j++) {
        for (int i = 0; i < Width; i++) {
            epaper_SendData((color << 6) | (color << 4) | (color << 2) | color);
        }
    }
    epaper_TurnOnDisplay();
}

/*
display
*/
void epaper_port_display(const uint8_t *Image) {
    uint16_t Width, Height;
    Width  = (EXAMPLE_LCD_WIDTH % 4 == 0) ? (EXAMPLE_LCD_WIDTH / 4) : (EXAMPLE_LCD_WIDTH / 4 + 1);
    Height = EXAMPLE_LCD_HEIGHT;

    epaper_SendCommand(0x10);
    for (int j = 0; j < Height; j++) {
        for (int i = 0; i < Width; i++) {
            epaper_SendData(Image[i + j * Width]);
        }
    }
    epaper_TurnOnDisplay();
}

/*
sleep
*/
void epaper_port_sleep(void) {

    epaper_SendCommand(0x02);
    epaper_SendData(0x00);
    epaper_readbusyh();
    epaper_SendCommand(0x07);
    epaper_SendData(0xA5);
}