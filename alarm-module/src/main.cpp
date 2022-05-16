/***
  The video streaming part was inspired by:

  Rui Santos
  Complete project details at https://RandomNerdTutorials.com/esp32-cam-video-streaming-web-server-camera-home-assistant/
  
  Permission is hereby granted, free of charge, to any person obtaining a copy of this part of software and associated documentation files.

  The above copyright notice and this permission notice shall be included in all copies or substantial portions of this part of the software.

  The image taking part was inspired by: https://randomnerdtutorials.com/esp32-cam-take-photo-save-microsd-card/

  The video recording part was adapted from: https://github.com/red-car-nz/Video_Camera/blob/main/Video_Camera.ino

  Delete icon: <a target="_blank" href="https://icons8.com/icon/X4fWgHt6q9So/delete">Delete</a> icon by <a target="_blank" href="https://icons8.com">Icons8</a>
  Download icon: <a target="_blank" href="https://icons8.com/icon/20FjgTazh8FG/download">Download</a> icon by <a target="_blank" href="https://icons8.com">Icons8</a>
  Home icon: <a target="_blank" href="https://icons8.com/icon/73/home">Home</a> icon by <a target="_blank" href="https://icons8.com">Icons8</a>
***/

#include <Arduino.h>
#include <WiFi.h>
#include <SPIFFS.h>
#include <esp_camera.h>
#include <esp_vfs_fat.h>
#include <FS.h>
#include <esp_http_server.h>
#include <EEPROM.h>
#include <SD_MMC.h>
#include <time.h>
#include <ESP32Time.h>
#include <esp_vfs.h>
#include <ArduinoSort.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "driver/sdmmc_host.h"

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
const char* ssid = NULL;
const char* password = NULL;

//Static AP IP settings - ESP32-CAM will be accessible under this address for the devices in its network
IPAddress AP_local_IP(192, 168, 1, 120);
IPAddress AP_gateway(192, 168, 1, 1);
IPAddress AP_subnet(255, 255, 0, 0);

//Variables used in the main loop
boolean motionDetected = false;
boolean alarmOn = false;
String AP_IP;
String lastPicName;

//Variables used by the video recording part
const uint16_t      AVI_HEADER_SIZE = 252;   // Size of the AVI file header.
const long unsigned FRAME_INTERVAL  = 120;   // Time (ms) between frame captures 
const uint8_t       INIT_FRAMES     = 50;    // Number of frames that will be recorded before motion detection
const int           RECORDING_TIME  = 5000;  // Time for which the video will keep recording after motion detection signal was received
char* BUFFER_REPEAT_FILES[INIT_FRAMES];      // Array used for storing filenames of images used in buffer repeat
char* REMAINING_BUFFER_FILES[int(RECORDING_TIME/FRAME_INTERVAL)]; // Array used for storing filenames of all the frames
String videoTimeStamp;

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
  0x00, 0x00, 0x00, 0x00,  // 0x20           dwMicroSecPerFrame     [based on FRAME_INTERVAL] 
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
  0x00, 0x00, 0x00, 0x00,  // 0x84           dwRate (frames per second)         [based on FRAME_INTERVAL]         
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

uint8_t  frameInPos  = 0;                  // Position within BUFFER_REPEAT_FILES where we write to a new filename.

int cyclicalFramesCaptured = 0;            // Number of frames that were captured by runBufferRepeat()
uint16_t fileFramesCaptured  = 0;          // Number of frames captured by camera.
uint16_t fileFramesWritten   = 0;          // Number of frames written to the AVI file.
uint32_t fileFramesTotalSize = 0;          // Total size of frames in file.
uint32_t fileStartTime       = 0;          // Used to calculate FPS. 
uint32_t filePadding         = 0;          // Total padding in the file.  

