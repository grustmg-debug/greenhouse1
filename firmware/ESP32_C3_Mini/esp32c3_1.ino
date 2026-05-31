// 🌱 Теплица | Этап 1: Прототип v2.0
// ESP32-C3 + AHT20+BMP280 + Ёмкостный датчик почвы + MQTT + OTA + Web

#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_AHTX0.h>
#include <Adafruit_BMP280.h>
#include <PubSubClient.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <Update.h>

// 🔧 КОНФИГ
const char* WIFI_SSID = "AHome";
const char* WIFI_PASS = "RNKA1791G513";
const char* MQTT_HOST = "192.168.100.234";
const uint16_t MQTT_PORT = 1883;
const char* MODULE_ID = "proto1";  // Уникальный ID модуля

// Топики (с динамическим ID модуля)
String TOPIC_TELEMETRY, TOPIC_COMMAND, TOPIC_STATUS;

// Пины
const int PIN_LED = 2;
const int PIN_BTN = 3;
const int PIN_SOIL_SIG = 1;   // ADC: GPIO1 = ADC1_CH0
const int PIN_SOIL_PWR = 4;   // Питание датчика почвы (включаем только при чтении)

// Калибровка датчика почвы (заполняется при калибровке)
const int SOIL_DRY_RAW = 2720;   // Значение в сухом грунте
const int SOIL_WET_RAW = 350;   // Значение в воде/влажном грунте

// Глобальные объекты
Adafruit_AHTX0 aht;
Adafruit_BMP280 bmp;
WiFiClient espClient;
PubSubClient mqtt(espClient);
AsyncWebServer webServer(80);
AsyncWebServer otaServer(8080);

unsigned long lastRead = 0;
unsigned long lastMqttCheck = 0;
bool valveState = false;
bool manualMode = false;

// 📢 ПРОТОТИПЫ
void connectWifi();
void connectMqtt();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void readSensors(float &temp, float &hum, float &pressure, int &soilRaw, float &soilPercent);
void setValve(bool state);
void setupOTA();
void calibrateSoil();
String buildJsonPayload(float t, float h, float p, int soilRaw, float soilPct);

// 🔌 ИНИЦИАЛИЗАЦИЯ
void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_BTN, INPUT_PULLUP);
  pinMode(PIN_SOIL_PWR, OUTPUT);
  digitalWrite(PIN_LED, LOW);
  digitalWrite(PIN_SOIL_PWR, LOW);  // Датчик почвы выключен по умолчанию

  WiFi.setSleep(false);

  // I2C: SDA=8, SCL=9
  Wire.begin(8, 9);
  
  // AHT20
  if (!aht.begin()) {
    Serial.println("❌ AHT20 not found");
    while(1) delay(100);
  }
  
  // BMP280
  if (!bmp.begin(0x76)) {  // Пробуем адрес 0x76
    if (!bmp.begin(0x77)) {  // Или 0x77
      Serial.println("❌ BMP280 not found");
      while(1) delay(100);
    }
  }
  bmp.setSampling(Adafruit_BMP280::MODE_NORMAL,
                  Adafruit_BMP280::SAMPLING_X2,
                  Adafruit_BMP280::SAMPLING_X16,
                  Adafruit_BMP280::FILTER_X16,
                  Adafruit_BMP280::STANDBY_MS_500);
  
  Serial.println("✅ Sensors OK");

  // Формируем топики с ID модуля
  TOPIC_TELEMETRY = "greenhouse/" + String(MODULE_ID) + "/telemetry";
  TOPIC_COMMAND   = "greenhouse/" + String(MODULE_ID) + "/command";
  TOPIC_STATUS    = "greenhouse/" + String(MODULE_ID) + "/status";

  connectWifi();

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  connectMqtt();

  // Веб-статус
  webServer.on("/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    float t, h, p; int soilRaw; float soilPct;
    readSensors(t, h, p, soilRaw, soilPct);
    request->send(200, "application/json", buildJsonPayload(t, h, p, soilRaw, soilPct));
  });
  webServer.begin();

  setupOTA();
  
  Serial.println("🚀 Ready. IP: " + WiFi.localIP().toString());
  Serial.println("🌐 Status: http://" + WiFi.localIP().toString() + "/status");
  //void calibrateSoil();
}

// 🔁 ЦИКЛ
void loop() {

  if (!mqtt.connected()) {
    if (millis() - lastMqttCheck > 5000) {
      connectMqtt();
      lastMqttCheck = millis();
    }
  } else {
    mqtt.loop();
  }

  if (millis() - lastRead > 2000) {
    lastRead = millis();
    float t, h, p; int soilRaw; float soilPct;
    readSensors(t, h, p, soilRaw, soilPct);
    //calibrateSoil();  
    Serial.printf("📊 T: %.1f°C | H: %.1f%% | P: %.1f hPa | Soil: %d (%.1f%%)\n", 
                  t, h, p, soilRaw, soilPct);

    String payload = buildJsonPayload(t, h, p, soilRaw, soilPct);
    mqtt.publish(TOPIC_TELEMETRY.c_str(), payload.c_str(), false);
  }

  // Кнопка
  static unsigned long lastBtn = 0;
  if (millis() - lastBtn > 50) {
    if (digitalRead(PIN_BTN) == LOW) {
      manualMode = !manualMode;
      Serial.println(manualMode ? "🔧 Manual" : "🤖 Auto");
      lastBtn = millis();
    }
  }
}

