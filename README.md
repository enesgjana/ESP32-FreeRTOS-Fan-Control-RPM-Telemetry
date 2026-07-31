# ESP32 FreeRTOS Fan Control and RPM Telemetry Platform

A real-time embedded fan-control and telemetry system developed in C with ESP-IDF and FreeRTOS. It reads a potentiometer through the ESP32 ADC, generates a 25 kHz PWM control signal, measures fan speed using interrupt-driven tachometer acquisition, and displays live power and RPM telemetry on a 128 x 64 SSD1306 OLED.

## Key Features

- 25 kHz hardware PWM using the LEDC peripheral
- 12-bit ADC acquisition for manual speed control
- Interrupt-driven tachometer measurement
- Dedicated FreeRTOS task for RPM calculation
- I2C SSD1306 OLED telemetry
- Custom 5 x 7 framebuffer font
- 100 ms control and display loop
- Tachometer noise-rejection window

## Pin Assignment

| ESP32 pin | Function |
|---|---|
| GPIO 21 | OLED SDA |
| GPIO 22 | OLED SCL |
| GPIO 25 | Fan PWM control |
| GPIO 33 / ADC1 channel 5 | Potentiometer input |
| GPIO 14 | Fan tachometer input |

Connect all circuit grounds together.

## Electrical Considerations

A standard four-wire PC fan normally expects an open-collector or open-drain PWM signal. Use a suitable transistor interface and verify the requirements of the specific fan.

Many tachometer outputs are open-collector. Pull the signal up to 3.3 V before connecting it to the ESP32. Never expose an ESP32 input to 5 V or 12 V.

## RPM Calculation

Assuming two tachometer pulses per revolution:

```text
RPM = pulses_per_second x 60 / 2
```

Change the divisor if the selected fan produces a different number of pulses per revolution.

## Build and Flash

ESP-IDF 5.2 or newer is required.

```bash
idf.py set-target esp32
idf.py build
idf.py -p PORT flash monitor
```

Replace `PORT` with the board serial port, such as `COM5` on Windows or `/dev/ttyUSB0` on Linux.

## Current Limitations

- PWM duty is commanded directly by the potentiometer.
- RPM measurement assumes two pulses per revolution.
- ADC input is not digitally filtered.
- RPM resolution is based on a one-second measurement window.

## Possible Extensions

- PID-based target-RPM control
- Fan-stall detection
- ADC and RPM filtering
- Configurable pulses per revolution
- Minimum startup-duty handling
- UART, Wi-Fi, or MQTT telemetry
