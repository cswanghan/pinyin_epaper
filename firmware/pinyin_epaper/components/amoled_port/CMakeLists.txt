idf_component_register(
  SRCS "amoled_port.c"
  REQUIRES esp_lcd esp_driver_spi esp_driver_gpio espressif__esp_lcd_co5300 i2c_bsp
  INCLUDE_DIRS "./")
# 只借 main/user_config.h 里的板级引脚。不能写 REQUIRES main —— 那会和 main 成环，
# 打乱链接顺序、把 IDF bt 组件里的同名 list.c 拽进来撞符号
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_SOURCE_DIR}/main")
