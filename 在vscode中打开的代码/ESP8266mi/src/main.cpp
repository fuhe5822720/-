#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <math.h>

// ================= 网络配置 =================
const char* ssid     = "wifi名称";
const char* password = "wifi密码";

// ================= 天气配置 =================
const float LATITUDE  = 所在地坐标;
const float LONGITUDE = 所在地坐标;
const char* host = "api.open-meteo.com";

// ================= 硬件核心配置 =================
#define PIN_NEOPIXEL D4
#define NUM_LEDS 100
int BRIGHTNESS = 80;   

Adafruit_NeoPixel np(NUM_LEDS, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);

// ================= 安全按键引脚 =================
#define PIN_UP     D5  // 亮度+
#define PIN_DOWN   D6  // 亮度-
#define PIN_LEFT   D7  // 天气
#define PIN_RIGHT  D2  // 计时
#define PIN_OK     D1  // 中键/主题

// ================= 板级偏移量 =================
const int BOARD_OFFSETS[5] = {0, 20, 40, 60, 80};
const int digit_pairs[10][2] = {
   {5, 15}, {4, 14}, {6, 16}, {3, 13}, {7, 17},
   {2, 12}, {8, 18}, {1, 11}, {9, 19}, {10, 0}
};
const int COLON_LED_1 = BOARD_OFFSETS[2] + 5;  // 45
const int COLON_LED_2 = BOARD_OFFSETS[2] + 15; // 55

// ================= 状态与主题 =================
enum AppState { CLOCK, WEATHER, TIMER };
AppState currentState = CLOCK;

enum ClockTheme { THEME_DEFAULT, THEME_CLOVER, THEME_MAHOGANY, THEME_RAINBOW };
ClockTheme currentTheme = THEME_DEFAULT;

unsigned long timerBeginMs = 0;
bool timerRunning = false;

int cachedTemp = -999;
int cachedHum = -999;
int cachedWeatherCode = 0;
bool hasWeatherData = false;

// ================= 按键状态与防抖 =================
bool lastUp = HIGH, lastDown = HIGH, lastLeft = HIGH, lastRight = HIGH, lastOk = HIGH;
unsigned long lastButtonTime[5] = {0, 0, 0, 0, 0}; 
const unsigned long DEBOUNCE_MS = 200;

unsigned long rainbowStartTime = 0;
int rainbowHue = 0;

// ================= 断网检测与本地补偿 =================
unsigned long lastWifiCheckMs = 0;
unsigned long lastSyncMs = 0;
time_t lastSyncedEpoch = 0;
bool isInternetDown = false;

// ================= 自动修复漂移新增变量 =================
unsigned long lastNTPResyncMs = 0;     // 上次自动校时的时间
const unsigned long NTP_RESYNC_INTERVAL = 6UL * 60UL * 60UL * 1000UL; // 6小时=21600000毫秒

// ================= 颜色定义 =================
uint32_t colorDefault = np.Color(95, 120, 155);    // 蓝紫主题色
uint32_t colorClover = np.Color(60, 179, 113);     // 三叶草绿
uint32_t colorMahogany = np.Color(74, 4, 4);       // 牛血红（断网警告色）
uint32_t colorTempCold = np.Color(100, 149, 237);  // 矢车菊蓝（极寒温度）
uint32_t colorTempMild = np.Color(250, 200, 152);  // 柔和橙色（舒适温度）
uint32_t colorTempHot = np.Color(255, 127, 80);    // 珊瑚色（炎热温度）

// ================= HSV 转 RGB =================
uint32_t HSVtoRGB(int hue, int saturation = 255, int value = 200) {
    float h = hue / 360.0;
    float s = saturation / 255.0;
    float v = value / 255.0;
    int i = floor(h * 6);
    float f = h * 6 - i;
    float p = v * (1 - s);
    float q = v * (1 - f * s);
    float t = v * (1 - (1 - f) * s);
    
    float r = 0, g = 0, b = 0; 
    if      (i % 6 == 0) { r = v; g = t; b = p; }
    else if (i % 6 == 1) { r = q; g = v; b = p; }
    else if (i % 6 == 2) { r = p; g = v; b = t; }
    else if (i % 6 == 3) { r = p; g = q; b = v; }
    else if (i % 6 == 4) { r = t; g = p; b = v; }
    else                 { r = v; g = p; b = q; }
    return np.Color((int)(r * 255), (int)(g * 255), (int)(b * 255));
}

