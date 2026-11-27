# Minimate ESP32-S3 client

Use an ESP32-S3, an INMP441 breakout, a 128x64 SSD1306 I2C OLED, a push button, and a USB cable. Press the button to start and stop recording, and read the status and intent on the OLED. Serial Monitor commands still work as a backup.

### Pinout

The ESP32-S3 board runs from an external 5 V supply. The microphone and OLED run from an external 3.3 V supply. All grounds must be tied together.

| Component | Pin | Connect to | Notes |
| --- | --- | --- | --- |
| External 5 V supply | + | ESP32-S3 **5V** (VIN) pin | The board's regulator makes its own 3.3 V |
| External 5 V supply | − | Common GND | |
| External 3.3 V supply | + | INMP441 VDD, OLED VCC | Leave the ESP32-S3 **3V3** pin unconnected |
| External 3.3 V supply | − | Common GND | |
| ESP32-S3 | GND | Common GND | |
| INMP441 | VDD / VCC | External 3.3 V | 3.3 V only. 5 V damages it |
| INMP441 | GND | Common GND | |
| INMP441 | SCK / BCLK | GPIO 5 | `MIC_SCK` |
| INMP441 | WS / LRCLK | GPIO 4 | `MIC_WS` |
| INMP441 | SD / DOUT | GPIO 6 | `MIC_SD` |
| INMP441 | L/R | Common GND | Left slot, `MIC_SLOT = 0`. Tie to 3.3 V for right, `MIC_SLOT = 1` |
| SSD1306 OLED | VCC | External 3.3 V | Keep on 3.3 V so its I2C pull-ups stay at ESP32 logic level |
| SSD1306 OLED | GND | Common GND | |
| SSD1306 OLED | SDA | GPIO 8 | `OLED_SDA` |
| SSD1306 OLED | SCL | GPIO 9 | `OLED_SCL` |
| Push button | Leg 1 | GPIO 7 | `BUTTON_PIN`, internal pull-up |
| Push button | Leg 2 | Common GND | |

- **Common ground.** Without it, the I2S and I2C signals have no shared reference and the mic and OLED will not work.
- **3.3 V rails.** Don't join the external 3.3 V supply to the board's 3V3 pin. Two regulators driving one rail can back-feed each other.
- **USB with external 5 V.** Don't plug in USB while the external 5 V supply is on unless your board has a protection diode on 5V. Otherwise turn off the 5 V supply while uploading over USB.

The OLED pins, its `0x3C` address, and the button pin are constants at the top of the sketch.

1. In Arduino IDE, install **esp32 by Espressif Systems** (3.x) in Boards Manager and **WebSockets by Markus Sattler** and **Adafruit SSD1306** (with its Adafruit GFX dependency) in Library Manager.
2. Copy `config.example.h` to `config.h` in this folder. Set your 2.4 GHz Wi-Fi credentials and the FastAPI host’s LAN IP in `ASR_HOST`. Use the IP alone and set `ASR_PORT` if it differs from 8000. Change the GPIO constants if your board uses different pins.

   If you already copied an earlier config, set `MIC_SCK = 5` and `MIC_WS = 4` to match the wiring above.
3. Start the ASR server and wait for readiness. Keep the ESP32 and server on a network that allows the device to reach the server port.
4. Open `minimate-esp32s3-client.ino`, select your ESP32-S3 board and port, then upload. For native USB, enable **USB CDC On Boot** if your board requires it for serial output.
5. Wait for the idle face on the OLED. Press the button. When the listening face appears, speak for up to five seconds. Press again after at least 0.4 seconds to stop early, or let the bar under the face fill. The thinking face shows while the server works. The reply face then shows for three seconds before the idle face returns. In Serial Monitor at **115200 baud**, **r** and **s** do the same as the button, and the intent ID and name are printed.

