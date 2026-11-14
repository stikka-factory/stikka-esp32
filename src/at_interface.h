#pragma once

// Serial AT configuration interface. Entered by holding the BOOT button for
// 10 seconds after boot; type AT+help for the command list. While active the
// serial port carries only the AT dialogue (log output is silenced on it).
void atInterfaceSetup();
void atInterfaceLoop();
bool atInterfaceActive();
