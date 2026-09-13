#ifndef EPAPER_PORT_H
#define EPAPER_PORT_H


/**********************************
Color Index
**********************************/
#define EPD_1IN54G_BLACK   0x0 
#define EPD_1IN54G_WHITE   0x1 
#define EPD_1IN54G_YELLOW  0x2 
#define EPD_1IN54G_RED     0x3 
// #define EPD_1IN54G_ORANGE  0x4 
#define EPD_1IN54G_BLUE    0x5 
#define EPD_1IN54G_GREEN   0x6 


#define EXAMPLE_LCD_WIDTH  200
#define EXAMPLE_LCD_HEIGHT 200


/**********************************
Color Index
**********************************/
#define EPD_1IN54G_BLACK   0x0   /// 000
#define EPD_1IN54G_WHITE   0x1   /// 001
#define EPD_1IN54G_YELLOW  0x2   /// 010
#define EPD_1IN54G_RED     0x3   /// 011
// #define EPD_1IN54G_ORANGE  0x4   /// 100
#define EPD_1IN54G_BLUE    0x5   /// 101
#define EPD_1IN54G_GREEN   0x6   /// 110

#ifdef __cplusplus
extern "C" {
#endif


void epaper_port_init(void);
void epaper_port_clear(uint8_t color);
void epaper_port_display(const uint8_t *Image);
void epaper_port_sleep(void);


#ifdef __cplusplus
}
#endif


#endif // !EPAPER_PORT_H
