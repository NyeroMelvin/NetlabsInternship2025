#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <esp_now.h>
#include <esp_wifi.h>

// =============================================
// Pin Configuration
// =============================================
#define DHT11_PIN 5
#define MQ135_AO_PIN 34
#define BUZZER_PIN 26
#define LED_PIN 27

// =============================================
// LCD Setup
// =============================================
LiquidCrystal_I2C lcd(0x27, 16, 4);

// =============================================
// WiFi Credentials
// =============================================
const char* ssid = "Environment";
const char* password = "cisco1234";

// =============================================
// Web Server Setup
// =============================================
WebServer server(80);

// =============================================
// Sensor Objects
// =============================================
DHT dht11(DHT11_PIN, DHT11);

// =============================================
// Data Structure for ESP-NOW Communication
// =============================================
typedef struct SensorData {
  uint8_t nodeId;
  float temperature;
  float humidity;
  int airQuality; // Analog value (0-4095)
} SensorData;

SensorData sensorData;

// =============================================
// Global Variables
// =============================================
String airQualityStatus = "UNKNOWN";
String ipAddress = "";
String webAlertMessage = ""; // Message for the web server alert

// =============================================
// Timing Variables
// =============================================
unsigned long lastRead = 0;
const unsigned long interval = 2000;
unsigned long lastBlinkMillis = 0;
int currentBlinkInterval = 0; // 0 for off, >0 for blinking speed, -1 for constant on
bool ledOn = false;
bool buzzerOn = false;

// =============================================
// ESP-NOW Gateway MAC Address
// =============================================
uint8_t gatewayAddress[] = {0x30, 0xC9, 0x22, 0xF1, 0x2D, 0x7C}; // CHANGE TO YOUR GATEWAY'S MAC

