#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <SPIFFS.h>
#include <FS.h>
#include <ESP_Mail_Client.h>
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

//Variables for connection with router
const char* ssid_router = "iPhone";
const char* password_router = "123456789";

//Variables for email handling
const char* smtp_host = "smtp.gmail.com";
const int smtp_port = 465;
const char* author_email = "ecothingsupv@gmail.com";
const char* author_password = "fwlvzfcgyzzrleej";
const char* recipient_email = "piotrek.laszkiewicz23@gmail.com";

//Email text content
const String htmlMsg = "Warning! Your ESP32 alarm module has detected movement on the premises. The image of the event is attached to this email.";

//HTTP request for taking image by camera
const char* serverTakePic = "http://192.168.4.1/img";

//Boolean variables to be used in the main loop
boolean motionDetected = false;
boolean imageObtainSuccess = false;
boolean emailSent = false;

//Function handling for C++
boolean requestCameraImage(void);
boolean sendEmail(void);
void setupCameraWiFi(void);
void setupRouterWiFi(void);
void smtpCallback(SMTP_Status status);

SMTPSession smtp;
Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);
SPIFFS_ImageReader reader;

void setup() {
  pinMode(PIR_PIN, INPUT);

  Serial.begin(9600);

  if(!SPIFFS.begin()){ Serial.println("Error initializing SPIFFS!");}

  tft.initR(INITR_BLACKTAB);  
}

void loop() {
  setupCameraWiFi();

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

  setupRouterWiFi();

  while (!emailSent){
    emailSent = sendEmail();
  }

  Serial.println("Email was sent");

  imageObtainSuccess = false;
  motionDetected = false;
  emailSent = false;

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
  File f = SPIFFS.open(FILE_PHOTO, "w");
  if (!f){
    Serial.println("Failed to create file");
    status = false;
  }
  if (f){
    http.begin(serverTakePic);
    int httpCode = http.GET();
    if (httpCode > 0) {
      if (httpCode == HTTP_CODE_OK) {
        http.writeToStream(&f);
        Serial.println("Image was downloaded");
        status = true;
      }
    } else {
      Serial.printf("[HTTP] GET... failed, error: %s\n", http.errorToString(httpCode).c_str());
      f.close();
      http.end();
      status = false;
    }
  }
  f.close();
  http.end();
  return status;
}

void setupCameraWiFi(void){
  if (WiFi.status() == WL_CONNECTED){WiFi.disconnect();}
  WiFi.begin(ssid_cam, password_cam);

  while (WiFi.status() != WL_CONNECTED){
    delay(1000);
    Serial.println("Connecting to ESP32-CAM's WiFi...");
  }
  Serial.print("Connected to ESP32-CAM's WiFi network with IP address: ");
  Serial.println(WiFi.localIP());
}

void setupRouterWiFi(void){
  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid_router, password_router);

  while (WiFi.status() != WL_CONNECTED){
    delay(1000);
    Serial.println("Connecting to WiFi...");
  }
  Serial.println("Connected to your WiFi network");
}

boolean sendEmail(void){

  boolean status;

  smtp.debug(0);
  //smtp.callback(smtpCallback); uncomment to see callbacks in serial monitor

  //Email session configuration
  ESP_Mail_Session session;

  session.server.host_name = smtp_host;
  session.server.port = smtp_port;
  session.login.email = author_email;
  session.login.password = author_password;
  session.login.user_domain = "";

  //Email content configuration
  SMTP_Message message;

  message.enable.chunking = true;
  message.sender.name = "ESP32 alarm module";
  message.sender.email = author_email;
  message.subject = "ALARM! Movement was detected on the premises!";
  message.addRecipient("House owner", recipient_email);

  message.html.content = htmlMsg.c_str();
  message.html.charSet = "utf-8";
  message.html.transfer_encoding = Content_Transfer_Encoding::enc_qp;
  
  message.priority = esp_mail_smtp_priority::esp_mail_smtp_priority_normal;
  message.response.notify = esp_mail_smtp_notify_success | esp_mail_smtp_notify_failure | esp_mail_smtp_notify_delay;

  SMTP_Attachment att;

  att.descr.filename = "image.jpg";
  att.descr.mime = "image/jpeg";
  att.file.path = FILE_PHOTO;
  att.file.storage_type = esp_mail_file_storage_type_flash;
  att.descr.transfer_encoding = Content_Transfer_Encoding::enc_base64;

  message.addAttachment(att);

  if (!smtp.connect(&session)){
    status = true;
  }

  if (!MailClient.sendMail(&smtp, &message, true)){
    Serial.println("Error sending Email, " + smtp.errorReason());
    status = false;
  }
  return status;
}

void smtpCallback(SMTP_Status status){
  /* Print the current status */
  Serial.println(status.info());

  /* Print the sending result */
  if (status.success()){
    Serial.println("----------------");
    ESP_MAIL_PRINTF("Message sent success: %d\n", status.completedCount());
    ESP_MAIL_PRINTF("Message sent failled: %d\n", status.failedCount());
    Serial.println("----------------\n");
    struct tm dt;

    for (size_t i = 0; i < smtp.sendingResult.size(); i++){
      /* Get the result item */
      SMTP_Result result = smtp.sendingResult.getItem(i);
      time_t ts = (time_t)result.timestamp;
      localtime_r(&ts, &dt);

      ESP_MAIL_PRINTF("Message No: %d\n", i + 1);
      ESP_MAIL_PRINTF("Status: %s\n", result.completed ? "success" : "failed");
      ESP_MAIL_PRINTF("Date/Time: %d/%d/%d %d:%d:%d\n", dt.tm_year + 1900, dt.tm_mon + 1, dt.tm_mday, dt.tm_hour, dt.tm_min, dt.tm_sec);
      ESP_MAIL_PRINTF("Recipient: %s\n", result.recipients);
      ESP_MAIL_PRINTF("Subject: %s\n", result.subject);
    }
    Serial.println("----------------\n");
  }
}