camera_fb_t *fb = NULL;

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
const char* video_miniature = "iVBORw0KGgoAAAANSUhEUgAAADIAAAAzCAYAAADVY1sUAAAAAXNSR0IArs4c6QAAAARnQU1BAACxjwv8YQUAAAAJcEhZcwAADsMAAA7DAcdvqGQAAAIwSURBVGhD7dhLS1VRGMbxkxJaUISEpBndyEkFCQ3EiTrzMm2gg4ykiSNJnEZ+jJoEgpBQfYFGGU0ciKAWeEGziZVaWYQJXfw/p73guPBIbfY6vsJ64Mc5e4/2c/bZl/XmYmJi9kx58hkqh3ABF/EDmwiS0EVUoA9d+IV3CFYmZDoxiT8YRj2CpCz5DJUjOPz3a+4o3PfME7pIyRKLWEssYi2xiLXEItYSi1hLLGItsUhBrmEAt3FaO/4j1zGImzilHfuZ+1jBAu7hDFxuYAZaWD3BZbg0YgSrmEYrUieLM/ITWptrWXsHvSgss1tUoh9aQZ7E70TqZLFmX8dxnEMNNGxQsUXUoQXVeIMxnIdKtOME5vEQL/AdqZJFEf01lnEMrozOjspUogGuiAYP3eiAK/EAo/gAE7kC/bJr0DWhickrvE+2pzCOz8n2LO5i3y/y3XIV+oV1lnSwW9A15H+fg9kSLjozhWV8KqHbtekSLsXKHKgSLn6ZoCVCjkw/4i10p9KF/xjPoIs/8+gWGTq10K1ZT/+v2hEifhFt64FVBT3QPsFSih6f/9eyPj3/5+Mr2fQ8ZYoen//SWLLpecoUPb64sLKWWMRaYhFr8Z/sGhYMQUOCl9C9Wu9LVtKMHmgl+hQ61tfYs4hWenpypl5HB4gGFWehZ8iOIn7aMAG3frDsES4hH/+MaOpxC03Q4MBqvkDLgufIv1H7RbSt125NDCu0w2i+YQkb+a0Yc8nltgEFbqfQlAUutgAAAABJRU5ErkJggg==";
const char* delete_icon = "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";
const char* home_icon = "iVBORw0KGgoAAAANSUhEUgAAAGQAAABkCAYAAABw4pVUAAAABmJLR0QA/wD/AP+gvaeTAAAESklEQVR4nO2cW4hVVRjHf5PmZOqYecm80N0IbwVJIhF4CbqI5IPUg0QPMk+Cj736GPjki6DgiyBGoEQmiJIiFhVkqZGEReXMkKSpqTilmfawzkHd+9tnzsy+fXuf/w82DJu11/rW95t1ztprr31ACFFvVgBfA181/hYl0QW8D9wEbjeOW8AHwH0lxtWR9AC7uSMienwKTCotug7jOeAUyTKax0/AgpJi7BjWAteIJ/9c44iev9a4RmTMaMJ3gzUSjgGPA7OALxPKbAXGFB51TZkJfIGd6B3A2LvKdgObE8p+AzxWWNQ15RXgLPHk/g2sa3Fd0kfbeeDVHOOtLV3ABuAG8aSeARa1UcfzwM/G9TeBjWhq3DYTgI+wP3b2AQ8Po64eYE9CXZ8AD2UWdU15FviBePLS3PA1byD/M+o9DcxLuG6oaXX0qB1vAX8R7+gF4LUM6l8K/GHUfxV42yjfsUKaU9pbxDv5HfBkhm3NJqx7JU2N77+rbEcKmQp8RvKU9sEc2uwGtiW0eRR4tFGu44S8DPxOvGP/EGZYefMuMGi0fw5YZpyPUishaae0WbGo0WY0Diu2KLUQMh7YhT3kDwOPlBDTZGB/Qky1FvIMcJJ4R5pT2lHlhdZyalxLIauBy8Q7cQlYVWJcUVYRYrKErI6UraSQUYRlCus/7xTh2YY3ngZOYI/kzdyZGldOyBTgAPZ/205gXHmhDckDwHbs2I8A043zrlkM9BMP+jqwvsS4hst6QszRflh9c0svdicGgCUlxjVSXgR+o4I3hu0M86oyBThIhYQ8BRzH/iLcRFivqjqjCX2x1txuE3LggjeBi8QDvAKsKTGuvEiaGl8mPjUulFY3Uz8Cc8sLLXfmAN/j6Ca31XLDx8DEogMqgfHAh9g5OARMKyqQF4BfjCD+JYyYrqICcUIv9mJkH/BS3o23WrJennfjjin8UUI34YmaNTw/B2bk0WjFmEZBD9tmE7b6Ww1pR+C9tNph+S3wRNoGkjYGDALvpa28xrxD2DwRzdufjHDDhvXuRfM4DcxPHXL9yWxL0yRgr1FR8+jJMuqa00NyHvfSxjssC7G3X7pds6kArXLZcg/BSuwprYSkY6h8DhJyH2PAKGzNrsTwaCenfdaFfZFCWwhTWglJRzR/Ywi5jX50xXiDMEp+5d7XwCQkHUn5W0vI9QDwevNkO+tOUQmdtlaVlmHlTy+tOENCnCEhzpAQZ0iIMyTEGRLiDAlxhoQ4w+OOwqKXZ1ytPGiEOENCnCEhzvD4HRIl6894148QNEKcISHOkBBnSIgzJMQZEuIMCXGGhDhDQpwhIc6QEGdIiDMkxBkS4gwJcYaEOENCnCEhzpAQZ0iIMyTEGRLiDAlxhoQ4Q0KcISHOkBBnSIgzJMQZEuIMCXGGhDhDQpwxkt/LEunQ72VViXaEDOQeRefQP1SBdoT0IilZ0E/IpRBCCCGEKJz/AWeKZltjdKpGAAAAAElFTkSuQmCC";

//Variables for setting time. By default set to UTC +2.00.
const int UTC = 2;
const long gmtOffset_sec = UTC*60*60;
const int daylightOffset_sec = 3600;       //Takes into account the daylight saving time
const char* ntpServer = "pool.ntp.org";
bool timeSet = false;
ESP32Time rtc;

//Function handling for C++
bool checkPhoto(const char* fileName);
void capturePhotoSaveSD(String photoFileName, String savePath);
void setupWiFi(void);
void startServer();
void setupCamera(void);
void initialiseSDCard();
void recordVideo();
void runBufferRepeat();
void captureFrame();
void addToFile(char* frameName);
void closeFile();
void writeIdx1Chunk();
void listDir(fs::FS &fs, const char * dirname, uint8_t levels, char* filenames[], int nFiles);

int countFiles(fs::FS &fs, const char * dirname, uint8_t levels);

String setupAP(void);
String getTimeStamp(void);

boolean startFile();