// =============================================
// Main HTML Page (served from PROGMEM)
// =============================================
const char PROGMEM INDEX_HTML[] = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>Environment Monitor</title>
    <meta charset='utf-8'>
    <meta name='viewport' content='width=device-width, initial-scale=1'>
    <script src='https://code.highcharts.com/highcharts.js'></script>
    <script src='https://code.highcharts.com/highcharts-more.js'></script>
    <script src='https://code.highcharts.com/modules/solid-gauge.js'></script>
    <style>
        body { font-family: sans-serif; background: #f0f2f5; margin: 0; padding: 0; color: #333; }
        header { background: #2196F3; color: #fff; padding: 20px; text-align: center; }
        .container { max-width: 900px; margin: 20px auto; padding: 0 15px; }
        .card { background: #fff; padding: 20px; margin-bottom: 20px; border-radius: 8px; box-shadow: 0 2px 6px rgba(0,0,0,0.15); }
        .card h2 { margin: 0 0 10px; font-size: 1.2em; color: #2196F3; }
        .value { font-weight: bold; font-size: 1.3em; }
        .chart-container { height: 300px; margin-bottom: 20px; }
        .gauge-container { height: 450px; margin-bottom: 20px; min-width: 300px; max-width: 500px; margin-left: auto; margin-right: auto; }
        .good { color: #4CAF50; }
        .moderate { color: #FFC107; } /* Changed to Amber for better distinction */
        .poor { color: #FF9800; }     /* Orange */
        .very-poor { color: #FF5722; }
        .hazardous { color: #F44336; }
        .alert-message { background-color: #f44336; color: white; padding: 15px; text-align: center; font-size: 1.5em; font-weight: bold; border-radius: 8px; margin: 20px 0; animation: pulse 1s infinite alternate; display: none; }
        @keyframes pulse { from { transform: scale(1); opacity: 1; } to { transform: scale(1.02); opacity: 0.8; } }
    </style>
</head>
<body>
    <header>
        <h1>Smart Environment Monitor</h1>
    </header>
    <div class='container'>
        <div class='alert-message' id='alertMessage'></div>
        <div class='card'>
            <h2>Current Climate Data</h2>
            <p><strong>Temperature:</strong> <span class='value' id='temperature'>--</span></p>
            <p><strong>Humidity:</strong> <span class='value' id='humidity'>--</span></p>
            <p><strong>Air Quality (Analog):</strong> <span class='value' id='airQualityRaw'>--</span></p>
            <p><strong>Air Quality Status:</strong> <span class='value' id='airStatus'>--</span></p>
        </div>
        <div class='card'>
            <h2>Sensor Trends</h2>
            <div id='chart-temperature' class='chart-container'></div>
            <div id='chart-humidity' class='chart-container'></div>
            <div id='gauge-co2' class='gauge-container'></div>
        </div>
    </div>
    <script>
        function createChart(renderTo, title, yTitle, color) {
            return new Highcharts.Chart({
                chart: { renderTo: renderTo, type: 'line', zoomType: 'x' },
                title: { text: title },
                xAxis: { type: 'datetime', title: { text: 'Time' } },
                yAxis: { title: { text: yTitle } },
                series: [{ name: title, data: [], color: color, marker: { enabled: false } }],
                plotOptions: { line: { animation: false, dataLabels: { enabled: false } } },
                credits: { enabled: false },
                legend: { enabled: false }
            });
        }
        function createCo2Gauge(renderTo, title, min, max, plotBands) {
            return Highcharts.chart(renderTo, {
                chart: { type: 'gauge', height: '100%' },
                title: { text: title },
                pane: {
                    startAngle: -150,
                    endAngle: 150,
                    background: [{
                        backgroundColor: {
                            linearGradient: { x1: 0, y1: 0, x2: 0, y2: 1 },
                            stops: [
                                [0, '#E0E0E0'], // Light gray at the top
                                [1, '#FFFFFF']  // White at the bottom
                            ]
                        },
                        borderWidth: 0,
                        outerRadius: '109%'
                    }]
                },
                yAxis: {
                    min: min,
                    max: max,
                    title: { text: '' },
                    plotBands: plotBands,
                    labels: {
                        formatter: function() {
                            // This function can be used to customize axis labels,
                            // but for zone labels, we'll use plotBand labels.
                            return this.value;
                        }
                    }
                },
                series: [{
                    name: 'Air Quality', data: [0],
                    dataLabels: { format: '{y:.0f}', y: 10, borderWidth: 0, style: { fontSize: '25px' } }
                }],
                credits: { enabled: false }
            });
        }
        // UPDATED: New plot bands for the gauge with labels and color gradients
        var co2PlotBands = [
            {
                from: 0, to: 500,
                color: {
                    linearGradient: { x1: 0, y1: 0, x2: 1, y2: 0 },
                    stops: [
                        [0, '#D4EDDA'], // Lighter green
                        [1, '#4CAF50']  // Darker green
                    ]
                },
                thickness: '25%',
                label: {
                    text: 'GOOD',
                    align: 'center',
                    x: 0,
                    y: -10, // Adjust vertical position for clarity
                    style: {
                        fontSize: '10px',
                        color: '#333'
                    }
                }
            },
            {
                from: 501, to: 800,
                color: {
                    linearGradient: { x1: 0, y1: 0, x2: 1, y2: 0 },
                    stops: [
                        [0, '#FFECB3'], // Lighter amber
                        [1, '#FFC107']  // Darker amber
                    ]
                },
                thickness: '25%',
                label: {
                    text: 'MODERATE',
                    align: 'center',
                    x: 0,
                    y: -10,
                    style: {
                        fontSize: '10px',
                        color: '#333'
                    }
                }
            },
            {
                from: 801, to: 1200,
                color: {
                    linearGradient: { x1: 0, y1: 0, x2: 1, y2: 0 },
                    stops: [
                        [0, '#FFE0B2'], // Lighter orange
                        [1, '#FF9800']  // Darker orange
                    ]
                },
                thickness: '25%',
                label: {
                    text: 'POOR',
                    align: 'center',
                    x: 0,
                    y: -10,
                    style: {
                        fontSize: '10px',
                        color: '#333'
                    }
                }
            },
            {
                from: 1201, to: 1600,
                color: {
                    linearGradient: { x1: 0, y1: 0, x2: 1, y2: 0 },
                    stops: [
                        [0, '#FFCDD2'], // Lighter red-orange
                        [1, '#FF5722']  // Darker red-orange
                    ]
                },
                thickness: '25%',
                label: {
                    text: 'VERY POOR',
                    align: 'center',
                    x: 0,
                    y: -10,
                    style: {
                        fontSize: '10px',
                        color: '#333'
                    }
                }
            },
            {
                from: 1601, to: 4095,
                color: {
                    linearGradient: { x1: 0, y1: 0, x2: 1, y2: 0 },
                    stops: [
                        [0, '#EF9A9A'], // Lighter red
                        [1, '#F44336']  // Darker red
                    ]
                },
                thickness: '25%',
                label: {
                    text: 'HAZARDOUS',
                    align: 'center',
                    x: 0,
                    y: -10,
                    style: {
                        fontSize: '10px',
                        color: '#333'
                    }
                }
            }
        ];
        var chartTemp = createChart('chart-temperature', 'Temperature', '°C', '#2196F3');
        var chartHum = createChart('chart-humidity', 'Humidity', '% RH', '#4CAF50');
        var co2Gauge = createCo2Gauge('gauge-co2', 'Air Quality (Analog)', 0, 4095, co2PlotBands);

        function updateText(id, value, unit) {
            const el = document.getElementById(id);
            if (!el) return;
            const decimalPlaces = (id === 'airQualityRaw') ? 0 : 1;
            el.textContent = (value === null || isNaN(value)) ? '--' : value.toFixed(decimalPlaces) + (unit ? ' ' + unit : '');
        }

        function fetchData() {
            fetch('/data')
                .then(response => {
                    if (!response.ok) throw new Error(`HTTP error! status: ${response.status}`);
                    return response.json();
                })
                .then(data => {
                    var x = Date.now();
                    if (data.temperature !== null) { chartTemp.series[0].addPoint([x, data.temperature], true, chartTemp.series[0].data.length > 40); }
                    updateText('temperature', data.temperature, '°C');

                    if (data.humidity !== null) { chartHum.series[0].addPoint([x, data.humidity], true, chartHum.series[0].data.length > 40); }
                    updateText('humidity', data.humidity, '%');

                    if (data.co2 !== null) { co2Gauge.series[0].points[0].update(data.co2); }
                    updateText('airQualityRaw', data.co2);

                    let airStatusElement = document.getElementById('airStatus');
                    let co2Value = data.co2;
                    let airStatusText = '--'; let airStatusClass = 'value';
                    
                    // UPDATED: New logic for status text on the web page
                    if (co2Value !== null && !isNaN(co2Value)) {
                        if (co2Value <= 400) { airStatusText = 'GOOD'; airStatusClass = 'value good'; }
                        else if (co2Value <= 900) { airStatusText = 'MODERATE'; airStatusClass = 'value moderate'; }
                        else if (co2Value <= 1600) { airStatusText = 'POOR'; airStatusClass = 'value poor'; }
                        else if (co2Value <= 2200) { airStatusText = 'VERY POOR'; airStatusClass = 'value very-poor'; }
                        else { airStatusText = 'HAZARDOUS'; airStatusClass = 'value hazardous'; }
                    }
                    airStatusElement.textContent = airStatusText;
                    airStatusElement.className = airStatusClass;

                    let alertMessageElement = document.getElementById('alertMessage');
                    if (data.alertMessage && data.alertMessage !== "") {
                        alertMessageElement.textContent = data.alertMessage;
                        alertMessageElement.style.display = 'block';
                    } else {
                        alertMessageElement.style.display = 'none';
                    }
                })
                .catch(error => console.error('Error fetching data:', error));
        }
        setInterval(fetchData, 2000);
        fetchData(); // Initial fetch
    </script>
</body>
</html>
)rawliteral";


// =============================================
// Function Prototypes
// =============================================
void readSensors();
void sendData();
void updateLCD();
void controlAlerts();
void handleRoot();
void handleData();

// =============================================
// Setup Function
// =============================================
void setup() {
  Serial.begin(115200);
  lcd.init();
  lcd.backlight();
  dht11.begin();
  pinMode(MQ135_AO_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(LED_PIN, LOW);

  // WiFi Connection
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi: ");
  Serial.println(ssid);
  lcd.setCursor(0, 0);
  lcd.print("Connecting WiFi...");
  lcd.setCursor(0, 1);
  lcd.print("Please wait...");
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    lcd.print(".");
  }
  ipAddress = WiFi.localIP().toString();
  Serial.println("\nWiFi Connected!");
  Serial.print("IP Address: ");
  Serial.println(ipAddress);

  // ESP-NOW Setup
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed!");
    return;
  }
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, gatewayAddress, 6);
  peerInfo.encrypt = false;
  if (esp_now_add_peer(&peerInfo) != ESP_OK){
    Serial.println("Failed to add peer");
    return;
  }

  // =============================================
  // Web Server Routes (CLEANED UP)
  // =============================================
  server.on("/", HTTP_GET, handleRoot);
  server.on("/data", HTTP_GET, handleData);

  server.begin();
  Serial.println("Web server started.");
  
  lcd.clear();
  readSensors();
  updateLCD();
  controlAlerts();
}

// =============================================
// Main Loop Function
// =============================================
void loop() {
  server.handleClient();
  controlAlerts(); // Manages blinking/constant alerts
  
  if (millis() - lastRead >= interval) {
    lastRead = millis();
    readSensors();
    updateLCD();
    sendData();
  }
}

// =============================================
// Sensor and Communication Functions
// =============================================
void readSensors() {
  sensorData.temperature = dht11.readTemperature();
  sensorData.humidity = dht11.readHumidity();
  sensorData.airQuality = analogRead(MQ135_AO_PIN);

  if (isnan(sensorData.temperature) || isnan(sensorData.humidity)) {
    Serial.println("Failed to read from DHT sensor!");
  }
  
  // Update air quality status and alert messages based on analog values
  if (sensorData.airQuality <= 400) {
      airQualityStatus = "GOOD";
      currentBlinkInterval = 0;   // Alert off
      webAlertMessage = "";       // No web alert
  } else if (sensorData.airQuality <= 900) {
      airQualityStatus = "MODERATE";
      currentBlinkInterval = 0;
      webAlertMessage = "";
  } else if (sensorData.airQuality <= 1600) {
      airQualityStatus = "POOR";
      currentBlinkInterval = 0;
      currentBlinkInterval = 600; // Slower blink for "Poor"
      webAlertMessage = "ATTENTION! Air Quality: POOR";
  } else if (sensorData.airQuality <= 2200) {
      airQualityStatus = "VERY POOR";
      currentBlinkInterval = 300; // Blink fast
      webAlertMessage = "WARNING! Air Quality: VERY POOR";
  } else {
      airQualityStatus = "HAZARDOUS";
      currentBlinkInterval = -1;  // Constant on
      webAlertMessage = "DANGER! Air Quality: HAZARDOUS";
  }
  
  sensorData.nodeId = 1;
}

void sendData() {
  esp_now_send(gatewayAddress, (uint8_t *)&sensorData, sizeof(sensorData));
}

void updateLCD() {
  lcd.clear();
  // Line 0: Temp and Humidity
  lcd.setCursor(0, 0);
  lcd.print("T:");
  if (isnan(sensorData.temperature)) lcd.print("--.-"); else lcd.print(sensorData.temperature, 1);
  lcd.print((char)223);
  lcd.print("C H:");
  if (isnan(sensorData.humidity)) lcd.print("--%"); else { lcd.print(sensorData.humidity, 0); lcd.print("%"); }
  
  // Line 1: Air Quality
  lcd.setCursor(0, 1);
  lcd.print("AQ:");
  lcd.print(sensorData.airQuality);
  lcd.print(" ");
  lcd.print(airQualityStatus);

  // Line 3: IP Address
  lcd.setCursor(0, 3);
  lcd.print("IP:");
  lcd.print(ipAddress);
}

// =============================================
// LED and Buzzer Control Function
// =============================================
void controlAlerts() {
  if (currentBlinkInterval == 0) { // Alert off
    digitalWrite(LED_PIN, LOW);
    digitalWrite(BUZZER_PIN, LOW);
    ledOn = false;
    buzzerOn = false;
  } else if (currentBlinkInterval == -1) { // Constant on for Hazardous
    digitalWrite(LED_PIN, HIGH);
    digitalWrite(BUZZER_PIN, HIGH);
    ledOn = true;
    buzzerOn = true;
  } else { // Blinking for Very Poor
    if (millis() - lastBlinkMillis >= currentBlinkInterval) {
      lastBlinkMillis = millis();
      ledOn = !ledOn;
      buzzerOn = !buzzerOn;
      digitalWrite(LED_PIN, ledOn);
      digitalWrite(BUZZER_PIN, buzzerOn);
    }
  }
}

// =============================================
// Web Server Handlers
// =============================================
void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

// CORRECTED: This function now handles NaN values correctly by sending JSON 'null'
void handleData() {
  StaticJsonDocument<256> doc;

  if (isnan(sensorData.temperature)) {
    doc["temperature"] = nullptr;
  } else {
    doc["temperature"] = sensorData.temperature;
  }

  if (isnan(sensorData.humidity)) {
    doc["humidity"] = nullptr;
  } else {
    doc["humidity"] = sensorData.humidity;
  }
  
  doc["co2"] = sensorData.airQuality; 
  doc["alertMessage"] = webAlertMessage;
  
  String output;
  serializeJson(doc, output);
  server.send(200, "application/json", output);
}