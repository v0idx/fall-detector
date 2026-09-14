# fall-detector

Simple fall detector, expanded from a university project, targetted at the ESP32 family of microcontrollers

## TODO

- [ ] Finalise IIC implementation within /components/mpu6500_driver/driver_mpu6500_interface_template.c
- [ ] Add proper threading to app_main()
- [ ] Remove driver headers that aren't required
- [ ] Implement ULP coprocessor iic reads for lower-power operation in asm
- [ ] Refactor app layout
- [ ] General cleanup
- [ ] Comment codebase

## Local Development

This utility uses [prek](https://github.com/https://github.com/j178/prek/tree/master) for pre-commit linting and formatting, to get this working on your system, first follow the install guide in the prek repository readme.
When prek is installed, open a terminal within this repository's source folder and run the command `prek init`. This will keep the current configuration and install the necessary pre-commit shim.

Built with ESP-IDF v6.0.2
Fill out `example-info.h` with required information as shown below:

- WIFI_SSID -> SSID for the esp32 to connect to
- WIFI_PASS -> Password for the SSID
- FEED -> MQTT feed to publish events to e.g. `user/feeds/feedname`
- BROKER_URI -> MQTT broker URI, e.g. `mqtt://io.adafruit.com/`
- BROKER_USER -> Username used for the MQTT broker
- BROKER_PASS -> api key for the broker used

Then rename the file to `info.h` so the provided values can be used within the project.

## Current Ability
