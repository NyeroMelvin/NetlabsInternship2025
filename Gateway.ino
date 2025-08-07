#include <WiFi.h>          // For WiFi connectivity
#include <esp_now.h>       // For ESP-NOW communication
#include <PubSubClient.h>  // For MQTT client functionality
#include <ArduinoJson.h>   // For JSON serialization/deserialization
#include <WiFiClient.h>    // For MQTT over WiFi
#include <SPI.h>
#include <SD.h>            // Changed from SD_MMC to SD for SPI adapter
#include <time.h>

// SD Card Configuration (SPI)
const int SD_CS_PIN = 5;    // Chip Select pin for SD card (adjust based on your wiring)
const char* LOG_FILE = "/data_log.csv";
const uint64_t MAX_LOG_SIZE = 1000000; // Rotate after 1MB
const char* LOG_FILE_BACKUP = "/data_log_old.csv";

// =============================================
//         DATA STRUCTURES FOR ALL NODES
// =============================================

// Node 1: Smart Environment Data
typedef struct {
  uint8_t nodeID;       // Must be first field (1)
  float temperature;
  float humidity;
  int airQuality;
} SmartEnvData;

// Node 2: Smart Class Controller Data
typedef struct {
  uint8_t nodeID;       // Must be first field (2)
  float temperature;
  float humidity;
  int lightLevel;
} SmartClassData;

// Node 3: Security Node Data
typedef struct {
  uint8_t nodeID;       // Must be first field (3)
  char uid[20];         // Last scanned UID
  char access[10];      // Access status
  float distance;       // Distance in cm
  bool motion;          // Motion detected
} SecurityData;

// =============================================
//         FUNCTION PROTOTYPES
// =============================================
void printEnvData(const SmartEnvData &data);
void printLightData(const SmartClassData &data);
void printSecurityData(const SecurityData &data);
void printSystemStatus();
void reconnectMQTT();
void publishToCampusTopic(uint8_t nodeID);
void OnDataRecv(const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int data_len);
bool initSDCard();
void logDataToSD(uint8_t nodeID);

// =============================================
//         GLOBAL VARIABLES
// =============================================
SmartEnvData lastEnvData;
SmartClassData lastClassData;
SecurityData lastSecurityData;

unsigned long lastEnvUpdate = 0;
unsigned long lastClassUpdate = 0;
unsigned long lastSecurityUpdate = 0;

const char* ssid = "Management";
const char* password = "cisco1234";
const char* mqtt_server = "192.168.10.13";
const int mqtt_port = 1883;

WiFiClient espClient;
PubSubClient client(espClient);

// NTP configuration
const long gmtOffset_sec = 3*3600; // Adjust for your timezone (e.g., 3 * 3600 for EAT - East Africa Time)
const int daylightOffset_sec = 0; // Adjust for daylight saving if applicable
const char* ntpServer1 = "pool.ntp.org";
const char* ntpServer2 = "time.nist.gov";


// =============================================
//         MQTT FUNCTIONS
// =============================================
void reconnectMQTT() {
  int retries = 0;
  while (!client.connected()) {
    if (retries > 5) {
      Serial.println("Too many MQTT retries, restarting...");
      ESP.restart();
    }

    Serial.print("Attempting MQTT connection...");
    if (client.connect("ESP32Gateway")) {
      Serial.println("connected");
      retries = 0;
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" retrying in 5 seconds");
      delay(5000);
      retries++;
    }
  }
}

void publishToCampusTopic(uint8_t nodeID) {
  const char* topic;
  switch(nodeID) {
    case 1: topic = "iot/campus/data/environment"; break;
    case 2: topic = "iot/campus/data/classroom"; break;
    case 3: topic = "iot/campus/data/security"; break;
    default:
      Serial.printf("Unknown nodeID %d\n", nodeID);
      return;
  }

  StaticJsonDocument<300> doc;
  doc["node_id"] = nodeID;

  switch(nodeID) {
    case 1:
      doc["temperature"] = lastEnvData.temperature;
      doc["humidity"] = lastEnvData.humidity;
      doc["air_quality"] = lastEnvData.airQuality;
      break;
    case 2:
      doc["temperature"] = lastClassData.temperature;
      doc["humidity"] = lastClassData.humidity;
      doc["light_level"] = lastClassData.lightLevel;
      break;
    case 3:
      doc["rfid_uid"] = lastSecurityData.uid;
      doc["access_status"] = lastSecurityData.access;
      doc["distance_cm"] = lastSecurityData.distance;
      doc["motion_detected"] = lastSecurityData.motion;
      break;
  }

  char payload[300];
  size_t n = serializeJson(doc, payload);

  if (n == 0) {
    Serial.println("Failed to serialize JSON");
    return;
  }

  if (!client.publish(topic, payload, n)) {
    Serial.println("Publish failed");
    if (!client.connected()) {
      reconnectMQTT();
    }
  }
}

