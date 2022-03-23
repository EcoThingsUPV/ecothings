#include <Arduino.h>
#include <WiFi.h>
#include <SPIFFS.h>
#include <ESPAsyncWebServer.h>
#include <esp_camera.h>
#include <FS.h>

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

boolean takeNewPhoto = false;
const char* ssid = "ESP32-cam";
const char* password =  "123456789";

int accessAnswer = -1; //-1 means that the ESP32-CAM did not get response to the access request
 
AsyncWebServer server(80);

bool checkPhoto( fs::FS &fs );
void capturePhotoSaveSpiffs(void);

void setup(){
  pinMode(4, OUTPUT);

  Serial.begin(115200);
  Serial.println("Starting the program...");

  Serial.println("Setting AP...");

  WiFi.softAP(ssid, password);

  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(IP);

  if (!SPIFFS.begin()){
    Serial.println("SPIFFS failed to initialize!");
  }

  server.on("/img", HTTP_GET, [](AsyncWebServerRequest *request){
    takeNewPhoto = true;
    accessAnswer = -1;
    delay(1000);
    request->send(SPIFFS, FILE_PHOTO, "image/jpeg");
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
    takeNewPhoto = false;
  }
  delay(1);
}

bool checkPhoto( fs::FS &fs ) {
  File f_pic = fs.open( FILE_PHOTO );
  unsigned int pic_sz = f_pic.size();
  return ( pic_sz > 100 );
}

void capturePhotoSaveSpiffs(void){
  camera_fb_t *fb = NULL;
  bool ok = 0;

  do{
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