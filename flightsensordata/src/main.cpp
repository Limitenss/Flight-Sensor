#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ctype.h>
#include <math.h>
#include <string.h>

#include "config.h"

namespace {

constexpr int kDisplayPowerPin = 15;
constexpr int kMaxPlanes = 5;
constexpr double kEarthRadiusNm = 3440.065;
constexpr double kMetersPerSecondToKnots = 1.94384449244;
constexpr double kMetersToFeet = 3.280839895;

TFT_eSPI tft;

struct PlaneInfo {
  String plane = "N/A";
  bool hasAltitude = false;
  int altitudeFeet = 0;
  bool hasSpeed = false;
  int speedKnots = 0;
  String from = "N/A";
  String to = "N/A";
  double distanceNm = INFINITY;
};

PlaneInfo closest[kMaxPlanes];
unsigned long lastRefreshMs = 0;
String statusLine = "Starting";
String openSkyToken;
unsigned long tokenExpiresAtMs = 0;

String trimField(const char *value) {
  if (value == nullptr) {
    return "N/A";
  }

  String text(value);
  text.trim();
  return text.length() == 0 ? "N/A" : text;
}

String urlEncode(const String &value) {
  String encoded;
  const char *hex = "0123456789ABCDEF";
  for (size_t i = 0; i < value.length(); ++i) {
    const unsigned char c = static_cast<unsigned char>(value[i]);
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += static_cast<char>(c);
    } else {
      encoded += '%';
      encoded += hex[(c >> 4) & 0x0F];
      encoded += hex[c & 0x0F];
    }
  }
  return encoded;
}

double degToRad(double degrees) {
  return degrees * PI / 180.0;
}

double haversineNm(double lat1, double lon1, double lat2, double lon2) {
  const double dLat = degToRad(lat2 - lat1);
  const double dLon = degToRad(lon2 - lon1);
  const double rLat1 = degToRad(lat1);
  const double rLat2 = degToRad(lat2);
  const double a = sin(dLat / 2.0) * sin(dLat / 2.0) +
                   cos(rLat1) * cos(rLat2) *
                       sin(dLon / 2.0) * sin(dLon / 2.0);
  const double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
  return kEarthRadiusNm * c;
}

void resetPlanes() {
  for (auto &plane : closest) {
    plane = PlaneInfo();
  }
}

void insertPlane(const PlaneInfo &candidate) {
  for (int i = 0; i < kMaxPlanes; ++i) {
    if (candidate.distanceNm >= closest[i].distanceNm) {
      continue;
    }

    for (int j = kMaxPlanes - 1; j > i; --j) {
      closest[j] = closest[j - 1];
    }
    closest[i] = candidate;
    return;
  }
}

void drawHeader() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("Plane", 2, 4, 2);
  tft.drawString("Alt", 82, 4, 2);
  tft.drawString("Kt", 132, 4, 2);
  tft.drawString("From", 184, 4, 2);
  tft.drawString("To", 254, 4, 2);
  tft.drawFastHLine(0, 24, 320, TFT_DARKGREY);
}

String formatNumber(bool valid, int value) {
  return valid ? String(value) : "N/A";
}

void drawPlanes() {
  drawHeader();
  tft.setTextColor(TFT_WHITE, TFT_BLACK);

  for (int i = 0; i < kMaxPlanes; ++i) {
    const int y = 28 + i * 28;
    tft.drawFastHLine(0, y - 4, 320, TFT_DARKGREY);

    const PlaneInfo &p = closest[i];
    tft.drawString(p.plane.substring(0, 9), 2, y, 2);
    tft.drawString(formatNumber(p.hasAltitude, p.altitudeFeet), 82, y, 2);
    tft.drawString(formatNumber(p.hasSpeed, p.speedKnots), 132, y, 2);
    tft.drawString(p.from.substring(0, 4), 184, y, 2);
    tft.drawString(p.to.substring(0, 4), 254, y, 2);
  }

  tft.drawFastHLine(0, 164, 320, TFT_DARKGREY);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString(statusLine.substring(0, 39), 2, 174, 2);
}

void showStatus(const String &message) {
  statusLine = message;
  Serial.println(message);
  drawPlanes();
}

void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  showStatus("Connecting WiFi");
  unsigned long started = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print('.');
    if (millis() - started > 30000UL) {
      showStatus("WiFi failed; retrying");
      WiFi.disconnect();
      delay(1000);
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      started = millis();
    }
  }

  showStatus("WiFi connected");
}

bool configHasCredentials() {
  return strlen(OPEN_SKY_CLIENT_ID) > 0 && strlen(OPEN_SKY_CLIENT_SECRET) > 0;
}