// 📡 MQTT CALLBACK
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  
  if (String(topic) == TOPIC_COMMAND) {
    if (msg.indexOf("\"valve\":1") >= 0 || msg.indexOf("\"valve\":true") >= 0) setValve(true);
    else if (msg.indexOf("\"valve\":0") >= 0 || msg.indexOf("\"valve\":false") >= 0) setValve(false);
  }
}

// 🔧 ФУНКЦИИ
// Калибровка датчика почвы:
void calibrateSoil() {
  Serial.println("🔧 Калибровка почвы:");
  Serial.println("1. Вытащи датчик из земли, дай высохнуть 5 мин");
  Serial.println("2. В Serial Monitor введи 'D' для записи DRY-значения");
  Serial.println("3. Погрузи в воду (не до электроники!) — введи 'W' для WET");
  
  while (!Serial.available()) delay(100);
  char cmd = Serial.read();
  
  int val = analogRead(PIN_SOIL_SIG);
  if (cmd == 'D' || cmd == 'd') {
    Serial.printf("✅ DRY: %d → вставь в код как SOIL_DRY_RAW\n", val);
  } else if (cmd == 'W' || cmd == 'w') {
    Serial.printf("✅ WET: %d → вставь в код как SOIL_WET_RAW\n", val);
  }
}

void connectWifi() {
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("📶 WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(300); Serial.print("."); }
  Serial.println("\n✅ " + WiFi.localIP().toString());
}

void connectMqtt() {
  String clientID = "esp32c3_" + String(MODULE_ID) + "_" + String(random(0xFFFF), HEX);
  if (mqtt.connect(clientID.c_str(), TOPIC_STATUS.c_str(), 1, true, "offline")) {
    mqtt.publish(TOPIC_STATUS.c_str(), "online", true);
    mqtt.subscribe(TOPIC_COMMAND.c_str());
    Serial.println("✅ MQTT connected");
  } else {
    Serial.printf("⏳ MQTT wait (rc=%d)\n", mqtt.state());
  }
}

// 📊 Чтение всех датчиков
void readSensors(float &temp, float &hum, float &pressure, int &soilRaw, float &soilPercent) {
  // AHT20
  sensors_event_t humidity, t_air;
  aht.getEvent(&humidity, &t_air);
  temp = t_air.temperature;
  hum = humidity.relative_humidity;
  
  // BMP280
  pressure = bmp.readPressure() / 100.0;  // Па → гПа (hPa)
  
  // Почва: включаем питание, читаем, выключаем
  //digitalWrite(PIN_SOIL_PWR, HIGH);
 // delay(10);  // Стабилизация
  soilRaw = analogRead(PIN_SOIL_SIG);
 //digitalWrite(PIN_SOIL_PWR, LOW);
  
  // Конвертация в проценты (линейная интерполяция)
  soilPercent = 100.0 * (SOIL_DRY_RAW - soilRaw) / (SOIL_DRY_RAW - SOIL_WET_RAW);
  soilPercent = constrain(soilPercent, 0, 100);
}

// 🧱 Сборка JSON-пакета
String buildJsonPayload(float t, float h, float p, int soilRaw, float soilPct) {
  char buf[256];
  snprintf(buf, sizeof(buf), 
    "{\"module\":\"%s\",\"ts\":%lu,"
    "\"air\":{\"t\":%.1f,\"h\":%.1f},"
    "\"pressure\":{\"v\":%.2f},"
    "\"soil\":{\"raw\":%d,\"pct\":%.1f},"
    "\"state\":{\"valve\":%s,\"manual\":%s}}",
    MODULE_ID,
    (unsigned long)(millis() / 1000),
    t, h,
    p,
    soilRaw, soilPct,
    valveState ? "true" : "false",
    manualMode ? "true" : "false"
  );
  return String(buf);
}

void setValve(bool state) {
  valveState = state;
  digitalWrite(PIN_LED, state ? HIGH : LOW);
  Serial.println(state ? "🔵 Valve OPEN" : "⚫ Valve CLOSED");
}

void setupOTA() {
  otaServer.on("/update", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/html", 
      "<form method='POST' action='/do_update' enctype='multipart/form-data'>"
      "<input type='file' name='firmware' accept='.bin'><br><br>"
      "<input type='submit' value='🔄 Update'>"
      "</form>");
  });

  otaServer.on("/do_update", HTTP_POST, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", Update.hasError() ? "❌ FAIL" : "✅ OK");
    if (!Update.hasError()) ESP.restart();
  }, [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
    if (!index) {
      Serial.printf("🔄 OTA: %s\n", filename.c_str());
      Update.begin(request->contentLength() > 0 ? request->contentLength() : UPDATE_SIZE_UNKNOWN);
    }
    Update.write(data, len);
    if (final) {
      Update.end(true);
      Serial.println(Update.isFinished() ? "✅ OTA done" : "❌ OTA fail");
    }
  });
  otaServer.begin();
  Serial.println("🔄 OTA: http://" + WiFi.localIP().toString() + ":8080/update");
}