uint32_t getThemeColor() {
    switch(currentTheme) {
        case THEME_CLOVER: return colorClover;
        case THEME_MAHOGANY: return colorMahogany;
        case THEME_RAINBOW: return HSVtoRGB(rainbowHue);
        default: return colorDefault;
    }
}

uint32_t getTemperatureColor(int temperature) {
    if (temperature < 0) return colorTempCold;
    else if (temperature >= 0 && temperature <= 30) return colorTempMild;
    return colorTempHot;
}

// ================= 平滑心跳冒号（5秒循环） =================
void updateHeartbeatColon(int currentMilliseconds, uint32_t color) {
    int cycleMs = currentMilliseconds % 5000; 
    float phase = (cycleMs / 5000.0) * 2.0 * M_PI;
    int brightness = (int)((sin(phase) + 1.0) * 127.5);
    if (brightness < 10) brightness = 10;

    uint8_t r = ((color >> 16) & 0xFF) * brightness / 255;
    uint8_t g = ((color >> 8) & 0xFF) * brightness / 255;
    uint8_t b = (color & 0xFF) * brightness / 255;

    np.setPixelColor(COLON_LED_1, np.Color(r, g, b));
    np.setPixelColor(COLON_LED_2, np.Color(r, g, b));
}

// ================= 带褪色效果的渲染 =================
void renderWithFade(int ledIndex, uint32_t color, int fadeStrength) {
    uint8_t r = ((color >> 16) & 0xFF) * fadeStrength / 255;
    uint8_t g = ((color >> 8) & 0xFF) * fadeStrength / 255;
    uint8_t b = (color & 0xFF) * fadeStrength / 255;
    np.setPixelColor(ledIndex, np.Color(r, g, b));
}

// ================= 显示数字 =================
void displayDigitOnBoard(int boardIndex, int digit, uint32_t color = np.Color(95, 120, 155)) {
    int offset = BOARD_OFFSETS[boardIndex];
    if (digit >= 0 && digit <= 9) {
        renderWithFade(offset + digit_pairs[digit][0], color, 255);
        renderWithFade(offset + digit_pairs[digit][1], color, 255);
    }
}

// ================= 显示天气图标 =================
void displayWeatherIcon(int weatherCode) {
    int offset = BOARD_OFFSETS[2]; 
    int digit = -1;
    if (weatherCode == 0 || weatherCode == 1) digit = 1;
    else if (weatherCode == 2) digit = 2;
    else if (weatherCode == 3) digit = 3;
    else if (weatherCode >= 51 && weatherCode <= 67) digit = 5;
    else if (weatherCode == 45 || weatherCode == 48) digit = 6;
    else if (weatherCode == 71 || weatherCode == 77) digit = 8;
    else if (weatherCode >= 80 && weatherCode <= 99) digit = 7;

    if (digit != -1) {
        // 【天气图标颜色】：浅米灰
        np.setPixelColor(offset + digit_pairs[digit][0], np.Color(180, 165, 155));
        np.setPixelColor(offset + digit_pairs[digit][1], np.Color(180, 165, 155));
    }
}

// ================= 获取天气 =================
void fetchWeather() {
    if (WiFi.status() == WL_CONNECTED) {
        WiFiClientSecure client;
        client.setInsecure(); 
        HTTPClient http;
        String url = "https://" + String(host) + "/v1/forecast?latitude=" + String(LATITUDE, 2) + 
                     "&longitude=" + String(LONGITUDE, 2) + 
                     "&current=temperature_2m,relative_humidity_2m,weather_code";
        http.begin(client, url);
        int httpCode = http.GET();
        if (httpCode == 200) {
            String payload = http.getString();
            JsonDocument doc; 
            DeserializationError error = deserializeJson(doc, payload);
            if (!error) {
                cachedTemp = doc["current"]["temperature_2m"].as<int>();
                cachedHum = doc["current"]["relative_humidity_2m"].as<int>();
                cachedWeatherCode = doc["current"]["weather_code"].as<int>();
                hasWeatherData = true;
                Serial.printf("天气数据: 温度=%d°C, 湿度=%d%%, 天气代码=%d\n", cachedTemp, cachedHum, cachedWeatherCode);
            }
        }
        http.end();
    }
}