uint8_t writeLittleEndian(uint32_t value, FILE *file, int32_t offset, relative position);
uint8_t framesInBuffer();

esp_err_t home_get_handler(httpd_req_t *req);
esp_err_t video_gallery_get_handler(httpd_req_t *req);
esp_err_t img_get_handler(httpd_req_t *req);
esp_err_t access_get_handler(httpd_req_t *req);
esp_err_t access_granted_get_handler(httpd_req_t *req);
esp_err_t stream_handler(httpd_req_t *req);
esp_err_t alarm_get_handler(httpd_req_t *req);
esp_err_t motion_get_handler(httpd_req_t *req);
esp_err_t image_display(httpd_req_t *req);
esp_err_t video_delete_ask_handler(httpd_req_t *req);
esp_err_t photo_gallery_get_handler(httpd_req_t *req);
esp_err_t photo_delete_ask_handler(httpd_req_t *req);
esp_err_t get_photo_handler(httpd_req_t *req);
esp_err_t delete_photo_handler(httpd_req_t *req);
esp_err_t configure_wifi_handler(httpd_req_t *req);

void setup(){

  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.begin(115200);
  Serial.println("Starting the program...");

  WiFi.mode(WIFI_AP_STA);
  AP_IP = setupAP();
  delay(500);

  startServer();
  delay(500);

  setupCamera();
  delay(500);

  initialiseSDCard();

  while (!timeSet) {
    delay(1000);
  }
}

void loop(){

  if (WiFi.status() != WL_CONNECTED && password != NULL && ssid != NULL){
    setupWiFi();
  }

  if (alarmOn) {
    recordVideo();
  }

  delay(500);

}

