#pragma once

// Fill these in before flashing.
#define WIFI_SSID "YOUR_WIFI_NAME"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// The T-Display S3 has no built-in GPS, so set your fixed location here.
// Decimal degrees: north/east positive, south/west negative.
#define USER_LATITUDE 41.8781
#define USER_LONGITUDE -87.6298

// Search radius around USER_LATITUDE/USER_LONGITUDE.
// 150 NM usually keeps the OpenSky response small enough for the ESP32-S3.
#define SEARCH_RADIUS_NM 150.0

// OpenSky can be used anonymously, but authenticated clients get more credits.
// Leave both blank for anonymous requests. If you create OpenSky API credentials,
// paste the OAuth client id and client secret here.
#define OPEN_SKY_CLIENT_ID ""
#define OPEN_SKY_CLIENT_SECRET ""

// Refresh interval. Anonymous OpenSky users have 10 second state resolution and
// daily credits, so avoid polling too aggressively.
#define REFRESH_INTERVAL_MS 60000UL

// Increase if your search radius returns many aircraft and JSON parsing fails.
#define OPEN_SKY_JSON_CAPACITY 262144
