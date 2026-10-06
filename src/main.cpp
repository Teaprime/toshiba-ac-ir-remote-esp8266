/*
 * Toshiba AC remote for Arduino
 * Works with my Toshiba Heat Pump RAS-10PKVP-ND, emulates remote WH-H07JE
 *
 * An IR LED must be connected to Arduino PWM pin 3.
 * Version 1, June, 2019
 * Author: 2019, Ilkka Tengvall
 * License: GPLv3
 *
 * Program waits for JSON input with command data. Possible values are:
 *   cmd:  on | off | swing | hipwr | sleep | vertical | stats
 *   temp: xx °celcius, where xx=17-30°C
 *   mode: auto | cool | dry | heat
 *   fan:  auto | 1 | 2 | 3 | 4 | 5
 *   pure: on | off
 *
 * For example:
 *   { "cmd":"on", "temp":"23", "mode":"auto", "fan": "auto" }
 *   { "cmd":"off" }
 *   { "cmd":"swing" }
 *   { "cmd":"hipwr" }
 *   { "cmd":"sleep" }
 *   { "cmd":"vertical" }
 *   { "cmd": "stats"}
 *
 * Program will acknowledge the command by sending it back as received,
 * and will report status in JSON, e.g:
 *   { "cmd": "swing", "status": "ok"}
 *   {"status": "fail", "error":"unknown command"}
 */



#include <ArduinoJson.h>
#include <ir_Toshiba_esp.h>
#include <WiFiServerSecureBearSSL.h>

// Set to 0 to read commands from the serial monitor instead of the web controller.
#define USE_WEB_SERVER 1

#if USE_WEB_SERVER
#include "web/Server.ino"
#endif

const size_t RESULT_MAX_LEN = 200;

class LedBlinker{
  enum class LedState {
    Off = 0,
    BlinkStart,
    BlinkGap,
    BlinkEnd,
    BlinkError
  };
  const uint success_led_blink = 100; // ms
  const uint error_led_blink = 300;   // ms
  ulong led_start = 0;
  LedState led_state = LedState::Off;

  public:
  LedBlinker() {}

  void init(){
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, HIGH); // turn the LED off by making the voltage HIGH
  }

  void updateLed() {
    switch (led_state)
    {
    case LedState::Off:
      return;
    case LedState::BlinkStart:
      if(millis() - led_start > success_led_blink) {
        led_state = LedState::BlinkGap;
        digitalWrite(LED_BUILTIN, HIGH); // turn the LED off by making the voltage HIGH
        led_start = millis();
        return;
      }
      break;
    case LedState::BlinkGap:
      if(millis() - led_start > success_led_blink) {
        led_state = LedState::BlinkEnd;
        digitalWrite(LED_BUILTIN, LOW); // turn the LED on (HIGH is
        led_start = millis();
        return;
      }
      break;
    case LedState::BlinkEnd:
      if (millis() - led_start > success_led_blink) {
        led_state = LedState::Off;
        led_start = millis();
        digitalWrite(LED_BUILTIN, HIGH); // turn the LED off by making the voltage HIGH
      }
      return;
    case LedState::BlinkError:
      if (millis() - led_start > error_led_blink) {
        led_state = LedState::Off;
        led_start = millis();
        digitalWrite(LED_BUILTIN, HIGH); // turn the LED off by making the voltage HIGH
      }
    default:
      break;
    }
  }
  void blinkSuccess() {
    led_state = LedState::BlinkStart;
    led_start = millis();
    digitalWrite(LED_BUILTIN, LOW); // turn the LED on (HIGH is
  }
  void blinkError() {
    led_state = LedState::BlinkError;
    led_start = millis();
    digitalWrite(LED_BUILTIN, LOW); // turn the LED on (HIGH is
  }
};

ToshibaSender irsend(D1);
LedBlinker led_blinker = LedBlinker();
struct state {
  int count = 0;
  byte cmd = 0;
  byte temp = 0;
  byte mode = 0;
  int hp = 0;
  int off = 0;
  int swing = 0;
  int hipwr = 0;
  int fan = 0;
  int pure = 0;
  int sleep = 0;
  int vertical = 0;
  int unknown = 0;
} in_state;

void setup()
{
  led_blinker.init();
  irsend.begin();
  Serial.begin(115200);
#if USE_WEB_SERVER
  webServerSetup();
#else
  while (!Serial) delay(50);
#endif
}