String setupAP(void){
  Serial.println("Setting up AP...");

  WiFi.softAP(ap_ssid, ap_password);
  delay(100);

  if (!WiFi.softAPConfig(AP_local_IP, AP_gateway, AP_subnet)) {
    Serial.println("AP configuration failed.");
  }

  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP setup with IP address: ");
  Serial.println(IP);
  return IP.toString();
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
  config.fb_count = 1;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    esp_camera_deinit();
    setupCamera();
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

void setupWiFi(void){
  WiFi.begin(ssid, password);
  WiFi.setSleep(false);

  while (WiFi.status() != WL_CONNECTED){
    delay(1000);
    Serial.println("Connecting to WiFi...");
  }
  Serial.print("Connected to your WiFi network with IP address: ");
  Serial.println(WiFi.localIP());
}

void initialiseSDCard()
{
  Serial.println("Initialising SD card");
 
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  
  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
  //slot_config.width = 1;
  
  esp_vfs_fat_sdmmc_mount_config_t mount_config = 
  {
    .format_if_mount_failed = false,
    .max_files = 3,
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

void capturePhotoSaveSD(String photoFileName, String savePath){
  bool ok = 0;
  lastPicName = photoFileName + ".jpg";

  do {

    fb = esp_camera_fb_get();

    if (!fb){
      Serial.println("Camera capture failed!");
      return;
    }

    String filePhoto_str = String(savePath + photoFileName + ".jpg");
    const char* filePhoto = filePhoto_str.c_str();

    FILE *photoFile = fopen(filePhoto, "w");

    if (!photoFile){
      Serial.println("Failed to open file in writing mode!");
    }
    else {
      fwrite(fb->buf, 1, fb->len, photoFile);
      //Serial.println("Picture was saved!");
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
  while (!motionDetected || cyclicalFramesCaptured < INIT_FRAMES) {
    if (!alarmOn) {
      return;
    }
    currentMillis = millis();

    if (currentMillis - lastPicTaken > FRAME_INTERVAL) {
      lastPicTaken = millis();
      runBufferRepeat();
    }
  }

  int t0 = millis();
  fileOpen = startFile();
  Serial.println("Starting the file...");
  capturePhotoSaveSD(videoTimeStamp, "/sdcard/images/");

  currentMillis = millis();
  while (fileOpen && currentMillis - t0 < RECORDING_TIME) {
    currentMillis = millis();

    if (currentMillis - lastPicTaken > FRAME_INTERVAL) {
      lastPicTaken = millis();
      captureFrame();
    }
  }

  sortArray(BUFFER_REPEAT_FILES, INIT_FRAMES);

  for (char* fileName : BUFFER_REPEAT_FILES) {
    addToFile(fileName);
  }

  for (int i = 0; i < fileFramesCaptured; i++) {
    addToFile(REMAINING_BUFFER_FILES[i]);
  }

  closeFile();
  motionDetected = false;
}

void runBufferRepeat() {
  frameInPos = cyclicalFramesCaptured % INIT_FRAMES;

  char formattedFrameNumber[6];
  sprintf(formattedFrameNumber, "%06d", cyclicalFramesCaptured);

  char savePath[24];

  strcpy(savePath, "/sdcard/temp/");
  strcat(savePath, formattedFrameNumber);
  strcat(savePath, ".jpg");
  
  capturePhotoSaveSD(String(formattedFrameNumber), "/sdcard/temp/");

  if (cyclicalFramesCaptured + 1 > INIT_FRAMES) {
    unlink(BUFFER_REPEAT_FILES[frameInPos]);
  }

  BUFFER_REPEAT_FILES[frameInPos] = strdup(savePath);

  cyclicalFramesCaptured++; 
}

void captureFrame() {
  frameInPos = fileFramesCaptured;

  char frameNumber[7];
  sprintf(frameNumber, "%06d", cyclicalFramesCaptured + fileFramesCaptured);

  char savePath[24];

  strcpy(savePath, "/sdcard/temp/");
  strcat(savePath, frameNumber);
  strcat(savePath, ".jpg");

  capturePhotoSaveSD(String(frameNumber), "/sdcard/temp/");

  REMAINING_BUFFER_FILES[frameInPos] = strdup(savePath);

  fileFramesCaptured++;
}

boolean startFile() {
  videoTimeStamp = getTimeStamp();

  String AVIFilename_str = "/sdcard/videos/" + videoTimeStamp + ".avi";
  const char* AVIFilename = AVIFilename_str.c_str();

  // Reset file statistics.
  fileFramesCaptured  = 0;        
  fileFramesTotalSize = 0;  
  fileFramesWritten   = 0; 
  filePadding         = 0;
  fileStartTime       = millis() - (INIT_FRAMES * FRAME_INTERVAL);


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

void addToFile(char* frameName) {
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

  FILE *frameFile = fopen(frameName, "r");

  fseek(frameFile, 0L, SEEK_END);
  uint32_t rawFrameSize = ftell(frameFile);
  rewind(frameFile);
  
  uint8_t* frame = (uint8_t*) malloc (sizeof(uint8_t)*rawFrameSize);
  
  fread (frame, 1, rawFrameSize, frameFile);
  fclose(frameFile);

  size_t bytesWritten;

  // Calculate if a padding byte is required (frame chunks need to be an even number of bytes).
  uint8_t paddingByte = rawFrameSize & 0x00000001;
  

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
  uint32_t frameSize = rawFrameSize + paddingByte;
  fileFramesTotalSize += rawFrameSize;

  bytesWritten = writeLittleEndian(frameSize, aviFile, 0x00, FROM_CURRENT);
  if (bytesWritten != 4)
  {
    Serial.println("Unable to write frame size to AVI file");
    return;
  }
  

  // Write the frame from the camera.
  bytesWritten = fwrite(frame, 1, rawFrameSize, aviFile);
  if (bytesWritten != rawFrameSize)
  {
    Serial.println("Unable to write frame to AVI file");
    return;
  }

  free(frame);
  unlink(frameName);

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
  unsigned long fileDuration = (INIT_FRAMES * FRAME_INTERVAL + RECORDING_TIME) / 1000UL;
 
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

  // Update the AVI header with microseconds per frame.
  writeLittleEndian(FRAME_INTERVAL * 1000, aviFile, 0x20, FROM_START);


  //Update the AVI header with FPS.
  writeLittleEndian(1000/FRAME_INTERVAL, aviFile, 0x84, FROM_START);


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
  return fileFramesCaptured + INIT_FRAMES - 1 - fileFramesWritten;
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
  sortArrayReverse(filenames, nFiles);
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
  httpd_resp_set_type(req, "text/html");

  const char* resp = "<center><p style=\"font-size:40px\"><b>You logged in to the ESP32 camera system.</b></p></center>";
  httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

  if (timeSet) {
    resp = "<center><p style=\"font-size:30px\">Please select the right action from below:</p></center>";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    //Inserting see camera image button
    resp = "<br><center><form action = \"http://";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    resp = AP_IP.c_str();
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    resp = "/stream\"><input type=\"submit\" value=\"See camera image\" style=\"height:60px; width:350px; font-size:30px\"/> </form></center>";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);


    //Inserting the save motion mode button
    resp = "<br><center><form method=\"GET\" action = \"http://";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    resp = AP_IP.c_str();
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    if (!alarmOn) {
      resp = "/alarm\"><input type=\"hidden\" name=\"on\" value=\"1\"><input type=\"submit\" value=\"Turn the save motion on\" style=\"height:60px; width:350px; font-size:30px\"/> </form></center>";
      httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);
    }

    else {
      resp = "/alarm\"><input type=\"hidden\" name=\"on\" value=\"0\"><input type=\"submit\" value=\"Turn the save motion off\" style=\"height:60px; width:350px; font-size:30px\"/> </form></center>";
      httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);


      //Inserting the trigger video save button
      resp = "<br><center><form method=\"GET\" action = \"http://";
      httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

      resp = AP_IP.c_str();
      httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

      resp = "/motion_detected\"><input type=\"submit\" value=\"Trigger video save\" style=\"height:60px; width:350px; font-size:30px\"/> </form></center>";
      httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);
    }


    //Inserting video gallery button
    resp = "<br><center><form action = \"http://";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    resp = AP_IP.c_str();
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    resp = "/video\"><input type=\"submit\" value=\"Video gallery\" style=\"height:60px; width:350px; font-size:30px\"/> </form></center>";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);


    //Inserting photo gallery button
    resp = "<br><center><form action = \"http://";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    resp = AP_IP.c_str();
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    resp = "/photos\"><input type=\"submit\" value=\"Photo gallery\" style=\"height:60px; width:350px; font-size:30px\"/> </form></center>";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);


    //Inserting take an image button
    resp = "<br><center><form action = \"http://";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    resp = AP_IP.c_str();
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    resp = "/img\"><input type=\"submit\" value=\"Take an image\" style=\"height:60px; width:350px; font-size:30px\"/> </form></center>";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);
  }

  else {
    resp = "<center><p style=\"font-size:25px\">Connect to WiFi or set the time manually below to use the camera. Do not put 0's before single digits!</p></center>";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    //Inserting time fields
    resp = "<br><center><form action = \"http://";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    resp = AP_IP.c_str();
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    //Year field
    resp = "/set_time\"><label for=\"yy\">Year: &nbsp&nbsp&nbsp</label><input type=\"text\" id=\"yy\" name=\"yy\"><br><br>";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    //Month field
    resp = "<label for=\"mm\">Month: &nbsp</label><input type=\"text\" id=\"mm\" name=\"mm\"><br><br>";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    //Day field
    resp = "<label for=\"dd\">Day: &nbsp&nbsp&nbsp&nbsp</label><input type=\"text\" id=\"dd\" name=\"dd\"><br><br>";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    //Hour field
    resp = "<label for=\"hh\">Hour: &nbsp&nbsp&nbsp</label><input type=\"text\" id=\"hh\" name=\"hh\"><br><br>";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    //Minute field
    resp = "<label for=\"min\">Minute: &nbsp</label><input type=\"text\" id=\"min\" name=\"min\"><br><br>";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    //Seconds field
    resp = "<label for=\"ss\">Seconds: </label><input type=\"text\" id=\"ss\" name=\"ss\"><br><br>";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    //Submit button
    resp = "<input type=\"submit\" value=\"Submit\"></form>";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);
  }

  httpd_resp_send_chunk(req, NULL, 0);

  return ESP_OK;
}

