idf_component_register(
  SRCS "user_app.cpp" "app_sr.cpp"
  PRIV_REQUIRES
  board_power_bsp
  driver
  main
  button_bsp
  audio_bsp
  i2c_bsp
  espressif__esp-sr
  REQUIRES
  epaper_src
  epaper_port
  led_bsp
  sdcard_bsp
  INCLUDE_DIRS ".")
