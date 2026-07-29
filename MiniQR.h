/*
  MiniQR.h
  Minimal QR generator for this Proshash ESP32 project.

  Scope:
  - QR Code Model 2, Version 3, ECC Level L, Mask 0
  - Byte mode payload up to 53 bytes
  - Output matrix size: 29 x 29

  This avoids Arduino QR library conflicts. It is intentionally limited,
  but enough for payloads like: PROSHASH|PROSHASH-001|A1B2C3D4
*/
#ifndef PROSHASH_MINI_QR_H
#define PROSHASH_MINI_QR_H

#include <Arduino.h>

namespace ProshashMiniQR {
  static const uint8_t VERSION = 3;
  static const uint8_t SIZE = 29;
  static const uint8_t DATA_CODEWORDS = 55;
  static const uint8_t ECC_CODEWORDS = 15;
  static const uint8_t TOTAL_CODEWORDS = 70;

  static uint8_t gfMul(uint8_t x, uint8_t y) {
    uint16_t z = 0;
    uint16_t a = x;
    uint8_t b = y;
    while (b) {
      if (b & 1) z ^= a;
      a <<= 1;
      if (a & 0x100) a ^= 0x11D;
      b >>= 1;
    }
    return (uint8_t)z;
  }

  static void reedSolomonGenerator(uint8_t degree, uint8_t gen[]) {
    for (uint8_t i = 0; i <= degree; i++) gen[i] = 0;
    gen[0] = 1;
    uint8_t len = 1;
    uint8_t root = 1;

    for (uint8_t i = 0; i < degree; i++) {
      uint8_t next[32];
      for (uint8_t j = 0; j < 32; j++) next[j] = 0;
      for (uint8_t j = 0; j < len; j++) {
        next[j]     ^= gen[j];
        next[j + 1] ^= gfMul(gen[j], root);
      }
      len++;
      for (uint8_t j = 0; j < len; j++) gen[j] = next[j];
      root = gfMul(root, 2);
    }
  }

  static void reedSolomonRemainder(const uint8_t data[], uint8_t dataLen, uint8_t ecc[]) {
    uint8_t gen[ECC_CODEWORDS + 1];
    reedSolomonGenerator(ECC_CODEWORDS, gen);

    for (uint8_t i = 0; i < ECC_CODEWORDS; i++) ecc[i] = 0;

    for (uint8_t i = 0; i < dataLen; i++) {
      uint8_t factor = data[i] ^ ecc[0];
      for (uint8_t j = 0; j < ECC_CODEWORDS - 1; j++) ecc[j] = ecc[j + 1];
      ecc[ECC_CODEWORDS - 1] = 0;
      for (uint8_t j = 0; j < ECC_CODEWORDS; j++) {
        ecc[j] ^= gfMul(gen[j + 1], factor);
      }
    }
  }

  struct BitBuffer {
    uint8_t data[DATA_CODEWORDS];
    uint16_t bitLen;

    void clear() {
      for (uint8_t i = 0; i < DATA_CODEWORDS; i++) data[i] = 0;
      bitLen = 0;
    }

    bool appendBits(uint32_t val, uint8_t len) {
      if (bitLen + len > DATA_CODEWORDS * 8) return false;
      for (int8_t i = len - 1; i >= 0; i--) {
        uint8_t bit = (val >> i) & 1;
        if (bit) data[bitLen >> 3] |= (uint8_t)(1 << (7 - (bitLen & 7)));
        bitLen++;
      }
      return true;
    }
  };

  static void setModule(bool modules[SIZE][SIZE], bool reserved[SIZE][SIZE], int x, int y, bool value, bool reserveCell) {
    if (x < 0 || y < 0 || x >= SIZE || y >= SIZE) return;
    modules[y][x] = value;
    if (reserveCell) reserved[y][x] = true;
  }

  static void drawFinder(bool modules[SIZE][SIZE], bool reserved[SIZE][SIZE], int x0, int y0) {
    for (int dy = -1; dy <= 7; dy++) {
      for (int dx = -1; dx <= 7; dx++) {
        int x = x0 + dx;
        int y = y0 + dy;
        if (x < 0 || y < 0 || x >= SIZE || y >= SIZE) continue;
        bool black = (dx >= 0 && dx <= 6 && dy >= 0 && dy <= 6 &&
          (dx == 0 || dx == 6 || dy == 0 || dy == 6 ||
          (dx >= 2 && dx <= 4 && dy >= 2 && dy <= 4)));
        setModule(modules, reserved, x, y, black, true);
      }
    }
  }

  static void drawAlignment(bool modules[SIZE][SIZE], bool reserved[SIZE][SIZE], int cx, int cy) {
    for (int dy = -2; dy <= 2; dy++) {
      for (int dx = -2; dx <= 2; dx++) {
        bool black = (max(abs(dx), abs(dy)) != 1);
        setModule(modules, reserved, cx + dx, cy + dy, black, true);
      }
    }
  }

