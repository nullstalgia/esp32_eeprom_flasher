// this needs to be first, or it all crashes and burns...
#include <FS.h>

// SPIFFS for web-page and binary storage
#include <SPIFFS.h>

// https://github.com/me-no-dev/AsyncTCP
#include <AsyncTCP.h>

// https://github.com/tzapu/WiFiManager
#include <WiFiManager.h>

// https://github.com/me-no-dev/ESPAsyncWebServer
#include "AsyncJson.h"
#include <ESPAsyncWebServer.h>

// https://github.com/bblanchon/ArduinoJson
#include <ArduinoJson.h>

// for LED status
#include <Ticker.h>
Ticker ticker;

// Big thanks to mihaigalos' e2prom and miniboot repos!

// For OTA Updates
#include <ArduinoOTA.h>
#include <WiFiUdp.h>

// For I2C Communication
#include <Wire.h>

// To save config for I2C EEPROM pages/attempts, etc
#include <Preferences.h>
Preferences preferences;

// For making it easier to access without knowing the IP Address
#include <ESPmDNS.h>

// This was part of a failed experiment to increase the address space
// Mainly since I thought 256K meant 256kilo*bytes* and not *bits*
// But, I'm keeping it since it looks nice.
typedef uint16_t AddrSize;

// LED to blink for Wifi/OTA Status
#define LED_BUILTIN 12
int LED = LED_BUILTIN;

void tick() {
  // set pin to the opposite state
  digitalWrite(LED, !digitalRead(LED));
}

// What action is being done right now?
// Used for 2 reasons:
// 1. Since if you run a long blocking task (~over 5 seconds) in a
// ESPAsyncWebserver Callback, a watchdog will complain and reboot the entire
// chip.
// 2. To update the web gui as to what the current action/status is.
typedef enum {
  READY,
  UPLOAD_TO_ESP,
  FLASHING_EEPROM,
  VERIFYING_EEPROM,
  DUMP_EEPROM_SERIAL,
  DUMP_EEPROM_SPIFFS,
  TEMP_MINIBOOT_ACTION,
  ARDUINOOTA_UPDATING,
  CLEARING_EEPROM,
  UPDATING_PREFERENCES
} Progress_action;
Progress_action current_action = READY;

// Responses from the I2C EEPROM after an operation.
typedef enum {
  EEPROM_OK,
  EEPROM_TOOBIG, // ?
  EEPROM_NO_ADD_ACK,
  EEPROM_NO_DAT_ACK,
  EEPROM_OTHER_ERROR
} EEPROM_Response;

// Struct that is returned by readByte as I needed more info than could fit in
// something simpler.
struct readByteResponse {
  byte response;
  bool success;
  byte data;
};

// Progress of current action.
// Actually has a hidden usage: giving the final status of an action!
/*

  The Progress attribute has hidden info if its past 100%

  100 - Blank. Should be ready for any action at this point.

  101 - Succeeded Uploading to EEPROM
  102 - Failed Uploading to EEPROM

  103 - Succeeded Verifying EEPROM
  104 - Failed Verifying EEPROM

  105 - Succeeded Uploading to SPIFFS
  106 - Failed Uploading to SPIFFS

  107 - Succeeded Dumping to SPIFFS/Serial
  108 - Failed Dumping to SPIFFS/Serial

*/
int progress = 0;

// Buffer that gets filled with info for the progress bar
char progress_buffer[105];
// Buffer that gets filled with info for miniboot specific actions
char miniboot_json_buffer[105];

AsyncWebServer server(80);

// byte working_bin_1[33000];
// byte working_bin_2[33000];

// How many I2C devices do you want to be allowed to scan/display?
#define MAX_I2C_COUNT 5

#define STATIC_I2C_ADDRESS 0

// Capacity for MAX_I2C_COUNT I2C addresses.
const size_t I2C_CAPACITY = JSON_ARRAY_SIZE(MAX_I2C_COUNT);
// allocate the memory for the document
StaticJsonDocument<I2C_CAPACITY> i2cdoc;
// Array for scanned I2C addresses
JsonArray I2C_Array = i2cdoc.to<JsonArray>();

// How many Binary/EEPROM files (each) do you want to be allowed to
// scan/display?
#define MAX_FILE_COUNT 15
#define CONSTANT_ADDITIONS 9
// Capacity for MAX_I2C_COUNT I2C addresses.
// Not sure why I need these +CONSTANT_ADDITIONS's on the constants for
// computing the size.. The assistant says this would be enough without it
// ¯\_(ツ)_/¯
const size_t binary_eeprom_array_size =
    ((MAX_FILE_COUNT + CONSTANT_ADDITIONS) * JSON_ARRAY_SIZE(2) +
     JSON_ARRAY_SIZE(MAX_FILE_COUNT + CONSTANT_ADDITIONS) +
     JSON_OBJECT_SIZE(1));
StaticJsonDocument<binary_eeprom_array_size> file_list_doc;

#define FORMAT_SPIFFS_IF_FAILED true
bool did_spiffs_fail = false;

// Globals used by callbacks/actions that can't be called in the callbacks.
String global_file;
byte global_address;
AddrSize global_offset;
AddrSize global_size;
byte global_clear_value;

// What byte failed during a verify?
AddrSize byte_failed;

// Grab a (kinda) random filename for a dump.
unsigned long millis_file_name;

// Used when getting the length from a miniboot header in an external EEPROM
uint16_t miniboot_extracted_length;

// Used for the JSON buffer when getting the CRC from a miniboot header in an
// external EEPROM/SPIFFS file
char crc_to_output[9];
// Used for the JSON buffer when getting the AppName from a miniboot header in
// an external EEPROM/SPIFFS file
char appname_to_output[11];

uint8_t eeprom_max_attempts;
uint8_t eeprom_delay_between_actions;
uint8_t eeprom_request_from_delay;

#define MINIBOOT_LENGTH_OFFSET 32
#define MINIBOOT_CRC_OFFSET 28
#define MINIBOOT_APPNAME_OFFSET 10

// EEPROM Pages. These will up your speed like crazy, even with a small 32-byte
// buffer/page.

// ~~Though so far, I have noticed that there is corruption more often. PLEASE
// verify after an upload!~~

// Issue has been found with using Pages. If you don't write at an offset that
// is divisible by the page, errors will pop up An offset of 0 works fine,
// however.

// Example 1:
// Page Size: 32
// Offset: 31
// Will error out.

// Example 2:
// Page size: 32
// Offset: 1792 (32 times 56)
// Succeeded.

bool use_eeprom_pages;
uint8_t eeprom_page_size;

// So I see 2 ways to help guarantee a response from the I2C EEPROM
// 1 (faster, but may not always work): Send the request over and over and over
// until it acknowledges both data and address 2 (slower, but should be more
// reliable): Have a small (5+ millisecond) delay between actions to give it
// time to react and respond

// My testing with the AT24C256 EEPROM @ 400kbit/s - 3.3V worked 100% of the
// time with: EEPROM_MAX_ATTEMPTS = 100 EEPROM_DELAY_BETWEEN_ACTIONS = 0
// EEPROM_REQUEST_FROM_DELAY = 0
// and also
// EEPROM_MAX_ATTEMPTS = 1
// EEPROM_DELAY_BETWEEN_ACTIONS = 4
// EEPROM_REQUEST_FROM_DELAY = 0

// EEPROM_REQUEST_FROM_DELAY is how many millis to wait per "attempt" after
// running a Wire.requestFrom in readByte().

void EEPROM_DELAY() {
  if (eeprom_delay_between_actions != 0) {
    delay(eeprom_delay_between_actions);
  } else {
    return;
  }
}