// =============================================
//         ESP-NOW CALLBACK
// =============================================
void OnDataRecv(const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int data_len) {
  if (data_len < 1) return;

  uint8_t nodeID = data[0];
  const uint8_t *mac_addr = esp_now_info->src_addr;

  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac_addr[0], mac_addr[1], mac_addr[2],
           mac_addr[3], mac_addr[4], mac_addr[5]);

  Serial.printf("\nReceived %d bytes from %s (Node %d)\n", data_len, macStr, nodeID);

  switch (nodeID) {
    case 1:
      if (data_len == sizeof(SmartEnvData)) {
        memcpy(&lastEnvData, data, sizeof(lastEnvData));
        lastEnvUpdate = millis();
        printEnvData(lastEnvData);
        publishToCampusTopic(1);
        logDataToSD(1);
      }
      break;
    case 2:
      if (data_len == sizeof(SmartClassData)) {
        memcpy(&lastClassData, data, sizeof(lastClassData));
        lastClassUpdate = millis();
        printLightData(lastClassData);
        publishToCampusTopic(2);
        logDataToSD(2);
      }
      break;
    case 3:
      if (data_len == sizeof(SecurityData)) {
        memcpy(&lastSecurityData, data, sizeof(lastSecurityData));
        lastSecurityUpdate = millis();
        printSecurityData(lastSecurityData);
        publishToCampusTopic(3);
        logDataToSD(3);
      }
      break;
    default:
      Serial.printf("Unknown nodeID: %d\n", nodeID);
      break;
  }
}

// =============================================
//         MAIN SETUP AND LOOP
// =============================================
void setup() {
  Serial.begin(115200);

  // Initialize SD card first (before WiFi)
  if (!initSDCard()) {
    Serial.println("Continuing without SD card support");
  }

  // WiFi Connection
  Serial.print("Connecting to WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  // Wait for WiFi connection with a timeout
  unsigned long wifiConnectStart = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (millis() - wifiConnectStart > 30000) { // 30 second timeout
      Serial.println("\nWiFi connection timeout, restarting...");
      ESP.restart();
    }
  }
  Serial.println("\nWiFi connected");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  // Display ESP32 MAC address
  Serial.print("ESP32 MAC Address: ");
  Serial.println(WiFi.macAddress());

  // Set up NTP time *after* WiFi is connected
  Serial.print("Attempting NTP time sync...");
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer1, ntpServer2);

  // Wait for NTP time sync with a timeout
  unsigned long ntpSyncStart = millis();
  time_t now = time(nullptr);
  while (now < 100000 && (millis() - ntpSyncStart < 60000)) { // 60 second timeout for NTP
    delay(500);
    Serial.print(".");
    now = time(nullptr);
  }

  if (now < 100000) {
    Serial.println("\nNTP time sync failed after timeout. Continuing with un-synced time.");
    // Consider restarting or implementing a retry mechanism in loop() if time is critical
  } else {
    Serial.println("\nTime synced!");
    struct tm timeinfo;
    getLocalTime(&timeinfo);
    Serial.print("Current time: ");
    Serial.println(asctime(&timeinfo));
  }


  // ESP-NOW Initialization
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    ESP.restart();
  }
  esp_now_register_recv_cb(OnDataRecv);

  // MQTT Setup
  client.setServer(mqtt_server, mqtt_port);
  reconnectMQTT();
}

void loop() {
  // MQTT Maintenance
  if (!client.connected()) {
    reconnectMQTT();
  }
  client.loop();

  // Periodic system status print
  static unsigned long lastStatusPrint = 0;
  if (millis() - lastStatusPrint > 5000) {
    lastStatusPrint = millis();
    printSystemStatus();
  }
}

// =============================================
//         HELPER FUNCTIONS
// =============================================
void printEnvData(const SmartEnvData &data) {
  Serial.println("=== ENVIRONMENT DATA ===");
  Serial.printf("Temperature: %.1f°C\n", data.temperature);
  Serial.printf("Humidity: %.1f%%\n", data.humidity);
  Serial.printf("Air Quality: %d\n", data.airQuality);
  Serial.println("========================");
}

void printLightData(const SmartClassData &data) {
  Serial.println("=== SMART CLASS DATA ===");
  Serial.printf("Temperature: %.1f°C\n", data.temperature);
  Serial.printf("Humidity: %.1f%%\n", data.humidity);
  Serial.printf("Light Level: %d\n", data.lightLevel);
  Serial.println("========================");
}

void printSecurityData(const SecurityData &data) {
  Serial.println("=== SECURITY DATA ===");
  Serial.printf("Last UID: %s\n", data.uid);
  Serial.printf("Access: %s\n", data.access);
  Serial.printf("Distance: %.1f cm\n", data.distance);
  Serial.printf("Motion: %s\n", data.motion ? "DETECTED" : "None");
  Serial.println("=====================");
}

