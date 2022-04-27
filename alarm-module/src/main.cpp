/***
  The video streaming part was inspired by:

  Rui Santos
  Complete project details at https://RandomNerdTutorials.com/esp32-cam-video-streaming-web-server-camera-home-assistant/
  
  Permission is hereby granted, free of charge, to any person obtaining a copy of this part of software and associated documentation files.

  The above copyright notice and this permission notice shall be included in all copies or substantial portions of this part of the software.

  Delete icon: <a target="_blank" href="https://icons8.com/icon/X4fWgHt6q9So/delete">Delete</a> icon by <a target="_blank" href="https://icons8.com">Icons8</a>
  Download icon: <a target="_blank" href="https://icons8.com/icon/20FjgTazh8FG/download">Download</a> icon by <a target="_blank" href="https://icons8.com">Icons8</a>
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
#include <ArduinoSort.h>

#define PART_BOUNDARY "123456789000000000000987654321"

#define SCRATCH_BUFSIZE 8192

#define PIR_PIN 0

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
const char* video_miniature = "iVBORw0KGgoAAAANSUhEUgAAAEAAAABACAYAAACqaXHeAAAABmJLR0QA/wD/AP+gvaeTAAABeElEQVR4nO2ZMU7DMBhGnytOAis36ModGBCsPQJrK1bEBAKprEXiPoz0BEwIZsIQR02jJJCo6Zcm35MsWY3TPH/541YxGGOMMcaYcRKE154AF7G/An6ELhKugCS2S5XERHVh4DjXP1FJKAPoBQ5ALaDGAagF1DgAtYAaB6AWUOMA1AJqHIBaQI0DUAuo6TqAG+AbuKXZ26cA3MVzFx147Y0vNm99ntgOYZ47Ns99HuLY7Nhnl4JdV8Ay158Bj9RXQohjZhXfcXAE4J7N3UyAZ9LgixUQgIfC2CUDWKeqQliwHcAgJ59RfK4T4KOiX7ZeDIKySihrg7rzRf4KYdCTz6gKYRSTzyiGIJu8cqEJwHnsv5IGYfZNkwro+25u53692M2toZVfk4WnF7u5NbTyG83PThUOQC2gxgGoBdQ4ALWAmqOW502B612K7IBpm5PaBnAW28HT5BFYd2axe97/O7BJBaxI/2efNtbZL2/Ai1rCGGOMMcb0nV/wQ2Ny2wsxgAAAAABJRU5ErkJggg==";
const char* delete_icon = "iVBORw0KGgoAAAANSUhEUgAAAEAAAABACAYAAACqaXHeAAAABmJLR0QA/wD/AP+gvaeTAAAJKUlEQVR4nO2be1DU1xXHP/tbWGBRQHB8IYRBQTslqLxijMZooqNOzYg6moka0VGitsakxgdVY310EquxxmhitQ3UtKatBpnGRAmllfCIBV8oxgdEV2B9IQ9RWRZYfv3jt4u6D9n9scDa+p35/XPvueee+/2de+65Z38LT2GBXEB8wp5suYtVWGkT5SrrZFhbS6tws9Wh0ZTKN6UDERIS3KbxgpPs+J+CCIgaTamo0ZSK0dHRnb2/LZ7Y2NgW+x5ql4VWPeDEiRNydbcbCgoKnKbLZgwwx4YL95w2aVuwZmAXp+r7v48BTwnobAM6G3bHAHOczzxEXsoOrhWdAiDw2SiGzVnMwFETnGZcR0AWAd9++B7Ze7Y+0nYlP5sr+dmMfHMZr7yz1inGdQQc3gLnMw+RvWcrKneBTcvC0OaMQJszgg/e7Y/KXSDr95u5ePRwq3oa63Xsnj6a3dNH01ivk2W8M+AwAXkpOwBYvySU5fOfoU8PD/r08GBFYgjrl4QCkJv8cat60jevoqwwn7LCfNI3r3bUDKfBYQKunZP2/BvxvS36Zsf3kWSKTj5Wx6WsdPL37UFwUyC4Kcjft5tLWemOmuIUyD4FRCvJp6HZ2KiwfTG7X3Wbg6sWIYoiP5ngy8AJvoiiSGpSIvdu35Rrjmw4TEBgRBQAn6ddt+gztZlkzCGKImmrFnHv9k0C+nkQOsqHfqN86B7uyf2qSlKTFiBaY7Yd4TABw+YsBuC9jy6zabeGa7f0XLulZ9NuDWu3X5ZkEhZbHVvwxR+48O9vcPcSiJoZgEIhOUvUjABU3gLF2RkU/PWPbViO43CYgIGjJjDyzWU0NDazcksJgcOzCRyezcotJTQ0NvPSwhUMeGmcxbiKHy9y5Le/AiBymj9e3R6cwJ6+SgZN9wfg8AcruFn8g9z1OAxZMeCVd9Yyc9cBQoeOxMO7Cyq1N6FDRzJz1wFeXrLGQt7Q1MiXKxNprNcR/Jw3gUPUFjK9I9UExXnTpNfz5fJ5GBob5JjmMGRnggNeGmf1TVtD5rb1aM+ewDvAjYj4bjblIqf6U3VFz/XzZ8j8aANj390g1zy70e53gavH88j9bDsKQUHUrADcPG1PqVQpiJ7VHUGpIOezbVw+ltXe5sknwNDYwN758exNnGzTXXW1NexfNpfmZgPhY33oFuLRql6/YBVhY3wQm0VSV85HV1sj10S7IJuAjK2/pjg7g+LvvuWfv1tnVeardW9z53p5y6LsRfhYX7qFeHDnxjXSVi2Sa6JdkEVASW4meSkfo3QDpRvkJm+nJDfzEZnTafs4+/UB3D0FYmZLbm0vFAJEG7fLDxn/4FTaX+SYaRccJuB+1W0OrpyPKIqMTxQYlyggiiIHls9ryeSqyzUc2rgUgIgp3VAHOB5r1QFuPDtZCpiH1r9N5dUfHdZhDxwiQBRF0lYvoLbiFv2iFLycIDAmQSA8VsH9ygpSkxbQbGgiNWkB+nt36T1ITVCst2zjguK8CYxS01CnY//SBAxNjbJ12YJDBBz78y4u/OsI3r7wxkYlgiC564x1Srx9oTg7gz2vj0FTkIOXnxuDjclNWxA51R8vPyXaolN8t2tzm/WZw24CblwsImOLlMm9tlqJX48HfX49pDaA8sICFAoYMsMfd3XbT1l3tcCQGVLafPTTTW3WZw67LUxd+hqN+kZemCwQOcoyoEWOUvDCFEmdp6+bXUeevege5km/0T40GwxO02mC3QRcL9HQK1TBpF/aHhK/VKBPfwW6mibOpVU7xUATBk7wxS9Y5VSd4AAB7iqY/RsBlefjZWZtEHBXgSb3HjfOOq/UJSgVDJkR4DR9LXrtFZz4lkCfsNbP8j5hCia+Jak9/UUVuuom+daZoWtPd6fpMsFuAl6cbn9Ae3G6wE+HK2ioM3BqX5XV6pGrwO5VPabKZVX29bVKfLrD7eJ6SjJr5djWIWi322CXbjBznRKFABcP36Fao2+vqdoEu3PUJTHy97KISPa2ji942oNWPSA2NrYj7HAIcXFxTtNl8yOpJ/AbIVkfST39dbizDXAUOp2OtLSDHDt2jDNnzjzcVQKcBb4G9gN37NH3xGyBpqYmdu7cQUpyMtU1rabZ94H3gQ+B+scJPhEEaLVaFi/+BSdPSh9sDcOb8XQlBi96GZ34Ok0cR8cRasmjzjT0NPAqUGZLt8sTUFFRwaRJr6LVaglBxQZ6EYvXY8cUUMdqbnKVBgAtMBQotybr0kFQr9czb95ctFotsXjxd4JbXTxALGr2E0yMJBsIfAVYvZ+7NAEpKckUFhYSgoqdBLIALVO5SiW26wKVGJjCVRaiZSeBBKMCGAwstSbvsgTU1tby6SefALCBXvigRI9IEfUkUGaVhEoMJFDGOerRI+KLko30NHUnARa1eZclID39CDV3ahiGd4vb76Ev4XhQjJ5ZlFLBg/S8CgNzKKMYPaGo2EUgAHGoeR41QBdgmvk8LktAVtZRACbQtaXNHyUpBBGOB5dpYDZlVNBElfHNXzIufi9BdH8oxRn/QMd483lcNhEqLCwEINos6PmjJJkgEoxve64xuBejJwwPUggiAOUjY2Jo+TV6sPk8LusBd+/eBaCnlXcUgJI/GT2hGH2L2yfT12LxQEuuAPQw73NZAurrpQTOkWKSLdnmx4xxWQKCgoIAuIVlHaISA7ONez4MD8KMMWEu5VZPh5sPdFh82OSyBISHhwNwnEcry+bRPpm+j2wH89NB0tGSGhfZM3en/yPk4WcYavECA8QLDBDz6C+G4yECYigqMYd+NvuyH+p7DrVJ3xzzxVrzgBx7WOoo5FFHgfENzqe8xe0/J/iRo850Opi2w0K0AHxPHf+RxtcCqeb6rREwAumS5ArPGoAkblCNAU8UROBp9agD6XRIIYgIPPFEwR0MrKWlFvk+VmoEsspIHQgVkA8MisGLnQTia2Xh1lCDgZ+j5YQUQ44jvViL2oDLBkEjGoCfAaXH0TGNUvIfBDSb+J46pnHVtPhSIB4bhRFX9wATngEOADEAz6NmPF2JRk1v3BCRCiInqOMb7pr2PMApJAKvdYrVToYnsAqoofXToxbp9tfqz8lPigc8DD9gEjARiAB6IiV71cBJIB34G3C3swx8ovBfMXiEx9x8H70AAAAASUVORK5CYII=";

//Variables for setting time. By default set to UTC +2.00.
const int UTC = 2;
const long gmtOffset_sec = UTC*60*60;
const int daylightOffset_sec = 3600;       //Takes into account the daylight saving time
const char* ntpServer = "pool.ntp.org";

//This defines the time between two detections of motion that cause sending an email
#define pirDelayTime 45000

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
  pinMode(PIR_PIN, INPUT);

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
  alarmOn = false;

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
  slot_config.width = 1;
  
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
  //int lastPirMeasurement = millis();
  int currentMillis;
  while (!motionDetected || cyclicalFramesCaptured < MAX_FRAMES) {
    currentMillis = millis();

    if (currentMillis - lastPicTaken > FRAME_INTERVAL) {
      lastPicTaken = millis();
      runBufferRepeat();
    }

    /***if (!motionDetected && currentMillis - lastPirMeasurement > 1500) {
      lastPirMeasurement = millis();
      motionDetected = digitalRead(PIR_PIN);
      Serial.println(motionDetected);
    }***/
  }

  initInPos = cyclicalFramesCaptured % MAX_FRAMES;
  initOutPos = (cyclicalFramesCaptured + 1) % MAX_FRAMES;

  int t0 = millis();
  fileOpen = startFile();
  Serial.println("Starting the file...");

  currentMillis = millis();
  while (fileOpen && currentMillis - t0 < RECORDING_TIME) {
    currentMillis = millis();

    if (currentMillis - lastPicTaken > FRAME_INTERVAL) {
      lastPicTaken = millis();
      captureFrame();
      addToFile();
    }
  }

  while (framesInBuffer() > 0) {
    addToFile();
  }

  closeFile();
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
  httpd_resp_set_type(req, "text/html");

  const char* resp = "<center><p style=\"font-size:40px\"><b>You logged in to the ESP32 camera system.</b></p></center> <center><p style=\"font-size:30px\">Please select the right action from below:</p></center>";
  httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

  resp = "<br><center><form action = \"http://";
  httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

  resp = WiFi_IP.c_str();
  httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

  resp = "/stream\"><input type=\"submit\" value=\"See camera image\" style=\"height:60px; width:350px; font-size:30px\"/> </form></center>";
  httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

  resp = "<br><center><form action = \"http://";
  httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

  resp = WiFi_IP.c_str();
  httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

  resp = "/alarm\"><input type=\"submit\" value=\"Turn the alarm mode on\" style=\"height:60px; width:350px; font-size:30px\"/> </form></center>";
  httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

  resp = "<br><center><form action = \"http://";
  httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

  resp = WiFi_IP.c_str();
  httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

  resp = "/video\"><input type=\"submit\" value=\"Video gallery\" style=\"height:60px; width:350px; font-size:30px\"/> </form></center>";
  httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

  resp = "<br><center><form action = \"http://";
  httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

  resp = WiFi_IP.c_str();
  httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

  resp = "/img\"><input type=\"submit\" value=\"Take an image\" style=\"height:60px; width:350px; font-size:30px\"/> </form></center>";
  httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

  httpd_resp_send_chunk(req, NULL, 0);

  return ESP_OK;
}