void EEPROM_REQUEST_DELAY() {
  if (eeprom_request_from_delay != 0) {
    delay(eeprom_request_from_delay);
  } else {
    return;
  }
}

// Some tests I did (not at all scientific, just with a stopwatch and slow human
// fingers) Tested with: AT24C256 EEPROM @ 400kbit/s - 3.3V EEPROM_MAX_ATTEMPTS
// = 100 EEPROM_DELAY_BETWEEN_ACTIONS = 0 EEPROM_REQUEST_FROM_DELAY = 0

/*

  (1 Byte at a time)
  Bytes: 4038
  Writing: ~10.19 seconds
  Reading and verifying against source file: ~6.49 seconds

  (32-byte pages)
  Bytes: 27492 (26.8kb)
  Writing: ~5.64 seconds
  Reading and verifying against source file: ~4.72 seconds

  (64-byte pages)
  Bytes: 27492 (26.8kb)
  Writing: ~3.76 seconds
  Reading and verifying against source file: ~4.58 seconds

*/

// Whether or not the ESP32 should use it's on board LED to slowly blink out
// it's IP if you don't have access to a Serial port.
bool blink_ip;

void updateProgress() {
  /*
    sprintf(progress_buffer,
    "{\"progress\":%d, \"action\": %d, \"battery\": %d, \"fail_byte\": %d,
    \"dump_name\": %lu, \"mb_len\": %d, \"mb_name\": %s}", progress,
    current_action, analogRead(33),  byte_failed,       millis_file_name,
    miniboot_extracted_length, miniboot_extracted_name);
  */
  sprintf(progress_buffer,
          "{\"progress\":%d, \"action\": %d, \"battery\": %d, \"fail_byte\": "
          "%d, \"dump_name\": %lu}",
          progress, current_action, analogRead(33), byte_failed,
          millis_file_name);
}

void updateMinibootJSONBuffer(bool success) {
  sprintf(miniboot_json_buffer,
          "{\"success\": %s, \"length\": %d, \"written_crc\": \"%s\", "
          "\"appname\": \"%s\"}",
          success ? "true" : "false", miniboot_extracted_length, crc_to_output,
          appname_to_output);
}

bool deleteFile(fs::FS &fs, String path) {
  Serial.print("Deleting file: ");
  Serial.println(path);
  if (fs.remove(path)) {
    Serial.println("- file deleted");
    return true;
  } else {
    Serial.println("- delete failed");
    return false;
  }
}

bool renameFile(fs::FS &fs, String path1, String path2) {
  if (fs.exists(path2)) {
    deleteFile(fs, path2);
  }
  Serial.print("Renaming/moving file ");
  Serial.print(path1);
  Serial.print(" to ");
  Serial.println(path2);
  if (fs.rename(path1, path2)) {
    Serial.println("- file renamed");
    return true;
  } else {
    Serial.println("- rename failed");
    return false;
  }
}

uint16_t two_bytes_to_decimal(byte byte1, byte byte2) {
  return (uint16_t)(byte1 << 8 | byte2);
}

// Based on/copied from
// https://github.com/mihaigalos/Drivers/blob/master/Eeprom/src/e2prom.cpp
struct readByteResponse readByte(byte eeprom_address,
                                 AddrSize registerAddress) {
  AddrSize counter = 0;
  readByteResponse funcResponse;

  byte response = EEPROM_OTHER_ERROR;
  do {
    if (counter >= eeprom_max_attempts) {
      funcResponse.response = response;
      funcResponse.success = false;
      funcResponse.data = 0xFF;
      break;
    }
    EEPROM_DELAY();
    Wire.beginTransmission(eeprom_address);
    // Too much for my chip. Feel free to uncap if you need I guess.
    // Wire.write(static_cast<uint8_t>(registerAddress >> 24));
    // Wire.write(static_cast<uint8_t>(registerAddress >> 16));
    Wire.write(static_cast<uint8_t>(registerAddress >> 8));
    Wire.write(static_cast<uint8_t>(registerAddress));
    response = Wire.endTransmission();
    counter++;
  } while (response != EEPROM_OK);

  // Ask the I2C device for data
  counter = 0;
  EEPROM_DELAY();
  Wire.requestFrom(eeprom_address, static_cast<uint8_t>(1));
  while (!Wire.available()) {
    if (counter >= eeprom_max_attempts) {
      funcResponse.response = response;
      funcResponse.success = false;
      funcResponse.data = 0xFF;
      break;
    }
    EEPROM_REQUEST_DELAY();
    counter++;
  }
  if (Wire.available()) {
    funcResponse.response = response;
    funcResponse.success = true;
    funcResponse.data = Wire.read();
  }
  return funcResponse;
}

// Based on/copied from
// https://github.com/mihaigalos/Drivers/blob/master/Eeprom/src/e2prom.cpp
struct readByteResponse readPage(byte eeprom_address, AddrSize registerAddress,
                                 uint8_t *databuffer, uint8_t datacount) {
  AddrSize counter = 0;
  readByteResponse funcResponse;

  byte response = EEPROM_OTHER_ERROR;

  funcResponse.success = true;
  funcResponse.data = 0xFF;

  if (datacount > eeprom_page_size) {
    funcResponse.response = EEPROM_TOOBIG;
    funcResponse.success = false;
    return funcResponse;
  }

  do {
    if (counter >= eeprom_max_attempts) {
      funcResponse.response = response;
      funcResponse.success = false;
      break;
    }
    EEPROM_DELAY();
    Wire.beginTransmission(eeprom_address);
    // Too much for my chip. Feel free to uncap if you need I guess.
    // Wire.write(static_cast<uint8_t>(registerAddress >> 24));
    // Wire.write(static_cast<uint8_t>(registerAddress >> 16));
    Wire.write(static_cast<uint8_t>(registerAddress >> 8));
    Wire.write(static_cast<uint8_t>(registerAddress));
    response = Wire.endTransmission();
    counter++;
  } while (response != EEPROM_OK);

  // Ask the I2C device for data
  EEPROM_DELAY();
  Wire.requestFrom(eeprom_address, static_cast<uint8_t>(datacount));
  funcResponse.response = response;
  for (int i = 0; i < datacount; i++) {
    if (funcResponse.success == false) {
      break;
    }
    counter = 0;
    while (!Wire.available()) {
      if (counter >= eeprom_max_attempts) {
        funcResponse.success = false;
        break;
      }
      EEPROM_REQUEST_DELAY();
      counter++;
    }
    if (Wire.available()) {
      databuffer[i] = Wire.read();
      funcResponse.success = true;
    }
  }
  return funcResponse;
}

// Based on/copied from
// https://github.com/mihaigalos/Drivers/blob/master/Eeprom/src/e2prom.cpp
byte writeByte(byte eepromAddress, AddrSize registerAddress, uint8_t data) {
  AddrSize counter = 0;
  // int max_attempts = 100;
  byte response = EEPROM_OTHER_ERROR;
  do {
    if (counter >= eeprom_max_attempts) {
      break;
    }
    EEPROM_DELAY();
    Wire.beginTransmission(eepromAddress);
    // Wire.write(static_cast<uint8_t>(registerAddress >> 24));
    // Wire.write(static_cast<uint8_t>(registerAddress >> 16));
    Wire.write(static_cast<uint8_t>(registerAddress >> 8));
    Wire.write(static_cast<uint8_t>(registerAddress));
    EEPROM_DELAY();
    Wire.write(data);
    response = Wire.endTransmission();
    counter++;
  } while (response != EEPROM_OK);

  return response;
}

