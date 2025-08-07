#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <MFRC522v2.h>
#include <MFRC522DriverSPI.h>
#include <MFRC522DriverPinSimple.h>
#include <ESP32Servo.h> // Keep this, it's the correct one for ESP32
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <esp_now.h>
#include <esp_wifi.h>


// LCD Setup
#define I2C_SDA 33
#define I2C_SCL 32
LiquidCrystal_I2C lcd(0x27, 16, 2);

// RFID Setup
MFRC522DriverPinSimple ss_pin(5);
MFRC522DriverSPI driver{ss_pin};
MFRC522 mfrc522{driver};

// Servo
Servo doorServo;

// Pins
int Red_Led = 4;
int Green_Led = 2;
int buzzerPin = 15;
int PIR_PIN = 27; // PIR sensor pin
int Motion_Led = 26; // LED to indicate motion
const int trigPin = 13;
const int echoPin = 12;

// Ultrasonic
#define SOUND_SPEED 0.034
long duration;
float distanceCm;

// WiFi credentials
const char* ssid = "Security";
const char* password = "cisco1234";

// Web Server
AsyncWebServer server(80);

// Shared Variables
String lastUID = "None";
String lastAccess = "None";
bool doorManualToggle = false; // Flag to signal a request to toggle door state from web
// Renamed for clarity: motionDetectionLightControl indicates if PIR *should* control the LED
bool motionDetectionLightControl = true; // NEW: Flag to control if PIR automatically controls Motion_Led
bool webLightOverride = false; // NEW: Flag to indicate if web server has overridden Motion_Led
bool currentMotionLedState = false; // NEW: Holds the actual current state of Motion_Led

// --- NEW DOOR LOGIC VARIABLES ---
int doorPosition = 0; // Current door angle (0 = closed, 90 = open)
bool doorIsOpen = false; // True if the door is currently open
unsigned long doorOpenStartTime = 0; // Time when the door finished opening (for normal duration)
unsigned long lastMotionTime = 0; // Time when motion was last detected (for extension/reopening)
bool doorIsCurrentlyOpen = false; // For web server to display door status

// Adjustable door parameters
const int OPEN_ANGLE = 90; // Angle for fully open door
const int CLOSED_ANGLE = 0; // Angle for fully closed door
const int NORMAL_OPEN_DURATION_MS = 1000; // How long the door stays open normally (e.g., 1 second)
const int MOTION_EXTENSION_MS = 2000; // How much longer to stay open after motion is last detected (e.g., 2 seconds extra)
const int MAX_OPEN_DURATION_TOTAL_MS = 5000; // ABSOLUTE MAXIMUM door open time (5 seconds)
const int DOOR_MOVE_DELAY_MS = 30; // Delay between each step when opening/closing (adjust for speed, matches original)
// --- END NEW DOOR LOGIC VARIABLES ---

// --- NEW SECURITY LOGIC VARIABLES ---
int deniedAttempts = 0; // Counter for consecutive denied RFID attempts
const int MAX_DENIED_ATTEMPTS = 3; // Number of denied attempts to trigger alarm
unsigned long buzzerAlarmStartTime = 0; // Time when the buzzer alarm started
const unsigned long BUZZER_ALARM_DURATION_MS = 20000; // Buzzer alarm duration (20 seconds)
bool buzzerAlarmActive = false; // Flag to indicate if the buzzer alarm is active
bool intruderAlertActive = false; // Flag to indicate if intruder alert should be shown on web server
bool stopBuzzerFromWeb = false; // Flag set by web server to stop buzzer
// --- END NEW SECURITY LOGIC VARIABLES ---

// --- NON-BLOCKING TIMER VARIABLES ---
unsigned long lastEspNowSendTime = 0;
// Using the existing const value here for harmonization
const unsigned long ESP_NOW_SEND_INTERVAL = 10000; // Send ESP-NOW data every 1 second
// --- END NON-BLOCKING TIMER VARIABLES ---