esp_err_t video_gallery_get_handler(httpd_req_t *req) {
  SD_MMC.begin();
  int nFiles = countFiles(SD_MMC, "/videos", 0);
  char* fileNames [nFiles];
  listDir(SD_MMC, "/videos", 0, fileNames, nFiles);

  //Inserting home button
  const char* resp = "<a href=\"http://";
  httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

  resp = AP_IP.c_str();
  httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

  resp = "/\"><img src = \"data:image/jpeg;base64,";
  httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

  httpd_resp_send_chunk(req, home_icon, HTTPD_RESP_USE_STRLEN);

  resp = "\" width=\"55\" height=\"55\"></a>";
  httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);
  
  resp = "<center><p style=\"font-size:35px\"><b>ESP32-cam video gallery</b></p></center>";
  httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

  for (char* fileName : fileNames) {
    char* videoName = NULL;
    videoName = strtok(fileName, "/");
    videoName = strtok(NULL, "/");

    char* miniatureName = (char*)malloc((strlen(videoName) + 1)*sizeof(char));
    strcpy(miniatureName, videoName);
    miniatureName = strtok(miniatureName, ".");
    strcat(miniatureName, ".jpg");

    //Inserting video miniature
    resp = "<br><center><img src = \"http://";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    resp = AP_IP.c_str();
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    resp = "/image?filename=";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    resp = miniatureName;
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    free(miniatureName);

    resp = "\"width=\"500\" height=\"375\"></a>";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);


    //Inserting video name
    resp = "<center><p style = \"font-size:17px\"><b>";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    httpd_resp_send_chunk(req, videoName, HTTPD_RESP_USE_STRLEN);

    resp = "</b></p></center>";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);


    //Inserting download button
    resp = "<br><center><a href=\"http://";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    resp = AP_IP.c_str();
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    resp = "/video_download?filename=";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    resp = videoName;
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);
    
    resp = "\"><img src = \"data:image/jpeg;base64,";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    httpd_resp_send_chunk(req, video_miniature, HTTPD_RESP_USE_STRLEN);

    resp = "\" width=\"55\" height=\"55\"></a>";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);


    //Gap between the icons
    resp = "&nbsp &nbsp &nbsp &nbsp &nbsp &nbsp &nbsp &nbsp &nbsp &nbsp &nbsp &nbsp &nbsp";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);


    //Inserting delete button
    resp = "<a href=\"http://";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    resp = AP_IP.c_str();
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    resp = "/video_delete_ask?filename=";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    resp = videoName;
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);
    
    resp = "\"><img src = \"data:image/jpeg;base64,";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    httpd_resp_send_chunk(req, delete_icon, HTTPD_RESP_USE_STRLEN);

    resp = "\" width=\"60\" height=\"60\"></a></center>";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN); 


    //Inserting buttons' description  
    resp = "<center><p style = \"font-size:12px\"><b>Download video";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    resp = "&nbsp &nbsp &nbsp &nbsp &nbsp &nbsp &nbsp &nbsp &nbsp &nbsp &nbsp &nbsp &nbsp &nbsp";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    resp = "Delete video</b></p></center><br><br><br><br><br>";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);
  }

  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

esp_err_t img_get_handler(httpd_req_t *req) {
  capturePhotoSaveSD(getTimeStamp(), "/sdcard/images/");

  httpd_resp_set_status(req, "303 See Other");
  httpd_resp_set_hdr(req, "Location", "/photos");

  delay(200);

  httpd_resp_sendstr(req, lastPicName.c_str());
  Serial.println(lastPicName);

  return ESP_OK;
}