// Based on/copied from
// https://github.com/mihaigalos/Drivers/blob/master/Eeprom/src/e2prom.cpp
byte writePage(byte eepromAddress, AddrSize registerAddress,
               uint8_t *databuffer, uint8_t datacount) {
  AddrSize counter = 0;
  // int max_attempts = 100;
  byte response = EEPROM_OTHER_ERROR;

  if (datacount > eeprom_page_size) {
    response = EEPROM_TOOBIG;
    return response;
  }

  do {
    if (counter >= eeprom_max_attempts) {
      break;
    }
    Wire.beginTransmission(eepromAddress);
    // Wire.write(static_cast<uint8_t>(registerAddress >> 24));
    // Wire.write(static_cast<uint8_t>(registerAddress >> 16));
    Wire.write(static_cast<uint8_t>(registerAddress >> 8));
    Wire.write(static_cast<uint8_t>(registerAddress));
    EEPROM_DELAY();
    for (int i = 0; i < datacount; i++) {
      Wire.write(databuffer[i]);
    }
    EEPROM_DELAY();
    response = Wire.endTransmission();
    counter++;
  } while (response != EEPROM_OK);

  return response;
}

// Dump external EEPROM to SPIFFS
void dumpEEPROMSPIFFS() {
  bool success = true;
  readByteResponse i2c_response;

  String filepath;
  filepath.concat("/eeprom/");
  filepath.concat(millis_file_name);
  filepath.concat(".eeprom");

  File file = SPIFFS.open(filepath, FILE_WRITE);

  AddrSize position = global_offset;
  int counter = 0;

  // If the size is under 100 bytes and I try to render the progress, the chip
  // thinks I'm dividing by 0 (which would be technically correct). So this is
  // overcome by having a larger size, but just for the progress to use.
  AddrSize progress_size = 100;
  if (global_size >= 100) {
    progress_size = global_size;
  }
  if (!file) {
    progress = 108;
    Serial.println("Fail due to bad file");
    success = false;
  }
  Serial.println("Starting EEPROM > SPIFFS dump.");
  if (success) {
    if (use_eeprom_pages == false || eeprom_page_size <= 1) {
      for (int i = 0; i < global_size; i++) {
        Serial.print("Reading pos: ");
        Serial.println(position);
        i2c_response = readByte(global_address, position);
        if (i2c_response.response == EEPROM_OK) {

          file.write(i2c_response.data);

          progress = counter / (progress_size / 100);
          updateProgress();
          position++;
          counter++;
        } else {
          progress = 108;
          Serial.println("Fail due to bad response");
          Serial.println((int)i2c_response.response);
          success = false;
          break;
        }
      }
    } else {
      while (counter < global_size) {
        uint8_t buffer[eeprom_page_size];
        i2c_response =
            readPage(global_address, position, &buffer[0], eeprom_page_size);
        Serial.print("Reading pos: ");
        Serial.println(position);
        if (i2c_response.response == EEPROM_OK && i2c_response.success) {
          for (int i = 0; i < eeprom_page_size; i++) {
            file.write(buffer[i]);
            progress = counter / (progress_size / 100);
            updateProgress();
            position++;
            counter++;
            if (counter >= global_size) {
              break;
            }
          }
        } else {
          progress = 108;
          Serial.println("Fail due to bad response");
          Serial.println((int)i2c_response.response);
          success = false;
          break;
        }
      }
    }
  }
  file.close();
  current_action = READY;
  if (success) {
    progress = 107;
  }
  Serial.println("EEPROM > SPIFFS dump ended.");
}

// Dump external EEPROM to Serial
void dumpEEPROMSerial() {
  readByteResponse i2c_response;
  millis_file_name = 0;
  AddrSize position = global_offset;
  bool success = true;
  int counter = 0;
  AddrSize progress_size = 100;
  if (global_size >= 100) {
    progress_size = global_size;
  }
  for (int i = 0; i < global_size; i++) {
    // Serial.print("Reading pos: ");
    // Serial.println(position);
    i2c_response = readByte(global_address, position);
    if (i2c_response.response == EEPROM_OK) {
      Serial.write(i2c_response.data);
      progress = counter / (progress_size / 100);
      updateProgress();
      position++;
      counter++;
    } else {
      progress = 108;
      Serial.println("Fail due to bad response");
      Serial.println((int)i2c_response.response);
      success = false;
      break;
    }
  }
  current_action = READY;
  if (success) {
    progress = 107;
  }
}

void flashEEPROM() {
  Serial.println("Starting SPIFFS > EEPROM flash.");
  Serial.print("File: ");
  Serial.println(global_file);
  Serial.print("I2C Address (d): ");
  Serial.println(global_address);

  bool success = true;
  progress = 0;

  File eeprom_to_flash = SPIFFS.open(global_file);
  if (!eeprom_to_flash || eeprom_to_flash.isDirectory()) {
    Serial.println("- failed to open file for reading");
    current_action = READY;
    success = false;
    eeprom_to_flash.close();
  }

  if (success) {
    AddrSize position = global_offset;
    AddrSize size = eeprom_to_flash.size();
    AddrSize counter = 0;
    AddrSize progress_size = 100;
    if (size >= 100) {
      // If the size is under 100 bytes and I try to render the progress, the
      // chip thinks I'm dividing by 0 (which would be technically correct). So
      // this is overcome by having a larger size, but just for the progress to
      // use.
      progress_size = size;
    }
    Serial.print("Size: ");
    Serial.println(size);
    while (eeprom_to_flash.available()) {
      if (success == false) {
        break;
      }

      if (use_eeprom_pages == false || eeprom_page_size <= 1) {

        Serial.print("Writing to pos: ");
        Serial.println(position);
        byte response =
            writeByte(global_address, position, eeprom_to_flash.read());
        if (response == EEPROM_OK) {
          progress = counter / (progress_size / 100);
          updateProgress();
          // Position and counter are seperate due to global_offset.
          position++;
          counter++;
        } else {
          eeprom_to_flash.close();
          progress = 102;
          Serial.println("Fail due to bad response");
          Serial.println((int)response);
          success = false;
        }

      } else {

        Serial.print("Writing to pos: ");
        Serial.println(position);
        uint8_t buffer[eeprom_page_size];
        for (int i = 0; i < eeprom_page_size; i++) {
          buffer[i] = eeprom_to_flash.read();
        }
        byte response =
            writePage(global_address, position, &buffer[0], eeprom_page_size);
        if (response == EEPROM_OK) {
          progress = counter / (progress_size / 100);
          updateProgress();
          // Position and counter are seperate due to global_offset.
          position += eeprom_page_size;
          counter += eeprom_page_size;
        } else {
          eeprom_to_flash.close();
          progress = 102;
          Serial.println("Fail due to bad response");
          Serial.println((int)response);
          success = false;
        }
      }
    }

    eeprom_to_flash.close();
    current_action = READY;
    if (success) {
      progress = 101;
    } else {
      progress = 102;
    }

  } else {
    progress = 102;
    Serial.println("Fail due to bad file");
  }
  Serial.println("SPIFFS > EEPROM dump ended.");
  current_action = READY;
}

