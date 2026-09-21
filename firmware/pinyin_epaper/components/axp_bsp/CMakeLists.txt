idf_component_register(
  SRCS "axp_bsp.c"
  REQUIRES esp_driver_i2c esp_timer i2c_bsp
  INCLUDE_DIRS "./")
# 只借 main/user_config.h 里的板级能力位，不能写 REQUIRES main（会成环，见 amoled_port）
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_SOURCE_DIR}/main")