void loop() {
#if USE_WEB_SERVER
  webServerLoop();
#endif
  led_blinker.updateLed();
  ir_toshiba_cmd_s cmd;
  char result[RESULT_MAX_LEN];

  JsonDocument doc;
  JsonDocument stats;

#if USE_WEB_SERVER
  char commandJson[96];
  if (!webServerTakeCommand(commandJson, sizeof(commandJson))) {
    return;
  }
  auto error = deserializeJson(doc, commandJson);
#else
  auto error = deserializeJson(doc, Serial);
#endif

  if (error) {
    // Serial.print(F("deserializeJson() failed: "));
    Serial.print("{\"status\":\"waiting\",\"error\":\"");
    Serial.print(error.c_str());
    Serial.println("\"}");
    return;
  }

  const char* in_cmd = doc["cmd"];
  const char* in_temp = doc["temp"];
  const char* in_mode = doc["mode"];
  const char* in_fan = doc["fan"];
  const char* in_pure = doc["pure"];

  serializeJson(doc, Serial);

  Serial.println("");

  // Clear out command
  cmd.values.bytes[0] = 0;
  cmd.values.bytes[1] = 0;
  cmd.values.bytes[2] = 0;

  // Send ON command with temperature, AC mode and fan speed settings
  if (!strcmp(in_cmd, "on")) {

    in_state.cmd = HEAT_PUMP_CMD;
    cmd.type = HEAT_PUMP_CMD;

    in_state.temp = atoi(in_temp);
    cmd.values.field.temp = in_state.temp - TOSHIBA_HEAT_BASE;
    if (cmd.values.field.temp < 0 || cmd.values.field.temp > 13) {
      Serial.println("{\"status\":\"fail\",\"error\":\"temperature out of range\"}");
      led_blinker.blinkError();
      return;
    }

    // Set AC mode
    if (!strcmp(in_mode, "auto"))      cmd.values.field.mode = TOSHIBA_MODE_AUTO;
    else if (!strcmp(in_mode, "cool")) cmd.values.field.mode = TOSHIBA_MODE_COOL;
    else if (!strcmp(in_mode, "heat")) cmd.values.field.mode = TOSHIBA_MODE_HEAT;
    else if (!strcmp(in_mode, "dry"))  cmd.values.field.mode = TOSHIBA_MODE_DRY;
    else {
      Serial.println("{\"status\":\"fail\",\"error\":\"invalid mode\"}");
      led_blinker.blinkError();
      return;
    }
    in_state.mode = cmd.values.field.mode;

    // Set fan speed
    if (!strcmp(in_fan, "auto"))   cmd.values.field.fan = TOSHIBA_FAN_AUTO;
    else if (!strcmp(in_fan, "1")) cmd.values.field.fan = TOSHIBA_FAN_1;
    else if (!strcmp(in_fan, "2")) cmd.values.field.fan = TOSHIBA_FAN_2;
    else if (!strcmp(in_fan, "3")) cmd.values.field.fan = TOSHIBA_FAN_3;
    else if (!strcmp(in_fan, "4")) cmd.values.field.fan = TOSHIBA_FAN_4;
    else if (!strcmp(in_fan, "5")) cmd.values.field.fan = TOSHIBA_FAN_5;
    else {
      Serial.println("{\"status\":\"fail\",\"error\":\"invalid fan speed\"}");
      led_blinker.blinkError();
      return;
    }
    in_state.fan = cmd.values.field.fan;

    irsend.send(cmd);

    in_state.hp++;
    snprintf( result, sizeof(result),
      "{\"cmd\":\"on\",\"temp\":\"%i\",\"mode\":\"%s\",\"fan\":\"%s\",\"status\":\"ok\"}",
      cmd.values.field.temp, in_mode, in_fan);
    Serial.println(result);
    led_blinker.blinkSuccess();

  } // Send OFF command to turn off the AC
  else if (!strcmp(in_cmd, "off")) {

    cmd.type = HEAT_PUMP_CMD;
    cmd.values.field.mode = TOSHIBA_MODE_OFF;

    irsend.send(cmd);

    in_state.off++;
    Serial.println("{\"cmd\":\"off\",\"status\":\"ok\"}");
    led_blinker.blinkSuccess();

  } // Send change swing horizontal direction command
  else if (!strcmp(in_cmd, "swing")) {
    in_state.cmd = SWING_CMD;
    cmd.type = SWING_CMD;

    irsend.send(cmd);

    in_state.swing++;
    Serial.println("{\"cmd\":\"swing\",\"status\":\"ok\"}");
    led_blinker.blinkSuccess();

  } // Send sleep command
  else if (!strcmp(in_cmd, "sleep")) {
    in_state.cmd = SLEEP_CMD;
    cmd.type = SLEEP_CMD;

    irsend.send(cmd);

    in_state.sleep++;
    Serial.println("{\"cmd\":\"sleep\",\"status\":\"ok\"}");
    led_blinker.blinkSuccess();

  } // Send high power command
  else if (!strcmp(in_cmd, "hipwr")) {
    in_state.cmd = HI_PWR_CMD;
    cmd.type = HI_PWR_CMD;

    irsend.send(cmd);

    in_state.hipwr++;
    Serial.println("{\"cmd\":\"hipwr\",\"status\":\"ok\"}");
    led_blinker.blinkSuccess();

  } // Send change vertical direction command
  else if (!strcmp(in_cmd, "vertical")) {
    in_state.cmd = UP_DOWN_CMD;
    cmd.type = UP_DOWN_CMD;

    irsend.send(cmd);

    in_state.vertical++;
    Serial.println("{\"cmd\":\"vertical\",\"status\":\"ok\"}");
    led_blinker.blinkSuccess();

  } // Send change vertical direction command
  else if (!strcmp(in_cmd, "stats")) {
    stats["count"] = in_state.count;
    stats["cmd"] = (int)in_state.cmd;
    stats["temp"] = (int)in_state.temp;
    stats["mode"] = (int)in_state.mode;
    stats["hp"] = in_state.hp;
    stats["off"] = in_state.off;
    stats["swing"] = in_state.swing;
    stats["hipwr"] = in_state.hipwr;
    stats["fan"] = in_state.fan;
    stats["pure"] = in_state.pure;
    stats["sleep"] = in_state.sleep;
    stats["vertical"] = in_state.vertical;
    serializeJson(stats, Serial);
    Serial.println("");
 } // Did we receive some garbage?
  else {
    in_state.unknown++;
    Serial.println("{\"status\":\"fail\",\"error\":\"unknown command\"}");
    led_blinker.blinkError();
    return;
  }
  in_state.count++;
}