void verifyEEPROM() {
  Serial.println("Starting a verify!");
  Serial.print("File: ");
  Serial.println(global_file);
  Serial.print("I2C Address (d): ");
  Serial.println(global_address);

  bool success = true;
  progress = 0;

  File eeprom_to_test_against = SPIFFS.open(global_file);
  if (!eeprom_to_test_against || eeprom_to_test_against.isDirectory()) {
    Serial.println("- failed to open file for reading");
    success = false;
    eeprom_to_test_against.close();
  }
  if (success) {
    AddrSize position = global_offset;
    AddrSize size = eeprom_to_test_against.size();

    AddrSize counter = 0;
    AddrSize progress_size = 100;
    if (size >= 100) {
      progress_size = size;
    }
    Serial.print("Size: ");
    Serial.println(size);
    if (use_eeprom_pages == false || eeprom_page_size <= 1) {
      while (eeprom_to_test_against.available()) {
        Serial.print("Reading pos: ");
        Serial.println(position);

        readByteResponse i2c_response = readByte(global_address, position);

        if (i2c_response.response == EEPROM_OK && i2c_response.success) {
          if (i2c_response.data == eeprom_to_test_against.read()) {
            progress = counter / (progress_size / 100);
            updateProgress();
            position++;
            counter++;
          } else {
            success = false;
            progress = 104;
            byte_failed = position;

            Serial.println("Verify failed! Bad I2C data!");
            Serial.println("Position DEC/HEX: ");
            Serial.print(position);
            Serial.print(" / ");
            Serial.print(position, HEX);
            Serial.println("");
            break;
          }
        } else {
          eeprom_to_test_against.close();
          success = false;
          progress = 104;
          byte_failed = position;
          Serial.println("Fail due to bad response");
          Serial.println((int)i2c_response.response);
          break;
        }
      }
    } else {

      // counter = 0;
      while (eeprom_to_test_against.available() && success) {
        Serial.print("Reading pos: ");
        Serial.println(position);

        uint8_t buffer[eeprom_page_size];

        readByteResponse i2c_response =
            readPage(global_address, position, &buffer[0], eeprom_page_size);

        if (i2c_response.response == EEPROM_OK && i2c_response.success) {
          for (int i = 0; i < eeprom_page_size; i++) {
            if (buffer[i] == eeprom_to_test_against.read()) {
              progress = counter / (progress_size / 100);
              updateProgress();
              position++;
              counter++;
            } else {
              success = false;
              progress = 104;
              byte_failed = position;

              Serial.println("Verify failed! Bad I2C data!");
              Serial.println("Position DEC/HEX: ");
              Serial.print(position);
              Serial.print(" / ");
              Serial.print(position, HEX);
              Serial.println("");
              break;
            }
            if (!eeprom_to_test_against.available() || counter >= size) {
              break;
            }
          }
        } else {
          eeprom_to_test_against.close();
          success = false;
          progress = 104;
          byte_failed = position;
          Serial.println("Fail due to bad response");
          Serial.println((int)i2c_response.response);
          break;
        }
      }
    }

    eeprom_to_test_against.close();
    current_action = READY;
    if (success) {
      progress = 103;
    } else {
      progress = 104;
    }

  } else {
    progress = 104;
    Serial.println("Fail due to bad file");
  }

  Serial.println("Verify has ended");
  current_action = READY;
}

void handleUpload(AsyncWebServerRequest *request, String filename, size_t index,
                  uint8_t *data, size_t len, bool final) {
  if (!index) {
    progress = 0;
    current_action = UPLOAD_TO_ESP;
    // filename.toLowerCase();
    Serial.println((String) "UploadStart: " + filename);
    // open the file on first call and store the file handle in the request
    // object
    request->_tempFile = SPIFFS.open("/" + filename, FILE_WRITE);
  }
  if (len) {
    progress = 50;
    // stream the incoming chunk to the opened file
    request->_tempFile.write(data, len);
  }
  if (final) {
    Serial.println((String) "UploadEnd: " + filename + ",size: " + index + len);
    // close the file handle as the upload is now done
    request->_tempFile.close();
    String lowerName = filename;
    lowerName.toLowerCase();
    if (lowerName.endsWith("bin")) {
      if (renameFile(SPIFFS, "/" + filename, "/bin/" + lowerName)) {
        progress = 105;
      } else {
        progress = 106;
      }
    } else if (lowerName.endsWith("eeprom")) {
      if (renameFile(SPIFFS, "/" + filename, "/eeprom/" + lowerName)) {
        progress = 105;
      } else {
        progress = 106;
      }
    } else {
      if (renameFile(SPIFFS, "/" + filename, "/unknown/" + lowerName)) {
        progress = 105;
      } else {
        progress = 106;
      }
    }
    current_action = READY;
    request->redirect("/");
  }
}

void setupSPIFFS() {
  Serial.println("mounting FS...");

  if (SPIFFS.begin(FORMAT_SPIFFS_IF_FAILED)) {
    Serial.println("mounted file system");
  } else {
    Serial.println("failed to mount FS");
    did_spiffs_fail = true;
    if (FORMAT_SPIFFS_IF_FAILED) {
      Serial.println("Formatted SPIFFS as per config.");
    }
  }
}

void notFound(AsyncWebServerRequest *request) {
  request->send(404, "text/plain", "Not found");
}

// gets called when WiFiManager enters configuration mode
void configModeCallback(WiFiManager *myWiFiManager) {
  Serial.println("Entered config mode");
  Serial.println(WiFi.softAPIP());
  // if you used auto generated SSID, print it
  Serial.println(myWiFiManager->getConfigPortalSSID());
  // entered config mode, make led toggle faster
  ticker.attach(0.2, tick);
}

// Getting the JSON for the file lists
void refreshJSON(String dir) {
  file_list_doc.clear();
  JsonArray files = file_list_doc.createNestedArray("files");
  File root_dir = SPIFFS.open("/" + dir);
  if (!root_dir) {
    Serial.println("- failed to open directory");
    return;
  }
  if (!root_dir.isDirectory()) {
    Serial.println(" - not a directory");
    return;
  }
  AddrSize counter = 0;
  File file = root_dir.openNextFile();
  for (int i = 0; i < MAX_FILE_COUNT; i++) {
    if (counter >= MAX_FILE_COUNT) {
      break;
    }
    if (file) {
      if (file.isDirectory()) {
        continue;
      } else {
        JsonArray files_0 = files.createNestedArray();
        String filename = file.name();
        int filesize = file.size();
        Serial.print("  FILE: ");
        Serial.print(filename);
        Serial.print("\tSIZE: ");
        Serial.println(filesize);
        files_0.add(filename);
        files_0.add(filesize);
        counter++;
      }
    } else {
      break;
    }
    file = root_dir.openNextFile();
  }
}

void scan_i2c() {
  byte error, address;
  int nDevices;

  Serial.println("Scanning I2C Devices...");

  // Decided to just reinit the whole array. This is a cleaner way, anyway.
  I2C_Array = i2cdoc.to<JsonArray>();
  nDevices = 0;
  if (STATIC_I2C_ADDRESS == 0) {
    for (address = 1; address < 127; address++) {
      // The I2C_scanner uses the return value of
      // the Write.endTransmisstion to see if
      // a device did acknowledge to the address.
      Wire.beginTransmission(address);
      error = Wire.endTransmission();

      if (error == EEPROM_OK) {
        Serial.print("I2C device found at address 0x");
        if (address < 16) {
          Serial.print("0");
        }
        Serial.print(address, HEX);
        Serial.println("  !");
        I2C_Array.add(address);
        nDevices++;
        if (nDevices >= MAX_I2C_COUNT) {
          Serial.println("Max I2C devices found.");
          break;
        }
      } else if (error == EEPROM_OTHER_ERROR) {
        Serial.print("Unknown error at address 0x");
        if (address < 16) {
          Serial.print("0");
        }
        Serial.println(address, HEX);
      }
    }
    if (nDevices == 0) {
      Serial.println("No I2C devices found\n");
    } else {
      Serial.println("done\n");
    }
  } else {
    I2C_Array.add(0x50);
    I2C_Array.add(0x51);
    I2C_Array.add(0x52);
    I2C_Array.add(0x53);
  }
}

