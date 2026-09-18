idf_component_register(
  SRCS "user_app.cpp" "app_sr.cpp" "app_audio.cpp" "app_store.cpp" "app_stroke.cpp" "app_power.cpp" "app_console.cpp"
  PRIV_REQUIRES
  board_power_bsp
  driver
  main
  button_bsp
  audio_bsp
  i2c_bsp
  espressif__esp-sr
  nvs_flash
  esp_timer
  esp_adc
  esp_driver_usb_serial_jtag
  vfs
  REQUIRES
  epaper_src
  epaper_port
  amoled_port
  touch_bsp
  led_bsp
  sdcard_bsp
  INCLUDE_DIRS ".")
