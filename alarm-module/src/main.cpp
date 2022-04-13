/***
  The video streaming part was inspired by:

  Rui Santos
  Complete project details at https://RandomNerdTutorials.com/esp32-cam-video-streaming-web-server-camera-home-assistant/
  
  Permission is hereby granted, free of charge, to any person obtaining a copy of this part of software and associated documentation files.

  The above copyright notice and this permission notice shall be included in all copies or substantial portions of this part of the software.
***/

#include <Arduino.h>
#include <WiFi.h>
#include <SPIFFS.h>
#include <esp_camera.h>
#include <esp_vfs_fat.h>
#include <FS.h>
#include <EmailSender.h>
#include <esp_http_server.h>
#include <EEPROM.h>
#include <SD_MMC.h>
#include <time.h>
#include <esp_vfs.h>

#define PART_BOUNDARY "123456789000000000000987654321"

#define SCRATCH_BUFSIZE 8192


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

//Structure for scratch buffer
struct file_server_data {
    /* Base path of file storage */
    char base_path[ESP_VFS_PATH_MAX + 1];

    /* Scratch buffer for temporary storage during file transfer */
    char scratch[SCRATCH_BUFSIZE];
};

//Stream parameters
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

//variable for HTTPD server
httpd_handle_t stream_httpd = NULL;

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

//Variables for email handling
const char* smtp_host = "smtp.gmail.com";
const int smtp_port = 465;
const char* author_email = "ecothingsupv@gmail.com";
const char* author_password = "fwlvzfcgyzzrleej";
const char* recipient_email = "piotrek.laszkiewicz23@gmail.com";

//Variables used in the main loop
boolean takeNewPhoto = false;
boolean emailSent = false;
boolean motionDetected = false;
boolean alarmOn = false;
String WiFi_IP;

int accessAnswer = -1; //-1 means that the ESP32-CAM did not get response to the access request

//Variables used by the video recording part
const uint16_t      AVI_HEADER_SIZE = 252;   // Size of the AVI file header.
const long unsigned FRAME_INTERVAL  = 250;   // Time (ms) between frame captures 
const uint8_t       MAX_FRAMES      = 15;    // Maximum number of frames we hold at any time
const int RECORDING_TIME = 5000;            // Time for which the video will keep recording after motion detection signal was received

const byte buffer00dc   [4]  = {0x30, 0x30, 0x64, 0x63}; // "00dc"
const byte buffer0000   [4]  = {0x00, 0x00, 0x00, 0x00}; // 0x00000000
const byte bufferAVI1   [4]  = {0x41, 0x56, 0x49, 0x31}; // "AVI1"            
const byte bufferidx1   [4]  = {0x69, 0x64, 0x78, 0x31}; // "idx1" 
                               