esp_err_t alarm_get_handler(httpd_req_t *req) {
  char on[2];

  char query[httpd_req_get_url_query_len(req) + 1];

  httpd_req_get_url_query_str(req, query, httpd_req_get_url_query_len(req) + 1);
  httpd_query_key_value(query, "on", on, 2);

  if (on[0] == '1') {
    alarmOn = true;
  }

  else {
    alarmOn = false;
  }

  httpd_resp_set_status(req, "303 See Other");
  httpd_resp_set_hdr(req, "Location", "/");

  httpd_resp_sendstr(req, "Motion recording is on");

  return ESP_OK;
}

esp_err_t motion_get_handler(httpd_req_t *req) {
  motionDetected = true;
  httpd_resp_set_status(req, "303 See Other");
  httpd_resp_set_hdr(req, "Location", "/");

  httpd_resp_sendstr(req, "Recording triggered");
  return ESP_OK;
}

esp_err_t stream_handler(httpd_req_t *req) {
  esp_err_t res = ESP_OK;
  size_t _jpg_buf_len = 0;
  uint8_t * _jpg_buf = NULL;
  char * part_buf[64];

  if (fb != NULL) {
    esp_camera_fb_return(fb);
  }

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

esp_err_t delete_video_handler(httpd_req_t *req) {
  char filePath[35];
  const char* rootPath = "/sdcard/videos/";
  char fileName[20];
  char query[httpd_req_get_url_query_len(req) + 1];
  
  httpd_req_get_url_query_str(req, query, httpd_req_get_url_query_len(req) + 1);
  httpd_query_key_value(query, "filename", fileName, 20);

  strcpy(filePath, rootPath);
  strcat(filePath, fileName);

  struct stat file_stat;

  if (stat(filePath, &file_stat) == -1) {
        Serial.println("File does not exist");
        /* Respond with 400 Bad Request */
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File does not exist");
        return ESP_FAIL;
  }

  Serial.println("Deleting file");
  /* Delete file */
   unlink(filePath);

  /* Redirect onto root to see the updated file list */
  httpd_resp_set_status(req, "303 See Other");
  httpd_resp_set_hdr(req, "Location", "/video");
#ifdef CONFIG_EXAMPLE_HTTPD_CONN_CLOSE_HEADER
    httpd_resp_set_hdr(req, "Connection", "close");
#endif
  httpd_resp_sendstr(req, "File deleted successfully");
  return ESP_OK;
}

esp_err_t image_display(httpd_req_t *req) {
  httpd_resp_set_type(req, "image/jpeg");

  char filePath[35];
  const char* rootPath = "/sdcard/images/";
  char fileName[20];
  char query[httpd_req_get_url_query_len(req) + 1];
  
  httpd_req_get_url_query_str(req, query, httpd_req_get_url_query_len(req) + 1);
  httpd_query_key_value(query, "filename", fileName, 20);

  strcpy(filePath, rootPath);
  strcat(filePath, fileName);

  struct stat file_stat;

  if (stat(filePath, &file_stat) == -1) {
        Serial.println("File does not exist");
        /* Respond with 400 Bad Request */
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File does not exist");
        return ESP_FAIL;
  }

  FILE* fd = fopen(filePath, "r");

  fseek(fd, 0L, SEEK_END);
  ssize_t photoSize = ftell(fd);
  rewind(fd);
  
  char* photo = (char*) malloc (sizeof(char)*photoSize);

  if (photo == NULL) {
    Serial.println("Error loading file");
        /* Respond with 400 Bad Request */
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Error loading file");
        fclose(fd);
        return ESP_FAIL;
  }
  
  fread (photo, 1, photoSize, fd);
  fclose(fd);

  httpd_resp_send(req, photo, photoSize);
  free(photo);

  return ESP_OK;
}

esp_err_t video_delete_ask_handler(httpd_req_t *req) {
  char fileName[20];
  char query[httpd_req_get_url_query_len(req) + 1];

  httpd_req_get_url_query_str(req, query, httpd_req_get_url_query_len(req) + 1);
  httpd_query_key_value(query, "filename", fileName, 20);

  httpd_resp_set_type(req, "text/html");

  const char* resp = "<center><p style=\"font-size:30px\">Are you sure that you want to delete the file?</p></center>";
  httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

  //Inserting YES button
  resp = "<br><center><form method = \"GET\" action = \"http://";
  httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

  resp = AP_IP.c_str();
  httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

  resp = "/video_delete\">";
  httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

  resp = "<input type = \"hidden\" name = \"filename\" value = \"";
  httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

  resp = fileName;
  httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);
  
  resp = "\"/><input type=\"submit\" value=\"YES\" style=\"height:60px; width:150px; font-size:20px\"/> </form>";
  httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

  resp = "&nbsp &nbsp &nbsp";


  //Inserting NO button
  resp = "<form action = \"http://";
  httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

  resp = AP_IP.c_str();
  httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

  resp = "/video\"><input type=\"submit\" value=\"NO\" style=\"height:60px; width:150px; font-size:20px\"/> </form></center>";
  httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

esp_err_t photo_gallery_get_handler(httpd_req_t *req) {
  SD_MMC.begin();
  int nFiles = countFiles(SD_MMC, "/images", 0);
  char* fileNames [nFiles];
  listDir(SD_MMC, "/images", 0, fileNames, nFiles);

  //Inserting home button
  const char* resp = "<a href=\"http://";
  httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

  resp = AP_IP.c_str();
  httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

  resp = "/\"><img src = \"data:image/jpeg;base64,";
  httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

  httpd_resp_send_chunk(req, home_icon, HTTPD_RESP_USE_STRLEN);

  resp = "\" width=\"55\" height=\"55\"></a>";
  httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);


  resp = "<center><p style=\"font-size:35px\"><b>ESP32-cam image gallery</b></p></center>";
  httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

  for (char* fileName : fileNames) {
    char* photoName = NULL;
    photoName = strtok(fileName, "/");
    photoName = strtok(NULL, "/");

    //Inserting image miniature
    resp = "<br><center><img src = \"http://";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    resp = AP_IP.c_str();
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    resp = "/image?filename=";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    resp = photoName;
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    resp = "\"width=\"500\" height=\"375\"></a>";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);


    //Inserting photo name
    resp = "<center><p style = \"font-size:17px\"><b>";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    httpd_resp_send_chunk(req, photoName, HTTPD_RESP_USE_STRLEN);

    resp = "</b></p></center>";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);


    //Inserting download button
    resp = "<br><center><a href=\"http://";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    resp = AP_IP.c_str();
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    resp = "/photo_download?filename=";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    resp = photoName;
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);
    
    resp = "\"><img src = \"data:image/jpeg;base64,";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    httpd_resp_send_chunk(req, video_miniature, HTTPD_RESP_USE_STRLEN);

    resp = "\" width=\"55\" height=\"55\"></a>";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);


    //Gap between the icons
    resp = "&nbsp &nbsp &nbsp &nbsp &nbsp &nbsp &nbsp &nbsp &nbsp &nbsp &nbsp &nbsp &nbsp";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);


    //Inserting delete button
    resp = "<a href=\"http://";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    resp = AP_IP.c_str();
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    resp = "/photo_delete_ask?filename=";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    resp = photoName;
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);
    
    resp = "\"><img src = \"data:image/jpeg;base64,";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    httpd_resp_send_chunk(req, delete_icon, HTTPD_RESP_USE_STRLEN);

    resp = "\" width=\"60\" height=\"60\"></a></center>";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN); 


    //Inserting buttons' description  
    resp = "<center><p style = \"font-size:12px\"><b>Download photo";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    resp = "&nbsp &nbsp &nbsp &nbsp &nbsp &nbsp &nbsp &nbsp &nbsp &nbsp &nbsp &nbsp &nbsp &nbsp";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    resp = "Delete photo</b></p></center><br><br><br><br><br>";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);
  }

  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

