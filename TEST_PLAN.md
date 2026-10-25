# Bench test plan (before testing with a person)

1. **Compile-only test:** build the project with the selected ESP32 board and declared libraries.
2. **Storage:** issue `status`, `storage`, `capture`; reboot; verify `/sessions.csv` and calibration state persist.
3. **HX711 presence:** disconnect each channel one at a time and verify the firmware does not claim a valid true weight.
4. **Floor calibration:** use a secured known test load; repeat at 2–3 different loads; verify repeatability.
5. **Bar-force calibration:** test the sensor in a safe mechanical fixture and confirm it measures the intended vertical support force with the correct sign.
6. **BCG feasibility:** confirm actual HX711 sampling rate using raw timestamps; then inspect waveforms with a stationary test load and controlled vibration source. Do not claim BCG heart rate is valid until independently measured/validated.
7. **Impedance:** do not connect electrodes to a person until a qualified review establishes safe current-limited excitation and protection. First validate with known resistors; implement ohm conversion only after that circuit is stable.
8. **Audio:** populate microSD with numbered prompts; test each `test_audio` command at low volume.
9. **SMS:** configure a test number locally; confirm `AT` response, `CREG` registration, `CSQ` and actual receipt. Test network loss and queue retry. Never use an emergency number for testing.
10. **Power:** measure the ESP32, DFPlayer and SIM800L rails under worst-case SMS transmission, including voltage dips and reset behaviour.
11. **Long-run:** run multiple sessions; inspect serial output and log rotation; reboot during a queued SMS and verify the pending message persists.
12. **Simulation/validation:** evaluate the rule logic using synthetic data, separate virtual people used for tuning from held-out test people, and report sensitivity, specificity, median lead time and false alerts per patient-month.

This is a prototype bench test plan. Passing these tests does not establish clinical validity.
