#include <Arduino.h>
#include <WiFi.h>
#include <SPIFFS.h>
#include <ESPAsyncWebServer.h>
#include <esp_camera.h>
#include <FS.h>
#include <ESP_Mail_Client.h>

//image file name for SPIFFS
#define FILE_PHOTO "/image.jpg"

//definitions for ESP32-CAM
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27

#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

//AP parameters
const char* ap_ssid = "ESP32-cam";
const char* ap_password =  "123456789";

//WiFi parameters
const char* ssid = "iPhone";
const char* password = "123456789";

//Static AP IP settings - ESP32-CAM will be accessible under this address for the devices in its network
IPAddress AP_local_IP(192, 168, 1, 120);
IPAddress AP_gateway(192, 168, 1, 1);
IPAddress AP_subnet(255, 255, 0, 0);
IPAddress AP_dns(8, 8, 8, 8);

//Variables for email handling
const char* smtp_host = "smtp.gmail.com";
const int smtp_port = 465;
const char* author_email = "ecothingsupv@gmail.com";
const char* author_password = "fwlvzfcgyzzrleej";
const char* recipient_email = "piotrek.laszkiewicz23@gmail.com";

//Variables used in the main loop
boolean takeNewPhoto = false;
boolean emailSent = false;
String WiFi_IP;

int accessAnswer = -1; //-1 means that the ESP32-CAM did not get response to the access request
 
AsyncWebServer server(80);

//Function handling for C++
bool checkPhoto( fs::FS &fs );
void capturePhotoSaveSpiffs(void);
void setupAP(void);
void smtpCallback(SMTP_Status status);
String setupWiFi(void);
boolean sendEmail(String WiFi_IP);

SMTPSession smtp;

void setup(){
  pinMode(4, OUTPUT);

  Serial.begin(115200);
  Serial.println("Starting the program...");

  WiFi.mode(WIFI_AP_STA);
  setupAP();
  WiFi_IP = setupWiFi();

  if (!SPIFFS.begin()){
    Serial.println("SPIFFS failed to initialize!");
  }

  server.on("/img", HTTP_GET, [](AsyncWebServerRequest *request){
    takeNewPhoto = true;
    accessAnswer = -1;
    request->send(200, "text/plain", "OK");
  });

  server.on("/access", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/plain", String(accessAnswer));
  });

  server.on("/access_denied", HTTP_GET, [](AsyncWebServerRequest *request){
    accessAnswer = 0;
    request->send(200, "text/plain", "You have denied the access");
  });

  server.on("/access_granted", HTTP_GET, [](AsyncWebServerRequest *request){
    accessAnswer = 1;
    request->send(200, "text/plain", "You have granted the access");
  });

  server.begin();

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_UXGA;
  config.jpeg_quality = 25;
  config.fb_count = 2;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    return;
  }

  sensor_t * s = esp_camera_sensor_get();
  // initial sensors are flipped vertically and colors are a bit saturated
  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1); // flip it back
    s->set_brightness(s, 1); // up the brightness just a bit
    s->set_saturation(s, -2); // lower the saturation
  }
  // drop down frame size for higher initial frame rate
  s->set_framesize(s, FRAMESIZE_SVGA);

}

void loop(){
  if (takeNewPhoto){
    capturePhotoSaveSpiffs();
    
    while (!emailSent){
      Serial.println("Sending the email...");
      emailSent = sendEmail(WiFi_IP);
    }
    Serial.println("Email was sent successfully!");

    emailSent = false;
    takeNewPhoto = false;
  }

  if (WiFi.status() != WL_CONNECTED){
    WiFi_IP = setupWiFi();
  }

  delay(500);

}

void setupAP(void){
  Serial.println("Setting up AP...");

  WiFi.softAP(ap_ssid, ap_password);
  delay(100);

 // if (!WiFi.softAPConfig(AP_local_IP, AP_gateway, AP_subnet)) {
 //   Serial.println("AP configuration failed.");
 // }

  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP setup with IP address: ");
  Serial.println(IP);
}

String setupWiFi(void){
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED){
    delay(1000);
    Serial.println("Connecting to WiFi...");
  }
  Serial.print("Connected to your WiFi network with IP address: ");
  Serial.println(WiFi.localIP());

  return WiFi.localIP().toString();
}

bool checkPhoto( fs::FS &fs ) {
  File f_pic = fs.open( FILE_PHOTO );
  unsigned int pic_sz = f_pic.size();
  return ( pic_sz > 100 );
}

void capturePhotoSaveSpiffs(void){
  camera_fb_t *fb = NULL;
  bool ok = 0;

  do {
    Serial.println("Taking a photo...");

    digitalWrite(4, HIGH);
    delay(100);
    fb = esp_camera_fb_get();
    delay(100);
    digitalWrite(4, LOW);
    if (!fb){
      Serial.println("Camera capture failed!");
      return;
    }

    File file = SPIFFS.open(FILE_PHOTO, FILE_WRITE);

    if (!file){
      Serial.println("Failed to open file in writing mode!");
    }
    else {
      file.write(fb->buf, fb->len);
      Serial.println("Picture was saved!");
    }

    file.close();
    esp_camera_fb_return(fb);
    ok = checkPhoto(SPIFFS);
  } while(!ok);
}

boolean sendEmail(String WiFi_IP){

  boolean status;
  String htmlMsg = "Warning! Your ESP32 alarm module has detected movement on the premises. The image of the event is attached to this email. Please choose an action: <br> <a href=\"http://" + WiFi_IP + "/access_denied\">Deny access!</a> <br> <a href=\"http://" + WiFi_IP + "/access_granted\">Grant access!</a>";

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