void listDir(fs::FS &fs, const char *dirname, uint8_t levels) {
  Serial.printf("Listing directory: %s\r\n", dirname);

  File root = fs.open(dirname);
  if (!root) {
    Serial.println("- failed to open directory");
    return;
  }
  if (!root.isDirectory()) {
    Serial.println(" - not a directory");
    return;
  }

  File file = root.openNextFile();
  while (file) {
    if (file.isDirectory()) {
      Serial.print("  DIR : ");
      Serial.println(file.name());
      if (levels) {
        listDir(fs, file.name(), levels - 1);
      }
    } else {
      Serial.print("  FILE: ");
      Serial.print(file.name());
      Serial.print("\tSIZE: ");
      Serial.println(file.size());
    }
    file.close();
    file = root.openNextFile();
  }
  root.close();
}

// Checking if an external EEPROM has a proper miniboot header
bool is_eeprom_a_miniboot() {
  AddrSize position = global_offset;
  position += 2;
  byte extracted[8];
  const byte original[8] = {'m', 'i', 'n', 'i', 'b', 'o', 'o', 't'};
  for (int i = 0; i < 8; i++) {
    readByteResponse i2c_response;
    i2c_response = readByte(global_address, position);
    if (i2c_response.success) {
      extracted[i] = i2c_response.data;
    } else {
      return false;
    }
    position++;
  }

  for (int i = 0; i < 8; i++) {
    if (original[i] == extracted[i]) {
      continue;
    } else {
      return false;
    }
  }

  return true;
}

uint16_t get_miniboot_length() {
  AddrSize position = global_offset;
  position += MINIBOOT_LENGTH_OFFSET;
  byte extracted[2];
  for (int i = 0; i < 2; i++) {
    readByteResponse i2c_response;
    i2c_response = readByte(global_address, position);
    if (i2c_response.success) {
      extracted[i] = i2c_response.data;
    } else {
      return 0;
    }
    position++;
  }

  return two_bytes_to_decimal(extracted[0], extracted[1]);
}

void clear_crc_appname_outputs() {
  for (int i = 0; i < sizeof(crc_to_output); i++) {
    crc_to_output[i] = (char)0;
  }
  for (int i = 0; i < sizeof(appname_to_output); i++) {
    appname_to_output[i] = (char)0;
  }
}

void get_miniboot_crc_appname() {
  AddrSize position = global_offset;
  position += MINIBOOT_CRC_OFFSET;
  byte extracted_crc[4];
  for (int i = 0; i < 4; i++) {
    readByteResponse i2c_response;
    i2c_response = readByte(global_address, position);
    if (i2c_response.success) {
      extracted_crc[i] = i2c_response.data;
    } else {
      clear_crc_appname_outputs();
      return;
    }
    position++;
  }
  sprintf(crc_to_output, "%X%X%X%X", extracted_crc[0], extracted_crc[1],
          extracted_crc[2], extracted_crc[3]);

  position = global_offset;
  position += MINIBOOT_APPNAME_OFFSET;
  // char extracted_appname[11];
  for (int i = 0; i < 10; i++) {
    readByteResponse i2c_response;
    i2c_response = readByte(global_address, position);
    if (i2c_response.success) {
      appname_to_output[i] = i2c_response.data;
    } else {
      clear_crc_appname_outputs();
      return;
    }
    position++;
  }
}

bool is_spiffs_eeprom_miniboot() {
  Serial.println("Checking if a SPIFFS file is a miniboot file.");
  byte extracted[8];
  const byte original[8] = {'m', 'i', 'n', 'i', 'b', 'o', 'o', 't'};
  Serial.print("File: ");
  Serial.println(global_file);

  bool success = true;

  File file = SPIFFS.open(global_file);
  if (!file || file.isDirectory()) {
    Serial.println("- failed to open file for reading");
    success = false;
    file.close();
  }

  if (success) {
    AddrSize position_of_wanted_data =
        2; // Skipping 2 garbage bits before expected 'miniboot' constant
    int size_of_wanted_data = 8;
    int extracted_position = 0;
    AddrSize counter = 0;
    while (file.available() && extracted_position < size_of_wanted_data) {
      if (counter < position_of_wanted_data) {
        Serial.print("Skipping pos: ");
        Serial.println(counter);
        file.read();
      } else {
        Serial.print("Getting from pos: ");
        Serial.println(counter);
        byte readbyte = file.read();
        extracted[extracted_position] = readbyte;
        extracted_position++;
      }
      counter++;
    }
    file.close();

    if (extracted_position < size_of_wanted_data) {
      success = false;
    }
  } else {
    Serial.println("Fail due to bad file");
  }

  if (success) {
    for (int i = 0; i < 8; i++) {
      if (original[i] == extracted[i]) {
        continue;
      } else {
        return false;
      }
    }
  } else {
    Serial.println("Fail due to bad file");
  }
  return success;
}

void read_spiffs_miniboot_crc() {

  Serial.println("Getting the CRC of a SPIFFS Miniboot file.");
  byte extracted[4];
  Serial.print("File: ");
  Serial.println(global_file);

  bool success = true;

  File file = SPIFFS.open(global_file);
  if (!file || file.isDirectory()) {
    Serial.println("- failed to open file for reading");
    success = false;
    file.close();
  }

  if (success) {
    AddrSize position_of_wanted_data = MINIBOOT_CRC_OFFSET;
    int size_of_wanted_data = 4;
    int extracted_position = 0;
    AddrSize counter = 0;
    while (file.available() && extracted_position < size_of_wanted_data) {
      if (counter < position_of_wanted_data) {
        Serial.print("Skipping pos: ");
        Serial.println(counter);
        file.read();
      } else {
        Serial.print("Getting from pos: ");
        Serial.println(counter);
        byte readbyte = file.read();
        extracted[extracted_position] = readbyte;
        extracted_position++;
      }
      counter++;
    }
    file.close();

    if (extracted_position < size_of_wanted_data) {
      success = false;
    }
  } else {
    Serial.println("Fail due to bad file");
  }

  if (success) {
    sprintf(crc_to_output, "%X%X%X%X", extracted[0], extracted[1], extracted[2],
            extracted[3]);
  } else {
    clear_crc_appname_outputs();
    Serial.println("Fail due to bad file");
  }
}

void read_spiffs_miniboot_appname() {

  Serial.println("Getting the AppName of a SPIFFS Miniboot file.");
  Serial.print("File: ");
  Serial.println(global_file);

  bool success = true;

  File file = SPIFFS.open(global_file);
  if (!file || file.isDirectory()) {
    Serial.println("- failed to open file for reading");
    success = false;
    file.close();
  }

  if (success) {
    AddrSize position_of_wanted_data = MINIBOOT_APPNAME_OFFSET;
    int size_of_wanted_data = 10;
    int extracted_position = 0;
    AddrSize counter = 0;
    while (file.available() && extracted_position < size_of_wanted_data) {
      if (counter < position_of_wanted_data) {
        Serial.print("Skipping pos: ");
        Serial.println(counter);
        file.read();
      } else {
        Serial.print("Getting from pos: ");
        Serial.println(counter);
        byte readbyte = file.read();
        appname_to_output[extracted_position] = readbyte;
        extracted_position++;
      }
      counter++;
    }
    file.close();

    if (extracted_position < size_of_wanted_data) {
      success = false;
    }
  } else {
    Serial.println("Fail due to bad file");
  }

  if (success) {

  } else {
    clear_crc_appname_outputs();
    Serial.println("Fail due to bad file");
  }
}

