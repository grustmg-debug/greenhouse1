// 🌱 Теплица | test_bench v1.0
// ESP32-C3 Super Mini + AHT20 + LED-клапан + Кнопка + MQTT + Mini Web

#include <WiFi.h>
#include <PubSubClient.h>
#include <Adafruit_AHTX0.h>
#include <WebServer.h>

// 🔧 КОНФИГ (ЗАМЕНИ НА СВОИ)
const char* ssid = "YOUR_WIFI_SSID";
const char* pass = "YOUR_WIFI_PASS";
const char* mqtt_server = "192.168.1.100"; // IP Orange Pi
const int   mqtt_port = 1883;

// Топики
const char* TOPIC_TELEMETRY = "greenhouse/proto1/telemetry";
const char* TOPIC_COMMAND   = "greenhouse/proto1/command";
const char* TOPIC_STATUS    = "greenhouse/proto1/status"; // LWT
const char* LWT_PAYLOAD_OFF = "offline";
const char* LWT_PAYLOAD_ON  = "online";

// Пины
const int PIN_LED = 2;
const int PIN_BTN = 3;
const float TEMP_OFFSET = 0.0;  // Калибровка °C
const float HUM_OFFSET  = 0.0;  // Калибровка %

Adafruit_AHTX0 aht;
WiFiClient espClient;
PubSubClient mqtt(espClient);
WebServer server(80);

unsigned long lastRead = 0;
unsigned long lastMqttReconnect = 0;
bool valveState = false;
bool manualOverride = false;

// 🔌 ИНИЦИАЛИЗАЦИЯ
void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_BTN, INPUT_PULLUP);
  digitalWrite(PIN_LED, LOW);

  Wire.begin(8, 9); // SDA=GPIO8, SCL=GPIO9
  if (!aht.begin()) {
    Serial.println("❌ AHT20 не найден. Проверь I2C и питание.");
    while (1) delay(100);
  }
  sensors_event_t humidity, temp;
  aht.getEvent(&humidity, &temp);
  Serial.println("✅ AHT20 инициализирован");

  setupWifi();
  mqtt.setServer(mqtt_server, mqtt_port);
  mqtt.setCallback(mqttCallback);
  connectMqtt();

  // Mini Web Server
  server.on("/status", HTTP_GET, []() {
    server.send(200, "application/json", 
      "{\"temp\":" + String(readTemp()) + 
      ",\"hum\":" + String(readHum()) + 
      ",\"valve\":" + String(valveState) + "}");
  });
  server.on("/valve/on", HTTP_POST, []() { setValve(true); server.send(200, "text/plain", "OK"); });
  server.on("/valve/off", HTTP_POST, []() { setValve(false); server.send(200, "text/plain", "OK"); });
  server.begin();
  Serial.println("🌐 Web: http://" + WiFi.localIP().toString() + "/status");
}

// 🔁 ОСНОВНОЙ ЦИКЛ
void loop() {
  server.handleClient();
  if (!mqtt.connected()) {
    if (millis() - lastMqttReconnect > 5000) {
      connectMqtt();
      lastMqttReconnect = millis();
    }
  }
  mqtt.loop();

  // Чтение датчиков каждые 2с
  if (millis() - lastRead > 2000) {
    lastRead = millis();
    float t = readTemp();
    float h = readHum();
    Serial.printf("📊 T: %.1f°C | H: %.1f%% | Valve: %s\n", t, h, valveState ? "ON" : "OFF");

    char payload[128];
    snprintf(payload, sizeof(payload), "{\"t\":%.1f,\"h\":%.1f,\"valve\":%s}", t, h, valveState ? "true" : "false");
    mqtt.publish(TOPIC_TELEMETRY, payload, true); // retain=false, но можно true для кэша
  }

  // Кнопка (дебаунс 50мс)
  static unsigned long lastBtn = 0;
  if (millis() - lastBtn > 50) {
    if (digitalRead(PIN_BTN) == LOW) {
      manualOverride = !manualOverride;
      Serial.println(manualOverride ? "🔧 Ручной режим ВКЛ" : "🤖 Авто режим");
      lastBtn = millis();
    }
  }
}

// 📡 MQTT CALLBACK
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  
  if (String(topic) == TOPIC_COMMAND) {
    if (msg.indexOf("\"valve\":true") >= 0) setValve(true);
    else if (msg.indexOf("\"valve\":false") >= 0) setValve(false);
  }
}

// 🔧 ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ
void setupWifi() {
  WiFi.begin(ssid, pass);
  Serial.print("📶 Подключение к WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500); Serial.print(".");
  }
  Serial.println("\n✅ WiFi: " + WiFi.localIP().toString());
}

void connectMqtt() {
  String clientId = "esp32c3_proto1_" + String(random(0xFFFF), HEX);
  if (mqtt.connect(clientId.c_str(), TOPIC_STATUS, 1, true, LWT_PAYLOAD_OFF)) {
    mqtt.publish(TOPIC_STATUS, LWT_PAYLOAD_ON, true);
    mqtt.subscribe(TOPIC_COMMAND);
    Serial.println("✅ MQTT подключен");
  } else {
    Serial.print("❌ MQTT ошибка, rc="); Serial.println(mqtt.state());
  }
}

float readTemp() {
  sensors_event_t h, t;
  aht.getEvent(&h, &t);
  return t.temperature + TEMP_OFFSET;
}

float readHum() {
  sensors_event_t h, t;
  aht.getEvent(&h, &t);
  return h.relative_humidity + HUM_OFFSET;
}

void setValve(bool state) {
  valveState = state;
  digitalWrite(PIN_LED, state ? HIGH : LOW);
  Serial.println(state ? "🔵 Клапан ОТКРЫТ" : "⚫ Клапан ЗАКРЫТ");
}