esp_err_t photo_delete_ask_handler(httpd_req_t *req) {
  char fileName[20];
  char query[httpd_req_get_url_query_len(req) + 1];

  httpd_req_get_url_query_str(req, query, httpd_req_get_url_query_len(req) + 1);
  httpd_query_key_value(query, "filename", fileName, 20);

  httpd_resp_set_type(req, "text/html");

  const char* resp = "<center><p style=\"font-size:30px\">Are you sure that you want to delete the file?</p></center>";
  httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

  //Inserting YES button
  resp = "<br><center><form method = \"GET\" action = \"http://";
  httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

  resp = AP_IP.c_str();
  httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

  resp = "/photo_delete\">";
  httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

  resp = "<input type = \"hidden\" name = \"filename\" value = \"";
  httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

  resp = fileName;
  httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);
  
  resp = "\"/><input type=\"submit\" value=\"YES\" style=\"height:60px; width:150px; font-size:20px\"/> </form>";
  httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

  resp = "&nbsp &nbsp &nbsp";


  //Inserting NO button
  resp = "<form action = \"http://";
  httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

  resp = AP_IP.c_str();
  httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

  resp = "/photos\"><input type=\"submit\" value=\"NO\" style=\"height:60px; width:150px; font-size:20px\"/> </form></center>";
  httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

esp_err_t get_photo_handler(httpd_req_t *req) {
  char filePath[35];
  const char* rootPath = "/sdcard/images/";
  char fileName[20];
  char query[httpd_req_get_url_query_len(req) + 1];
  
  httpd_req_get_url_query_str(req, query, httpd_req_get_url_query_len(req) + 1);
  httpd_query_key_value(query, "filename", fileName, 20);

  strcpy(filePath, rootPath);
  strcat(filePath, fileName);

  FILE* fd = fopen(filePath, "r");
  Serial.println("Sending the file...");

  httpd_resp_set_type(req, "image/jpeg");

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

esp_err_t delete_photo_handler(httpd_req_t *req) {
  char filePath[35];
  const char* rootPath = "/sdcard/images/";
  char fileName[20];
  char query[httpd_req_get_url_query_len(req) + 1];
  
  httpd_req_get_url_query_str(req, query, httpd_req_get_url_query_len(req) + 1);
  httpd_query_key_value(query, "filename", fileName, 20);

  strcpy(filePath, rootPath);
  strcat(filePath, fileName);

  struct stat file_stat;

  if (stat(filePath, &file_stat) == -1) {
        Serial.println("File does not exist");
        /* Respond with 400 Bad Request */
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File does not exist");
        return ESP_FAIL;
  }

  Serial.println("Deleting file");
  /* Delete file */
   unlink(filePath);

  /* Redirect onto root to see the updated file list */
  httpd_resp_set_status(req, "303 See Other");
  httpd_resp_set_hdr(req, "Location", "/photos");
#ifdef CONFIG_EXAMPLE_HTTPD_CONN_CLOSE_HEADER
    httpd_resp_set_hdr(req, "Connection", "close");
#endif
  httpd_resp_sendstr(req, "File deleted successfully");
  return ESP_OK;
}

esp_err_t configure_wifi_handler(httpd_req_t *req) {
  char ssid_value[40];
  char password_value[40];

  char query[httpd_req_get_url_query_len(req) + 1];

  httpd_req_get_url_query_str(req, query, httpd_req_get_url_query_len(req) + 1);
  httpd_query_key_value(query, "ssid", ssid_value, 40);
  httpd_query_key_value(query, "password", password_value, 40);

  ssid = ssid_value;
  password = password_value;

  if (WiFi.status() == WL_CONNECTED) {
    WiFi.disconnect();
    delay(500);
  }

  setupWiFi();

  if (!timeSet) {
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    timeSet = true;
  }

  httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);

  return ESP_OK;
}