esp_err_t video_gallery_get_handler(httpd_req_t *req) {
  SD_MMC.begin();
  int nFiles = countFiles(SD_MMC, "/videos", 0);
  char* fileNames [nFiles];
  listDir(SD_MMC, "/videos", 0, fileNames, nFiles);

  const char* resp = "<center><p style=\"font-size:35px\"><b>ESP32-cam video gallery</b></p></center>";
  httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

  for (char* fileName : fileNames) {
    char* videoName = NULL;
    videoName = strtok(fileName, "/");
    videoName = strtok(NULL, "/");

    //Inserting download button
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

    resp = "\" width=\"100\" height=\"100\"></a>";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);


    //Gap between the icons
    resp = "&nbsp &nbsp &nbsp &nbsp &nbsp";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);


    //Inserting delete button
    resp = "<a href=\"http://";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    resp = WiFi_IP.c_str();
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    resp = "/video_delete?filename=";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    resp = videoName;
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);
    
    resp = "\"><img src = \"data:image/jpeg;base64,";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    httpd_resp_send_chunk(req, delete_icon, HTTPD_RESP_USE_STRLEN);

    resp = "\" width=\"100\" height=\"100\"></a></center>";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    resp = "<center><p style = \"font-size:15px\"><b>";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);

    httpd_resp_send_chunk(req, videoName, HTTPD_RESP_USE_STRLEN);

    resp = "</b></p></center><br>";
    httpd_resp_send_chunk(req, resp, HTTPD_RESP_USE_STRLEN);   
  }

  httpd_resp_send_chunk(req, NULL, 0);
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

httpd_uri_t delete_video_uri = {
  .uri = "/video_delete",
  .method = HTTP_GET,
  .handler = delete_video_handler,
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
  config.max_uri_handlers = 11;
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
    httpd_register_uri_handler(server, &delete_video_uri);
  }
}