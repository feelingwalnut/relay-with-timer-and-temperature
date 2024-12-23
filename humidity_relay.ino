#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <PubSubClient.h>

// WiFi credentials
const char* ssid = "ssid";
const char* password = "password";

// MQTT server details
const char* mqtt_server = "192.168.1.1";
const int mqtt_port = 1883;
const char* mqtt_topic = "home/temperature";

// GPIO pins
const int tempSensorPin = D1;
const int relayPin = D2;

// Variables for relay timing
unsigned long relayOnDuration = 10000; // 10 seconds
unsigned long relayOffDuration = 30000; // 30 seconds
unsigned long lastSwitchTime = 0;
bool relayState = false;

// Web server
ESP8266WebServer server(80);

// Temperature sensor
OneWire oneWire(tempSensorPin);
DallasTemperature sensors(&oneWire);
float cachedTemperature = 0.0;
unsigned long lastTemperatureUpdate = 0;

// MQTT client
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// EEPROM addresses for storing durations and minimum temperature
const int EEPROM_ON_ADDR = 0;
const int EEPROM_OFF_ADDR = 4;
const int EEPROM_MIN_TEMP_ADDR = 8; // Address for storing minimum temperature

// Declare minTemperature globally
float minTemperature = 15.0; // Default minimum temperature (15°C)

// Function to control the relay state
void controlRelay() {
  unsigned long currentMillis = millis();

  // If the temperature is below the minimum, keep relay OFF
  if (cachedTemperature < minTemperature) {
    digitalWrite(relayPin, HIGH); // Turn relay OFF
    relayState = false;
    lastSwitchTime = currentMillis;
    return;
  }

  // Relay control based on timing
  if (relayState && currentMillis - lastSwitchTime >= relayOnDuration) {
    digitalWrite(relayPin, HIGH); // Turn relay OFF
    relayState = false;
    lastSwitchTime = currentMillis;
  } else if (!relayState && currentMillis - lastSwitchTime >= relayOffDuration) {
    digitalWrite(relayPin, LOW); // Turn relay ON
    relayState = true;
    lastSwitchTime = currentMillis;
  }
}

// Function to save relay timings and minimum temperature to EEPROM
void saveSettingsToEEPROM() {
  EEPROM.put(EEPROM_ON_ADDR, relayOnDuration);
  EEPROM.put(EEPROM_OFF_ADDR, relayOffDuration);
  EEPROM.commit();
}

// Function to load relay timings and minimum temperature from EEPROM
void loadSettingsFromEEPROM() {
  EEPROM.get(EEPROM_ON_ADDR, relayOnDuration);
  EEPROM.get(EEPROM_OFF_ADDR, relayOffDuration);
  
  // Validate loaded values
  if (relayOnDuration <= 0 || relayOnDuration > 1200000) relayOnDuration = 10000; // Default to 10 seconds
  if (relayOffDuration <= 0 || relayOffDuration > 1200000) relayOffDuration = 30000; // Default to 30 seconds

  // Load minimum temperature value
  float minTemp;
  EEPROM.get(EEPROM_MIN_TEMP_ADDR, minTemp);
  if (minTemp < 35 || minTemp > 100) minTemp = 40.0; // Default to 40°C if invalid
  // Set the minimum temperature globally
  minTemperature = minTemp;
}

// Web server root handler
void handleRoot() {
  String html = "<!DOCTYPE html><html><body>";
  html += "<h1>Relay Controller</h1>";
  html += "<p>Current Temperature: <span id='temperature'>" + String(cachedTemperature, 1) + "</span> &#8451;</p>";
  html += "<script>";
  html += "function updateTemperature() {";
  html += "  fetch('/temperature').then(response => response.text()).then(temp => {";
  html += "    document.getElementById('temperature').innerText = temp;";
  html += "  });";
  html += "}";
  html += "setInterval(updateTemperature, 5000);"; // Update every 5 seconds
  html += "updateTemperature();"; // Initial fetch
  html += "</script>";

  // Form to set relay ON/OFF durations and minimum temperature
  html += "<form action=\"/set\">";
  html += "Relay ON duration (seconds): <input type=\"number\" name=\"on\" value=\"" + String(relayOnDuration / 1000) + "\"><br>";
  html += "Relay OFF duration (seconds): <input type=\"number\" name=\"off\" value=\"" + String(relayOffDuration / 1000) + "\"><br>";
  html += "Minimum Temperature (&#8451;): <input type=\"number\" name=\"min_temp\" value=\"" + String(minTemperature, 1) + "\"><br>";
  html += "<input type=\"submit\" value=\"Update\">";
  html += "</form>";

  html += "</body></html>";
  server.send(200, "text/html", html);
}