// --- ESP-NOW STRUCT FOR GATEWAY (MATCHES OLD CODE'S DESIRED FIELDS) ---
typedef struct {
  uint8_t nodeID;
  char uid[20];
  char access[10];
  float distance;
  bool motion;
} SecurityData; // Renamed to avoid confusion with internal web struct

SecurityData secData;

// --- INTERNAL STRUCT FOR WEB SERVER (RETAINS NEW FIELDS) ---
typedef struct {
  uint8_t nodeID;
  char uid[20];
  char access[10];
  float distance;
  bool motion;
  bool intruderAlert; // Added for web server pop-up
  bool doorStatus; // Added: true if open, false if closed
} WebSecurityData; // New struct for internal use and web server

WebSecurityData webData; // Instance of the struct for internal use/web server

uint8_t gatewayAddress[] = {0x30, 0xC9, 0x22, 0xF1, 0x2D, 0x7C}; // Replace with real MAC

esp_now_peer_info_t peerInfo;

// --- NEW DOOR LOGIC FUNCTIONS ---
void openDoor() {
  Serial.println("Opening door...");
  // Move from current position to OPEN_ANGLE
  for (int pos = doorPosition; pos <= OPEN_ANGLE; pos += 1) {
    doorServo.write(pos);
    doorPosition = pos;
    delay(DOOR_MOVE_DELAY_MS); // Control opening speed
    // While opening, if motion is detected, reset the 'lastMotionTime'
    if (digitalRead(PIR_PIN) == HIGH) {
      lastMotionTime = millis(); // Important for setting initial extension for *this* open cycle
    }
  }
  doorIsOpen = true;
  doorIsCurrentlyOpen = true; // Update door status for web display
  doorOpenStartTime = millis(); // Record when the door finished opening
  lastMotionTime = doorOpenStartTime; // Initialize last motion time to ensure initial hold
  Serial.println("Door is open.");
}

void closeDoor() {
  Serial.println("Closing door...");
  // Move from current position to CLOSED_ANGLE
  for (int pos = doorPosition; pos >= CLOSED_ANGLE; pos -= 1) {
    doorServo.write(pos);
    doorPosition = pos;
    delay(DOOR_MOVE_DELAY_MS); // Control closing speed

    // IMPORTANT: Check for motion during closing
    if (digitalRead(PIR_PIN) == HIGH) {
      Serial.println("Motion detected during closing! Reopening.");
      openDoor(); // Reopen the door immediately
      return; // Exit the closeDoor function immediately to stop further closing
    }
  }
  doorIsOpen = false;
  doorIsCurrentlyOpen = false; // Update door status for web display
  Serial.println("Door is closed.");
}
// --- END NEW DOOR LOGIC FUNCTIONS ---


