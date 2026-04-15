#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include "esp_camera.h"          // для камеры (понадобится позже)
#include <esp_ota_ops.h>
#include <WiFiUdp.h>
#include <esp_wifi.h>


// Настройки UDP (ПК)
const char* udpAddress = "192.168.0.10"; // Например, 192.168.1.100
const int udpPort = 12345;

WiFiUDP udp;
// ------------------------------------------------------------
// 2. Протокол передачи (Заголовок фрагмента)
// ------------------------------------------------------------
// Мы будем отправлять каждый кадр фрагментами. Чтобы получатель
// мог правильно собрать кадр, каждый фрагмент будет предваряться
// небольшим заголовком с метаинформацией.
// ------------------------------------------------------------
// Отправка целого числа в сетевом порядке байт (Big Endian)
// ------------------------------------------------------------
void writeUint16(uint8_t* buf, uint16_t val) {
  buf[0] = (val >> 8) & 0xFF;
  buf[1] = val & 0xFF;
}

void writeUint32(uint8_t* buf, uint32_t val) {
  buf[0] = (val >> 24) & 0xFF;
  buf[1] = (val >> 16) & 0xFF;
  buf[2] = (val >> 8) & 0xFF;
  buf[3] = val & 0xFF;
}

const int CHUNK_SIZE = 1400; // Макс. размер данных в UDP-пакете (меньше MTU)
// !!! ВАЖНО: Выбор разрешения. От этого зависит скорость и качество !!!
// FRAMESIZE_QVGA (320x240)  -> Очень быстро, до 30+ FPS.
// FRAMESIZE_HVGA (480x320)  -> Хороший баланс качества и скорости (~20 FPS).
// FRAMESIZE_VGA (640x480)   -> Медленнее (8-12 FPS), но детальнее.
static const framesize_t FRAME_SIZE = FRAMESIZE_HVGA;

// !!! ВАЖНО: Качество JPEG (0 - лучшее, 63 - худшее) !!!
// Меньшее число = лучше качество, но больше размер файла.
// Значения 10-20 - хороший компромисс. Для скорости ставьте выше.
static const int JPEG_QUALITY = 12;


// ========== Настройки Wi-Fi ==========
const char* ssid = "RT-GPON-6A98";
const char* password = "f64UGz7afr";
IPAddress staticIP(192, 168, 0, 128);
IPAddress gateway(192, 168, 1, 1);    // IP Address of your network gateway (router)
IPAddress subnet(255, 255, 255, 0);   // Subnet mask
IPAddress primaryDNS(192, 168, 1, 1); // Primary DNS (optional)
IPAddress secondaryDNS(0, 0, 0, 0);   // Secondary DNS (optional)

// ========== Настройки веб-сервера ==========
WebServer server(80);

// ========== Пин-конфигурация для ESP32-CAM ==========
// (стандартная для AI-Thinker)
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

// ========== Инициализация камеры ==========
void initCamera() {
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
  config.frame_size = FRAME_SIZE;  // можно изменить позже
  config.pixel_format = PIXFORMAT_JPEG;   // <-- Ключевое изменение: используем JPEG!
  config.jpeg_quality = JPEG_QUALITY;
  config.fb_count = 2;                    // Двойная буферизация для скорости
  config.grab_mode = CAMERA_GRAB_LATEST;  // Берем самый свежий кадр

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    return;
  }
  Serial.println("Camera initialized.");

  // Небольшая оптимизация датчика для лучшей картинки
 sensor_t * s = esp_camera_sensor_get();
  if (s) {
    s->set_pixformat(s, PIXFORMAT_JPEG);
    s->set_framesize(s, FRAME_SIZE);
    s->set_quality(s, JPEG_QUALITY);
    Serial.println("Sensor format forced to JPEG.");
  }
}

// ========== HTML-страница загрузки ==========
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<meta charset="utf-8">
<head>
    <title>ESP32-CAM OTA Update</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
</head>
<body>
    <h1>ESP32-CAM OTA Update V2.0.9</h1>
    <form method="POST" action="/update" enctype="multipart/form-data">
        <input type="file" name="firmware" accept=".bin">
        <input type="submit" value="Обновить">
    </form>
    <hr>
    <p>Выберите файл прошивки (.bin) и нажмите "Обновить".</p>
    <form method="POST" action="handleStream">
        <input type="submit" value="Видео">
    </form>
    <form method="POST" action="/rollback">
        <input type="submit" value="Откат прошивки">
    </form>
</body>
</html>
)rawliteral";

// ========== Обработчик корневой страницы ==========
void handleRoot() {
  server.send(200, "text/html", index_html);
}


//  ========== Откат до прошлой прошивки ==========
void handleRollback() {
  const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
  if (partition == NULL) {
    server.send(500, "text/plain", "No rollback partition");
    return;
  }
  esp_ota_set_boot_partition(partition);
  server.send(200, "text/plain", "Rollback set, rebooting...");
  delay(1000);
  ESP.restart();
}

