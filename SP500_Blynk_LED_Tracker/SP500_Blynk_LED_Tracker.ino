#include "secrets.h" // passwords

#define BLYNK_PRINT Serial   // route Blynk debug output to Serial Monitor

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <BlynkSimpleEsp8266.h>
#include <ArduinoJson.h>

// Fixed configs
const char*          TICKER               = "SPY";    // can change to other tickers
const unsigned long  REFRESH_INTERVAL_MS  = 60000UL;  // 1 minute
const float          MAX_CHANGE_PCT       = 2.0f;     // 100% intensity at ±2 %

// market opening and closing times 
// times change based on months, so covers earlist and latest possible close
const int MARKET_OPEN_HOUR_UTC  = 13;   // earliest possible open  (PDT)
const int MARKET_OPEN_MIN_UTC   = 30;
const int MARKET_CLOSE_HOUR_UTC = 21;   // latest  possible close (PST)
const int MARKET_CLOSE_MIN_UTC  = 0;

BlynkTimer timer;

// --- INTENSITY CALCULATION ---------------------------------------------------
/**
 * Maps an absolute percentage change to a 0–255 LED intensity.
 * Uses lround() to avoid float-truncation bias.
 *
 * @param changePct  Absolute percentage change (0.0 – MAX_CHANGE_PCT)
 * @return           Integer brightness in range [0, 255]
 */
int calcIntensity(float changePct) {
  if (changePct <= 0.0f) return 0;
  if (changePct >= MAX_CHANGE_PCT) return 255;
  return (int)lround((changePct / MAX_CHANGE_PCT) * 255.0f);
}

// Check if market is open
// Return true if market is open
// Return false if market is closed
bool isMarketOpen() {
  time_t now = time(nullptr);
  struct tm* t = gmtime(&now);

  // Weekends are always closed
  if (t->tm_wday == 0 || t->tm_wday == 6) {
    Serial.println("[MARKET] Weekend -- market closed.");
    return false;
  }

  // Convert time to minutes for calculations
  int nowMins   = t->tm_hour * 60 + t->tm_min;
  int openMins  = MARKET_OPEN_HOUR_UTC  * 60 + MARKET_OPEN_MIN_UTC;
  int closeMins = MARKET_CLOSE_HOUR_UTC * 60 + MARKET_CLOSE_MIN_UTC;

  bool open = (nowMins >= openMins && nowMins < closeMins);
  Serial.printf("[MARKET] UTC %02d:%02d -> Market %s\n",
                t->tm_hour, t->tm_min, open ? "OPEN" : "CLOSED");
  return open;
}

// Writes RGB values to Blynk virtual pins V2/V3/V4.
// Skips the write and logs a warning if Blynk is not currently connected.
void setLED(int r, int g, int b) {
  if (!Blynk.connected()) {
    Serial.println("[WARN] Blynk disconnected -- skipping LED update.");
    return;
  }
  Blynk.virtualWrite(V2, r);
  Blynk.virtualWrite(V3, g);
  Blynk.virtualWrite(V4, b);
  Serial.printf("[LED] R=%d  G=%d  B=%d\n", r, g, b);
}