bool refreshOpenSkyToken() {
  if (!configHasCredentials()) {
    openSkyToken = "";
    return true;
  }

  if (openSkyToken.length() > 0 && millis() < tokenExpiresAtMs) {
    return true;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, "https://auth.opensky-network.org/auth/realms/opensky-network/protocol/openid-connect/token")) {
    statusLine = "Token connection failed";
    return false;
  }

  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  const String body = "grant_type=client_credentials&client_id=" +
                      urlEncode(OPEN_SKY_CLIENT_ID) + "&client_secret=" +
                      urlEncode(OPEN_SKY_CLIENT_SECRET);
  const int code = http.POST(body);
  if (code != HTTP_CODE_OK) {
    statusLine = "OpenSky token HTTP " + String(code);
    http.end();
    return false;
  }

  DynamicJsonDocument doc(8192);
  const DeserializationError error = deserializeJson(doc, http.getStream());
  http.end();

  if (error) {
    statusLine = "Token JSON error";
    return false;
  }

  openSkyToken = doc["access_token"].as<String>();
  const unsigned long expiresIn = doc["expires_in"] | 300UL;
  const unsigned long refreshIn = expiresIn > 120UL ? expiresIn - 60UL : max(30UL, expiresIn);
  tokenExpiresAtMs = millis() + refreshIn * 1000UL;
  return openSkyToken.length() > 0;
}

String openSkyUrl() {
  const double lat = USER_LATITUDE;
  const double lon = USER_LONGITUDE;
  const double latDelta = SEARCH_RADIUS_NM / 60.0;
  const double cosLat = max(0.01, fabs(cos(degToRad(lat))));
  const double lonDelta = SEARCH_RADIUS_NM / (60.0 * cosLat);

  const double lamin = max(-90.0, lat - latDelta);
  const double lamax = min(90.0, lat + latDelta);
  const double lomin = max(-180.0, lon - lonDelta);
  const double lomax = min(180.0, lon + lonDelta);

  char url[180];
  snprintf(url, sizeof(url),
           "https://opensky-network.org/api/states/all?lamin=%.5f&lomin=%.5f&lamax=%.5f&lomax=%.5f",
           lamin, lomin, lamax, lomax);
  return String(url);
}

bool fetchPlanes() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWifi();
  }

  if (!refreshOpenSkyToken()) {
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  const String url = openSkyUrl();
  if (!http.begin(client, url)) {
    statusLine = "OpenSky connection failed";
    return false;
  }

  if (openSkyToken.length() > 0) {
    http.addHeader("Authorization", "Bearer " + openSkyToken);
  }

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    statusLine = "OpenSky HTTP " + String(code);
    if (code == 429) {
      statusLine += " rate limit";
    }
    http.end();
    return false;
  }

  DynamicJsonDocument doc(OPEN_SKY_JSON_CAPACITY);
  const DeserializationError error = deserializeJson(doc, http.getStream());
  http.end();

  if (error) {
    statusLine = "OpenSky JSON too large";
    return false;
  }

  JsonArray states = doc["states"].as<JsonArray>();
  resetPlanes();

  if (states.isNull()) {
    statusLine = "No aircraft in area";
    return true;
  }

  int considered = 0;
  for (JsonVariant stateValue : states) {
    JsonArray state = stateValue.as<JsonArray>();
    if (state.isNull()) {
      continue;
    }

    if (state[5].isNull() || state[6].isNull()) {
      continue;
    }

    const double aircraftLon = state[5].as<double>();
    const double aircraftLat = state[6].as<double>();
    PlaneInfo plane;
    const char *callsign = state[1].isNull() ? nullptr : state[1].as<const char *>();
    plane.plane = trimField(callsign);
    plane.distanceNm =
        haversineNm(USER_LATITUDE, USER_LONGITUDE, aircraftLat, aircraftLon);

    if (!state[7].isNull()) {
      plane.hasAltitude = true;
      plane.altitudeFeet = lround(state[7].as<double>() * kMetersToFeet);
    }

    if (!state[9].isNull()) {
      plane.hasSpeed = true;
      plane.speedKnots = lround(state[9].as<double>() * kMetersPerSecondToKnots);
    }

    insertPlane(plane);
    ++considered;
  }

  statusLine = "Updated " + String(considered) + " aircraft";
  return true;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(kDisplayPowerPin, OUTPUT);
  digitalWrite(kDisplayPowerPin, HIGH);
#ifdef TFT_BL
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, TFT_BACKLIGHT_ON);
#endif

  tft.init();
  tft.setRotation(1);
  tft.setTextWrap(false);
  drawPlanes();

  connectWifi();
  fetchPlanes();
  drawPlanes();
  lastRefreshMs = millis();
}

void loop() {
  if (millis() - lastRefreshMs >= REFRESH_INTERVAL_MS) {
    showStatus("Refreshing");
    fetchPlanes();
    drawPlanes();
    lastRefreshMs = millis();
  }

  delay(100);
}
