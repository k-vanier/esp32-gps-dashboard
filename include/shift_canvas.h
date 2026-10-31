// Canvas 16 bits avec décalage horizontal : permet de dessiner une page
// à n'importe quelle position x (transitions glissées) sans toucher au code des pages.
// Toutes les primitives Adafruit GFX finissent dans drawPixel / drawFastHLine / drawFastVLine.
#pragma once

#include <Adafruit_GFX.h>

class ShiftCanvas : public GFXcanvas16 {
public:
  ShiftCanvas(uint16_t w, uint16_t h) : GFXcanvas16(w, h) {}

  void setOffset(int16_t ox) { offset = ox; }

  void drawPixel(int16_t x, int16_t y, uint16_t color) override {
    GFXcanvas16::drawPixel(x + offset, y, color);
  }
  void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) override {
    GFXcanvas16::drawFastHLine(x + offset, y, w, color);
  }
  void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) override {
    GFXcanvas16::drawFastVLine(x + offset, y, h, color);
  }

private:
  int16_t offset = 0;
};
