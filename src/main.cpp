#include <Arduino.h>
#include <WiFi.h>
#include <FirebaseESP32.h>
#include <chrono>
#include <iostream>
#include <string>
#include <sstream>
#include <iomanip>
// Libraries
#include <iostream>
#include <ctime>
#include <TimeLib.h>
#include <WiFiUdp.h>
#include <cctype> // for std::toupper

//Provide the token generation process info.
#include "addons/TokenHelper.h"
//Provide the RTDB payload printing info and other helper functions.
#include "addons/RTDBHelper.h"

// Wifi Credential
#define WIFI_SSID "Sengiman"
#define WIFI_PASSWORD "hamster150103"

// Database API requirements
#define API_KEY "AIzaSyAdx0bC79HCFRzqUkBjBpygXbGKCHJvsQQ"
#define DATABASE_URL "https://esp32-firebase-demo-f6138-default-rtdb.asia-southeast1.firebasedatabase.app/"

// Time server
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 21600;
const int   daylightOffset_sec = 3600;

//Define Firebase Data object
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// Global Variables
unsigned long sendDataPrevMillis = 0;
int count = 0;
bool signupOK = false;
int threshold;
bool pumpIsOn;
const int sensorPin = 32;
const int pumpPin = GPIO_NUM_23;
const float sensorMin = 0.0; // Minimum sensor output voltage
const float sensorMax = 3.0; // Maximum sensor output voltage
const int reversedMin = 4095; // Minimum reversed voltage
const int reversedMax = 0; // Maximum reversed voltage
bool doneInitRelay = false;
int pumpActiveDuration = 1000;
int pumpWaitDuration = 1000;

// Calibration Equation Params
const float a = 2.48;
const float b = -0.72;

// Function to zero-pad single-digit values
std::string zeroPad(int value) {
    std::ostringstream oss;
    oss << std::setw(2) << std::setfill('0') << value;
    return oss.str();
}

void makeHistory(std::string actionType);

void setup() {
  // Relay Init
  pinMode(pumpPin, OUTPUT);
  digitalWrite(pumpPin, HIGH);

  // Connect esp32 to network
  Serial.begin(9600);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED){
    Serial.print(".");
    delay(300);
  }
  Serial.println();
  Serial.print("Connected with IP: ");
  Serial.println(WiFi.localIP());
  Serial.println();

  /* Assign the api key (required) */
  config.api_key = API_KEY;

  /* Assign the RTDB URL (required) */
  config.database_url = DATABASE_URL;

  // Sign Up
  if (Firebase.signUp(&config, &auth, "", "")){
    Serial.println("ok");
    signupOK = true;
  }
  else{
    Serial.printf("%s\n", config.signer.signupError.message.c_str());
  }
  /* Assign the callback function for the long running token generation task */
  config.token_status_callback = tokenStatusCallback; //see addons/TokenHelper.h

  // Init and get the time
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  configTzTime("Asia/Jakarta", ntpServer);

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
}

void loop() {
  if (Firebase.ready() && signupOK && (millis() - sendDataPrevMillis > 1000 || sendDataPrevMillis == 0)){
    sendDataPrevMillis = millis();
    
    // Inform this device already connected to firebase
    Firebase.RTDB.setBool(&fbdo, "/settings/isOnline", true);
    // Get sensor output
    // Read the sensor output voltage
    float sensorValue = analogRead(sensorPin) * (3.3 / 4095.0); // Convert ADC reading to voltage
    // Map the sensor output voltage to the reversed range
    int reversedValue = map(sensorValue * 1000, sensorMin * 1000, sensorMax * 1000, reversedMin, reversedMax); // Multiply by 1000 to avoid floating point errors
    // Calculate the predicted moisture value
    float moistureValue = a * reversedValue / 4095.0 + b;
    // Convert the moisture value to an integer percentage
    int moisturePercentage = constrain((int)(moistureValue * 100), 0, 100);

    // Settings Values
    // Get threshold information
    if(Firebase.RTDB.getInt(&fbdo, "/settings/threshold")){
      if(fbdo.dataType() == "int"){
        threshold = fbdo.intData();
        Serial.printf("[INFO] Threshold value : %d\n", threshold);
      }
    }
    // Get pump status information
    if(Firebase.RTDB.getBool(&fbdo, "/settings/pumpIsOn")){
      if(fbdo.dataType() == "boolean"){
        pumpIsOn = fbdo.boolData();
        Serial.printf("[INFO] pumpIsOn value : %d\n", pumpIsOn);
      }
    }
    // Get pump active duration
    if(Firebase.RTDB.getInt(&fbdo, "/settings/pumpActiveDuration")){
      if(fbdo.dataType() == "int"){
        pumpActiveDuration = fbdo.intData();
      }
    }
    // Get pump wait duration
    if(Firebase.RTDB.getInt(&fbdo, "/settings/pumpWaitDuration")){
      if(fbdo.dataType() == "int"){
        pumpWaitDuration = fbdo.intData();
      }
    }

    // Turn on pump based on pumpStatus set from mobile app
    if(pumpIsOn){
      // Pump operation
      digitalWrite(pumpPin, LOW); // Pump on
      pumpIsOn = true;
      delay(pumpActiveDuration);
      digitalWrite(pumpPin, HIGH); // Pump off
      // Make history record
      makeHistory("MANUAL");
      // Set pump status to off in database
      if(Firebase.RTDB.setBool(&fbdo, "/settings/pumpIsOn", false)){
        Serial.println("[DATABASE] Succesfully dispensed water and set pump status to false");
      }
    }
    // Turn on pump based on threshold
    if(moisturePercentage < threshold){
      // Pump operation
      digitalWrite(pumpPin, LOW); // Pump on
      delay(pumpActiveDuration);
      digitalWrite(pumpPin, HIGH); // Pump off
      delay(pumpWaitDuration);
      makeHistory("AUTOMATIC");
    }

    // Write percentage value to database path moisutreData/value
    if (Firebase.RTDB.setInt(&fbdo, "moistureData/value", moisturePercentage)){
      Serial.printf("[INFO] Moisture updated : %d\n", moisturePercentage);
    }
    else {
      Serial.println("FAILED");
      Serial.println(fbdo.errorReason());
    }
  }
}

