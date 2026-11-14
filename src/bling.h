#pragma once

// "Bling" build (-D BLING, env esp32-s3-devkitc-1-bling): an extra NeoPixel
// strip (rainbow animation), a GC9A01 round display showing the log, and an
// I2C bus brought up for experimenting with sensors. In normal builds these
// are empty inline stubs.
#ifdef BLING
void blingSetup();
void blingLoop();
#else
inline void blingSetup() {}
inline void blingLoop() {}
#endif