const byte aviHeader[AVI_HEADER_SIZE] =      // This is the AVI file header.  Some of these values get overwritten.
{
  0x52, 0x49, 0x46, 0x46,  // 0x00 "RIFF"
  0x00, 0x00, 0x00, 0x00,  // 0x04           Total file size less 8 bytes [gets updated later] 
  0x41, 0x56, 0x49, 0x20,  // 0x08 "AVI "

  0x4C, 0x49, 0x53, 0x54,  // 0x0C "LIST"
  0x44, 0x00, 0x00, 0x00,  // 0x10 68        Structure length
  0x68, 0x64, 0x72, 0x6C,  // 0x04 "hdrl"

  0x61, 0x76, 0x69, 0x68,  // 0x08 "avih"    fcc
  0x38, 0x00, 0x00, 0x00,  // 0x0C 56        Structure length
  0x90, 0xD0, 0x03, 0x00,  // 0x20 250000    dwMicroSecPerFrame     [based on FRAME_INTERVAL] 
  0x00, 0x00, 0x00, 0x00,  // 0x24           dwMaxBytesPerSec       [gets updated later] 
  0x00, 0x00, 0x00, 0x00,  // 0x28 0         dwPaddingGranularity
  0x10, 0x00, 0x00, 0x00,  // 0x2C 0x10      dwFlags - AVIF_HASINDEX set.
  0x00, 0x00, 0x00, 0x00,  // 0x30           dwTotalFrames          [gets updated later]
  0x00, 0x00, 0x00, 0x00,  // 0x34 0         dwInitialFrames (used for interleaved files only)
  0x01, 0x00, 0x00, 0x00,  // 0x38 1         dwStreams (just video)
  0x00, 0x00, 0x00, 0x00,  // 0x3C 0         dwSuggestedBufferSize
  0x20, 0x03, 0x00, 0x00,  // 0x40 800       dwWidth - 800 (S-VGA)  [based on FRAMESIZE] 
  0x58, 0x02, 0x00, 0x00,  // 0x44 600       dwHeight - 600 (S-VGA) [based on FRAMESIZE]      
  0x00, 0x00, 0x00, 0x00,  // 0x48           dwReserved
  0x00, 0x00, 0x00, 0x00,  // 0x4C           dwReserved
  0x00, 0x00, 0x00, 0x00,  // 0x50           dwReserved
  0x00, 0x00, 0x00, 0x00,  // 0x54           dwReserved

  0x4C, 0x49, 0x53, 0x54,  // 0x58 "LIST"
  0x84, 0x00, 0x00, 0x00,  // 0x5C 144
  0x73, 0x74, 0x72, 0x6C,  // 0x60 "strl"

  0x73, 0x74, 0x72, 0x68,  // 0x64 "strh"    Stream header
  0x30, 0x00, 0x00, 0x00,  // 0x68  48       Structure length
  0x76, 0x69, 0x64, 0x73,  // 0x6C "vids"    fccType - video stream
  0x4D, 0x4A, 0x50, 0x47,  // 0x70 "MJPG"    fccHandler - Codec
  0x00, 0x00, 0x00, 0x00,  // 0x74           dwFlags - not set
  0x00, 0x00,              // 0x78           wPriority - not set
  0x00, 0x00,              // 0x7A           wLanguage - not set
  0x00, 0x00, 0x00, 0x00,  // 0x7C           dwInitialFrames
  0x01, 0x00, 0x00, 0x00,  // 0x80 1         dwScale
  0x04, 0x00, 0x00, 0x00,  // 0x84 4         dwRate (frames per second)         [based on FRAME_INTERVAL]         
  0x00, 0x00, 0x00, 0x00,  // 0x88           dwStart               
  0x00, 0x00, 0x00, 0x00,  // 0x8C           dwLength (frame count)             [gets updated later]
  0x00, 0x00, 0x00, 0x00,  // 0x90           dwSuggestedBufferSize
  0x00, 0x00, 0x00, 0x00,  // 0x94           dwQuality
  0x00, 0x00, 0x00, 0x00,  // 0x98           dwSampleSize

  0x73, 0x74, 0x72, 0x66,  // 0x9C "strf"    Stream format header
  0x28, 0x00, 0x00, 0x00,  // 0xA0 40        Structure length
  0x28, 0x00, 0x00, 0x00,  // 0xA4 40        BITMAPINFOHEADER length (same as above)
  0x20, 0x03, 0x00, 0x00,  // 0xA8 800       Width                  [based on FRAMESIZE] 
  0x58, 0x02, 0x00, 0x00,  // 0xAC 600       Height                 [based on FRAMESIZE] 
  0x01, 0x00,              // 0xB0 1         Planes  
  0x18, 0x00,              // 0xB2 24        Bit count (bit depth once uncompressed)                   
  0x4D, 0x4A, 0x50, 0x47,  // 0xB4 "MJPG"    Compression 
  0x00, 0x00, 0x04, 0x00,  // 0xB8 262144    Size image (approx?)                              [what is this?]
  0x00, 0x00, 0x00, 0x00,  // 0xBC           X pixels per metre 
  0x00, 0x00, 0x00, 0x00,  // 0xC0           Y pixels per metre
  0x00, 0x00, 0x00, 0x00,  // 0xC4           Colour indices used  
  0x00, 0x00, 0x00, 0x00,  // 0xC8           Colours considered important (0 all important).


  0x49, 0x4E, 0x46, 0x4F, // 0xCB "INFO"
  0x1C, 0x00, 0x00, 0x00, // 0xD0 28         Structure length
  0x70, 0x61, 0x75, 0x6c, // 0xD4 
  0x2e, 0x77, 0x2e, 0x69, // 0xD8 
  0x62, 0x62, 0x6f, 0x74, // 0xDC 
  0x73, 0x6f, 0x6e, 0x40, // 0xE0 
  0x67, 0x6d, 0x61, 0x69, // 0xE4 
  0x6c, 0x2e, 0x63, 0x6f, // 0xE8 
  0x6d, 0x00, 0x00, 0x00, // 0xEC 

  0x4C, 0x49, 0x53, 0x54, // 0xF0 "LIST"
  0x00, 0x00, 0x00, 0x00, // 0xF4           Total size of frames        [gets updated later]
  0x6D, 0x6F, 0x76, 0x69  // 0xF8 "movi"
};

camera_fb_t *frameBuffer[MAX_FRAMES];
uint8_t  frameInPos  = 0;                  // Position within buffer where we write to.
uint8_t  frameOutPos = 0;                  // Position within buffer where we read from.
int initOutPos;                            // Initial read position
int initInPos;                             // Initial write position

int cyclicalFramesCaptured = 0;            // Number of frames that were captured by runBufferRepeat()
uint16_t fileFramesCaptured  = 0;          // Number of frames captured by camera.
uint16_t fileFramesWritten   = 0;          // Number of frames written to the AVI file.
uint32_t fileFramesTotalSize = 0;          // Total size of frames in file.
uint32_t fileStartTime       = 0;          // Used to calculate FPS. 
uint32_t filePadding         = 0;          // Total padding in the file.  

boolean fileOpen        = false;           // This is set when we have an open AVI file.

FILE *aviFile;                             // AVI file
FILE *idx1File;                            // Temporary file used to hold the index information

enum relative                              // Used when setting position within a file stream.
{
  FROM_START,
  FROM_CURRENT,
  FROM_END
};