  static void drawFunctionPatterns(bool modules[SIZE][SIZE], bool reserved[SIZE][SIZE]) {
    drawFinder(modules, reserved, 0, 0);
    drawFinder(modules, reserved, SIZE - 7, 0);
    drawFinder(modules, reserved, 0, SIZE - 7);

    for (uint8_t i = 8; i < SIZE - 8; i++) {
      setModule(modules, reserved, i, 6, (i % 2) == 0, true);
      setModule(modules, reserved, 6, i, (i % 2) == 0, true);
    }

    // Version 3 alignment pattern centers are 6 and 22. Only center (22,22) is used here.
    drawAlignment(modules, reserved, 22, 22);

    // Dark module.
    setModule(modules, reserved, 8, 4 * VERSION + 9, true, true);

    // Reserve format information areas.
    for (uint8_t i = 0; i < 9; i++) {
      if (i != 6) {
        reserved[8][i] = true;
        reserved[i][8] = true;
      }
    }
    for (uint8_t i = 0; i < 8; i++) {
      reserved[8][SIZE - 1 - i] = true;
      reserved[SIZE - 1 - i][8] = true;
    }
  }

  static void drawFormatBits(bool modules[SIZE][SIZE], bool reserved[SIZE][SIZE]) {
    // ECC Level L + Mask 0, already BCH encoded and XOR masked.
    const uint16_t fmt = 0x77C4;

    for (uint8_t j = 0; j < 6; j++) setModule(modules, reserved, 8, j, ((fmt >> j) & 1) != 0, true);
    setModule(modules, reserved, 8, 7, ((fmt >> 6) & 1) != 0, true);
    setModule(modules, reserved, 8, 8, ((fmt >> 7) & 1) != 0, true);
    setModule(modules, reserved, 7, 8, ((fmt >> 8) & 1) != 0, true);
    for (uint8_t j = 9; j < 15; j++) setModule(modules, reserved, 14 - j, 8, ((fmt >> j) & 1) != 0, true);

    for (uint8_t j = 0; j < 8; j++) setModule(modules, reserved, SIZE - 1 - j, 8, ((fmt >> j) & 1) != 0, true);
    for (uint8_t j = 8; j < 15; j++) setModule(modules, reserved, 8, SIZE - 15 + j, ((fmt >> j) & 1) != 0, true);
  }

  static bool make(const char* text, bool out[SIZE][SIZE]) {
    uint8_t len = strlen(text);
    if (len > 53) return false;

    BitBuffer bb;
    bb.clear();
    if (!bb.appendBits(0x4, 4)) return false;       // Byte mode
    if (!bb.appendBits(len, 8)) return false;        // Version 1-9 byte count
    for (uint8_t i = 0; i < len; i++) {
      if (!bb.appendBits((uint8_t)text[i], 8)) return false;
    }

    uint16_t capacityBits = DATA_CODEWORDS * 8;
    uint8_t terminator = min((uint16_t)4, (uint16_t)(capacityBits - bb.bitLen));
    bb.appendBits(0, terminator);
    while ((bb.bitLen % 8) != 0) bb.appendBits(0, 1);

    uint8_t dataLen = (bb.bitLen + 7) / 8;
    bool padToggle = false;
    while (dataLen < DATA_CODEWORDS) {
      bb.data[dataLen++] = padToggle ? 0x11 : 0xEC;
      padToggle = !padToggle;
    }

    uint8_t ecc[ECC_CODEWORDS];
    reedSolomonRemainder(bb.data, DATA_CODEWORDS, ecc);

    uint8_t codewords[TOTAL_CODEWORDS];
    for (uint8_t i = 0; i < DATA_CODEWORDS; i++) codewords[i] = bb.data[i];
    for (uint8_t i = 0; i < ECC_CODEWORDS; i++) codewords[DATA_CODEWORDS + i] = ecc[i];

    bool modules[SIZE][SIZE];
    bool reserved[SIZE][SIZE];
    for (uint8_t y = 0; y < SIZE; y++) {
      for (uint8_t x = 0; x < SIZE; x++) {
        modules[y][x] = false;
        reserved[y][x] = false;
      }
    }

    drawFunctionPatterns(modules, reserved);

    uint16_t bitIndex = 0;
    int dir = -1;
    for (int right = SIZE - 1; right > 0; right -= 2) {
      if (right == 6) right--;
      for (int vert = 0; vert < SIZE; vert++) {
        int y = (dir == -1) ? (SIZE - 1 - vert) : vert;
        for (int j = 0; j < 2; j++) {
          int x = right - j;
          if (!reserved[y][x]) {
            bool bit = false;
            if (bitIndex < TOTAL_CODEWORDS * 8) {
              uint8_t cw = codewords[bitIndex >> 3];
              bit = ((cw >> (7 - (bitIndex & 7))) & 1) != 0;
            }
            // Fixed mask pattern 0.
            if (((x + y) & 1) == 0) bit = !bit;
            modules[y][x] = bit;
            bitIndex++;
          }
        }
      }
      dir = -dir;
    }

    drawFormatBits(modules, reserved);

    for (uint8_t y = 0; y < SIZE; y++) {
      for (uint8_t x = 0; x < SIZE; x++) {
        out[y][x] = modules[y][x];
      }
    }
    return true;
  }
}

#endif