The sketch selects the microphone's I2S slot, sends 16 kHz mono PCM16 as binary WebSocket messages to `/ws/asr`, then sends text `END`. Use `MIC_SLOT = 1` only if the microphone’s L/R pin is wired to 3V3. Connection loss drops the current recording. Wait for the reconnect message before trying again.

Each successful reply sends filtered audio as small binary PCM16 chunks, then a plain-text ID from the sketch's `IntentId` enum. The ESP32 reports how many audio bytes it received. Use the dashboard to play the returned audio. `-1` means no intent. If you use a new checkpoint with a different class order, update the enum to match its `id2label`.

| ID | Intent |
| --- | --- |
| 0 | `check_focus_session` |
| 1 | `focus_session_control` |
| 2 | `greeting` |
| 3 | `open_question` |
| 4 | `out_of_scope` |
| 5 | `repeat_response` |
| 6 | `schedule_reminder` |

### Logs without USB

Every log line goes to Serial at 115200 baud and to the server, so you can watch it at `http://127.0.0.1:8000/dashboard` under **Device log** when the board runs on external 5 V. Lines start with the device's uptime. The sketch keeps the last 24 lines while Wi-Fi or the server is down and sends them once it reconnects. If more lines were dropped, it reports how many. The first lines after boot give the reset reason (for example `brownout, check the 5 V supply`), and the Wi-Fi IP and RSSI once Wi-Fi connects.

### OLED faces

While connecting, the OLED shows **Wi-Fi...** or **Server...** as text. After that it shows animated faces at 15 fps.

| Device state or reply | Face |
| --- | --- |
| Ready | idle |
| Recording | listening, with a progress bar on the bottom two rows |
| Waiting for the server | processing |
| Intents 0–3, 5, 6 | speaking, for 3 s |
| `4` out_of_scope, or `-1` no intent | clarification, for 3 s |
| Mic, send, timeout, or unexpected reply | error, for 3 s |

To change which face an intent gets, edit `INTENT_FACES` in the sketch.

`face_frames_128x64.h` is generated, so don't edit it by hand. It holds the 128x128 face animations scaled to 128x64. One crop and a 0.68 scale apply to every frame, so the face doesn't jump when the state changes. The frames are stored in SSD1306 page layout, so each frame is copied straight into the display buffer. After changing the source faces, regenerate the header with Python, numpy, and Pillow:

```sh
python tools/make_oled_faces.py ~/face_animations/face_animations.h --preview faces.png
```

The frames take 90 KiB of flash. The whole sketch is about 1.2 MB. That's 38% of a 3 MB app partition (**16M Flash (3MB APP/9.9MB FATFS)** on a 16 MB board) but 92% of the default 1.25 MB partition, so choose a larger **Partition Scheme** if your board is on the default.

### Check the microphone

Open [inmp441_check.ino](diagnostics/inmp441_check/inmp441_check.ino) as a separate sketch. Upload it and open Serial Monitor at 115200 baud. It prints how much the left and right samples varied over each second. Compare a quiet second with a second when you speak close to the mic. With L/R wired to GND, the left value should rise when you speak. With L/R wired to 3V3, watch the right value and set `MIC_SLOT=1` in the streaming sketch.

If neither slot reacts to speech, check 3V3 and common ground, confirm L/R is tied to a rail, and match SCK, WS, and SD to the GPIOs in the diagnostic sketch. Enable USB CDC On Boot if using native USB and no serial output appears. Reupload the streaming sketch after testing.

If the span stays zero, check `nonzero` and `changing`. A zero `nonzero` count means every raw I2S word was zero. A nonzero `changing` mask with zero span means some raw bits varied below the PCM16 bits the streaming sketch keeps.

[Arduino ESP32 I2S API](https://docs.espressif.com/projects/arduino-esp32/en/latest/api/i2s.html) · [INMP441 datasheet](https://invensense.tdk.com/wp-content/uploads/2015/02/INMP441.pdf) · [Arduino WebSockets](https://github.com/Links2004/arduinoWebSockets)