void printSystemStatus() {
  Serial.println("\n=== SYSTEM STATUS ===");
  uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("SD Card: Not inserted");
  } else {
    Serial.printf("SD Card: %s (%lluMB)\n",
                  cardType == CARD_MMC ? "MMC" :
                  cardType == CARD_SD ? "SDSC" :
                  cardType == CARD_SDHC ? "SDHC" : "UNKNOWN",
                  SD.cardSize() / (1024 * 1024));

    if (SD.exists(LOG_FILE)) {
      File logFile = SD.open(LOG_FILE);
      Serial.printf("Log size: %zu bytes\n", logFile.size());
      logFile.close();
    }
  }
  // Node activity status
  auto printNodeStatus = [](const char* name, unsigned long lastUpdate) {
    if (lastUpdate == 0) {
      Serial.printf("%s: Never Active\n", name);
    } else if (millis() - lastUpdate < 15000) {
      Serial.printf("%s: Active\n", name);
    } else {
      Serial.printf("%s: Inactive (%lus ago)\n", name, (millis() - lastUpdate) / 1000);
    }
  };

  printNodeStatus("Environment Node", lastEnvUpdate);
  printNodeStatus("Smart Class Node", lastClassUpdate);
  printNodeStatus("Security Node", lastSecurityUpdate);

  // MQTT status
  Serial.printf("MQTT: %s\n", client.connected() ? "Connected" : "Disconnected");
  if (!client.connected()) {
    Serial.printf("MQTT State: %d\n", client.state());
  }

  Serial.println("====================");
}

// Updated initSDCard() function for SPI:
bool initSDCard() {
  if (!SD.begin(SD_CS_PIN)) {
    Serial.println("SD Card Mount Failed");
    return false;
  }

  uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("No SD Card inserted");
    return false;
  }

  Serial.print("SD Card Type: ");
  if (cardType == CARD_MMC) {
    Serial.println("MMC");
  } else if (cardType == CARD_SD) {
    Serial.println("SDSC");
  } else if (cardType == CARD_SDHC) {
    Serial.println("SDHC");
  } else {
    Serial.println("UNKNOWN");
  }

  Serial.printf("SD Card Size: %lluMB\n", SD.cardSize() / (1024 * 1024));

  // Check if existing log file is too big
  if (SD.exists(LOG_FILE)) {
    File logFile = SD.open(LOG_FILE);
    if (logFile.size() > MAX_LOG_SIZE) {
      logFile.close();
      SD.remove(LOG_FILE_BACKUP);
      SD.rename(LOG_FILE, LOG_FILE_BACKUP);
    } else {
      logFile.close();
    }
  }

  // Create header if file doesn't exist
  if (!SD.exists(LOG_FILE)) {
    File logFile = SD.open(LOG_FILE, FILE_WRITE);
    if (logFile) {
      logFile.println("Timestamp,NodeID,Label,Temperature,Humidity,AirQuality,LightLevel,UID,AccessStatus,Distance,Motion");
      logFile.close();
      Serial.println("Created new log file");
    } else {
      Serial.println("Error creating log file");
      return false;
    }
  }

  return true;
}

void logDataToSD(uint8_t nodeID) {
  struct tm timeinfo;
  // Make sure to attempt getting time multiple times or have a retry mechanism
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Failed to obtain time for logging. Time not synced yet?");
    return; // Don't log if time isn't available
  }

  char timestamp[30];
  strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &timeinfo);

  File logFile = SD.open(LOG_FILE, FILE_APPEND);
  if (!logFile) {
    Serial.println("Failed to open log file for writing");
    return;
  }

  logFile.print(timestamp);
  logFile.print(",");
  logFile.print(nodeID);

  switch (nodeID) {
    case 1:
      logFile.print(",Environment");
      logFile.print(",");
      logFile.print(lastEnvData.temperature);
      logFile.print(",");
      logFile.print(lastEnvData.humidity);
      logFile.print(",");
      logFile.print(lastEnvData.airQuality);
      logFile.print(",,,,,");
      break;
    case 2:
      logFile.print(",Classroom");
      logFile.print(",");
      logFile.print(lastClassData.temperature);
      logFile.print(",");
      logFile.print(lastClassData.humidity);
      logFile.print(",,");  // No airQuality
      logFile.print(lastClassData.lightLevel);
      logFile.print(",,,,");
      break;
    case 3:
      logFile.print(",Security");
      logFile.print(",,,,,,");  // No temp/humidity/light
      logFile.print(lastSecurityData.uid);
      logFile.print(",");
      logFile.print(lastSecurityData.access);
      logFile.print(",");
      logFile.print(lastSecurityData.distance);
      logFile.print(",");
      logFile.print(lastSecurityData.motion ? "Yes" : "No");
      break;
    default:
      logFile.print(",Unknown,,,,,,,,,,");
      break;
  }

  logFile.println();
  logFile.close();

  Serial.println("✔️ Data logged to SD card.");
}