// ========== Обработчик OTA-обновления ==========
void handleUpdate() {
  // Проверяем, что пришёл файл
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    // Начинаем OTA-процесс
    Serial.printf("Update start: %s\n", upload.filename.c_str());
    // Размер прошивки пока неизвестен, начинаем без указания размера
    // Update.begin() может определить размер позже по данным
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
      return;
    }
  } 
  else if (upload.status == UPLOAD_FILE_WRITE) {
    // Пишем данные во флеш
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }
  } 
  else if (upload.status == UPLOAD_FILE_END) {
    // Завершаем OTA
    if (Update.end(true)) {  // true – проверка целостности
      Serial.printf("Update success: %u bytes\n", upload.totalSize);
      server.send(200, "text/html", "<h2>Update successful! Rebooting...</h2>");
      delay(1000);
      ESP.restart();
    } else {
      Update.printError(Serial);
      server.send(500, "text/html", "<h2>Update failed</h2>");
    }
  }
  delay(0);
}


// void handleStream() {
//   WiFiClient client = server.client();
//   client.println("HTTP/1.1 200 OK");
//   client.println("Content-Type: multipart/x-mixed-replace; boundary=--jpgboundary");
//   client.println();

//   while (client.connected()) {
//     camera_fb_t *fb = esp_camera_fb_get();
//     if (!fb) continue;
//     client.printf("--jpgboundary\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", fb->len);
//     client.write(fb->buf, fb->len);
//     client.println("\r\n");
//     esp_camera_fb_return(fb);
//     delay(25);  // регулировка кадров
//   }
// }



// ========== Настройка маршрутов сервера ==========
void setupServer() {
  server.on("/", HTTP_GET, handleRoot);
  // Для обработки загрузки файла используем специальный обработчик
  server.on("/update", HTTP_POST, []() {
    // Этот обработчик вызывается после завершения загрузки
    // (если не используем server.on с Upload, то вызывается после end)
    // Но мы используем server.on для POST и сами обрабатываем upload через server.upload()
  }, handleUpdate);  // передаём функцию, которая будет вызвана в процессе загрузки
  server.begin();
  Serial.println("HTTP server started");
}




// ========== Главная функция setup ==========
void setup() {
  Serial.begin(115200);
  Serial.println("Booting...");

  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  //esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
  // Принудительно N, если роутер поддерживает:
  esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11N);

WiFi.mode(WIFI_STA);
  int8_t countWiFi = WiFi.scanNetworks();
  bool findWiFi = false;
  for (int8_t i = 0; i < countWiFi; i++) {
    if (WiFi.SSID(i) == ssid)
      findWiFi = true;
  }

  if (findWiFi) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    if(!WiFi.config(staticIP, gateway, subnet, primaryDNS, secondaryDNS)) {
      Serial.println("Failed to configure Static IP");
    } else {
      Serial.println("Static IP configured!");
    }
    while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      Serial.print(".");
    Serial.println("\nWiFi connected");
    }
  }
  else {
    // Создание Wi-Fi Wi-Fi
    WiFi.softAP("ESP", "1122334455");
    if(!WiFi.softAPConfig(staticIP, staticIP, subnet)) {
      Serial.println("Failed to configure Static IP");
    } 
    else {
      Serial.println("Static IP configured!");
    }
    }
  }
  else {
    // Создание Wi-Fi Wi-Fi
    WiFi.softAP("ESP", "1122334455");
    if(!WiFi.softAPConfig(staticIP, staticIP, subnet)) {
      Serial.println("Failed to configure Static IP");
    } 
    else {
      Serial.println("Static IP configured!");
    }
    Serial.println("\nWiFi created with name: " + *ssid);
  }

  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  // Инициализация камеры
  initCamera();

  // Запуск веб-сервера
  setupServer();
  server.on("/rollback", HTTP_GET, handleRollback);
  server.begin();
  //  server.on("/video", HTTP_GET, handleStream);
  server.begin();
  
}

uint16_t currentImageId = 0;



void loop() {
  server.handleClient();  // обрабатываем входящие запросы
  // 1. Захват кадра с камеры (уже в JPEG)
  camera_fb_t * fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Capture failed");
    return;
  }

  // Проверка JPEG-заголовка
  if (fb->len < 2 || fb->buf[0] != 0xFF || fb->buf[1] != 0xD8) {
    Serial.printf("ERROR: Not a JPEG frame! size=%u, first bytes: %02X %02X\n", 
                  fb->len, fb->buf[0], fb->buf[1]);
    esp_camera_fb_return(fb);
    return;
  }

  Serial.printf("JPEG frame: id=%u, size=%u bytes\n", currentImageId, fb->len);

  uint16_t totalFragments = (fb->len + CHUNK_SIZE - 1) / CHUNK_SIZE;
  
  for (uint16_t i = 0; i < totalFragments; i++) {
    // Формируем заголовок вручную (8 байт)
    uint8_t header[8];
    writeUint16(&header[0], currentImageId);
    writeUint16(&header[2], i);
    writeUint16(&header[4], totalFragments);
    writeUint16(&header[6], fb->len);  // размер помещается в uint16_t? Да, до 65535 байт
    
    // Если размер больше 65535 (маловероятно для JPEG), придется использовать 4 байта,
    // но для простоты пока ограничимся 16 битами (JPEG до 64 КБ).
    // В нашем случае fb->len обычно < 30000.

    size_t offset = i * CHUNK_SIZE;
    size_t chunkSize = min((size_t)CHUNK_SIZE, fb->len - offset);
    
    udp.beginPacket(udpAddress, udpPort);
    udp.write(header, sizeof(header));
    udp.write(fb->buf + offset, chunkSize);
    if (udp.endPacket() <= 0) {
      Serial.println("UDP send error");
    }
    delay(20);
  }
  
  esp_camera_fb_return(fb);
  currentImageId++;
}