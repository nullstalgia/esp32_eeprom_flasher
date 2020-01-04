# ESP32 EEPROM Flasher
I2C EEPROM Flasher built with a bare ESP32 and freely available libraries

Made with [Miniboot for AVRs](https://github.com/mihaigalos/miniboot) in mind

## Required Libraries:
- [https://github.com/me-no-dev/AsyncTCP](https://github.com/me-no-dev/AsyncTCP)
- [https://github.com/tzapu/WiFiManager](https://github.com/tzapu/WiFiManager)  ([dev branch](https://github.com/tzapu/WiFiManager/tree/development)  recommended)
- [https://github.com/me-no-dev/ESPAsyncWebServer](https://github.com/me-no-dev/ESPAsyncWebServer) 
- [https://github.com/bblanchon/ArduinoJson](https://github.com/bblanchon/ArduinoJson)

![Screenshot](https://i.imgur.com/FIr2VI1.png  "Screenshot")

## How to use:

1. Upload sketch to ESP32
2. Upload SPIFFS with HTML data with [arduino-esp32fs-plugin](https://github.com/me-no-dev/arduino-esp32fs-plugin).

If you want to upload to an I2C EEPROM:

1. Get a file that you want to upload, and make sure it has the `.eeprom` file extension.
2. Upload it to the ESP32
3. Select the newly uploaded file and the I2C address of the EEPROM you want to upload to
4. Press **Flash** and wait for it to finish.
5. If you want, you can make sure the content is correct with **Verify**

## Notes:
Most of this was written in a rush to get something working, but then I decided to expand it to make it something I can release/have others look at and work on :)