// ================= 自动修复漂移（后台静默校时） =================
void autoResyncTime() {
    if (WiFi.status() == WL_CONNECTED) {
        // 如果距离上次同步已经过去6小时，重新从NTP获取时间
        if (millis() - lastNTPResyncMs > NTP_RESYNC_INTERVAL) {
            configTime(8 * 3600, 0, "pool.ntp.org", "time.nist.gov");
            
            // 等待最多2秒，2秒内获取到就同步，获取不到不阻塞，下次再试
            time_t now = time(nullptr);
            int retries = 0;
            while (time(nullptr) < 100000 && retries < 20) { 
                delay(100);
                retries++;
            }

            if (time(nullptr) > 100000) {
                lastSyncedEpoch = time(nullptr);
                lastSyncMs = millis();
                lastNTPResyncMs = millis();
                Serial.println("[自动修复] 后台完成自动校时");
            }
        }
    } else {
        // 如果断网，本地补偿计时
        lastNTPResyncMs = millis(); // 重新计时，避免一恢复就开始空等
    }
}

// ================= 安全按键处理 =================
void handleButton(int pin, int buttonIndex, void (*action)(), bool &lastState) {
    bool curr = digitalRead(pin);
    unsigned long now = millis();
    
    if (curr == LOW && lastState == HIGH && (now - lastButtonTime[buttonIndex] > DEBOUNCE_MS)) {
        lastButtonTime[buttonIndex] = now;
        action();
    }
    lastState = curr;
}

void actionBrightnessUp() {
    BRIGHTNESS = min(255, BRIGHTNESS + 20);
    np.setBrightness(BRIGHTNESS);
    Serial.println("亮度+");
}

void actionBrightnessDown() {
    BRIGHTNESS = max(0, BRIGHTNESS - 20);
    np.setBrightness(BRIGHTNESS);
    Serial.println("亮度-");
}

void actionOK() {
    if (currentState == CLOCK) {
        switch(currentTheme) {
            case THEME_DEFAULT: currentTheme = THEME_CLOVER; break;
            case THEME_CLOVER: currentTheme = THEME_MAHOGANY; break;
            case THEME_MAHOGANY: currentTheme = THEME_RAINBOW; rainbowStartTime = millis(); break;
            case THEME_RAINBOW: currentTheme = THEME_DEFAULT; break;
        }
    } else {
        currentState = CLOCK;
        timerRunning = false;
    }
    Serial.println("中间键处理");
}

void actionWeather() {
    currentState = WEATHER;
    timerRunning = false;
    hasWeatherData = false;
    fetchWeather();
    Serial.println("切换到天气");
}

void actionTimer() {
    currentState = TIMER;
    timerBeginMs = millis();
    timerRunning = true;
    Serial.println("切换到计时");
}

// ================= 初始化 =================
void setup() {
    Serial.begin(115200);
    np.begin();
    np.setBrightness(BRIGHTNESS);
    np.show();

    pinMode(PIN_UP, INPUT_PULLUP);
    pinMode(PIN_DOWN, INPUT_PULLUP);
    pinMode(PIN_LEFT, INPUT_PULLUP);
    pinMode(PIN_RIGHT, INPUT_PULLUP);
    pinMode(PIN_OK, INPUT_PULLUP);

    Serial.print("连接WiFi中");
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi已连接!");
    
    configTime(8 * 3600, 0, "pool.ntp.org", "time.nist.gov");
    while (time(nullptr) < 100000) {
        delay(100);
    }
    Serial.println("时间获取成功！");

    lastSyncedEpoch = time(nullptr);
    lastSyncMs = millis();
    lastNTPResyncMs = millis(); // 初始化自动校时计时器
}