// fetch data from Yahoo Finance via GET request
/* Legend:
 *   UP   -> green,  intensity proportional to % gain  (capped at MAX_CHANGE_PCT)
 *   DOWN -> red,    intensity proportional to % loss
 *   FLAT -> turn off (0, 0, 0)
 *   Error / closed -> blue (0, 0, 100)
*/
void fetchAndUpdateLED() {
  Serial.println("\n========================================");
  Serial.println("[FETCH] Starting market data update...");

  if (!isMarketOpen()) {
    Serial.println("[STATUS] Market closed -> BLUE");
    setLED(0, 0, 100); // sets to blue
    if (Blynk.connected()) {
      Blynk.virtualWrite(V10, "Market Closed");
      Blynk.virtualWrite(V11, "No data (closed)");
    }
    return;
  }

  // HTTPS client
  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
  client->setInsecure();

  HTTPClient https;
  String url = String("https://query1.finance.yahoo.com/v8/finance/chart/") +
               TICKER + "?interval=1d&range=1d";

  Serial.printf("[HTTP] GET %s\n", url.c_str());
  https.begin(*client, url);
  https.addHeader("User-Agent", "Mozilla/5.0");

  int httpCode = https.GET();
  Serial.printf("[HTTP] Response code: %d\n", httpCode);

  // http code for rate limit
  if (httpCode == 429) {
    Serial.println("[WARN] Yahoo Finance rate limit hit (HTTP 429) -- skipping update.");
    https.end();
    return;
  }

  // http error code
  if (httpCode != 200) {
    Serial.printf("[ERROR] HTTP failed: %d\n", httpCode);
    setLED(50, 0, 50);   // magenta = fetch error
    if (Blynk.connected()) Blynk.virtualWrite(V10, "Fetch Error");
    https.end();
    return;
  }

  String payload = https.getString();
  https.end();
  Serial.printf("[HTTP] Payload length: %d bytes\n", payload.length());

  // StaticJsonDocument avoids heap fragmentation on the ESP8266
  StaticJsonDocument<8192> doc;
  DeserializationError err = deserializeJson(doc, payload);

  if (err) {
    Serial.printf("[ERROR] JSON parse failed: %s\n", err.c_str());
    setLED(50, 0, 50);
    if (Blynk.connected()) Blynk.virtualWrite(V10, "JSON Error");
    return;
  }

  JsonObject meta      = doc["chart"]["result"][0]["meta"];
  float currentPrice   = meta["regularMarketPrice"] | 0.0f;
  float openPrice      = meta["regularMarketOpen"]   | 0.0f;

  // Fall back to previous close if today's open is not yet available
  if (openPrice == 0.0f) {
    openPrice = meta["chartPreviousClose"] | 0.0f;
    Serial.println("[WARN] Open price missing -- using previousClose as baseline.");
  }

  Serial.printf("[PRICE] %s  Open: $%.2f  Current: $%.2f\n",
                TICKER, openPrice, currentPrice);

  if (openPrice == 0.0f || currentPrice == 0.0f) {
    Serial.println("[ERROR] Invalid price data received.");
    setLED(50, 0, 50);
    if (Blynk.connected()) Blynk.virtualWrite(V10, "Bad Data");
    return;
  }

  float changePct    = ((currentPrice - openPrice) / openPrice) * 100.0f;
  float absChangePct = fabsf(changePct);
  int   intensity    = calcIntensity(absChangePct);

  Serial.printf("[CHANGE] %.4f%%  -> Intensity: %d/255\n", changePct, intensity);

  String statusLabel;
  if (changePct > 0.0f) {
    Serial.printf("[STATUS] UP   +%.2f%% -> GREEN (intensity %d)\n", changePct, intensity);
          (0, intensity, 0);
    statusLabel = String("UP +") + String(changePct, 2) + "%";
  } else if (changePct < 0.0f) {
    Serial.printf("[STATUS] DOWN  %.2f%% -> RED   (intensity %d)\n", changePct, intensity);
    setLED(intensity, 0, 0);
    statusLabel = String("DOWN ") + String(changePct, 2) + "%";
  } else {
    Serial.println("[STATUS] FLAT 0.00% -> turn off");
    setLED(0, 0, 0);
    statusLabel = "FLAT 0.00%";
  }

  if (Blynk.connected()) {
    Blynk.virtualWrite(V10, statusLabel);
    String priceStr = String(TICKER) + " $" + String(currentPrice, 2) +
                      " (open $" + String(openPrice, 2) + ")";
    Blynk.virtualWrite(V11, priceStr);
  }

  Serial.println("========================================\n");
}

void setup() {
  Serial.begin(9600);
  delay(500);

  // Catch case where secrets.h was never filled in
  if (String(BLYNK_TEMPLATE_ID)  == "TMPL_XXXXXXXXXX" ||
    String(BLYNK_AUTH_TOKEN)   == "your_blynk_auth_token_here" ||
    String(WIFI_SSID)          == "your_wifi_ssid_here") 
  {
    Serial.println("[ERROR] secrets.h is not filled in! FIll it out and restart");
    ESP.deepSleep(0);
  }

  Serial.println("\n[BOOT] S&P 500 Blynk LED Tracker");
  Serial.printf("[BOOT] Template : %s (%s)\n", BLYNK_TEMPLATE_NAME, BLYNK_TEMPLATE_ID);
  Serial.printf("[BOOT] SSID     : %s\n",      WIFI_SSID);
  Serial.printf("[BOOT] Host     : %s  Port: %d\n", BLYNK_HOST, BLYNK_PORT);
  Serial.println("[BOOT] Connecting...");

  Blynk.begin(BLYNK_AUTH_TOKEN, WIFI_SSID, WIFI_PASS, BLYNK_HOST, BLYNK_PORT);

  // Sync NTP for market-hours detection (UTC, no offset)
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("[NTP] Syncing time");
  while (time(nullptr) < 1000000000UL) {
    delay(500);
    Serial.print(".");
  }
  Serial.println(" done.");

  fetchAndUpdateLED();

  timer.setInterval(REFRESH_INTERVAL_MS, fetchAndUpdateLED);
  Serial.printf("[TIMER] Refresh every %lu ms\n", REFRESH_INTERVAL_MS);
}

void loop() {
  Blynk.run();
  timer.run();
}
