#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <SinricPro.h>
#include <SinricProTemperaturesensor.h>
#include <DHT.h>

// Wi-Fi Configuration
const char* ssid = "Wifi name";
const char* password = "Wifi password";

// Local Mosquitto Broker Configuration
const char* mqtt_server = "ipv4";
const int mqtt_port = 1883;

// Sinric Pro Credentials
#define APP_KEY           "device app key"
#define APP_SECRET        "app secret"
#define TEMP_SENSOR_ID    "device id"

// Hardware Pin
#define DHTPIN 26
#define DHTTYPE DHT22

DHT dht(DHTPIN, DHTTYPE);
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// Shared sensor data (protected variables)
volatile float global_temp = 0.0;
volatile float global_hum = 0.0;
volatile bool data_valid = false;

// -----------------------------------------------------------------
// Task 1: Dedicated Sinric Pro Cloud Worker (Core 0)
// -----------------------------------------------------------------
void sinricTask(void *pvParameters) {
  SinricProTemperaturesensor& mySensor = SinricPro[TEMP_SENSOR_ID];
  
  SinricPro.onConnected([](){ Serial.println("[SinricPro] Cloud Connected!"); });
  SinricPro.onDisconnected([](){ Serial.println("[SinricPro] Cloud Disconnected!"); });
  SinricPro.begin(APP_KEY, APP_SECRET);

  unsigned long lastCloudSend = 0;
  float lastReportedTemp = 0.0;
  float lastReportedHum = 0.0;

  for (;;) {
    SinricPro.handle();

    unsigned long now = millis();
    // Sync to Google Home every 60 seconds if data has changed
    if (now - lastCloudSend > 60000) {
      lastCloudSend = now;
      if (data_valid) {
        if (global_temp != lastReportedTemp || global_hum != lastReportedHum) {
          mySensor.sendTemperatureEvent(global_temp, global_hum);
          lastReportedTemp = global_temp;
          lastReportedHum = global_hum;
          Serial.printf("[SinricPro Task] Pushed to Google Home: %.1f C, %.1f %%\n", global_temp, global_hum);
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10)); // Yield to prevent watchdog triggers
  }
}

// -----------------------------------------------------------------
// Task 2: Dedicated Local MQTT & Sensor Worker (Core 1)
// -----------------------------------------------------------------
void reconnectMQTT() {
  if (!mqttClient.connected()) {
    String clientId = "ESP32DHT-" + String(random(0xffff), HEX);
    if (mqttClient.connect(clientId.c_str())) {
      Serial.println("[MQTT Task] Connected to local broker!");
    } else {
      Serial.print("[MQTT Task] Broker offline (rc=");
      Serial.print(mqttClient.state());
      Serial.println("). Sinric Pro remains unaffected.");
    }
  }
}

void mqttTask(void *pvParameters) {
  mqttClient.setServer(mqtt_server, mqtt_port);
  unsigned long lastMqttSend = 0;

  for (;;) {
    // Attempt MQTT loop only if connected
    if (mqttClient.connected()) {
      mqttClient.loop();
    }

    unsigned long now = millis();
    if (now - lastMqttSend > 2000) {
      lastMqttSend = now;

      // Read sensor
      float h = dht.readHumidity();
      float t = dht.readTemperature();

      if (!isnan(h) && !isnan(t)) {
        global_temp = t;
        global_hum = h;
        data_valid = true;

        // Try reconnecting without stalling the processor
        if (!mqttClient.connected()) {
          reconnectMQTT();
        }

        // Publish to Mosquitto if connected
        if (mqttClient.connected()) {
          char payload[64];
          snprintf(payload, sizeof(payload), "{\"temperature\": %.1f, \"humidity\": %.1f}", t, h);
          mqttClient.publish("esp32/dht", payload);
          Serial.printf("[MQTT Task] Published: %s\n", payload);
        }
      } else {
        Serial.println("[Sensor] Reading failed from DHT22.");
      }
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void setup() {
  Serial.begin(115200);
  dht.begin();

  // Connect Wi-Fi first
  Serial.print("Connecting to WiFi: ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
  }
  Serial.print("\nConnected! IP: ");
  Serial.println(WiFi.localIP());

  // Launch Sinric Pro on Core 0
  xTaskCreatePinnedToCore(
    sinricTask,
    "SinricProTask",
    8192,
    NULL,
    1,
    NULL,
    0
  );

  // Launch Local MQTT + Sensor on Core 1
  xTaskCreatePinnedToCore(
    mqttTask,
    "MQTTTask",
    4096,
    NULL,
    1,
    NULL,
    1
  );
}

void loop() {
  // Empty - Both components are running independently in their own RTOS tasks
  vTaskDelete(NULL);
}