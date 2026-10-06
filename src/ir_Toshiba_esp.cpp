#include <ir_Toshiba_esp.h>

//==============================================================================
//
//                           T O S H I B A
//
//  Toshiba Heat Pump RAS-10PKVP-ND has a remote WH-H07JE. It sends three
//  different lenghts signals. Most of the buttons go within 9 byte sequence.
//  The last byte is parity. 
//
//  Ilkka Tengvall
//
//==============================================================================

#define TOSHIBA_9_BYTES        9

#define TOSHIBA_7_BYTES        7

#define TOSHIBA_10_BYTES      10

#define TOSHIBA_HDR_MARK    4400
#define TOSHIBA_HDR_SPACE   4400
#define TOSHIBA_BIT_MARK     550
#define TOSHIBA_ONE_SPACE   1600
#define TOSHIBA_ZERO_SPACE   550



ToshibaSender::ToshibaSender(uint16_t pin) : sender(pin) {}

void ToshibaSender::begin() {
  sender.begin();
}

void ToshibaSender::send(const ir_toshiba_cmd_s& command) {
  byte data[TOSHIBA_10_BYTES] = {};
  uint8_t bytes = 0;

  switch (command.type) {
    case HEAT_PUMP_CMD:
      data[0] = 0xf2;
      data[1] = 0x0d;
      data[2] = 0x03;
      data[3] = 0xfc;
      data[4] = 0x01;
      data[5] = command.values.bytes[0];
      data[6] = command.values.bytes[1];
      data[7] = command.values.bytes[2];
      for (uint8_t i = 0; i < 8; i++) {
        data[8] ^= data[i];
      }
      bytes = TOSHIBA_9_BYTES;
      break;
    case HI_PWR_CMD:
      data[0] = 0xf2;
      data[1] = 0x0d;
      data[2] = 0x04;
      data[3] = 0xfb;
      data[4] = 0x09;
      data[8] = 0x01;
      data[9] = 0x08;
      bytes = TOSHIBA_10_BYTES;
      break;
    case SLEEP_CMD:
      data[0] = 0xf2;
      data[1] = 0x0d;
      data[2] = 0x04;
      data[3] = 0xfb;
      data[4] = 0x09;
      data[8] = 0x03;
      data[9] = 0x0a;
      bytes = TOSHIBA_10_BYTES;
      break;
    case SWING_CMD:
      data[0] = 0xf2;
      data[1] = 0x0d;
      data[2] = 0x01;
      data[3] = 0xfe;
      data[4] = 0x21;
      data[5] = 0x04;
      data[6] = 0x25;
      bytes = TOSHIBA_7_BYTES;
      break;
    case UP_DOWN_CMD:
      data[0] = 0xf2;
      data[1] = 0x0d;
      data[2] = 0x01;
      data[3] = 0xfe;
      data[4] = 0x21;
      data[6] = 0x21;
      bytes = TOSHIBA_7_BYTES;
      break;
    default:
      return;
  }

  sender.enableIROut(38);
  sender.mark(TOSHIBA_HDR_MARK);
  sender.space(TOSHIBA_HDR_SPACE);

  for (uint8_t i = 0; i < bytes; i++) {
    for (uint8_t mask = 0x80; mask != 0; mask >>= 1) {
      sender.mark(TOSHIBA_BIT_MARK);
      sender.space((data[i] & mask) ? TOSHIBA_ONE_SPACE : TOSHIBA_ZERO_SPACE);
    }
  }

  sender.mark(TOSHIBA_BIT_MARK);
  sender.space(0);
}
