# Wiring assumptions and confirmations needed

Before applying power, have the person responsible for the schematic confirm each item:

- [ ] Exact ESP32 dev board and exposed GPIO numbering.
- [ ] Four load-cell type (full bridge versus half bridge; number of wires; excitation ratings).
- [ ] Which cells are under the floor plate and which, if any, are in grab-bar mounts.
- [ ] How each group forms a Wheatstone bridge and how many HX711 channels are used.
- [ ] HX711 RATE selection for 10 or 80 samples/second; 80 SPS is required for the experimental load-cell BCG processing.
- [ ] Whether the intended BCG source is platform load cells or an MPU6050/ADXL345 accelerometer. The latter is listed in the BOM but not clearly wired in the submitted schematic.
- [ ] The impedance excitation waveform, amplitude/current limit, reference network, analogue conditioning/demodulation and ADC output range.
- [ ] ESP32 ADC pin receiving a conditioned signal no higher than 3.3 V.
- [ ] SIM800L supply capable of transmit-current peaks, logic levels, UART pins, SIM and actual local 2G/SMS coverage.
- [ ] DFPlayer UART pins, common ground, SD-card file layout and speaker connection.
- [ ] Safe mechanical load and calibration fixture for bar-force measurement.

The pin map in the sketch is provisional and must be reconciled with the final schematic. The current code intentionally marks weight invalid unless the floor and grab-bar channels are both calibrated. It intentionally does not convert impedance ADC counts into ohms.
