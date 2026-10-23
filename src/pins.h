#pragma once
// Hardware pin map (NodeMCU / Wemos D1 mini style ESP8266 boards).
// Target connector: VCC, GND, SWCLK (P300), SWDIO (P108), RES (optional but recommended).
// Only GPIO0..15 can be used for SWCLK / SWDIO (GPIO16 is not on the fast GPIO registers).

#ifndef PIN_SWCLK
#define PIN_SWCLK 14   // D5 -> target SWCLK (P300)
#endif
#ifndef PIN_SWDIO
#define PIN_SWDIO 12   // D6 <-> target SWDIO (P108), internal pull-up enabled
#endif
#ifndef PIN_NRST
#define PIN_NRST 13    // D7 -> target RES (open drain: driven low or released). -1 if not wired
#endif
#ifndef PIN_BUTTON
#define PIN_BUTTON 0   // D3 = onboard "FLASH" button on NodeMCU, active low
#endif
#ifndef PIN_LED_PASS
#define PIN_LED_PASS 5 // D1 -> green LED (active high)
#endif
#ifndef PIN_LED_FAIL
#define PIN_LED_FAIL 4 // D2 -> red LED (active high)
#endif
#ifndef PIN_LED_BUSY
#define PIN_LED_BUSY 2 // D4 = onboard blue LED (active low)
#endif
#ifndef PIN_BUZZER
#define PIN_BUZZER 15  // D8 -> buzzer via transistor (active high). -1 to disable
#endif

#define FW_VERSION "1.0.0"