void clearEEPROM() {
  Serial.println("Starting to clear EEPROM.");
  Serial.print("I2C Address (d): ");
  Serial.println(global_address);

  bool success = true;
  progress = 0;

  if (success) {
    AddrSize position = global_offset;
    AddrSize size = global_size;
    int counter = 0;
    AddrSize progress_size = 100;
    if (size >= 100) {
      // If the size is under 100 bytes and I try to render the progress, the
      // chip thinks I'm dividing by 0 (which would be technically correct). So
      // this is overcome by having a larger size, but just for the progress to
      // use.
      progress_size = size;
    }
    Serial.print("Size: ");
    Serial.println(size);
    while (counter < size) {
      if (success == false) {
        break;
      }
      if (use_eeprom_pages == false || eeprom_page_size <= 1) {
        Serial.print("Writing to pos: ");
        Serial.println(position);
        byte response = writeByte(global_address, position, global_clear_value);
        if (response == EEPROM_OK) {
          progress = counter / (progress_size / 100);
          updateProgress();
          // Position and counter are seperate due to global_offset.
          position++;
          counter++;
        } else {
          progress = 102;
          Serial.println("Fail due to bad response");
          Serial.println((int)response);
          success = false;
        }
      } else {

        Serial.print("Writing to pos: ");
        Serial.println(position);
        uint8_t buffer[eeprom_page_size];
        for (int i = 0; i < eeprom_page_size; i++) {
          buffer[i] = global_clear_value;
        }
        byte response =
            writePage(global_address, position, &buffer[0], eeprom_page_size);
        if (response == EEPROM_OK) {
          progress = counter / (progress_size / 100);
          updateProgress();
          // Position and counter are seperate due to global_offset.
          position += eeprom_page_size;
          counter += eeprom_page_size;
        } else {
          progress = 102;
          Serial.println("Fail due to bad response");
          Serial.println((int)response);
          success = false;
        }
      }

      if (counter >= size) {
        break;
      }
    }

    current_action = READY;
    if (success) {
      progress = 101;
    } else {
      progress = 102;
    }

  } else {
    progress = 102;
    Serial.println("Fail???");
  }
  Serial.println("EEPROM clear ended.");
  current_action = READY;
}

String processor(const String &var) {
  if (var == "PAGE_SIZE") {
    return (String)eeprom_page_size;
  }
  return String();
}

void printPreferences() {
  Serial.printf("Use pages: %s\n", use_eeprom_pages ? "true" : "false");
  Serial.printf("Page Size: %d\n", eeprom_page_size);
  Serial.printf("Max Attempts: %d\n", eeprom_max_attempts);
  Serial.printf("Delay: %d\n", eeprom_delay_between_actions);
  Serial.printf("ReqFrom Delay: %d\n", eeprom_request_from_delay);
  Serial.printf("Blink IP: %s\n", blink_ip ? "true" : "false");
  Serial.println("------");
}

void blinkNum(int num) {
  for (int i = 0; i < num; i++) {
    digitalWrite(LED, HIGH);
    delay(230);
    digitalWrite(LED, LOW);
    delay(230);
  }
}