esp_err_t configure_time_handler(httpd_req_t *req) {
  char year_char[5];
  char month_char[3];
  char day_char[3];
  char hour_char[3];
  char minute_char[3];
  char second_char[3];

  int year;
  int month;
  int day;
  int hour;
  int minute;
  int second;

  char query[httpd_req_get_url_query_len(req) + 1];

  httpd_req_get_url_query_str(req, query, httpd_req_get_url_query_len(req) + 1);
  httpd_query_key_value(query, "yy", year_char, 5);
  httpd_query_key_value(query, "mm", month_char, 3);
  httpd_query_key_value(query, "dd", day_char, 3);
  httpd_query_key_value(query, "hh", hour_char, 3);
  httpd_query_key_value(query, "min", minute_char, 3);
  httpd_query_key_value(query, "ss", second_char, 3);

  sscanf(year_char, "%d", &year);
  sscanf(month_char, "%d", &month);
  sscanf(day_char, "%d", &day);
  sscanf(hour_char, "%d", &hour);
  sscanf(minute_char, "%d", &minute);
  sscanf(second_char, "%d", &second);

  rtc.setTime(second, minute, hour, day, month, year);
  timeSet = true;

  httpd_resp_set_status(req, "303 See Other");
  httpd_resp_set_hdr(req, "Location", "/");

  httpd_resp_sendstr(req, "Time was set");

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

httpd_uri_t stream_uri = {
  .uri = "/stream",
  .method = HTTP_GET,
  .handler = stream_handler,
  .user_ctx = NULL
};

httpd_uri_t delete_video_uri = {
  .uri = "/video_delete",
  .method = HTTP_GET,
  .handler = delete_video_handler,
  .user_ctx = NULL
};

httpd_uri_t image_display_uri = {
  .uri = "/image",
  .method = HTTP_GET,
  .handler = image_display,
  .user_ctx = NULL
};

httpd_uri_t video_delete_ask_uri = {
  .uri = "/video_delete_ask",
  .method = HTTP_GET,
  .handler = video_delete_ask_handler,
  .user_ctx = NULL
};

httpd_uri_t photo_gallery_get_uri = {
  .uri = "/photos",
  .method = HTTP_GET,
  .handler = photo_gallery_get_handler,
  .user_ctx = NULL
};

httpd_uri_t photo_delete_ask_uri = {
  .uri = "/photo_delete_ask",
  .method = HTTP_GET,
  .handler = photo_delete_ask_handler,
  .user_ctx = NULL
};

httpd_uri_t delete_photo_uri = {
  .uri = "/photo_delete",
  .method = HTTP_GET,
  .handler = delete_photo_handler,
  .user_ctx = NULL
};

httpd_uri_t configure_wifi_uri = {
  .uri = "/wifi_config",
  .method = HTTP_GET,
  .handler = configure_wifi_handler,
  .user_ctx = NULL
};

httpd_uri_t configure_time_uri = {
  .uri = "/set_time",
  .method = HTTP_GET,
  .handler = configure_time_handler,
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
  config.max_uri_handlers = 16;
  httpd_handle_t server = NULL;

  httpd_uri_t get_video_uri = {
  .uri = "/video_download",
  .method = HTTP_GET,
  .handler = get_video_handler,
  .user_ctx = server_data
};

httpd_uri_t get_photo_uri = {
  .uri = "/photo_download",
  .method = HTTP_GET,
  .handler = get_photo_handler,
  .user_ctx = server_data
};

  if (httpd_start(&server, &config) == ESP_OK) {
    httpd_register_uri_handler(server, &img_uri);
    httpd_register_uri_handler(server, &home_uri);
    httpd_register_uri_handler(server, &video_gallery_uri);
    httpd_register_uri_handler(server, &stream_uri);
    httpd_register_uri_handler(server, &alarm_uri);
    httpd_register_uri_handler(server, &motion_uri);
    httpd_register_uri_handler(server, &get_video_uri);
    httpd_register_uri_handler(server, &delete_video_uri);
    httpd_register_uri_handler(server, &image_display_uri);
    httpd_register_uri_handler(server, &video_delete_ask_uri);
    httpd_register_uri_handler(server, &photo_gallery_get_uri);
    httpd_register_uri_handler(server, &photo_delete_ask_uri);
    httpd_register_uri_handler(server, &get_photo_uri);
    httpd_register_uri_handler(server, &delete_photo_uri);
    httpd_register_uri_handler(server, &configure_wifi_uri);
    httpd_register_uri_handler(server, &configure_time_uri);
  }
}