#pragma once

constexpr char WIFI_SSID[] = "your-wifi";
constexpr char WIFI_PASSWORD[] = "your-password";
constexpr char ASR_HOST[] = "192.168.1.100";
constexpr unsigned short ASR_PORT = 8000;

// Default wiring. Adjust for your board's available GPIOs.
constexpr int MIC_SCK = 5;
constexpr int MIC_WS = 4;
constexpr int MIC_SD = 6;
constexpr unsigned MIC_SLOT = 0;  // L/R to GND = 0 (left), to 3V3 = 1 (right).
static_assert(MIC_SLOT <= 1, "MIC_SLOT must be 0 or 1");