// the setup function runs once when you press reset or power the board
void setup() {
  WiFi.mode(WIFI_STA); // explicitly set mode, esp defaults to STA+AP
  // initialize digital pin LED_BUILTIN as an output.
  pinMode(12, OUTPUT);
  pinMode(0, INPUT_PULLUP);

  Serial.begin(115200);

  WiFiManager wm;

  // start ticker with 0.6 because we start in AP mode and try to connect
  ticker.attach(0.6, tick);

  // set callback that gets called when connecting to previous WiFi fails, and
  // enters Access Point mode
  wm.setAPCallback(configModeCallback);

  Serial.println("Hold GPIO0 LOW to reset WiFi settings!");
  delay(3000);

  if (!digitalRead(0)) {
    wm.resetSettings();
  }

  // fetches ssid and pass and tries to connect
  // if it does not connect it starts an access point with the specified name
  // here  "AutoConnectAP"
  // and goes into a blocking loop awaiting configuration
  if (!wm.autoConnect()) {
    Serial.println("failed to connect and hit timeout");
    // reset and try again, or maybe put it to deep sleep
    ESP.restart();
    delay(1000);
  }

  // Get settings from persistent settings.
  preferences.begin("i2c_eeprom", false);

  use_eeprom_pages = preferences.getBool("use_pages", true);
  eeprom_page_size = preferences.getInt("page_size", 32);
  eeprom_max_attempts = preferences.getInt("max_att", 1);
  eeprom_delay_between_actions = preferences.getInt("delay", 5);
  eeprom_request_from_delay = preferences.getInt("req_delay", 5);
  blink_ip = preferences.getBool("blink_ip", true);
  // Close session.
  // Not sure why, but I feel like this might avoid issues if I keep it closed
  // when not in use.
  preferences.end();

  Serial.begin(115200);

  Serial.println("Loaded preferences: ");
  printPreferences();

  // Hostname defaults to esp3232-[MAC]
  ArduinoOTA.setHostname("eeprom32");

  ArduinoOTA
      .onStart([]() {
        SPIFFS.end();
        String type;
        if (ArduinoOTA.getCommand() == U_FLASH)
          type = "sketch";
        else // U_SPIFFS
          type = "filesystem";

        // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS
        // using SPIFFS.end()
        ticker.attach(0.1, tick);
        Serial.println("Start updating " + type);
        current_action = ARDUINOOTA_UPDATING;
        progress = 0;
        updateProgress();
      })
      .onEnd([]() { Serial.println("\nEnd"); })
      .onProgress([](unsigned int otaprogress, unsigned int total) {
        Serial.printf("Progress: %u%%\r", (otaprogress / (total / 100)));
        // The +1 is to avoid a divide by 0 error in case the update is stupidly
        // small.
        progress = (otaprogress / (total / 100) + 1);
        if (progress >= 100) {
          progress = 109;
        }
        updateProgress();
      })
      .onError([](ota_error_t error) {
        Serial.printf("Error[%u]: ", error);
        if (error == OTA_AUTH_ERROR)
          Serial.println("Auth Failed");
        else if (error == OTA_BEGIN_ERROR)
          Serial.println("Begin Failed");
        else if (error == OTA_CONNECT_ERROR)
          Serial.println("Connect Failed");
        else if (error == OTA_RECEIVE_ERROR)
          Serial.println("Receive Failed");
        else if (error == OTA_END_ERROR)
          Serial.println("End Failed");
        progress = 110;
        updateProgress();
      });

  ArduinoOTA.begin();

  setupSPIFFS();

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    // request->send(200, "text/html", page);
    if (!did_spiffs_fail) {
      request->send(SPIFFS, "/index.html", String(), false, processor);
    } else {
      request->send(200, "text/plain",
                    "SPIFFS Failed to mount. Formatted. Reflash please!");
    }
  });

  server.on("/index.html", HTTP_GET, [](AsyncWebServerRequest *request) {
    // request->send(200, "text/html", page);
    if (!did_spiffs_fail) {
      request->send(SPIFFS, "/index.html", String(), false, processor);
    } else {
      request->send(200, "text/plain",
                    "SPIFFS Failed to mount. Formatted. Reflash please!");
    }
  });

  server.on("/i2c_devices", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (current_action != READY) {
      request->send(503);
    } else {
      AsyncResponseStream *response =
          request->beginResponseStream("application/json");
      scan_i2c();
      serializeJson(i2cdoc, *response);
      request->send(response);
    }
  });

  server.on("/refresh", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (current_action != READY) {
      request->send(503);
    } else {
      AsyncResponseStream *response =
          request->beginResponseStream("application/json");
      String message;
      if (request->hasParam("dir")) {
        message = request->getParam("dir")->value();
        refreshJSON(message);
        serializeJson(file_list_doc, *response);
        request->send(response);
      } else {
        request->send(503);
      }
    }
  });

  server.on("/miniboot.html", HTTP_ANY, [](AsyncWebServerRequest *request) {
    request->send(SPIFFS, "/miniboot.html", String(), false, processor);
  });

  server.on("/upload.html", HTTP_ANY, [](AsyncWebServerRequest *request) {
    request->send(SPIFFS, "/upload.html", String(), false, processor);
  });

  server.on("/files.html", HTTP_ANY, [](AsyncWebServerRequest *request) {
    request->send(SPIFFS, "/files.html", String(), false, processor);
  });

  server.on("/dump.html", HTTP_ANY, [](AsyncWebServerRequest *request) {
    request->send(SPIFFS, "/dump.html", String(), false, processor);
  });

  server.on("/clear.html", HTTP_ANY, [](AsyncWebServerRequest *request) {
    request->send(SPIFFS, "/clear.html", String(), false, processor);
  });

  server.on("/config.html", HTTP_ANY, [](AsyncWebServerRequest *request) {
    request->send(SPIFFS, "/config.html", String(), false, processor);
  });

  server.on("/js/main.js", HTTP_ANY, [](AsyncWebServerRequest *request) {
    request->send(SPIFFS, "/js/main.js", String(), false);
  });

  server.on("/css/index.css", HTTP_ANY, [](AsyncWebServerRequest *request) {
    request->send(SPIFFS, "/css/index.css", String(), false);
  });

  server.on("/css/main.css", HTTP_ANY, [](AsyncWebServerRequest *request) {
    request->send(SPIFFS, "/css/main.css", String(), false);
  });

  // Get current progress and action
  server.on("/progress", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "application/json", progress_buffer);
  });

  // Return JSON of current settings
  server.on("/get_pref", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (current_action != READY) {
      request->send(503);
    } else {
      char buffer[160];
      sprintf(buffer,
              "{\"use_pages\":%d, \"page_size\": %d, \"max_att\": %d, "
              "\"delay\": %d, \"req_delay\": %d, \"blink_ip\": %d}",
              use_eeprom_pages, eeprom_page_size, eeprom_max_attempts,
              eeprom_delay_between_actions, eeprom_request_from_delay,
              blink_ip);
      request->send(200, "application/json", buffer);
      Serial.println("Requested preferences: ");
      printPreferences();
    }
  });

  // Save new settings to Preferences
  server.on("/set_pref", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (current_action != READY) {
      request->send(503);
    } else {
      if (request->hasParam("use_pages") && request->hasParam("page_size") &&
          request->hasParam("max_att") && request->hasParam("delay") &&
          request->hasParam("req_delay") && request->hasParam("blink_ip")) {
        current_action = UPDATING_PREFERENCES;
        progress = 0;
        use_eeprom_pages = request->getParam("use_pages")->value().toInt();
        eeprom_page_size = request->getParam("page_size")->value().toInt();
        eeprom_max_attempts = request->getParam("max_att")->value().toInt();
        eeprom_delay_between_actions =
            request->getParam("delay")->value().toInt();
        eeprom_request_from_delay =
            request->getParam("req_delay")->value().toInt();
        blink_ip = request->getParam("blink_ip")->value().toInt();

        preferences.begin("i2c_eeprom", false);

        preferences.putBool("use_pages", use_eeprom_pages);
        preferences.putInt("page_size", eeprom_page_size);
        preferences.putInt("max_att", eeprom_max_attempts);
        preferences.putInt("delay", eeprom_delay_between_actions);
        preferences.putInt("req_delay", eeprom_request_from_delay);
        preferences.putBool("blink_ip", blink_ip);

        preferences.end();

        Serial.println("New preferences: ");
        printPreferences();

        current_action = READY;
      } else {
        progress = 106;
        request->send(503, "text/plain",
                      "need 'use_pages,' 'page_size,' 'max_att,' 'delay,' and "
                      "'req_delay' params");
      }
      progress = 105;
      request->send(200, "text/plain", "saved");
    }
  });

  // Get current progress and action
  server.on("/serial_files", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (current_action != READY) {
      request->send(503);
    } else {
      listDir(SPIFFS, "/", 2);
      request->send(200, "text/plain", "OK");
    }
  });

  // Burn to I2C EEPROM
  server.on("/flash_eeprom", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (current_action != READY) {
      request->send(503);
    } else {
      if (request->hasParam("file") && request->hasParam("address")) {
        global_file = request->getParam("file")->value();
        // Removes quotemarks
        global_file.replace("\"", "");
        global_address = request->getParam("address")->value().toInt();

        if (request->hasParam("offset")) {
          global_offset = request->getParam("offset")->value().toInt();
        } else {
          global_offset = 0;
        }

        current_action = FLASHING_EEPROM;
      } else {
        request->send(503, "text/plain", "need 'file' and 'address' params");
      }
      request->send(200, "text/plain", "starting flash");
    }
  });

  // Verify I2C EEPROM
  server.on("/verify_eeprom", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (current_action != READY) {
      request->send(503);
    } else {
      if (request->hasParam("file") && request->hasParam("address")) {
        global_file = request->getParam("file")->value();
        // Removes quotemarks
        global_file.replace("\"", "");
        global_address = request->getParam("address")->value().toInt();

        if (request->hasParam("offset")) {
          global_offset = request->getParam("offset")->value().toInt();
        } else {
          global_offset = 0;
        }

        current_action = VERIFYING_EEPROM;
      } else {
        request->send(503, "text/plain", "need 'file' and 'address' params");
      }
      request->send(200, "text/plain", "starting verify");
    }
  });

  // Delete file
  server.on("/delete_file", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (current_action != READY) {
      request->send(503);
    } else {
      String file;
      if (request->hasParam("file")) {
        file = request->getParam("file")->value();
        // Removes quotemarks
        file.replace("\"", "");
        Serial.println(file);
        if (deleteFile(SPIFFS, file)) {
          request->send(200, "text/plain", "deleted");
        } else {
          request->send(503, "text/plain", "fail???");
        }

      } else {
        request->send(503, "text/plain", "need 'file' param");
      }
      request->send(200, "text/plain", "deleted");
    }
  });

  // Get current progress and action
  server.on("/verify_blink", HTTP_GET, [](AsyncWebServerRequest *request) {
    // listDir(SPIFFS, "/", 2);
    if (current_action != READY) {
      request->send(503);
    } else {
      progress = 0;
      current_action = VERIFYING_EEPROM;

      request->send(200, "text/plain", "OK");
      // Serial.println("verifying flash");
    }
  });

  //
  server.on(
      "/dump_eeprom_serial", HTTP_GET, [](AsyncWebServerRequest *request) {
        // listDir(SPIFFS, "/", 2);
        if (current_action != READY) {
          request->send(503);
        } else {
          progress = 0;
          if (request->hasParam("size") && request->hasParam("address")) {

            global_address = request->getParam("address")->value().toInt();
            global_size = request->getParam("size")->value().toInt();

            if (request->hasParam("offset")) {
              global_offset = request->getParam("offset")->value().toInt();
            } else {
              global_offset = 0;
            }

            millis_file_name = millis();

            current_action = DUMP_EEPROM_SERIAL;
            request->send(200, "text/plain", "OK");

          } else {
            request->send(503, "text/plain",
                          "need 'size' and 'address' params");
          }
        }
      });

  //
  server.on(
      "/dump_eeprom_spiffs", HTTP_GET, [](AsyncWebServerRequest *request) {
        // listDir(SPIFFS, "/", 2);
        if (current_action != READY) {
          request->send(503);
        } else {
          progress = 0;
          if (request->hasParam("size") && request->hasParam("address")) {

            global_address = request->getParam("address")->value().toInt();
            global_size = request->getParam("size")->value().toInt();

            if (request->hasParam("offset")) {
              global_offset = request->getParam("offset")->value().toInt();
            } else {
              global_offset = 0;
            }

            millis_file_name = millis();

            current_action = DUMP_EEPROM_SPIFFS;
            request->send(200, "text/plain", "OK");

          } else {
            request->send(503, "text/plain",
                          "need 'size' and 'address' params");
          }
        }
      });

  // Burn to I2C EEPROM
  server.on(
      "/get_minibyte_length", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (current_action != READY) {
          request->send(503);
        } else {
          current_action = TEMP_MINIBOOT_ACTION;
          progress = 0;
          Serial.println("Getting length from connected EEPROM");
          if (request->hasParam("address")) {
            global_address = request->getParam("address")->value().toInt();

            if (request->hasParam("offset")) {
              global_offset = request->getParam("offset")->value().toInt();
            } else {
              global_offset = 0;
            }

            if (is_eeprom_a_miniboot()) {
              Serial.println("Is a proper miniboot");
              // miniboot_json_buffer
              miniboot_extracted_length = get_miniboot_length();
              clear_crc_appname_outputs();
              updateMinibootJSONBuffer(true);
              request->send(200, "application/json", miniboot_json_buffer);
            } else {
              request->send(200, "application/json",
                            "{\"success\": false, \"length\": 0}");
            }
          } else {
            request->send(503, "text/plain", "need 'address' param");
          }
          request->send(200, "text/plain", "getting length");
          Serial.println("Done getting length");
          current_action = READY;
          progress = 100;
        }
      });

  // Burn to I2C EEPROM
  server.on("/get_minicrc", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (current_action != READY) {
      request->send(503);
    } else {
      current_action = TEMP_MINIBOOT_ACTION;
      progress = 0;
      Serial.println("Getting written CRC from connected EEPROM");
      if (request->hasParam("address")) {
        global_address = request->getParam("address")->value().toInt();

        if (request->hasParam("offset")) {
          global_offset = request->getParam("offset")->value().toInt();
        } else {
          global_offset = 0;
        }

        if (is_eeprom_a_miniboot()) {
          Serial.println("Is a proper miniboot");
          miniboot_extracted_length = 0;
          // get crc here
          clear_crc_appname_outputs();
          get_miniboot_crc_appname();
          updateMinibootJSONBuffer(true);
          request->send(200, "application/json", miniboot_json_buffer);
        } else {
          request->send(200, "application/json",
                        "{\"success\": false, \"length\": 0}");
        }
      } else {
        request->send(503, "text/plain", "need 'address' param");
      }
      request->send(200, "text/plain", "getting length");
      Serial.println("Done getting CRC");
      current_action = READY;
      progress = 100;
    }
  });

  // Burn to I2C EEPROM
  server.on(
      "/get_spiffs_minicrc", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (current_action != READY) {
          request->send(503);
        } else {
          current_action = TEMP_MINIBOOT_ACTION;
          progress = 0;
          Serial.println("Getting written CRC from connected EEPROM");
          if (request->hasParam("file")) {
            global_file = request->getParam("file")->value();
            global_file.replace("\"", "");
            if (is_spiffs_eeprom_miniboot()) {
              Serial.println("Is a proper miniboot");
              // get crc here
              clear_crc_appname_outputs();
              read_spiffs_miniboot_crc();
              read_spiffs_miniboot_appname();
              updateMinibootJSONBuffer(true);
              request->send(200, "application/json", miniboot_json_buffer);
            } else {
              Serial.println("Wasn't a proper miniboot");
              request->send(
                  200, "application/json",
                  "{\"success\": false, \"length\": 0, \"written_crc\": \"\"}");
            }
          } else {
            request->send(503, "text/plain", "need 'file' param");
          }
          request->send(200, "text/plain", "getting crc");
          Serial.println("Done getting CRC");
          current_action = READY;
          progress = 100;
        }
      });

  // Clearing I2C EEPROM
  server.on("/clear_eeprom", HTTP_GET, [](AsyncWebServerRequest *request) {
    // listDir(SPIFFS, "/", 2);
    if (current_action != READY) {
      request->send(503);
    } else {
      progress = 0;
      if (request->hasParam("size") && request->hasParam("address") &&
          request->hasParam("clear_value")) {

        global_address = request->getParam("address")->value().toInt();
        global_size = request->getParam("size")->value().toInt();

        if (request->hasParam("offset")) {
          global_offset = request->getParam("offset")->value().toInt();
        } else {
          global_offset = 0;
        }

        int given_clear_value =
            request->getParam("clear_value")->value().toInt();
        if (given_clear_value < 0 || given_clear_value > 255) {
          request->send(503, "text/plain", "Clear value must be 0-255");
        } else {
          global_clear_value = (byte)given_clear_value;
          current_action = CLEARING_EEPROM;
          request->send(200, "text/plain", "OK");
        }

      } else {
        request->send(503, "text/plain",
                      "need 'size,' 'address,' and 'clear_value' params");
      }
    }
  });

  server.onNotFound(notFound);

  server.onFileUpload(handleUpload);

  // attach filesystem root at URL /fs
  server.serveStatic("/fs", SPIFFS, "/");

  // CORS stuff.... ew.
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");

  // If they say hi just to ask about CORS, say Hi,
  // Otherwise, we don't know what you want.
  server.onNotFound([](AsyncWebServerRequest *request) {
    if (request->method() == HTTP_OPTIONS) {
      request->send(200);
    } else {
      request->send(404);
    }
  });

  // Set hostname for mDNS
  if (!MDNS.begin("eeprom32")) {
    Serial.println("Error setting up MDNS responder!");
  } else {
    Serial.println("MDNS has started.");
  }

  server.begin();

  Wire.setClock(400000);
  Wire.begin(25, 26);

  // if you get here you have connected to the WiFi
  Serial.println("Ready to go! IP:");
  Serial.println(WiFi.localIP());
  ticker.detach();
  if (blink_ip) {
    digitalWrite(LED, LOW);
    delay(2000);
    for (int i = 0; i < 4; i++) {
      int current_num = WiFi.localIP()[i];
      int num0 = -1;
      int num1 = 0;
      int num2 = 0;
      if (current_num >= 100) {
        num0 = current_num / 100;
        num1 = (current_num / 10) % 10;
      } else {
        num1 = current_num / 10;
      }

      num2 = current_num % 10;
      if (num0 != -1) {
        blinkNum(num0);
        delay(500);
      }
      blinkNum(num1);
      delay(500);
      blinkNum(num2);
      delay(500);
      delay(1000);
    }
  }

  // Set the service and port.
  MDNS.addService("http", "tcp", 80);

  // keep LED on
  digitalWrite(LED, HIGH);
}

// the loop function runs over and over again forever
void loop() {
  ArduinoOTA.handle();
  delay(500);
  updateProgress();
  if (current_action == FLASHING_EEPROM) {
    flashEEPROM();
  } else if (current_action == VERIFYING_EEPROM) {
    verifyEEPROM();
  } else if (current_action == DUMP_EEPROM_SERIAL) {
    dumpEEPROMSerial();
  } else if (current_action == DUMP_EEPROM_SPIFFS) {
    dumpEEPROMSPIFFS();
  } else if (current_action == CLEARING_EEPROM) {
    clearEEPROM();
  }
}