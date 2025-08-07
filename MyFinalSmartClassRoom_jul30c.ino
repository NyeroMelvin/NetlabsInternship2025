#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>
#include <esp_now.h>

// WiFi credentials
const char* ssid = "Environment";
const char* password = "cisco1234";

// Pin Definitions
#define DHTPIN 5
#define DHTTYPE DHT11
#define RELAY_PIN 13  // Fan control (active LOW)
#define LDR_PIN 32    // Light sensor
#define LED_PIN 4     // LED control
#define PIR_PIN 18    // Motion sensor

// Thresholds
#define TEMP_THRESHOLD 32
#define LIGHT_THRESHOLD 100

// Manual override flags
bool manualFanOverride = false;
bool manualLedOverride = false;

DHT dht(DHTPIN, DHTTYPE);
LiquidCrystal_I2C lcd(0x27, 16, 2);
AsyncWebServer server(80);

// ESP-NOW setup
const uint8_t NODE_ID = 2;
uint8_t gatewayMAC[] = {0x30, 0xC9, 0x22, 0xF1, 0x2D, 0x7C};

typedef struct {
  uint8_t nodeID;
  float temperature;
  float humidity;
  int lightLevel;
} SmartClassData;

SmartClassData dataToSend = {NODE_ID, 0, 0, 0};


const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<title>Classroom monitor</title>
<meta charset='utf-8'>
<meta name='viewport' content='width=device-width, initial-scale=1'>
<style>
body {
  font-family: Arial, sans-serif;
  background: #f0f2f5;
  margin: 0;
  padding: 20px;
}
header {
  background: #2196F3;
  color: white;
  padding: 15px;
  text-align: center;
  border-radius: 8px 8px 0 0;
}
.card {
  background: white;
  border-radius: 8px;
  padding: 20px;
  margin-bottom: 20px;
  box-shadow: 0 2px 4px rgba(0,0,0,0.1);
}
.btn {
  padding: 10px 15px;
  border: none;
  border-radius: 4px;
  cursor: pointer;
  font-weight: bold;
  margin-right: 10px;
}
.btn-on {
  background: #2196F3;
  color: white;
}
.btn-off {
  background: white;
  color: #2196F3;
  border: 1px solid #2196F3;
}
.status {
  font-weight: bold;
}
.status-light {
  display: inline-block;
  width: 15px;
  height: 15px;
  border-radius: 50%;
  margin-left: 5px;
}
.status-on { 
  background: #4CAF50; 
}
.status-off { 
  background: #ffffff;
  border: 1px solid #cccccc;
}
.mode-indicator {
  font-style: italic;
  margin-left: 10px;
}
.manual-mode { color: #2196F3; }
.auto-mode { color: #4CAF50; }
</style>
</head>
<body>
<header><h1>Smart Classroom Monitor</h1></header>
<div class='container'>
  <div class='card'>
    <h2>Current Classroom </h2>
    <p>Temperature: <span id='temperature'>--</span> °C</p>
    <p>Humidity: <span id='humidity'>--</span> %</p>
    <p>Light: <span id='light'>--</span></p>
    <p>Motion: <span id='motion'>--</span></p>
  </div>

  <div class='card'>
    <h2>Device Control</h2>
    <div class='control-group'>
      <button onclick="controlDevice('fan', 'on')" class='btn' id='fan-on'>FAN ON</button>
      <button onclick="controlDevice('fan', 'off')" class='btn' id='fan-off'>FAN OFF</button>
      <span class='status'>Status: <span id='fan-status'>--</span>
        <span class='status-light' id='fan-status-light'></span>
        <span class='mode-indicator' id='fan-mode'></span>
      </span>
    </div>
    <div class='control-group' style='margin-top: 15px;'>
      <button onclick="controlDevice('led', 'on')" class='btn' id='led-on'>LIGHT ON</button>
      <button onclick="controlDevice('led', 'off')" class='btn' id='led-off'>LIGHT OFF</button>
      <span class='status'>Status: <span id='led-status'>--</span>
        <span class='status-light' id='led-status-light'></span>
        <span class='mode-indicator' id='led-mode'></span>
      </span>
    </div>
  </div>
</div>

<script>
function controlDevice(device, action) {
  fetch('/' + device + '/' + action)
    .then(response => response.text())
    .then(text => {
      console.log(text);
      updateStatus();
    });
}

function updateStatus() {
  fetch('/data')
    .then(response => response.json())
    .then(data => {
      // Update sensor readings
      document.getElementById('temperature').textContent = data.temperature || '--';
      document.getElementById('humidity').textContent = data.humidity || '--';
      document.getElementById('light').textContent = data.light || '--';
      document.getElementById('motion').textContent = data.motion ? 'Detected' : 'No Motion';
      
      // Update fan control
      const fanOn = data.fan === "ON";
      document.getElementById('fan-status').textContent = fanOn ? "ON" : "OFF";
      document.getElementById('fan-status-light').className = 
        fanOn ? "status-light status-on" : "status-light status-off";
      document.getElementById('fan-on').className = 
        fanOn ? "btn btn-on" : "btn";
      document.getElementById('fan-off').className = 
        !fanOn ? "btn btn-off" : "btn";
      document.getElementById('fan-mode').textContent = 
        data.fanManualOverride ? "(Manual)" : "(Auto)";
      document.getElementById('fan-mode').className = 
        data.fanManualOverride ? "mode-indicator manual-mode" : "mode-indicator auto-mode";
      
      // Update LED control
      const ledOn = data.led === "ON";
      document.getElementById('led-status').textContent = ledOn ? "ON" : "OFF";
      document.getElementById('led-status-light').className = 
        ledOn ? "status-light status-on" : "status-light status-off";
      document.getElementById('led-on').className = 
        ledOn ? "btn btn-on" : "btn";
      document.getElementById('led-off').className = 
        !ledOn ? "btn btn-off" : "btn";
      document.getElementById('led-mode').textContent = 
        data.ledManualOverride ? "(Manual)" : "(Auto)";
      document.getElementById('led-mode').className = 
        data.ledManualOverride ? "mode-indicator manual-mode" : "mode-indicator auto-mode";
    });
}

// Update every 2 seconds
setInterval(updateStatus, 2000);
updateStatus(); // Initial update
</script>
</body>
</html>
)rawliteral";
String readDHTTemperature() {
  float t = dht.readTemperature();
  return isnan(t) ? "--" : String(t, 1);
}

String readDHTHumidity() {
  float h = dht.readHumidity();
  return isnan(h) ? "--" : String(h, 1);
}

String readLDR() {
  return String(analogRead(LDR_PIN));
}

String readPIR() {
  return digitalRead(PIR_PIN) ? "Detected" : "No Motion";
}

String readFanStatus() {
  return digitalRead(RELAY_PIN) == LOW ? "ON" : "OFF";
}

String readLEDStatus() {
  return digitalRead(LED_PIN) == HIGH ? "ON" : "OFF";
}

void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  Serial.print("Send Status: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Success" : "Fail");
}

