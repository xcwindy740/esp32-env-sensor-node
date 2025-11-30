#include <M5Unified.h>
#include <M5UnitENV.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include "secrets.h"


// ===== SENSOR OBJECTS =====
SHT3X sht30;
QMP6988 qmp;

// ===== WIFI CREDS =====
const char* WIFI_SSID = WIFI_SSID_ENV;
const char* WIFI_PASS = WIFI_PASS_ENV;

// ===== MQTT SETTINGS =====
const char* MQTT_BROKER = "broker.hivemq.com";
const int   MQTT_PORT   = 1883;
const char* MQTT_TOPIC  = "m5stack/enviii-m5stack/data";

WiFiClient espClient;
PubSubClient mqttClient(espClient);

// ===== FREERTOS OBJECTS =====
QueueHandle_t jsonQueue;
SemaphoreHandle_t dataMutex;

// Shared sensor variables
float g_temperature = 0.0f;
float g_humidity    = 0.0f;
float g_pressure    = 0.0f;


void connectWiFi() {
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    int attempts = 0;

    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        attempts++;
    }
}

void connectMQTT() {
    while (!mqttClient.connected()) {
        mqttClient.connect("M5StackClientRTOS");
        if (!mqttClient.connected()) {
            delay(2000);
        }
    }
}


void sensorTask(void *pvParameters) {
    for (;;) {
        // Read sensors
        sht30.update();
        qmp.update();   // <<< REQUIRED FOR CORRECT PRESSURE

        // Protect shared data
        xSemaphoreTake(dataMutex, portMAX_DELAY);
        g_temperature = sht30.cTemp;
        g_humidity    = sht30.humidity;
        g_pressure    = qmp.pressure;    // <<< FIX: pressure now correct
        xSemaphoreGive(dataMutex);

        // Build JSON
        StaticJsonDocument<256> doc;
        doc["temperature"] = g_temperature;
        doc["humidity"]    = g_humidity;
        doc["pressure"]    = g_pressure;

        char jsonBuffer[256];
        serializeJson(doc, jsonBuffer);

        // Send JSON to queue
        xQueueSend(jsonQueue, jsonBuffer, portMAX_DELAY);

        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}


void mqttTask(void *pvParameters) {
    char incomingJson[256];

    for (;;) {
        if (!mqttClient.connected()) {
            connectMQTT();
        }
        mqttClient.loop();

        // Wait for JSON from sensor task
        if (xQueueReceive(jsonQueue, &incomingJson, portMAX_DELAY)) {
            mqttClient.publish(MQTT_TOPIC, incomingJson);

            // Display on screen (optional)
            M5.Display.clear();
            M5.Display.setCursor(0,0);
            M5.Display.println("Published:");
            M5.Display.println(incomingJson);
        }

        vTaskDelay(5000 / portTICK_PERIOD_MS); // Publish every 5 sec
    }
}

void setup() {
    auto cfg = M5.config();
    M5.begin(cfg);
    Wire.begin();
    M5.Display.setTextSize(2);

    // Sensor init
    sht30.begin();
    qmp.begin(&Wire, 0x70);

    // WiFi + MQTT
    connectWiFi();
    mqttClient.setServer(MQTT_BROKER, MQTT_PORT);

    // Create queue for JSON strings
    jsonQueue = xQueueCreate(5, 256);

    // Create mutex for shared sensor data
    dataMutex = xSemaphoreCreateMutex();

    // Create tasks on different CPU cores
    xTaskCreatePinnedToCore(
        sensorTask, "Sensor Task",
        4096, NULL, 2, NULL, 0   // Core 0
    );

    xTaskCreatePinnedToCore(
        mqttTask, "MQTT Task",
        4096, NULL, 1, NULL, 1   // Core 1
    );
}
void loop() {
    vTaskDelay(10);
}