void makeHistory(std::string actionType){
  // Obtain current time
  struct tm localTime;
  if (!getLocalTime(&localTime))
  {
    Serial.println("Failed to obtain time");
    return;
  }
  else
  {
    // Extract information
    int dayOfMonth = localTime.tm_mday;
    int dayOfWeek = localTime.tm_wday;
    int dayOfYear = localTime.tm_yday;
    int hour = localTime.tm_hour;
    int minute = localTime.tm_min;
    int nano = 0;
    int second = 0;
    // Month Names
    const char *monthNames[] = {"JANUARY", "FEBRUARY", "MARCH", "APRIL", "MAY", "JUNE", "JULY", "AUGUST", "SEPTEMBER", "OCTOBER", "NOVEMBER", "DECEMBER"};
    const char *dayNames[] = {"MONDAY", "TUESDAY", "WEDNESDAY", "THURSDAY", "FRIDAY", "SATURDAY", "SUNDAY"};
    // Extract month information
    const char *month = monthNames[localTime.tm_mon];
    // Extract day of week information
    char strDayOfWeek[20];
    std::strftime(strDayOfWeek, sizeof(dayOfWeek), "%A", &localTime);
    // const char* dayName = dayNames[localTime->tm_wday];
    int monthValue = localTime.tm_mon + 1;
    int year = localTime.tm_year + 1900;
    // Make main node for history record
    std::ostringstream oss;
    oss << actionType << "_" << year << "-" << zeroPad(monthValue) << "-" << zeroPad(dayOfMonth) << "T" << zeroPad(hour) << ":" << zeroPad(minute);
    std::string mainNode = oss.str();

    // Send to firebase
    FirebaseJson nodeChronology;
    FirebaseJson nodeDateTime;
    FirebaseJson nodeMainNode;
    FirebaseJson json;

    // Create Chronology Node
    nodeChronology.add("calendarType", "iso8601");
    nodeChronology.add("id", "ISO");

    // Create dateTime Node
    nodeDateTime.add("chronology", nodeChronology);
    nodeDateTime.add("dayOfMonth", dayOfMonth);
    nodeDateTime.add("dayOfWeek", strDayOfWeek);
    nodeDateTime.add("dayOfYear", dayOfYear);
    nodeDateTime.add("hour", hour);
    nodeDateTime.add("minute", minute);
    nodeDateTime.add("month", month);
    nodeDateTime.add("monthValue", monthValue);
    nodeDateTime.add("nano", 0);
    nodeDateTime.add("second", 0);
    nodeDateTime.add("year", year);

    // Create the main node like AUTOMATIC_2023-05-19T15:2023
    nodeMainNode.add("actionType", actionType);
    nodeMainNode.add("dateTime", nodeDateTime);

    // Serialize JSON Object nodeMainNode
    std::string serializedData;
    nodeMainNode.toString(serializedData);

    // Make path for the formated main node name
    std::string tempHistoryPath = "/history";

    // Send serialized JSON object(string) to firebase
    json.set(mainNode.c_str(), nodeMainNode);

    Firebase.RTDB.pushJSON(&fbdo, tempHistoryPath.c_str(), &nodeMainNode);
  }
}