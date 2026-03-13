/*
Tests the logic of the program, as well as the functionality of secrets.h
Needs Blynk Board
Does not connect to internet
*/

#include "secrets.h"
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <ArduinoJson.h>
#include <time.h>

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

// check if market is open
// return true if market is open
// return false if market is closed
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
// Replace this with real LED hardware
void setLED(int r, int g, int b) {
  Serial.printf("[LED] R=%d G=%d B=%d\n", r, g, b);
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
    setLED(50, 0, 50); // magenta = fetch error
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

  Serial.printf("[PRICE] %s Open: $%.2f Current: $%.2f\n",
                TICKER, openPrice, currentPrice);

  if (openPrice == 0.0f || currentPrice == 0.0f) {
    Serial.println("[ERROR] Invalid price data received.");
    setLED(50, 0, 50);
    return;
  }

  float changePct     = ((currentPrice - openPrice) / openPrice) * 100.0f;
  float absChangePct  = fabsf(changePct);
  int   intensity     = calcIntensity(absChangePct);

  Serial.printf("[CHANGE] %.4f%%  -> Intensity: %d/255\n", changePct, intensity);

  if (changePct > 0.0f) {
    Serial.printf("[STATUS] UP   +%.2f%% -> GREEN (intensity %d)\n", changePct, intensity);
    setLED(0, intensity, 0);
  } else if (changePct < 0.0f) {
    Serial.printf("[STATUS] DOWN  %.2f%% -> RED   (intensity %d)\n", changePct, intensity);
    setLED(intensity, 0, 0);
  } else {
    Serial.println("[STATUS] FLAT 0.00% -> turn off");
    setLED(0, 0, 0);
  }
}

// --- TESTING ---

int testsPassed = 0;
int testsFailed = 0;

void assert_eq(const char* name, int expected, int actual) {
  if (expected == actual) {
    Serial.printf("  [PASS] %s\n", name);
    testsPassed++;
  } else {
    Serial.printf("  [FAIL] %s -- expected %d, got %d\n", name, expected, actual);
    testsFailed++;
  }
}

void assert_float_eq(const char* name, float expected, float actual, float tolerance = 0.01f) {
  if (fabsf(expected - actual) <= tolerance) {
    Serial.printf("  [PASS] %s\n", name);
    testsPassed++;
  } else {
    Serial.printf("  [FAIL] %s -- expected %.4f, got %.4f\n", name, expected, actual);
    testsFailed++;
  }
}

// --- calcIntensity tests ---
void test_calcIntensity() {
  Serial.println("\n[TEST] calcIntensity()");

  assert_eq("zero change   -> 0",   0,   calcIntensity(0.0f));
  assert_eq("negative      -> 0",   0,   calcIntensity(-1.0f));
  assert_eq("at cap (2.0)  -> 255", 255, calcIntensity(2.0f));
  assert_eq("over cap(2.1) -> 255", 255, calcIntensity(2.1f));
  assert_eq("midpoint(1.0) -> 128", 128, calcIntensity(1.0f));  // lround(127.5) = 128
  assert_eq("quarter(0.5)  -> 64",  64,  calcIntensity(0.5f));  // lround(63.75) = 64
}

// --- LED color logic tests ---
struct RGB { int r, g, b; };

RGB simulateLEDOutput(float currentPrice, float openPrice) {
  if (openPrice == 0.0f || currentPrice == 0.0f) return {50, 0, 50}; // invalid
  
  float changePct    = ((currentPrice - openPrice) / openPrice) * 100.0f;
  float absChangePct = fabsf(changePct);
  int   intensity    = calcIntensity(absChangePct);

  if      (changePct > 0.0f) return {0, intensity, 0};  // green
  else if (changePct < 0.0f) return {intensity, 0, 0};  // red
  else                       return {0, 0, 0};           // flat
}

void assert_rgb(const char* name, RGB expected, RGB actual) {
  if (expected.r == actual.r && expected.g == actual.g && expected.b == actual.b) {
    Serial.printf("  [PASS] %s\n", name);
    testsPassed++;
  } else {
    Serial.printf("  [FAIL] %s -- expected (%d,%d,%d), got (%d,%d,%d)\n",
                  name,
                  expected.r, expected.g, expected.b,
                  actual.r,   actual.g,   actual.b);
    testsFailed++;
  }
}

void test_ledColorLogic() {
  Serial.println("\n[TEST] LED color logic");

  // UP cases
  assert_rgb("up   +1%  -> green 128",   {0,128,0}, simulateLEDOutput(101.0f, 100.0f));
  assert_rgb("up   +2%  -> green 255",   {0,255,0}, simulateLEDOutput(102.0f, 100.0f));
  assert_rgb("up   +5%  -> green 255",   {0,255,0}, simulateLEDOutput(105.0f, 100.0f)); // clamp

  // DOWN cases
  assert_rgb("down -1%  -> red 128",     {128,0,0}, simulateLEDOutput(99.0f,  100.0f));
  assert_rgb("down -2%  -> red 255",     {255,0,0}, simulateLEDOutput(98.0f,  100.0f));
  assert_rgb("down -5%  -> red 255",     {255,0,0}, simulateLEDOutput(95.0f,  100.0f)); // clamp

  // Flat
  assert_rgb("flat  0%  -> off",         {0,0,0},   simulateLEDOutput(100.0f, 100.0f));

  // Invalid prices
  assert_rgb("openPrice  == 0 -> magenta", {50,0,50}, simulateLEDOutput(100.0f, 0.0f));
  assert_rgb("currentPrice== 0 -> magenta",{50,0,50}, simulateLEDOutput(0.0f, 100.0f));
}

void test_secrets() {
  Serial.println("\n[TEST] secrets.h configuration");

  // Change these strings to match whatever defaults secrets.h ships with
  if (strcmp(WIFI_SSID, "your_ssid_here") == 0) {
    Serial.println("  [FAIL] WIFI_SSID is still the default -- update secrets.h");
    testsFailed++;
  } else {
    Serial.println("  [PASS] WIFI_SSID has been changed");
    testsPassed++;
  }

  if (strcmp(WIFI_PASS, "your_password_here") == 0) {
    Serial.println("  [FAIL] WIFI_PASS is still the default -- update secrets.h");
    testsFailed++;
  } else {
    Serial.println("  [PASS] WIFI_PASS has been changed");
    testsPassed++;
  }
}

void runTests() {
  Serial.println("\n========================================");
  Serial.println("[TESTS] Running self-tests...");

  test_calcIntensity();
  test_ledColorLogic();
  test_secrets();

  Serial.println("\n========================================");
  Serial.printf("[TESTS] %d passed, %d failed\n", testsPassed, testsFailed);
  Serial.println("========================================\n");
}

void setup() {
  Serial.begin(9600);
  delay(500);

  runTests();
}

void loop() {
}