//Video icon base64 encoded
const char* video_miniature = "iVBORw0KGgoAAAANSUhEUgAAAoAAAAKACAYAAAAMzckjAACpBUlEQVR42uzdd3zdVf3H8dd33Huzd5M0TZo2hQ6QbdlLpiwFBQVFRFEQfoCgAoLspSCogCAoS1DEHygK/kRApohsEJTdkWY3aXZy13f8/vjee5OU5iYpoyPv5+NRC+U2tTffe877nPM55xi+7/uIiIiIyLRh6i0QERERUQAUEREREQVAEREREVEAFBEREREFQBERERFRABQRERERBUARERERUQAUEREREQVAEREREVEAFBEREREFQBERERFRABQRERERBUARERERUQAUERERUQAUEREREQVAEREREVEAFBEREREFQBERERFRABQRERERBUARERERUQAUEREREQVAEREREVEAFBEREREFQBERERFRABQRERERBUARERERUQAUEREREQVAEREREQVAEREREVEAFBEREREFQBERERFRABQRERERBUARERERUQAUEREREQVAEREREVEAFBEREREFQBERERFRABQRERERBUARERERUQAUEREREQVAEREREQVAEREREVEAFBEREREFQBERERFRABQRERERBUARERERUQAUEREREQVAEREREVEAFBEREREFQBERERFRABQRERERBUARERERUQAUEREREQVAEREREVEAFBEREVEAFBEREREFQBERERFRABQRERERBUARERERUQAUEREREQVAEREREVEAFBEREREFQBERERFRABQRERERBUARERERUQAUEREREQVAEREREVEAFBEREVEAFBEREREFQBERERFRABQRERERBUARERERUQAUEREREQVAEREREVEAFBEREREFQBERERFRABQRERERBUARERERUQAUEREREQVAEREREVEAFBEREREFQBEREREFQBERERFRABQRERERBUARERERUQAUEREREQVAEREREVEAFBEREREFQBERERFRABQRERERBUARERERUQAUEREREQVAEREREVEAFBEREREFQBERERFRABQRERFRABQRERERBUARERERUQAUEREREQVAEREREVEAFBEREREFQBERERFRABQRERERBUARERERUQAUEREREQVAEREREVEAFBEREREFQBERERFRABQRERFRABQRERERBUARERERUQAUEREREQVAEREREVEAFBEREREFQBERERFRABQRERERBUARERERUQAUEREREQVAEREREVEAFBEREREFQBERERFRABQRERERBUARERERBUARERERUQAUEREREQVAEREREVEAFBEREREFQBERERFRABQRERERBUARERERUQAUEREREQVAEREREVEAFBEREREFQBERERFRABQRERERBUARERERBUARERERUQAUEREREQVAEREREVEAFBEREREFQBERERFRABQRERERBUARERERUQAUEREREQVAEREREVEAFBEREREFQBERERFRABQRERERBUARERERUQAUERERUQAUEREREQVAEREREVEAFBEREREFQBERERFRABQRERERBUARERERUQAUEREREQVAEREREVEAFBEREZEPj623QEQ2ZL7vZ/7Z8zwMwxjz7xOOgk1zzNca/e/AmK8nIrKxMPzRraeIyHoW7HzfX2OQsyzrYwtnruuyelNpmmbmz1dIFBEFQBGRKQa91UPe2oQ7z/MYGBggFosRjUaJx+PEYjEcx3nf1zcMI/NnRCIRIpEIOTk5RCIR8vPzyc3NnfLfw3EchUMRUQAUEZko7E0m6HV2drJy5Ura29tpamqio6ODlStXsmrVKrq7u+nr68sEv2Qyieu6Y36k/6zVA2D6z7Usa8yPUChEKBSioKCAwsJCSkpKqKiooKKigsrKSmbNmsWsWbOorKykqqpqwrDoed6Yv69CoYgoAIrItAl7tj1+yXFXVxfLly9nyZIlvPXWWyxdupTGxkY6OzszM3rJZBLTNLEsC9M0sW2bUCiEbdvYtj3mv8HILNyaZuJWrxscHdTS/58dx8n8WD1YGoZBbm4uOTk5FBQUUFdXx5w5c9h0001ZuHAhtbW1bLLJJpnAN14oXP3/o4iIAqCIbHBG18qNF/iWLl3K66+/zssvv8x7773He++9R1dXF4ODg3ieRygUIhwOk5ubSzgczszKrb45Ix0wRzdhq//7lBvE1cJi+uc1/dmO4+C6Lo7jEI1GM8vNrusSiUQoKiqivr6eTTfdlM0335xtttmGRYsWUVhYuMaw7LquAqGIKACKyPovHVzGC3y9vb28+OKLPPvss7z66qu8++67dHd3E4/HsW2bnJwccnNziUQiY0Le6JnDyezkXWcNaCqsGYYxJiS6rksikSAajTI8PEw8HscwDIqKiqipqWHLLbdk2223ZaeddmLhwoXv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";

//Variables for setting time. By default set to UTC +2.00.
const int UTC = 2;
const long gmtOffset_sec = UTC*60*60;
const int daylightOffset_sec = 3600;       //Takes into account the daylight saving time
const char* ntpServer = "pool.ntp.org";

//Function handling for C++
bool checkPhoto(const char* fileName);
void capturePhotoSaveSD(String photoFileName);
void setupAP(void);
void startServer();
void setupCamera(void);
void initialiseSDCard();
void recordVideo();
void runBufferRepeat();
void captureFrame();
void addToFile();
void closeFile();
void writeIdx1Chunk();
void listDir(fs::FS &fs, const char * dirname, uint8_t levels, char* filenames[], int nFiles);

int countFiles(fs::FS &fs, const char * dirname, uint8_t levels);

String setupWiFi(void);
String getTimeStamp(void);

boolean sendEmail(String WiFi_IP);
boolean startFile();

uint8_t writeLittleEndian(uint32_t value, FILE *file, int32_t offset, relative position);
uint8_t framesInBuffer();

esp_err_t home_get_handler(httpd_req_t *req);
esp_err_t video_gallery_get_handler(httpd_req_t *req);
esp_err_t img_get_handler(httpd_req_t *req);
esp_err_t access_get_handler(httpd_req_t *req);
esp_err_t access_granted_get_handler(httpd_req_t *req);
esp_err_t access_denied_get_handler(httpd_req_t *req);
esp_err_t stream_handler(httpd_req_t *req);
esp_err_t alarm_get_handler(httpd_req_t *req);
esp_err_t motion_get_handler(httpd_req_t *req);

EMailSender emailSend(author_email, author_password);

void setup(){
  pinMode(4, OUTPUT);

  Serial.begin(115200);
  Serial.println("Starting the program...");

  WiFi.mode(WIFI_AP_STA);
  setupAP();
  WiFi_IP = setupWiFi();

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  if (!SPIFFS.begin()){
    Serial.println("SPIFFS failed to initialize!");
  }

  setupCamera();

  startServer();

  initialiseSDCard();
}

