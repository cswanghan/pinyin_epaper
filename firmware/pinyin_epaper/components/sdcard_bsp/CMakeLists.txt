idf_component_register(
  SRCS "sdcard_bsp.c"
  PRIV_REQUIRES 
  fatfs 
  REQUIRES
  ListLib 
  esp_driver_sdmmc   
  INCLUDE_DIRS "./")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
# 只借 main/user_config.h 里的板级引脚，不走 REQUIRES —— 那会和 main 形成依赖环，
# 打乱链接顺序，把 IDF bt 组件里的同名 list.c 一起拽进来撞 multiple definition
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_SOURCE_DIR}/main")
#REQUIRES fatfs
#PRIV_REQUIRES