#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <SPIFFS.h>
#include <FS.h>
#include <Adafruit_I2CDevice.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPIFFS_ImageReader.h>

//Pinout definition for the TFT screen
#define TFT_DC 2
#define TFT_RST 4
#define TFT_CS 5

#define PIR_PIN 26
#define FILE_PHOTO "/image.jpg"

//This defines the time between two detections of motion that cause sending an email
#define pirDelayTime 45000

//Variables for connection with camera's access point
const char* ssid_cam = "ESP32-cam";
const char* password_cam =  "123456789";

//HTTP request for the communication with the camera
const char* serverTakePic = "http://192.168.1.120/img";
const char* serverAccessQuest = "http://192.168.1.120/access";

//Boolean variables to be used in the main loop
boolean motionDetected = false;
boolean imageObtainSuccess = false;

//Int variable used for handling the response from the e-mail
int accessAnswer = -1;

//Function handling for C++
boolean requestCameraImage(void);
void setupCameraWiFi(void);
void setupRouterWiFi(void);
int requestAccessAnswer(void);

Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);
SPIFFS_ImageReader reader;

void setup() {
  pinMode(PIR_PIN, INPUT);

  Serial.begin(9600);

  if(!SPIFFS.begin()){ Serial.println("Error initializing SPIFFS!");}

  tft.initR(INITR_BLACKTAB);

  setupCameraWiFi();
}

void loop() {

  if (WiFi.status() != WL_CONNECTED){
    setupCameraWiFi();
  }

  while (!motionDetected){
    motionDetected = digitalRead(PIR_PIN);
    tft.fillScreen(ST77XX_BLACK);
    delay(1500);
  }

  Serial.println("Motion detected! Sending image request to ESP32-CAM");
  int t0 = millis();
  reader.drawBMP("/acc_s.bmp", tft, 0, 0);

  while (!imageObtainSuccess){
    imageObtainSuccess = requestCameraImage();
  }

  delay (5000);

  while (accessAnswer == -1){
    delay(1000);
    accessAnswer = requestAccessAnswer(); //-1 means that the ESP32-CAM has not received an answer from the email receipent
  }

  if (accessAnswer == 0){
    reader.drawBMP("/acc_d.bmp", tft, 0, 0); //Access denied
    delay(5000);
  } else {
    reader.drawBMP("/acc_g.bmp", tft, 0, 0); //Access granted
    delay(5000);
  }

  imageObtainSuccess = false;
  motionDetected = false;
  accessAnswer = -1;

  //This part handles the time - so that the image request is not sent more often than pirDelayTime
  int t = millis() - t0;
  if (t < pirDelayTime){
    delay(pirDelayTime - t);
  } else{
    delay(1);
  }
}

boolean requestCameraImage(void){
  boolean status;
  HTTPClient http;

  http.begin(serverTakePic);
  int httpCode = http.GET();
  if (httpCode > 0) {
    if (httpCode == HTTP_CODE_OK) {
      Serial.println("Image request sent successfully");
      status = true;
    }
  } else {
    Serial.printf("[HTTP] GET... failed, error: %s\n", http.errorToString(httpCode).c_str());
    status = false;
  }
  http.end();
  return status;
}

int requestAccessAnswer(void){
  int answer = -1;
  HTTPClient http;
  
  http.begin(serverAccessQuest);
  int httpCode = http.GET();
  if (httpCode > 0) {
    if (httpCode == HTTP_CODE_OK) {
      answer = http.getString().toInt();
      //Serial.println("Answer from ESP32-CAM was received.");
      }
  } else {
    Serial.printf("[HTTP] GET... failed, error: %s\n", http.errorToString(httpCode).c_str());
    http.end();
    answer = -1;
  }
  http.end();
  return answer;
}

void setupCameraWiFi(void){
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid_cam, password_cam);

  while (WiFi.status() != WL_CONNECTED){
    delay(1000);
    Serial.println("Connecting to ESP32-CAM's WiFi...");
  }
  Serial.print("Connected to ESP32-CAM's WiFi network with IP address: ");
  Serial.println(WiFi.localIP());
}