// Web server set handler
void handleSet() {
  if (server.hasArg("on")) {
    relayOnDuration = server.arg("on").toInt() * 1000;
  }
  if (server.hasArg("off")) {
    relayOffDuration = server.arg("off").toInt() * 1000;
  }
  if (server.hasArg("min_temp")) {
    minTemperature = server.arg("min_temp").toFloat();
    EEPROM.put(EEPROM_MIN_TEMP_ADDR, minTemperature);
    EEPROM.commit();
  }
  saveSettingsToEEPROM();
  // Send HTML response with the "Go Back to Home" button
  String html = "<!DOCTYPE html><html><body>";
  html += "<h1>Settings updated successfully!</h1>";
  html += "<form action=\"/\">";
  html += "<button type=\"submit\">Go Back to Home</button>";
  html += "</form>";
  html += "</body></html>";
  
  server.send(200, "text/html", html);
}

// Web server reboot handler
void handleReboot() {
  server.send(200, "text/plain", "Rebooting...");
  delay(1000);
  ESP.restart();
}

// Web server temperature handler
void handleTemperature() {
  server.send(200, "text/plain", String(cachedTemperature, 1));
}

// Publish temperature as a JSON object to MQTT
void publishTemperature() {
  sensors.requestTemperatures();
  float temperature = sensors.getTempCByIndex(0);
  
  // Create a JSON formatted message
  String payload = "{\"idx\": [insert your idx here], \"nvalue\": 0, \"svalue\": \"" + String(temperature, 1) + "\"}";
  // String payload = "{\"idx\": 321, \"nvalue\": 0, \"svalue\": \"" + String(temperature, 1) + "\"}";
  mqttClient.publish(mqtt_topic, payload.c_str());
}

// Update temperature cache
void updateTemperature() {
  sensors.requestTemperatures();
  float temperature = sensors.getTempCByIndex(0);
  if (temperature != DEVICE_DISCONNECTED_C) {
    cachedTemperature = temperature;
  }
}

// Connect to MQTT broker
void reconnectMQTT() {
  while (!mqttClient.connected()) {
    if (mqttClient.connect("ESP8266Client")) {
      Serial.println("Connected to MQTT broker");
    } else {
      delay(5000);
    }
  }
}

void setup() {
  // Initialize serial, EEPROM, and GPIO
  Serial.begin(115200);
  EEPROM.begin(512);
  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, HIGH);

  // Load settings from EEPROM
  loadSettingsFromEEPROM();

  // Connect to WiFi
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConnected to WiFi");

  // Print IP address
  Serial.println(WiFi.localIP());

  // Start the temperature sensor
  sensors.begin();

  // Start MQTT client
  mqttClient.setServer(mqtt_server, mqtt_port);

  // Configure web server routes
  server.on("/", handleRoot);
  server.on("/set", handleSet);
  server.on("/reboot", handleReboot);
  server.on("/temperature", handleTemperature);

  // Start the web server
  server.begin();
  Serial.println("Web server started");
}

void loop() {
  // Handle client requests
  server.handleClient();

  // Maintain MQTT connection and publish temperature
  if (!mqttClient.connected()) {
    reconnectMQTT();
  }
  mqttClient.loop();

  static unsigned long lastPublish = 0;
  if (millis() - lastPublish >= 10000) { // Publish temperature every 10 seconds
    publishTemperature();
    lastPublish = millis();
  }

  // Update temperature cache
  if (millis() - lastTemperatureUpdate >= 5000) { // Update temperature every 5 seconds
    updateTemperature();
    lastTemperatureUpdate = millis();
  }

  // Control relay state
  controlRelay();
}
