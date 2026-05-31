// 🌱 Теплица | Этап 1: Прототип v1.2
// ESP32-C3 Super Mini + AHT20 + LED + Кнопка + MQTT + OTA + Web
// Библиотеки: pubsubclient3, ESPAsyncWebSrv (dvarrel/ESP32Async), Adafruit_AHTX0

#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_AHTX0.h>
#include <PubSubClient.h>      // Работает с pubsubclient3
#include <ESPAsyncWebServer.h>    // или ESPAsyncWebServer.h, если ставил официальный форк
#include <AsyncTCP.h>
#include <Update.h>

// 🔧 КОНФИГ (ЗАМЕНИ ПЕРЕД ПРОШИВКОЙ)
const char* WIFI_SSID = "AHome";
const char* WIFI_PASS = "RNKA1791G513";
const char* MQTT_HOST = "192.168.100.25"; // IP Orange Pi / Mosquitto
const uint16_t MQTT_PORT = 1883;

// Топики
const char* TOPIC_TELEMETRY = "greenhouse/proto1/telemetry";
const char* TOPIC_COMMAND   = "greenhouse/proto1/command";
const char* TOPIC_STATUS    = "greenhouse/proto1/status";

// Пины
const int PIN_LED = 2;
const int PIN_BTN = 3;

// Глобальные объекты
Adafruit_AHTX0 aht;
WiFiClient espClient;
PubSubClient mqtt(espClient);
AsyncWebServer webServer(80);
AsyncWebServer otaServer(8080);

unsigned long lastRead = 0;
unsigned long lastMqttCheck = 0;
bool valveState = false;
bool manualMode = false;

// 📢 ПРОТОТИПЫ (обязательно для Arduino C++)
void connectWifi();
void connectMqtt();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void readSensors(float &t, float &h);
void setValve(bool state);
void setupOTA();

// 🔌 ИНИЦИАЛИЗАЦИЯ
void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_BTN, INPUT_PULLUP);
  digitalWrite(PIN_LED, LOW);

  WiFi.setSleep(false); // Стабильность TCP на C3

  // I2C: SDA=GPIO8, SCL=GPIO9 (Super Mini)
  Wire.begin(8, 9);
  if (!aht.begin()) {
    Serial.println("❌ AHT20 не найден. Проверь питание и I2C.");
    while (1) delay(100);
  }
  Serial.println("✅ AHT20 OK");

  connectWifi();

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  connectMqtt();

  // 🌐 Основной веб-сервер (статус)
  webServer.on("/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    float t, h;
    readSensors(t, h);
    String json = "{\"t\":" + String(t, 1) + ",\"h\":" + String(h, 1) + ",\"valve\":" + String(valveState ? "true" : "false") + ",\"manual\":" + String(manualMode ? "true" : "false") + "}";
    request->send(200, "application/json", json);
  });
  webServer.begin();

  setupOTA();
  
  Serial.println("🚀 Система готова. IP: " + WiFi.localIP().toString());
}

// 🔁 ОСНОВНОЙ ЦИКЛ
void loop() {
  if (!mqtt.connected()) {
    if (millis() - lastMqttCheck > 5000) {
      connectMqtt();
      lastMqttCheck = millis();
    }
  } else {
    mqtt.loop();
  }

  // Чтение датчиков каждые 2с
  if (millis() - lastRead > 2000) {
    lastRead = millis();
    float t, h;
    readSensors(t, h);
    Serial.printf("📊 T: %.1f°C | H: %.1f%% | V: %s | M: %s\n", t, h, valveState ? "ON" : "OFF", manualMode ? "MAN" : "AUTO");

    char buf[128];
    snprintf(buf, sizeof(buf), "{\"t\":%.1f,\"h\":%.1f,\"v\":%s}", t, h, valveState ? "1" : "0");
    mqtt.publish(TOPIC_TELEMETRY, buf, false);
  }

  // Кнопка (дебаунс 50мс)
  static unsigned long lastBtn = 0;
  if (millis() - lastBtn > 50) {
    if (digitalRead(PIN_BTN) == LOW) {
      manualMode = !manualMode;
      Serial.println(manualMode ? "🔧 Ручной режим" : "🤖 Авто режим");
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
void connectWifi() {
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("📶 WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(300); Serial.print("."); }
  Serial.println("\n✅ " + WiFi.localIP().toString());
}

void connectMqtt() {
  String clientID = "esp32c3_gh_" + String(random(0xFFFF), HEX);
  if (mqtt.connect(clientID.c_str(), TOPIC_STATUS, 1, true, "offline")) {
    mqtt.publish(TOPIC_STATUS, "online", true);
    mqtt.subscribe(TOPIC_COMMAND);
    Serial.println("✅ MQTT connected");
  } else {
    Serial.printf("❌ MQTT fail (rc=%d)\n", mqtt.state());
  }
}

void readSensors(float &t, float &h) {
  sensors_event_t humidity, temp;
  aht.getEvent(&humidity, &temp);
  t = temp.temperature;
  h = humidity.relative_humidity;
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
      "<input type='submit' value='🔄 SET UPGRADE'>"
      "</form>");
  });

  otaServer.on("/do_update", HTTP_POST, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", Update.hasError() ? "❌ FAIL" : "✅ OK. Rebooting...");
    if (!Update.hasError()) ESP.restart();
  }, [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
    if (!index) {
      Serial.printf("🔄 OTA start: %s (%u bytes)\n", filename.c_str(), request->contentLength());
      Update.begin(request->contentLength() > 0 ? request->contentLength() : UPDATE_SIZE_UNKNOWN);
    }
    Update.write(data, len);
    if (final) {
      Update.end(true);
      Serial.println(Update.isFinished() ? "✅ OTA success" : "❌ OTA failed");
    }
  });

  otaServer.begin();
  Serial.println("🌐 OTA: http://" + WiFi.localIP().toString() + ":8080/update");
}