void setup() {
  Serial.begin(115200); // Increased baud rate for faster debugging output

  Wire.begin(I2C_SDA, I2C_SCL);
  lcd.begin(16, 2);
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("IP: Connecting...");

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
  }
  lcd.setCursor(0, 0);
  lcd.print("IP:");
  lcd.print(WiFi.localIP());
  lcd.print("    ");

  // Routes
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    String html = R"rawliteral(
      <!DOCTYPE html><html><head>    <meta charset="UTF-8">    <title>Security Access Monitor</title>    <meta name="viewport" content="width=device-width, initial-scale=1">    <link rel="stylesheet" href="https://cdnjs.cloudflare.com/ajax/libs/font-awesome/6.5.0/css/all.min.css">    <style>
        body {
          font-family: sans-serif;
          background: #f0f2f5;
          margin: 0;
          padding: 0;
          color: #333;
        }
        header {
          background: #2196F3;
          color: white;
          padding: 20px;
          text-align: center;
        }
        .container {
          max-width: 900px;
          margin: 20px auto;
          padding: 0 15px;
        }
        .card {
          background: white;
          padding: 20px;
          margin-bottom: 20px;
          border-radius: 8px;
          box-shadow: 0 2px 6px rgba(0,0,0,0.15);
        }
        .card h2 {
          margin: 0 0 10px;
          font-size: 1.2em;
          color: #2196F3;
        }
        .data {
          font-size: 1.1em;
          margin: 10px 0;
        }
        .value {
          font-weight: bold;
          font-size: 1.3em;
        }
        .green { color: #4CAF50; }
        .red { color: #F44336; }
        .icon-btn {
          display: inline-block;
          margin: 10px 15px 0 0;
          padding: 15px;
          font-size: 24px;
          color: white;
          background-color: #2196F3;
          border: none;
          border-radius: 50%;
          text-align: center;
          cursor: pointer;
          transition: background-color 0.3s ease;
        }
        .icon-btn:hover {
          background-color: #1976D2;
        }
        .icon-btn i {
          pointer-events: none;
        }
        .icon-label {
          display: block;
          margin-top: 5px;
          font-size: 14px;
          color: #333;
        }
        .control-group {
          text-align: center;
        }
        /* Intruder Alert Banner Styles */
        .alert-banner {
          display: none; /* Hidden by default */
          position: fixed; /* Position relative to viewport */
          top: 0;
          left: 0;
          width: 100%;
          background: #D32F2F; /* Red color for alert */
          color: white;
          padding: 10px 20px;
          text-align: center;
          font-size: 1.2em;
          font-weight: bold;
          box-shadow: 0 2px 5px rgba(0,0,0,0.2);
          z-index: 999; /* Below modal overlays, but above regular content */
          animation: slideDown 0.5s ease-out; /* Animation for appearance */
        }
        @keyframes slideDown {
          from { top: -50px; opacity: 0; }
          to { top: 0; opacity: 1; }
        }
        .alert-banner .close-btn {
          float: right;
          cursor: pointer;
          font-size: 1.5em;
          line-height: 1;
          margin-left: 10px;
        }
        /* Optional: Add a pulse effect to the text within the banner */
        .alert-banner span {
          animation: pulseText 1s infinite alternate;
        }
        @keyframes pulseText {
          from { transform: scale(1); opacity: 1; }
          to { transform: scale(1.02); opacity: 0.9; }
        }
      </style></head><body>    <header><h1>Security Access Monitor</h1></header>    <div class="container">    <div class="card">    <h2>Access Log</h2>    <p class="data"><strong>Last UID:</strong> <span class="value" id="uid">Loading...</span></p>    <p class="data"><strong>Access:</strong> <span class="value" id="access">Loading...</span></p>    <p class="data"><strong>Distance:</strong> <span class="value" id="distance">Loading...</span></p>    <p class="data"><strong>Presence:</strong> <span class="value" id="motion">Loading...</span></p>
        <p class="data"><strong>Door Status:</strong> <span class="value" id="doorStatus">Loading...</span></p>
    </div>
    <div class="card">    <h2>Controls</h2>    <div class="control-group">    <button class="icon-btn" id="doorBtn" title="Toggle Door">    <i class="fas fa-door-open"></i>    <span class="icon-label">Door</span>    </button>
        <button class="icon-btn" id="lightBtn" title="Toggle Light">    <i class="fas fa-lightbulb"></i>    <span class="icon-label">Light</span>    </button>
        <button class="icon-btn" id="stopBuzzerBtn" title="Stop Buzzer Alarm">
          <i class="fas fa-volume-mute"></i>
          <span class="icon-label">Silence Alarm</span>
        </button>
      </div>
    </div>
  </div>

  <div id="intruderAlertBanner" class="alert-banner">
    <span class="close-btn" onclick="hideIntruderAlert()">&#x2715;</span>
    <i class="fas fa-exclamation-triangle"></i> <span>INTRUDER ALERT!</span>
  </div>

  <script>
    // Function to fetch and update all data
    function fetchData() {
      fetch('/status')
        .then(response => response.json())
        .then(data => {
          document.getElementById('uid').textContent = data.uid;
          document.getElementById('access').textContent = data.access;
          document.getElementById('access').className = data.access == 'Granted' ? 'green' : 'red';
          document.getElementById('distance').textContent = data.distance + " cm";
          document.getElementById('motion').textContent = data.motion;
          // Update door status
          document.getElementById('doorStatus').textContent = data.doorStatus ? 'Open' : 'Closed';

          // Handle Intruder Alert Banner
          const intruderAlertBanner = document.getElementById('intruderAlertBanner');
          if (data.intruderAlert === true) {
            intruderAlertBanner.style.display = 'block'; // Show banner
          } else {
            intruderAlertBanner.style.display = 'none'; // Hide banner
          }
        })
        .catch(error => console.error('Error fetching data:', error)); // Log fetch errors
    }

    // Function to hide the banner (called by the close button)
    function hideIntruderAlert() {
      document.getElementById('intruderAlertBanner').style.display = 'none';
      // Note: This only hides the banner client-side.
      // If the alarm is still active on the ESP32, it will reappear on next fetch.
      // Use the "Silence Alarm" button to stop the actual buzzer and server-side alert.
    }

    // Function to send commands to ESP32 without full page reload
    function sendCommand(url) {
      fetch(url)
        .then(response => {
          if (response.ok) {
            console.log(`Command sent successfully to ${url}`);
            fetchData(); // Immediately refresh status after command
          } else {
            console.error(`Failed to send command to ${url}.`);
          }
        })
        .catch(error => console.error(`Error sending command to ${url}:`, error));
    }

    // Event listeners for control buttons
    document.getElementById('doorBtn').addEventListener('click', function() {
      sendCommand('/door'); // This will now toggle the door
    });

    document.getElementById('lightBtn').addEventListener('click', function() {
      sendCommand('/light');
    });

    document.getElementById('stopBuzzerBtn').addEventListener('click', function() {
      sendCommand('/stopBuzzer');
      document.getElementById('intruderAlertBanner').style.display = 'none'; // Hide banner immediately on silence command
    });

    // Poll status every second
    setInterval(fetchData, 1000);
    fetchData(); // Initial fetch on page load
  </script>
</body>
</html>
    )rawliteral";
    request->send(200, "text/html", html);
  });

  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request){
    String json = "{";
    json += "\"uid\":\"" + lastUID + "\",";
    json += "\"access\":\"" + lastAccess + "\",";
    json += "\"distance\":\"" + String((int)distanceCm) + "\",";
    json += "\"motion\":\"" + String(digitalRead(PIR_PIN) == HIGH ? "Detected" : "None") + "\",";
    json += "\"intruderAlert\":" + String(intruderAlertActive ? "true" : "false") + ","; // Send alert status
    json += "\"doorStatus\":" + String(doorIsCurrentlyOpen ? "true" : "false");          // Send door status
    json += "}";
    request->send(200, "application/json", json);
  });

  // --- NEW WEB SERVER ROUTE TO STOP BUZZER ---
  server.on("/stopBuzzer", HTTP_GET, [](AsyncWebServerRequest *request){
    stopBuzzerFromWeb = true; // Set flag to stop buzzer in loop()
    request->send(200, "text/plain", "Buzzer stop command received."); // Send a response for fetch()
  });
  // --- END NEW WEB SERVER ROUTE ---

  // --- EXISTING WEB SERVER ROUTES FOR CONTROLS ---
  server.on("/door", HTTP_GET, [](AsyncWebServerRequest *request){
    doorManualToggle = true; // Set new flag to trigger door toggle in loop()
    request->send(200, "text/plain", "Door toggle command received.");
  });

  server.on("/light", HTTP_GET, [](AsyncWebServerRequest *request){
    webLightOverride = !webLightOverride; // Toggle the web override state
    if (webLightOverride) { // If web override is active, toggle the LED state directly
      currentMotionLedState = !currentMotionLedState; // Toggle LED
      digitalWrite(Motion_Led, currentMotionLedState);
      motionDetectionLightControl = false; // Disable PIR control
    } else { // If web override is turned OFF, revert to PIR control
      motionDetectionLightControl = true; // Enable PIR control
      // The loop will then set the LED based on PIR state
    }
    request->send(200, "text/plain", "Light toggle command received.");
  });
  // --- END EXISTING WEB SERVER ROUTES FOR CONTROLS ---


  server.begin();

  mfrc522.PCD_Init();

  // --- ESP32 Servo setup ---
  // Allow allocation of all timers to `ESP32Servo`
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);
  doorServo.setPeriodHertz(50); // Standard 50hz servo
  doorServo.attach(22, 500, 2500); // Attach the servo to its pin, specifying min/max pulse widths
                                  // 500us and 2500us are common, adjust if your servo has different limits
  doorServo.write(CLOSED_ANGLE); // Ensure door starts closed
  doorPosition = CLOSED_ANGLE; // Initialize doorPosition to match physical state
  doorIsOpen = false; // Initialize door state
  doorIsCurrentlyOpen = false; // Initialize web display status
  // --- End ESP32 Servo setup ---


  pinMode(Red_Led, OUTPUT);
  pinMode(Green_Led, OUTPUT);
  pinMode(buzzerPin, OUTPUT);
  pinMode(PIR_PIN, INPUT);
  pinMode(Motion_Led, OUTPUT);
  pinMode(trigPin, OUTPUT);
  pinMode(echoPin, INPUT);

  digitalWrite(buzzerPin, LOW); // Ensure buzzer is off on startup
  digitalWrite(Motion_Led, LOW); // Ensure LED is off on startup (initial state)
  currentMotionLedState = LOW; // Initialize the tracking variable
  

  WiFi.mode(WIFI_STA);
  //WiFi.disconnect(); // Important for ESP-NOW stability

  // Set WiFi channel to 1 (must match gateway)
  esp_wifi_set_promiscuous(true);      // Enable promiscuous mode
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);      // Disable promiscuous mode

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW Init Failed");
    return;
  }

  memcpy(peerInfo.peer_addr, gatewayAddress, 6);
  peerInfo.channel = 1;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Failed to add peer");
    return;
  }

}

