# Libraries and hardware references

- ESP32 Arduino core documentation: https://docs.espressif.com/projects/arduino-esp32/en/latest/
- HX711 library by bogde: https://github.com/bogde/HX711
- DFRobot DFPlayer Mini library: https://github.com/DFRobot/DFRobotDFPlayerMini
- SIMCom SIM800 series hardware design reference: https://www.simcom.com/

The firmware uses the HX711 and DFRobot DFPlayer Mini libraries listed in `platformio.ini`. SMS is sent through the SIM800L AT-command interface without an additional GSM library.

Hardware warning: SIM800-series reference designs specify a 3.4–4.4 V VBAT range and transmit current bursts that can reach approximately 2 A. The actual power supply, grounding, UART levels and module breakout must be checked against the exact module documentation before connecting it. See the SIMCom hardware design references above.
