#include "secrets.h"
#include <iostream>
#include <cmath>
#include <cstring>


// Fixed configs
const char* TICKER = "SPY";
const float MAX_CHANGE_PCT = 2.0f;

// --- INTENSITY CALCULATION ---
int calcIntensity(float changePct) {
  if (changePct <= 0.0f) return 0;
  if (changePct >= MAX_CHANGE_PCT) return 255;
  return (int)lround((changePct / MAX_CHANGE_PCT) * 255.0f);
}

// --- LED OUTPUT ---
void setLED(int r, int g, int b) {
  printf("[LED] R=%d G=%d B=%d\n", r, g, b);
}

// --- TESTING ---
int testsPassed = 0;
int testsFailed = 0;

void assert_eq(const char* name, int expected, int actual) {
  if (expected == actual) {
    printf("  [PASS] %s\n", name);
    testsPassed++;
  } else {
    printf("  [FAIL] %s -- expected %d, got %d\n", name, expected, actual);
    testsFailed++;
  }
}

void assert_float_eq(const char* name, float expected, float actual, float tolerance = 0.01f) {
  if (fabs(expected - actual) <= tolerance) {
    printf("  [PASS] %s\n", name);
    testsPassed++;
  } else {
    printf("  [FAIL] %s -- expected %.4f, got %.4f\n", name, expected, actual);
    testsFailed++;
  }
}

// --- calcIntensity tests ---
void test_calcIntensity() {
  std::cout << "\n[TEST] calcIntensity()\n";

  assert_eq("zero change   -> 0",   0,   calcIntensity(0.0f));
  assert_eq("negative      -> 0",   0,   calcIntensity(-1.0f));
  assert_eq("at cap (2.0)  -> 255", 255, calcIntensity(2.0f));
  assert_eq("over cap(2.1) -> 255", 255, calcIntensity(2.1f));
  assert_eq("midpoint(1.0) -> 128", 128, calcIntensity(1.0f));
  assert_eq("quarter(0.5)  -> 64",  64,  calcIntensity(0.5f));
}

// --- LED color logic tests ---
struct RGB { int r, g, b; };

RGB simulateLEDOutput(float currentPrice, float openPrice) {
  if (openPrice == 0.0f || currentPrice == 0.0f)
    return {50,0,50};

  float changePct = ((currentPrice - openPrice) / openPrice) * 100.0f;
  float absChangePct = fabs(changePct);
  int intensity = calcIntensity(absChangePct);

  if (changePct > 0.0f)      return {0,intensity,0};
  else if (changePct < 0.0f) return {intensity,0,0};
  else                       return {0,0,0};
}

void assert_rgb(const char* name, RGB expected, RGB actual) {
  if (expected.r == actual.r &&
      expected.g == actual.g &&
      expected.b == actual.b) {

    printf("  [PASS] %s\n", name);
    testsPassed++;
  } 
  else {
    printf("  [FAIL] %s -- expected (%d,%d,%d), got (%d,%d,%d)\n",
           name,
           expected.r, expected.g, expected.b,
           actual.r, actual.g, actual.b);

    testsFailed++;
  }
}

void test_ledColorLogic() {
  std::cout << "\n[TEST] LED color logic\n";

  assert_rgb("up   +1%  -> green 128",   {0,128,0}, simulateLEDOutput(101.0f,100.0f));
  assert_rgb("up   +2%  -> green 255",   {0,255,0}, simulateLEDOutput(102.0f,100.0f));
  assert_rgb("up   +5%  -> green 255",   {0,255,0}, simulateLEDOutput(105.0f,100.0f));

  assert_rgb("down -1%  -> red 128",     {128,0,0}, simulateLEDOutput(99.0f,100.0f));
  assert_rgb("down -2%  -> red 255",     {255,0,0}, simulateLEDOutput(98.0f,100.0f));
  assert_rgb("down -5%  -> red 255",     {255,0,0}, simulateLEDOutput(95.0f,100.0f));

  assert_rgb("flat  0%  -> off",         {0,0,0}, simulateLEDOutput(100.0f,100.0f));

  assert_rgb("openPrice  == 0 -> magenta", {50,0,50}, simulateLEDOutput(100.0f,0.0f));
  assert_rgb("currentPrice== 0 -> magenta",{50,0,50}, simulateLEDOutput(0.0f,100.0f));
}

void test_secrets() {
  std::cout << "\n[TEST] secrets.h configuration\n";

  if (strcmp(WIFI_SSID,"your_wifi_ssid_here")==0) {
    std::cout << "  [FAIL] WIFI_SSID is still default\n";
    testsFailed++;
  } else {
    std::cout << "  [PASS] WIFI_SSID has been changed\n";
    testsPassed++;
  }

  if (strcmp(WIFI_PASS,"your_wifi_pass_here")==0) {
    std::cout << "  [FAIL] WIFI_PASS is still default\n";
    testsFailed++;
  } else {
    std::cout << "  [PASS] WIFI_PASS has been changed\n";
    testsPassed++;
  }
}

void runTests() {
  std::cout << "\n========================================\n";
  std::cout << "[TESTS] Running self-tests...\n";

  test_calcIntensity();
  test_ledColorLogic();
  test_secrets();

  std::cout << "\n========================================\n";
  std::cout << "[TESTS] " << testsPassed << " passed, "
            << testsFailed << " failed\n";
  std::cout << "========================================\n";
}

int main() {

  runTests();

  return 0;
}