void loop() {
  // These should only be HIGH when an event is happening, otherwise LOW
  // Consider moving these to be set high only when needed, and low after action.
  // For now, keeping them here ensures they don't stick ON.
  digitalWrite(Green_Led, LOW);
  digitalWrite(Red_Led, LOW);


  // --- MANUAL DOOR TOGGLE TRIGGER (from web server) ---
  // This doorManualToggle flag is set by the web server route /door
  if (doorManualToggle) {
    if (doorIsCurrentlyOpen) { // If door is open, close it
      closeDoor();
    } else { // If door is closed, open it
      openDoor();
    }
    doorManualToggle = false; // Reset the flag immediately after triggering the toggle
  }

  // PIR and Ultrasonic sensing
  int pirState = digitalRead(PIR_PIN); // Read PIR sensor state
  bool inRange = false;

  // Update lastMotionTime if PIR is HIGH, regardless of door state
  // This ensures that 'lastMotionTime' always reflects the most recent motion
  if (pirState == HIGH) {
    lastMotionTime = millis();
  }

  // --- MOTION_LED Control Logic ---
  if (motionDetectionLightControl) { // If PIR control is enabled
    if (pirState == HIGH) {
      if (!currentMotionLedState) { // Only change if it's currently off
        digitalWrite(Motion_Led, HIGH);
        currentMotionLedState = HIGH;
      }
    } else {
      if (currentMotionLedState) { // Only change if it's currently on
        digitalWrite(Motion_Led, LOW);
        currentMotionLedState = LOW;
      }
    }
  }
  // The `webLightOverride` flag in the /light route handles direct web control.
  // If `webLightOverride` is true, `motionDetectionLightControl` is false, and the web route
  // directly sets `currentMotionLedState` and `digitalWrite(Motion_Led, currentMotionLedState)`.
  // When `webLightOverride` becomes false, `motionDetectionLightControl` is set back to true,
  // allowing PIR to resume control from its current state.
  // --- END MOTION_LED Control Logic ---


  if (pirState == HIGH) { // Use pirState directly for motion detection for ultrasonic
    digitalWrite(trigPin, LOW); delayMicroseconds(2);
    digitalWrite(trigPin, HIGH); delayMicroseconds(10);
    digitalWrite(trigPin, LOW);

    duration = pulseIn(echoPin, HIGH);
    distanceCm = duration * SOUND_SPEED / 2;

    if (distanceCm > 0 && distanceCm < 100) {
      inRange = true;
      lcd.setCursor(0, 1);
      lcd.print("Dist:");
      lcd.print((int)distanceCm);
      lcd.print("cm      ");
    } else {
      lcd.setCursor(0, 1);
      lcd.print("Out of Range   ");
    }
  } else {
    lcd.setCursor(0, 1);
    lcd.print("No Person      ");
  }

  // --- RFID ACCESS LOGIC ---
  // Only process RFID if buzzer alarm is not active
  if (!buzzerAlarmActive && inRange && mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
    String uidString = "";
    for (byte i = 0; i < mfrc522.uid.size; i++) {
      if (mfrc522.uid.uidByte[i] < 0x10) uidString += "0";
      uidString += String(mfrc522.uid.uidByte[i], HEX);
    }

    lastUID = uidString;

    String authorizedUID = "7d060f05"; // Your card UID (example)

    if (uidString == authorizedUID) {
      lastAccess = "Granted";
      deniedAttempts = 0; // Reset denied attempts on successful access

      lcd.setCursor(0, 1);
      lcd.print("Access Granted ");

      digitalWrite(Green_Led, HIGH);
      delay(500); digitalWrite(Green_Led, LOW);
      delay(500); digitalWrite(Green_Led, HIGH);
      delay(500); digitalWrite(Green_Led, LOW);

      // --- Trigger door open via RFID ---
      if (!doorIsOpen) { // Only open if not already open
        openDoor();
      }
    } else {
      lastAccess = "Denied";
      deniedAttempts++; // Increment denied attempts

      lcd.setCursor(0, 1);
      lcd.print("Access Denied  ");

      // Short buzzer for single denial (only if alarm is not already active)
      if (!buzzerAlarmActive) {
        for (int i = 0; i < 3; i++) {
          digitalWrite(buzzerPin, HIGH);
          digitalWrite(Red_Led, HIGH);
          delay(200);
          digitalWrite(buzzerPin, LOW);
          digitalWrite(Red_Led, LOW);
          delay(200);
        }
      }

      // --- INTRUDER ALERT LOGIC ---
      if (deniedAttempts >= MAX_DENIED_ATTEMPTS && !buzzerAlarmActive) {
        Serial.println("!!! INTRUDER ALERT TRIGGERED !!!");
        buzzerAlarmActive = true;
        intruderAlertActive = true; // Set flag for web server
        buzzerAlarmStartTime = millis(); // Start alarm timer
        // Buzzer will be continuously managed below in the main loop
      }
    }

    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();
  }

  // --- ELEVATOR DOOR LOGIC (MAIN STATE MACHINE) ---
  if (doorIsOpen) {
    // Condition 1: Normal closure after initial open duration + motion extension
    bool normalCloseCondition = (millis() - doorOpenStartTime > NORMAL_OPEN_DURATION_MS) &&
                                (millis() - lastMotionTime > MOTION_EXTENSION_MS);

    // Condition 2: Hard maximum open time elapsed
    bool maxTimeElapsedCondition = (millis() - doorOpenStartTime > MAX_OPEN_DURATION_TOTAL_MS);

    if (normalCloseCondition || maxTimeElapsedCondition) {
       if (maxTimeElapsedCondition) {
           Serial.println("Maximum open duration reached. Closing door.");
       } else {
           Serial.println("Normal open duration plus motion extension expired. Closing door.");
       }
       closeDoor();
    }
  } else { // Door is closing or closed
    // If motion is detected AND the door is currently closing (not fully closed, or just started to close)
    // The `doorPosition > CLOSED_ANGLE` check ensures it won't try to open if it's already fully closed.
    if (pirState == HIGH && doorPosition > CLOSED_ANGLE) {
      Serial.println("Motion detected while door is closing. Reopening door.");
      openDoor(); // Reopen the door
    }
  }
  // --- END ELEVATOR DOOR LOGIC ---

  // --- BUZZER ALARM MANAGEMENT ---
  if (buzzerAlarmActive) {
    digitalWrite(buzzerPin, HIGH); // Keep buzzer on

    // Check if alarm duration has passed
    if (millis() - buzzerAlarmStartTime >= BUZZER_ALARM_DURATION_MS) {
      Serial.println("Buzzer alarm duration ended.");
      digitalWrite(buzzerPin, LOW); // Turn off buzzer
      buzzerAlarmActive = false;
      intruderAlertActive = false; // Reset web alert flag
      deniedAttempts = 0; // Reset attempts after alarm finishes
    }

    // Check if buzzer stop command received from web
    if (stopBuzzerFromWeb) {
      Serial.println("Buzzer stopped from web server.");
      digitalWrite(buzzerPin, LOW); // Turn off buzzer
      buzzerAlarmActive = false;
      intruderAlertActive = false; // Reset web alert flag
      deniedAttempts = 0; // Reset attempts
      stopBuzzerFromWeb = false; // Reset the flag
    }
  }
  // --- END BUZZER ALARM MANAGEMENT ---

  // --- Update webData struct for web server /status endpoint ---
  // These are used internally for the web server and do NOT affect ESP-NOW payload
  memset(webData.uid, 0, sizeof(webData.uid));
  memset(webData.access, 0, sizeof(webData.access));
  strncpy(webData.uid, lastUID.c_str(), sizeof(webData.uid) - 1);
  webData.uid[sizeof(webData.uid) - 1] = '\0';
  strncpy(webData.access, lastAccess.c_str(), sizeof(webData.access) - 1);
  webData.access[sizeof(webData.access) - 1] = '\0';
  webData.distance = distanceCm;
  webData.motion = (pirState == HIGH);
  webData.intruderAlert = intruderAlertActive;
  webData.doorStatus = doorIsCurrentlyOpen;
  webData.nodeID = 3; // Node ID for web server display, if needed

  // --- NON-BLOCKING ESP-NOW SEND (USES GatewaySecurityData) ---
  // Use the ESP_NOW_SEND_INTERVAL constant here for harmonization
  if (millis() - lastEspNowSendTime >= ESP_NOW_SEND_INTERVAL) {
    memset(secData.uid, 0, sizeof(secData.uid));
    memset(secData.access, 0, sizeof(secData.access));

    // Copy and null-terminate
    strncpy(secData.uid, lastUID.c_str(), sizeof(secData.uid) - 1);
    secData.uid[sizeof(secData.uid) - 1] = '\0';

    strncpy(secData.access, lastAccess.c_str(), sizeof(secData.access) - 1);
    secData.access[sizeof(secData.access) - 1] = '\0';

    secData.distance = distanceCm;
    secData.motion = (pirState == HIGH);
    secData.nodeID = 3; // set nodeID each time you send data

    esp_err_t result = esp_now_send(gatewayAddress, (uint8_t *)&secData, sizeof(secData));

    if (result == ESP_OK) {
      Serial.println("ESP-NOW data sent successfully");
    } else {
      Serial.printf("ESP-NOW send failed: %d\n", result);
    }
    lastEspNowSendTime = millis(); // Update the last send time
  }
  // --- END NON-BLOCKING ESP-NOW SEND ---

  // Remove the blocking delay(1000); here.
  // The loop will now run as fast as possible, checking all conditions continuously.
}