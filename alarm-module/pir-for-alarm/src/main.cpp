#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <SPIFFS.h>
#include <FS.h>
#include <Adafruit_I2CDevice.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPIFFS_ImageReader.h>
#include <esp_http_server.h>

//Pinout definition for the TFT screen
#define TFT_DC 2
#define TFT_RST 4
#define TFT_CS 5

#define PIR_PIN 26
#define BUTTON_PIN 25
#define FILE_PHOTO "/image.jpg"

//This defines the maximum intercom waiting time
#define maxIntercomTime 45000

//This defines the time between two detections of motion that cause sending an email
#define pirDelayTime 45000

//Variables for connection with camera's access point
const char* ssid_cam = "ESP32-cam";
const char* password_cam =  "123456789";

//Variables for ESP32's AP - used to set alarm or intercom mode
const char* ap_ssid = "ESP32 alarm/intercom";
const char* ap_password = "123456789";

//Static AP IP settings - ESP32 will be accessible under this address for the devices in its network
IPAddress AP_local_IP(192, 168, 1, 120);
IPAddress AP_gateway(192, 168, 1, 1);
IPAddress AP_subnet(255, 255, 0, 0);

//HTTP request for the communication with the camera
const char* serverTakePic = "http://192.168.1.120/img";
const char* serverAccessQuest = "http://192.168.1.120/access";

//Boolean variables to be used in the main loop
boolean motionDetected = false;
boolean imageObtainSuccess = false;
boolean intercomCalled = false;

//Int variable used for handling the response from the e-mail
int accessAnswer = -1;

//String variable ALARM/INTERCOM which changes the mode of operation of the system
String mode = "";

//Function handling for C++
boolean requestCameraImage(void);

void setupCameraWiFi(void);
void runAlarmMode(void);
void runIntercomMode(void);
void setupAP(void);
void startServer();

int requestAccessAnswer(void);

esp_err_t home_get_handler(httpd_req_t *req);
esp_err_t alarm_get_handler(httpd_req_t *req);
esp_err_t intercom_get_handler(httpd_req_t *req);

Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);
SPIFFS_ImageReader reader;

void setup() {
  pinMode(PIR_PIN, INPUT);
  pinMode(BUTTON_PIN, INPUT);

  Serial.begin(9600);

  if(!SPIFFS.begin()){ Serial.println("Error initializing SPIFFS!");}

  tft.initR(INITR_BLACKTAB);

  setupAP();
  startServer();

  while (mode == "") {
    delay(1000);
  }

  WiFi.softAPdisconnect(true);
  setupCameraWiFi();

}

void loop() {

  if (WiFi.status() != WL_CONNECTED){
    setupCameraWiFi();
  }

  if (mode == "ALARM") {
    runAlarmMode();
  } else if (mode == "INTERCOM") {
    runIntercomMode();
  }
  
}

void runIntercomMode(void) {
  while (!intercomCalled){
    if (WiFi.status() != WL_CONNECTED){
    setupCameraWiFi();
    }

    intercomCalled = !digitalRead(BUTTON_PIN);
    tft.fillScreen(ST77XX_BLACK);
    delay(500);
  }

  //Here some kind of buzzer ring is needed
  Serial.println("Somebody is ringing on the intercom!");
  int t0 = millis();
  reader.drawBMP("/acc_s.bmp", tft, 0, 0);

  while (accessAnswer == -1){ 
    if (WiFi.status() != WL_CONNECTED){
    setupCameraWiFi();
    }

    delay(1000);
    accessAnswer = requestAccessAnswer(); //-1 means that the ESP32-CAM has not received an answer from the email receipent
    int t = millis() - t0;
    if (t >= maxIntercomTime && accessAnswer == -1) {
      accessAnswer = 0;
    }
  }

  if (accessAnswer == 0){
    reader.drawBMP("/acc_d.bmp", tft, 0, 0); //Access denied
    delay(5000);
  } else {
    reader.drawBMP("/acc_g.bmp", tft, 0, 0); //Access granted
    delay(5000);
  }

  accessAnswer = -1;
  intercomCalled = false;

}

void runAlarmMode(void) {
  while (!motionDetected){
    if (WiFi.status() != WL_CONNECTED){
    setupCameraWiFi();
    }

    motionDetected = digitalRead(PIR_PIN);
    tft.fillScreen(ST77XX_BLACK);
    delay(1500);
  }

  Serial.println("Motion detected! Sending image request to ESP32-CAM");
  int t0 = millis();

  while (!imageObtainSuccess){
    if (WiFi.status() != WL_CONNECTED){
    setupCameraWiFi();
    }
    
    imageObtainSuccess = requestCameraImage();
  }

  imageObtainSuccess = false;
  motionDetected = false;

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

void setupAP(void) {
  WiFi.mode(WIFI_AP);
  Serial.println("Setting up AP...");

  WiFi.softAP(ap_ssid, ap_password);

  if (!WiFi.softAPConfig(AP_local_IP, AP_gateway, AP_subnet)) {
    Serial.println("AP configuration failed.");
  }

  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP setup with IP address: ");
  Serial.println(IP);
}

esp_err_t home_get_handler(httpd_req_t *req) {
  const char resp[] = "<center><p style=\"font-size:40px\"><b>You logged in to the ESP-32 alarm/intercom system.</b></p></center> <center><p style=\"font-size:30px\">Please select the right operation mode from below:</p></center> <br><center><form action = \"http://192.168.1.120/alarm\"><input type=\"submit\" value=\"Alarm mode\" style=\"height:60px; width:350px; font-size:30px\"/> </form></center> <br><center><form action = \"http://192.168.1.120/intercom\"><input type=\"submit\" value=\"Intercom mode\" style=\"height:60px; width:350px; font-size:30px\"/> </form></center>";

  httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

esp_err_t alarm_get_handler(httpd_req_t *req) {
  const char resp[] = "Alarm mode is on!";
  mode = "ALARM";
  httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

esp_err_t intercom_get_handler(httpd_req_t *req) {
  const char resp[] = "Intercom mode is on!";
  httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
  mode = "INTERCOM";
  return ESP_OK;
}

httpd_uri_t home_uri = {
  .uri = "/",
  .method = HTTP_GET,
  .handler = home_get_handler,
  .user_ctx = NULL
};

httpd_uri_t alarm_uri = {
  .uri = "/alarm",
  .method = HTTP_GET,
  .handler = alarm_get_handler,
  .user_ctx = NULL
};

httpd_uri_t intercom_uri = {
  .uri = "/intercom",
  .method = HTTP_GET,
  .handler = intercom_get_handler,
  .user_ctx = NULL
};

void startServer(){
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  httpd_handle_t server = NULL;

  if (httpd_start(&server, &config) == ESP_OK) {
    httpd_register_uri_handler(server, &home_uri);
    httpd_register_uri_handler(server, &alarm_uri);
    httpd_register_uri_handler(server, &intercom_uri);
  }
}