// ================= 主循环 =================
void loop() {
    handleButton(PIN_UP, 0, actionBrightnessUp, lastUp);
    handleButton(PIN_DOWN, 1, actionBrightnessDown, lastDown);
    handleButton(PIN_LEFT, 2, actionWeather, lastLeft);
    handleButton(PIN_RIGHT, 3, actionTimer, lastRight);
    handleButton(PIN_OK, 4, actionOK, lastOk);
    
    // ==================== 核心：后台自动修复漂移 ====================
    autoResyncTime();

    static unsigned long lastRefresh = 0;
    if (millis() - lastRefresh > 30) {
        lastRefresh = millis();
        
        if (currentTheme == THEME_RAINBOW && currentState == CLOCK) {
            rainbowHue = (rainbowHue + 1) % 360;
        }

        for (int i = 0; i < NUM_LEDS; i++) np.setPixelColor(i, np.Color(0, 0, 0));

        // ==================== 断网检测与本地时间补偿 ====================
        if (millis() - lastWifiCheckMs > 1000) {
            lastWifiCheckMs = millis();
            if (WiFi.status() == WL_CONNECTED) {
                isInternetDown = false;
                if (time(nullptr) > 100000) {
                    lastSyncedEpoch = time(nullptr);
                    lastSyncMs = millis();
                    Serial.println("[网络恢复] 本地时间已重新校准");
                }
            } else {
                isInternetDown = true;
                Serial.println("[断网] 启动本地时钟补偿 (红9警告)");
            }
        }
        
        // ==================== 计算当前时间 ====================
        time_t now;
        if (isInternetDown) {
            now = lastSyncedEpoch + (millis() - lastSyncMs) / 1000;
        } else {
            now = time(nullptr);
        }

        // ==================== 正常显示模式（无论是否断网） ====================
        if (currentState == CLOCK) {
            struct tm* timeinfo = localtime(&now);
            int hour = timeinfo->tm_hour, minute = timeinfo->tm_min;
            int hour_tens = hour / 10, hour_ones = hour % 10;
            int minute_tens = minute / 10, minute_ones = minute % 10;
            uint32_t clockColor = getThemeColor();
            
            displayDigitOnBoard(4, hour_tens, clockColor);
            displayDigitOnBoard(3, hour_ones, clockColor);
            displayDigitOnBoard(1, minute_tens, clockColor);
            displayDigitOnBoard(0, minute_ones, clockColor);

            // 冒号颜色不变！使用当前主题色呼吸
            updateHeartbeatColon(millis(), clockColor);
        } 
        else if (currentState == WEATHER) {
            if (hasWeatherData) {
                uint32_t tempColor = getTemperatureColor(cachedTemp);
                int t_tens = cachedTemp / 10, t_ones = cachedTemp % 10;
                int h_tens = cachedHum / 10, h_ones = cachedHum % 10;
                displayDigitOnBoard(0, t_ones, tempColor);
                if (t_tens > 0 || cachedTemp < 0) {
                    if (cachedTemp < 0 && t_tens == 0) {
                        int offset = BOARD_OFFSETS[1];
                        np.setPixelColor(offset + 5, tempColor);
                        np.setPixelColor(offset + 15, tempColor);
                    } else displayDigitOnBoard(1, t_tens, tempColor);
                }
                displayDigitOnBoard(3, h_ones, np.Color(115, 147, 179));
                if (h_tens > 0) displayDigitOnBoard(4, h_tens, np.Color(115, 147, 179));
                displayWeatherIcon(cachedWeatherCode); 
            } else {
                displayDigitOnBoard(0, 8, np.Color(50, 50, 50));
                displayDigitOnBoard(1, 8, np.Color(50, 50, 50));
                displayDigitOnBoard(3, 8, np.Color(50, 50, 50));
                displayDigitOnBoard(4, 8, np.Color(50, 50, 50));
            }
        } 
        else if (currentState == TIMER) {
            unsigned long elapsed = timerRunning ? (millis() - timerBeginMs) / 1000 : 0;
            int seconds = elapsed % 60;
            int minutes = (elapsed / 60) % 60;
            displayDigitOnBoard(0, seconds % 10, np.Color(255, 255, 120));
            displayDigitOnBoard(1, seconds / 10, np.Color(255, 255, 120));
            displayDigitOnBoard(3, minutes % 10, np.Color(255, 255, 120));
            displayDigitOnBoard(4, minutes / 10, np.Color(255, 255, 120));
            displayDigitOnBoard(2, 4, np.Color(152, 251, 152)); 
        }

        // ==================== 断网警告显示 ====================
        if (isInternetDown) {
            displayDigitOnBoard(2, 9, colorMahogany);
        }

        np.show();
    }
}