void loop(){
  if (takeNewPhoto){
  capturePhotoSaveSD(getTimeStamp());

    while (!emailSent){
      if (WiFi.status() != WL_CONNECTED){
        WiFi_IP = setupWiFi();
      }
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

  if (alarmOn) {
    recordVideo();
  }

  delay(500);

}

void setupAP(void){
  Serial.println("Setting up AP...");

  WiFi.softAP(ap_ssid, ap_password, 1, 1);
  delay(100);

  if (!WiFi.softAPConfig(AP_local_IP, AP_gateway, AP_subnet)) {
    Serial.println("AP configuration failed.");
  }

  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP setup with IP address: ");
  Serial.println(IP);
}

void setupCamera(void){
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
  config.frame_size = FRAMESIZE_SVGA;
  config.jpeg_quality = 25;
  config.fb_count = MAX_FRAMES;

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

String setupWiFi(void){
  WiFi.begin(ssid, password);
  WiFi.setSleep(false);

  while (WiFi.status() != WL_CONNECTED){
    delay(1000);
    Serial.println("Connecting to WiFi...");
  }
  Serial.print("Connected to your WiFi network with IP address: ");
  Serial.println(WiFi.localIP());

  return WiFi.localIP().toString();
}

void initialiseSDCard()
{
  Serial.println("Initialising SD card");
 
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  
  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
  
  esp_vfs_fat_sdmmc_mount_config_t mount_config = 
  {
    .format_if_mount_failed = false,
    .max_files = 2,
  };
  
  sdmmc_card_t *card;

  esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &mount_config, &card);

  if (ret == ESP_OK) 
  {
    Serial.println("SD card ready");
  }
  else  
  { 
    Serial.print("SD card initialisation error ");
    Serial.println(esp_err_to_name(ret));
  }
}

bool checkPhoto(const char* fileName) {
  FILE *f_pic = fopen(fileName, "r");
  fseek(f_pic, 0L, SEEK_END);
  unsigned int pic_sz = ftell(f_pic);
  fclose(f_pic);
  return ( pic_sz > 100 );
}

void capturePhotoSaveSD(String photoFileName){
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

    String filePhoto_str = String("/sdcard/images/" + photoFileName + ".jpg");
    const char* filePhoto = filePhoto_str.c_str();

    FILE *photoFile = fopen(filePhoto, "w");

    if (!photoFile){
      Serial.println("Failed to open file in writing mode!");
    }
    else {
      fwrite(fb->buf, 1, fb->len, photoFile);
      Serial.println("Picture was saved!");
    }

    fclose(photoFile);
    esp_camera_fb_return(fb);
    ok = checkPhoto(filePhoto);
  } while(!ok);
}

void recordVideo() {
  cyclicalFramesCaptured = 0;

  int lastPicTaken = millis();
  int currentMillis;
  while (!motionDetected || cyclicalFramesCaptured < MAX_FRAMES) {
    currentMillis = millis();

    if (currentMillis - lastPicTaken > FRAME_INTERVAL) {
      lastPicTaken = millis();
      runBufferRepeat();
    }
  }

  initInPos = cyclicalFramesCaptured % MAX_FRAMES;
  initOutPos = (cyclicalFramesCaptured + 1) % MAX_FRAMES;

  int t0 = millis();
  fileOpen = startFile(); //Implement this function
  Serial.println("Starting the file...");

  currentMillis = millis();
  while (fileOpen && currentMillis - t0 < RECORDING_TIME) {
    currentMillis = millis();

    if (currentMillis - lastPicTaken > FRAME_INTERVAL) {
      lastPicTaken = millis();
      captureFrame();
      addToFile(); //Implement
    }
  }

  while (framesInBuffer() > 0) { //Implement
    addToFile();
  }

  closeFile(); //Implement
  motionDetected = false;
  alarmOn = false;
}

void runBufferRepeat() {
  frameInPos = cyclicalFramesCaptured % MAX_FRAMES;
  
  if (frameBuffer[frameInPos] != NULL) {
    esp_camera_fb_return(frameBuffer[frameInPos]);
  }

  frameBuffer[frameInPos] = esp_camera_fb_get();
  if (frameBuffer[frameInPos]->buf == NULL)
  {
    Serial.print("Frame capture failed.");
    return;    
  }

  cyclicalFramesCaptured++; 
}

void captureFrame() {
  frameInPos = (initInPos + fileFramesCaptured) % MAX_FRAMES;

  if (frameBuffer[frameInPos] != NULL) {
    esp_camera_fb_return(frameBuffer[frameInPos]);
  }

  frameBuffer[frameInPos] = esp_camera_fb_get();
  if (frameBuffer[frameInPos]->buf == NULL)
  {
    Serial.print("Frame capture failed.");
    return;    
  }

  fileFramesCaptured++;
}

boolean startFile() {
  String timeStamp = getTimeStamp();

  String AVIFilename_str = "/sdcard/videos/" + timeStamp + ".avi";
  const char* AVIFilename = AVIFilename_str.c_str();

  // Reset file statistics.
  fileFramesCaptured  = 0;        
  fileFramesTotalSize = 0;  
  fileFramesWritten   = 0; 
  filePadding         = 0;
  fileStartTime       = millis() - (MAX_FRAMES * FRAME_INTERVAL);


  // Open the AVI file.
  aviFile = fopen(AVIFilename, "w");
  if (aviFile == NULL)  
  {
    Serial.print ("Unable to open AVI file ");
    Serial.println(AVIFilename);
    return false;  
  }  
  else
  {
    Serial.print(AVIFilename);
    Serial.println(" opened.");
  }
  
  // Write the AVI header to the file.
  size_t written = fwrite(aviHeader, 1, AVI_HEADER_SIZE, aviFile);
  if (written != AVI_HEADER_SIZE)
  {
    Serial.println("Unable to write header to AVI file");
    return false;
   }

  // Open the idx1 temporary file.  This is read/write because we read back in after writing.
  idx1File = fopen("/sdcard/idx1.tmp", "w+");
  if (idx1File == NULL)  
  {
    Serial.println ("Unable to open idx1 file for read/write");
    return false;  
  }  

  // Set the flag to indicate we are ready to start recording.  
  return true;
}

void addToFile() {
  // For each frame we write a chunk to the AVI file made up of:
  //  "00dc" - chunk header.  Stream ID (00) & type (dc = compressed video)
  //  The size of the chunk (frame size + padding)
  //  The frame from camera frame buffer
  //  Padding (0x00) to ensure an even number of bytes in the chunk.  
  // 
  // We then update the FOURCC in the frame from JFIF to AVI1  
  //
  // We also write to the temporary idx file.  This keeps track of the offset & size of each frame.
  // This is read back later (before we close the AVI file) to update the idx1 chunk.
  
  size_t bytesWritten;
  

  // Determine the position to read from in the buffer.
  frameOutPos = (fileFramesWritten + initOutPos) % MAX_FRAMES;


  // Calculate if a padding byte is required (frame chunks need to be an even number of bytes).
  uint8_t paddingByte = frameBuffer[frameOutPos]->len & 0x00000001;
  

  // Keep track of the current position in the file relative to the start of the movi section.  This is used to update the idx1 file.
  uint32_t frameOffset = ftell(aviFile) - AVI_HEADER_SIZE;

  
  // Add the chunk header "00dc" to the file.
  bytesWritten = fwrite(buffer00dc, 1, 4, aviFile); 
  if (bytesWritten != 4)
  {
    Serial.println("Unable to write 00dc header to AVI file");
    return;
  }


  // Add the frame size to the file (including padding).
  uint32_t frameSize = frameBuffer[frameOutPos]->len + paddingByte;
  fileFramesTotalSize += frameBuffer[frameOutPos]->len;

  bytesWritten = writeLittleEndian(frameSize, aviFile, 0x00, FROM_CURRENT);
  if (bytesWritten != 4)
  {
    Serial.println("Unable to write frame size to AVI file");
    return;
  }
  

  // Write the frame from the camera.
  bytesWritten = fwrite(frameBuffer[frameOutPos]->buf, 1, frameBuffer[frameOutPos]->len, aviFile);
  if (bytesWritten != frameBuffer[frameOutPos]->len)
  {
    Serial.println("Unable to write frame to AVI file");
    return;
  }

    
  // Release this frame from memory.
  esp_camera_fb_return(frameBuffer[frameOutPos]);   


  // The frame from the camera contains a chunk header of JFIF (bytes 7-10) that we want to replace with AVI1.
  // So we move the write head back to where the frame was just written + 6 bytes. 
  fseek(aviFile, (bytesWritten - 6) * -1, SEEK_END);
  

  // Then overwrite with the new chunk header value of AVI1.
  bytesWritten = fwrite(bufferAVI1, 1, 4, aviFile);
  if (bytesWritten != 4)
  {
    Serial.println("Unable to write AVI1 to AVI file");
    return;
  }

 
  // Move the write head back to the end of the file.
  fseek(aviFile, 0, SEEK_END);

    
  // If required, add the padding to the file.
  if(paddingByte > 0)
  {
    bytesWritten = fwrite(buffer0000, 1, paddingByte, aviFile); 
    if (bytesWritten != paddingByte)
    {
      Serial.println("Unable to write padding to AVI file");
      return;
    }
  }


  // Write the frame offset to the idx1 file for this frame (used later).
  bytesWritten = writeLittleEndian(frameOffset, idx1File, 0x00, FROM_CURRENT);
  if (bytesWritten != 4)
  {
    Serial.println("Unable to write frame offset to idx1 file");
    return;
  } 


  // Write the frame size to the idx1 file for this frame (used later).
  bytesWritten = writeLittleEndian(frameSize - paddingByte, idx1File, 0x00, FROM_CURRENT);
  if (bytesWritten != 4)
  {
    Serial.println("Unable to write frame size to idx1 file");
    return;
  } 

  
  // Increment the frames written count, and keep track of total padding.
  fileFramesWritten++;
  filePadding = filePadding + paddingByte;
}

void closeFile() {
  // Update the flag immediately to prevent any further frames getting written to the buffer.
  fileOpen = false;

  // Calculate how long the AVI file runs for.
  unsigned long fileDuration = (MAX_FRAMES * FRAME_INTERVAL + RECORDING_TIME) / 1000UL;
  Serial.println(fileDuration);

  // Flush any remaining frames from the buffer.  
  while(framesInBuffer() > 0)
  {
    addToFile();
  }
 
  // Update AVI header with total file size. This is the sum of:
  //   AVI header (252 bytes less the first 8 bytes)
  //   fileFramesWritten * 8 (extra chunk bytes for each frame)
  //   fileFramesTotalSize (frames from the camera)
  //   filePadding
  //   idx1 section (8 + 16 * fileFramesWritten)
  writeLittleEndian((AVI_HEADER_SIZE - 8) + fileFramesWritten * 8 + fileFramesTotalSize + filePadding + (8 + 16 * fileFramesWritten), aviFile, 0x04, FROM_START);


  // Update the AVI header with maximum bytes per second.
  uint32_t maxBytes = fileFramesTotalSize / fileDuration;  
  writeLittleEndian(maxBytes, aviFile, 0x24, FROM_START);
  

  // Update AVI header with total number of frames.
  writeLittleEndian(fileFramesWritten, aviFile, 0x30, FROM_START);
  
  
  // Update stream header with total number of frames.
  writeLittleEndian(fileFramesWritten, aviFile, 0x8C, FROM_START);


  // Update movi section with total size of frames.  This is the sum of:
  //   fileFramesWritten * 8 (extra chunk bytes for each frame)
  //   fileFramesTotalSize (frames from the camera)
  //   filePadding
  writeLittleEndian(fileFramesWritten * 8 + fileFramesTotalSize + filePadding, aviFile, 0xF4, FROM_START);


  // Move the write head back to the end of the AVI file.
  fseek(aviFile, 0, SEEK_END);

   
  // Add the idx1 section to the end of the AVI file
  writeIdx1Chunk();
  
  
  fclose(aviFile);
  
  Serial.print("File closed, size: ");
  Serial.println(AVI_HEADER_SIZE + fileFramesWritten * 8 + fileFramesTotalSize + filePadding + (8 + 16 * fileFramesWritten));

}

void writeIdx1Chunk() {
  // The idx1 chunk consists of:
  // +--- 1 per file ----------------------------------------------------------------+ 
  // | fcc         FOURCC 'idx1'                                                     |
  // | cb          DWORD  length not including first 8 bytes                         |
  // | +--- 1 per frame -----------------------------------------------------------+ |
  // | | dwChunkId DWORD  '00dc' StreamID = 00, Type = dc (compressed video frame) | |
  // | | dwFlags   DWORD  '0000'  dwFlags - none set                               | | 
  // | | dwOffset  DWORD   Offset from movi for this frame                         | |
  // | | dwSize    DWORD   Size of this frame                                      | |
  // | +---------------------------------------------------------------------------+ | 
  // +-------------------------------------------------------------------------------+  
  // The offset & size of each frame are read from the idx1.tmp file that we created
  // earlier when adding each frame to the main file.
  // 
  size_t bytesWritten = 0;


  // Write the idx1 header to the file
  bytesWritten = fwrite(bufferidx1, 1, 4, aviFile);
  if (bytesWritten != 4)
  {
    Serial.println("Unable to write idx1 chunk header to AVI file");
    return;
  }


  // Write the chunk size to the file.
  bytesWritten = writeLittleEndian((uint32_t)fileFramesWritten * 16, aviFile, 0x00, FROM_CURRENT);
  if (bytesWritten != 4)
  {
    Serial.println("Unable to write idx1 size to AVI file");
    return;
  }


  // We need to read the idx1 file back in, so move the read head to the start of the idx1 file.
  fseek(idx1File, 0, SEEK_SET);
  
  
  // For each frame, write a sub chunk to the AVI file (offset & size are read from the idx file)
  char readBuffer[8];
  for (uint32_t x = 0; x < fileFramesWritten; x++)
  {
    // Read the offset & size from the idx file.
    bytesWritten = fread(readBuffer, 1, 8, idx1File);
    if (bytesWritten != 8)
    {
      Serial.println("Unable to read from idx file");
      return;
    }
    
    // Write the subchunk header 00dc
    bytesWritten = fwrite(buffer00dc, 1, 4, aviFile);
    if (bytesWritten != 4)
    {
      Serial.println("Unable to write 00dc to AVI file idx");
      return;
    }

    // Write the subchunk flags
    bytesWritten = fwrite(buffer0000, 1, 4, aviFile);
    if (bytesWritten != 4)
    {
      Serial.println("Unable to write flags to AVI file idx");
      return;
    }

    // Write the offset & size
    bytesWritten = fwrite(readBuffer, 1, 8, aviFile);
    if (bytesWritten != 8)
    {
      Serial.println("Unable to write offset & size to AVI file idx");
      return;
    }
  }


  // Close the idx1 file.
  fclose(idx1File);
  
}

uint8_t writeLittleEndian(uint32_t value, FILE *file, int32_t offset, relative position) {
  uint8_t digit[1];
  uint8_t writeCount = 0;

  
  // Set position within file.  Either relative to: SOF, current position, or EOF.
  if (position == FROM_START)          
    fseek(file, offset, SEEK_SET);    // offset >= 0
  else if (position == FROM_CURRENT)
    fseek(file, offset, SEEK_CUR);    // Offset > 0, < 0, or 0
  else if (position == FROM_END)
    fseek(file, offset, SEEK_END);    // offset <= 0 ??
  else
    return 0;  


  // Write the value to the file a byte at a time (LSB first).
  for (uint8_t x = 0; x < 4; x++)
  {
    digit[0] = value % 0x100;
    writeCount = writeCount + fwrite(digit, 1, 1, file);
    value = value >> 8;
  }


  // Return the number of bytes written to the file.
  return writeCount;
}

uint8_t framesInBuffer() {
  return fileFramesCaptured + MAX_FRAMES - 1 - fileFramesWritten;
}

void listDir(fs::FS &fs, const char * dirname, uint8_t levels, char* filenames[], int nFiles){
  int counter = nFiles - 1;

  File root = fs.open(dirname);
  if(!root){
    Serial.println("Failed to open directory");
    return;
  }
  if(!root.isDirectory()){
    Serial.println("Not a directory");
    return;
  }

  File file = root.openNextFile();
  while(file){
    if(file.isDirectory()){
      Serial.print("  DIR : ");
      Serial.println(file.name());
      if(levels){
        listDir(fs, file.name(), levels -1, filenames, nFiles);
      }
    } else {
      Serial.print("  FILE: ");
      Serial.print(file.name());
      filenames[counter] = strdup(file.name());
      Serial.print("  SIZE: ");
      Serial.println(file.size());
      counter -= 1;
    }
    file = root.openNextFile();
  }
}

int countFiles(fs::FS &fs, const char * dirname, uint8_t levels){

  int fileCount = 0;

  File root = fs.open(dirname);
  if(!root){
    Serial.println("Failed to open directory");
    return 0;
  }
  if(!root.isDirectory()){
    Serial.println("Not a directory");
    return 0;
  }

  File file = root.openNextFile();
  while(file){
    if(file.isDirectory()){
    } else {
      fileCount += 1;
    }
    file = root.openNextFile();
  }
  return fileCount;
}

boolean sendEmail(String WiFi_IP){
  //esp_vfs_fat_sdmmc_unmount();

  boolean status;
  String htmlMsg = "Warning! Your ESP32 alarm module has detected movement on the premises. The image of the event is attached to this email.";

  //Email content configuration
  EMailSender::EMailMessage message;

  message.subject = "ALARM! Movement was detected on the premises!";
  message.message = htmlMsg.c_str();
  
  EMailSender::FileDescriptior fileDescriptor[1];
  fileDescriptor[0].filename = "event_recording.avi";
  fileDescriptor[0].mime = "video/x-msvideo";
  fileDescriptor[0].url = "/sdcard/event_recording.avi";
  fileDescriptor[0].encode64 = true;
  fileDescriptor[0].storageType = EMailSender::EMAIL_STORAGE_TYPE_FFAT;

  //EMailSender::Attachments attachs = {1, fileDescriptor};

  EMailSender::Response resp = emailSend.send(recipient_email, message);

  if (!resp.status) {
    Serial.println("Error sending Email, " + resp.desc);
    status = false;
  }
  return resp.status;
}

String getTimeStamp(void) {
  struct tm timeinfo;
  getLocalTime(&timeinfo);

  char timeStamp_char[20];
  sprintf(timeStamp_char, "%04d%02d%02d_%02d%02d%02d", timeinfo.tm_year+1900, timeinfo.tm_mon+1, timeinfo.tm_mday, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  
  String timestamp ="";

  for (int i = 0; i < 15; i++) {
        timestamp = timestamp + timeStamp_char[i];
    }

  return timestamp;
}

esp_err_t home_get_handler(httpd_req_t *req) {
  String response = "<center><p style=\"font-size:40px\"><b>You logged in to the ESP-32 intercom system.</b></p></center> <center><p style=\"font-size:30px\">Please select the right action from below:</p></center> <br><center><form action = \"http://" + WiFi_IP + "/stream\"><input type=\"submit\" value=\"See camera image\" style=\"height:60px; width:350px; font-size:30px\"/> </form></center><br><center><form action = \"http://" + WiFi_IP + "/access_granted\"><input type=\"submit\" value=\"Open the door\" style=\"height:60px; width:350px; font-size:30px\"/> </form></center> <br><center><form action = \"http://" + WiFi_IP + "/access_denied\"><input type=\"submit\" value=\"Don't open the door\" style=\"height:60px; width:350px; font-size:30px\"/> </form></center>";

  const char* resp = response.c_str();

  httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

esp_err_t video_gallery_get_handler(httpd_req_t *req) {
  SD_MMC.begin();
  int nFiles = countFiles(SD_MMC, "/videos", 0);
  char* fileNames [nFiles];
  listDir(SD_MMC, "/videos", 0, fileNames, nFiles);

  const char* resp = "<center><p style=\"font-size:30px\"><b>ESP32-cam video gallery.</b></p></center>";
  httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

  for (char* fileName : fileNames) {
    char* videoName = NULL;
    videoName = strtok(fileName, "/");
    videoName = strtok(NULL, "/");

    resp = "<br><center><a href=\"http://";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    resp = WiFi_IP.c_str();
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    resp = "/video_download?filename=";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    resp = videoName;
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);
    
    resp = "\"><img src = \"data:image/jpeg;base64,";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    httpd_resp_send_chunk(req, video_miniature, HTTPD_RESP_USE_STRLEN);

    resp = "\" width=\"125\" height=\"125\"></a></center>";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    resp = "<center><p style = \"font-size:15px\">";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    httpd_resp_send_chunk(req, videoName, HTTPD_RESP_USE_STRLEN);

    resp = "</p></center>";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);    
  }

  return ESP_OK;
}

esp_err_t img_get_handler(httpd_req_t *req) {
  const char resp[] = "Image request was sent to ESP32-CAM";
  takeNewPhoto = true;
  httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

esp_err_t alarm_get_handler(httpd_req_t *req) {
  const char resp[] = "Alarm mode on";
  alarmOn = true;
  httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

esp_err_t motion_get_handler(httpd_req_t *req) {
  const char resp[] = "OK";
  motionDetected = true;
  httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

esp_err_t access_get_handler(httpd_req_t *req) {
  String accAns = String(accessAnswer);
  const char* resp = accAns.c_str();
  httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
  if (accessAnswer != -1) {
    accessAnswer = -1;
  }
  return ESP_OK;
}

esp_err_t access_granted_get_handler(httpd_req_t *req) {
  const char resp[] = "You have granted the access.";
  accessAnswer = 1;
  httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

esp_err_t access_denied_get_handler(httpd_req_t *req) {
  const char resp[] = "You have denied the access.";
  accessAnswer = 0;
  httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

esp_err_t stream_handler(httpd_req_t *req){
  camera_fb_t * fb = NULL;
  esp_err_t res = ESP_OK;
  size_t _jpg_buf_len = 0;
  uint8_t * _jpg_buf = NULL;
  char * part_buf[64];

  res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
  if(res != ESP_OK){
    return res;
  }

  while(true){
    fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Camera capture failed");
      res = ESP_FAIL;
    } else {
      if(fb->width > 400){
        if(fb->format != PIXFORMAT_JPEG){
          bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
          esp_camera_fb_return(fb);
          fb = NULL;
          if(!jpeg_converted){
            Serial.println("JPEG compression failed");
            res = ESP_FAIL;
          }
        } else {
          _jpg_buf_len = fb->len;
          _jpg_buf = fb->buf;
        }
      }
    }
    if(res == ESP_OK){
      size_t hlen = snprintf((char *)part_buf, 64, _STREAM_PART, _jpg_buf_len);
      res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
    }
    if(res == ESP_OK){
      res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
    }
    if(res == ESP_OK){
      res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
    }
    if(fb){
      esp_camera_fb_return(fb);
      fb = NULL;
      _jpg_buf = NULL;
    } else if(_jpg_buf){
      free(_jpg_buf);
      _jpg_buf = NULL;
    }
    if(res != ESP_OK){
      break;
    }
    //Serial.printf("MJPG: %uB\n",(uint32_t)(_jpg_buf_len));
  }
  return res;
}

esp_err_t get_video_handler(httpd_req_t *req) {
  char filePath[35];
  const char* rootPath = "/sdcard/videos/";
  char fileName[20];
  char query[httpd_req_get_url_query_len(req) + 1];
  
  httpd_req_get_url_query_str(req, query, httpd_req_get_url_query_len(req) + 1);
  httpd_query_key_value(query, "filename", fileName, 20);

  strcpy(filePath, rootPath);
  strcat(filePath, fileName);

  FILE* fd = fopen(filePath, "r");
  Serial.println("Sending the file...");

  httpd_resp_set_type(req, "video/x-msvideo");

  char *chunk = ((struct file_server_data *)req->user_ctx)->scratch;
  size_t chunksize;

  do {
    chunksize = fread(chunk, 1, SCRATCH_BUFSIZE, fd);

    if (chunksize > 0) {
      if (httpd_resp_send_chunk(req, chunk, chunksize) != ESP_OK) {
        fclose(fd);
        Serial.println("File sending failed!");
        /* Abort sending file */
        httpd_resp_sendstr_chunk(req, NULL);
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to send file");
        return ESP_FAIL;
      }
    }
  } while (chunksize != 0);

  fclose(fd);
  Serial.println("File sending complete!");

  #ifdef CONFIG_EXAMPLE_HTTPD_CONN_CLOSE_HEADER
    httpd_resp_set_hdr(req, "Connection", "close");
  #endif
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

httpd_uri_t home_uri = {
  .uri = "/",
  .method = HTTP_GET,
  .handler = home_get_handler,
  .user_ctx = NULL
};

httpd_uri_t video_gallery_uri = {
  .uri = "/video",
  .method = HTTP_GET,
  .handler = video_gallery_get_handler,
  .user_ctx = NULL
};

httpd_uri_t img_uri = {
  .uri = "/img",
  .method = HTTP_GET,
  .handler = img_get_handler,
  .user_ctx = NULL
};

httpd_uri_t alarm_uri = {
  .uri = "/alarm",
  .method = HTTP_GET,
  .handler = alarm_get_handler,
  .user_ctx = NULL
};

httpd_uri_t motion_uri = {
  .uri = "/motion_detected",
  .method = HTTP_GET,
  .handler = motion_get_handler,
  .user_ctx = NULL
};

httpd_uri_t access_uri = {
  .uri = "/access",
  .method = HTTP_GET,
  .handler = access_get_handler,
  .user_ctx = NULL
};

httpd_uri_t access_granted_uri = {
  .uri = "/access_granted",
  .method = HTTP_GET,
  .handler = access_granted_get_handler,
  .user_ctx = NULL
};

httpd_uri_t access_denied_uri = {
  .uri = "/access_denied",
  .method = HTTP_GET,
  .handler = access_denied_get_handler,
  .user_ctx = NULL
};

httpd_uri_t stream_uri = {
  .uri = "/stream",
  .method = HTTP_GET,
  .handler = stream_handler,
  .user_ctx = NULL
};

void startServer(){
  static struct file_server_data *server_data = NULL;

  if (server_data) {
      ESP_LOGE(TAG, "File server already started");
  }

  /* Allocate memory for server data */
  server_data = (file_server_data*)calloc(1, sizeof(struct file_server_data));
  if (!server_data) {
      ESP_LOGE(TAG, "Failed to allocate memory for server data");
  }

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.stack_size = 30000;
  config.max_uri_handlers = 10;
  httpd_handle_t server = NULL;

  httpd_uri_t get_video_uri = {
  .uri = "/video_download",
  .method = HTTP_GET,
  .handler = get_video_handler,
  .user_ctx = server_data
};

  if (httpd_start(&server, &config) == ESP_OK) {
    httpd_register_uri_handler(server, &img_uri);
    httpd_register_uri_handler(server, &access_uri);
    httpd_register_uri_handler(server, &access_granted_uri);
    httpd_register_uri_handler(server, &access_denied_uri);
    httpd_register_uri_handler(server, &home_uri);
    httpd_register_uri_handler(server, &video_gallery_uri);
    httpd_register_uri_handler(server, &stream_uri);
    httpd_register_uri_handler(server, &alarm_uri);
    httpd_register_uri_handler(server, &motion_uri);
    httpd_register_uri_handler(server, &get_video_uri);
  }
}