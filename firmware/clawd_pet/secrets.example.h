#pragma once
// Copy to secrets.h and fill in (secrets.h is gitignored). ESP32 only supports 2.4 GHz Wi-Fi.
// 复制成 secrets.h 再填写（secrets.h 不进 git）。ESP32 只支持 2.4GHz Wi-Fi。
#define WIFI_SSID "your-wifi-name"
#define WIFI_PASS "your-wifi-password"

// Name on the local network: http://clawd.local
#define HOSTNAME  "clawd"

// Optional but recommended: password for over-the-air updates.
// Without it, anyone on your LAN can flash the device.
// #define OTA_PASS  "something"

// POSIX TZ string for your local time (sky colors and the clock follow it). Default: UTC.
// Examples: "CST-8" (China), "JST-9" (Japan), "PST8PDT,M3.2.0,M11.1.0" (US Pacific),
//           "CET-1CEST,M3.5.0,M10.5.0/3" (Central Europe)
#define TZ_INFO   "UTC0"
// Optional: a nearby NTP server (pool.ntp.org is used as a fallback anyway)
// #define NTP_SERVER "pool.ntp.org"

// Optional: weather from Open-Meteo (no API key). Leave undefined to hide weather.
// #define WEATHER_LAT "51.5074"
// #define WEATHER_LON "-0.1278"