void setup() {
  Serial.begin(115200);
  
  // Initialize hardware
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(PIR_PIN, INPUT);
  digitalWrite(RELAY_PIN, HIGH); // Start with fan OFF
  digitalWrite(LED_PIN, LOW);    // Start with LED OFF

  dht.begin();
  lcd.init();
  lcd.backlight();

  // Connect to WiFi
  WiFi.begin(ssid, password);
  Serial.println("Connecting to WiFi...");
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected! IP: " + WiFi.localIP().toString());
    lcd.print("Connected:");
    lcd.setCursor(0, 1);
    lcd.print(WiFi.localIP());
    
    // Initialize ESP-NOW
    if (esp_now_init() != ESP_OK) {
      Serial.println("ESP-NOW init failed");
      ESP.restart();
    }
    esp_now_register_send_cb(OnDataSent);
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, gatewayMAC, 6);
    peerInfo.encrypt = false;
    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
      Serial.println("Failed to add peer");
      ESP.restart();
    }
  } else {
    Serial.println("\nWiFi Failed!");
    while(1) delay(1000);
  }

  // Set up server routes
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/html", index_html);
  });

  server.on("/data", HTTP_GET, [](AsyncWebServerRequest *request){
    String json = "{";
    json += "\"temperature\":" + readDHTTemperature() + ",";
    json += "\"humidity\":" + readDHTHumidity() + ",";
    json += "\"light\":" + readLDR() + ",";
    json += "\"motion\":" + String(digitalRead(PIR_PIN)) + ",";
    json += "\"fan\":\"" + readFanStatus() + "\",";
    json += "\"led\":\"" + readLEDStatus() + "\",";
    json += "\"fanManualOverride\":" + String(manualFanOverride ? "true" : "false") + ",";
    json += "\"ledManualOverride\":" + String(manualLedOverride ? "true" : "false");
    json += "}";
    request->send(200, "application/json", json);
  });

  // Control endpoints
  server.on("/fan/on", HTTP_GET, [](AsyncWebServerRequest *request){
    manualFanOverride = true;
    digitalWrite(RELAY_PIN, LOW);
    request->send(200, "text/plain", "Fan manually turned ON (override active)");
  });
  
  server.on("/fan/off", HTTP_GET, [](AsyncWebServerRequest *request){
    manualFanOverride = false;
    digitalWrite(RELAY_PIN, HIGH);
    request->send(200, "text/plain", "Fan turned OFF (override cleared)");
  });
  
  server.on("/led/on", HTTP_GET, [](AsyncWebServerRequest *request){
    manualLedOverride = true;
    digitalWrite(LED_PIN, HIGH);
    request->send(200, "text/plain", "LED manually turned ON (override active)");
  });
  
  server.on("/led/off", HTTP_GET, [](AsyncWebServerRequest *request){
    manualLedOverride = false;
    digitalWrite(LED_PIN, LOW);
    request->send(200, "text/plain", "LED turned OFF (override cleared)");
  });

  server.begin();
}

void loop() {
  // Read sensors
  float temp = dht.readTemperature();
  float hum = dht.readHumidity();
  int ldrValue = analogRead(LDR_PIN);
  bool motion = digitalRead(PIR_PIN);

  // Automatic control (only when not in manual override)
  if (!manualFanOverride && !isnan(temp)) {
    digitalWrite(RELAY_PIN, temp > TEMP_THRESHOLD ? LOW : HIGH);
  }
  if (!manualLedOverride) {
    digitalWrite(LED_PIN, (ldrValue < LIGHT_THRESHOLD) ? HIGH : LOW);
  }

  // Update LCD
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("T:");
  lcd.print(isnan(temp) ? "--" : String(temp, 1));
  lcd.print("C H:");
  lcd.print(isnan(hum) ? "--" : String(hum, 1));
  lcd.print("%");
  
  lcd.setCursor(0, 1);
  lcd.print("L:");
  lcd.print(ldrValue < LIGHT_THRESHOLD ? "Dark " : "Light");
  lcd.print(" Motion:");
  lcd.print(motion ? "YES" : "NO ");

  // Send ESP-NOW data
  dataToSend.temperature = isnan(temp) ? 0 : temp;
  dataToSend.humidity = isnan(hum) ? 0 : hum;
  dataToSend.lightLevel = ldrValue;
  esp_now_send(gatewayMAC, (uint8_t *)&dataToSend, sizeof(dataToSend));

  delay(2000);
}