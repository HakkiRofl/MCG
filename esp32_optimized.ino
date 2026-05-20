#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiUdp.h>

// ===========================================
// КОНФИГУРАЦИЯ WiFi
// ===========================================
const char* ssid = "RT-GPON-6A98";        // Замените на ваш SSID
const char* password = "f64UGz7afr"; // Замените на ваш пароль

// ===========================================
// КОНФИГУРАЦИЯ UDP
// ===========================================
const char* udpAddress = "192.168.0.10";  // IP адрес вашего ПК
const int udpPort = 12345;
const int CHUNK_SIZE = 1400;  // Размер фрагмента (оптимально для UDP)

WiFiUDP udp;

// ===========================================
// КОНФИГУРАЦИЯ КАМЕРЫ AI-THINKER
// ===========================================
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

// ===========================================
// ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ
// ===========================================
uint16_t imageCounter = 0;
unsigned long lastFrameTime = 0;
int fps = 0;
int frameCount = 0;

// ===========================================
// ИНИЦИАЛИЗАЦИЯ КАМЕРЫ
// ===========================================
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
  config.xclk_freq_hz = 20000000;  // 20MHz для стабильности
  config.pixel_format = PIXFORMAT_JPEG;

  // Оптимизированные настройки для баланса качества и скорости
  config.frame_size = FRAMESIZE_VGA;     // 640x480 - хороший баланс
  config.jpeg_quality = 12;               // 10-15 оптимально (меньше = лучше качество, но больше размер)
  config.fb_count = 2;                    // Двойная буферизация для плавности
  config.grab_mode = CAMERA_GRAB_LATEST;  // Всегда берем последний кадр
  
  // Инициализация камеры
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("❌ Camera init failed with error 0x%x\n", err);
    return;
  }

  // Дополнительные настройки сенсора для улучшения качества
  sensor_t * s = esp_camera_sensor_get();
  if (s != NULL) {
    s->set_brightness(s, 0);     // -2 до 2
    s->set_contrast(s, 0);       // -2 до 2
    s->set_saturation(s, 0);     // -2 до 2
    s->set_special_effect(s, 0); // 0 = нет эффектов
    s->set_whitebal(s, 1);       // Автобаланс белого
    s->set_awb_gain(s, 1);       // Автоматическая настройка усиления
    s->set_wb_mode(s, 0);        // Режим баланса белого (0 = авто)
    s->set_exposure_ctrl(s, 1);  // Автоэкспозиция
    s->set_aec2(s, 0);           // AEC DSP
    s->set_ae_level(s, 0);       // -2 до 2
    s->set_aec_value(s, 300);    // 0 до 1200
    s->set_gain_ctrl(s, 1);      // Автоусиление
    s->set_agc_gain(s, 0);       // 0 до 30
    s->set_gainceiling(s, (gainceiling_t)0); // 0 до 6
    s->set_bpc(s, 0);            // Black pixel correction
    s->set_wpc(s, 1);            // White pixel correction
    s->set_raw_gma(s, 1);        // Гамма-коррекция
    s->set_lenc(s, 1);           // Коррекция линз
    s->set_hmirror(s, 0);        // Горизонтальное отражение
    s->set_vflip(s, 0);          // Вертикальное отражение
    s->set_dcw(s, 1);            // Масштабирование
    s->set_colorbar(s, 0);       // Тестовая палитра (отключено)
  }

  Serial.println("✅ Camera initialized successfully");
  Serial.printf("   Resolution: VGA (640x480)\n");
  Serial.printf("   JPEG Quality: %d\n", config.jpeg_quality);
  Serial.printf("   Frame buffers: %d\n", config.fb_count);
}

// ===========================================
// ОТПРАВКА КАДРА ПО UDP
// ===========================================
void sendFrameUDP(camera_fb_t * fb) {
  if (!fb || fb->len == 0) {
    Serial.println("⚠️ Invalid frame buffer");
    return;
  }

  uint16_t totalSize = fb->len;
  if (totalSize > 100 * 1024) {  // Защита от слишком больших кадров
    Serial.printf("⚠️ Frame too large: %d bytes, skipping\n", totalSize);
    return;
  }

  uint16_t totalFragments = (totalSize + CHUNK_SIZE - 1) / CHUNK_SIZE;
  uint16_t currentImgId = imageCounter++;

  // Отправка фрагментов
  for (uint16_t fragId = 0; fragId < totalFragments; fragId++) {
    size_t offset = fragId * CHUNK_SIZE;
    size_t fragmentSize = min(CHUNK_SIZE, totalSize - offset);

    // Создание заголовка (8 байт)
    uint8_t header[8];
    header[0] = (currentImgId >> 8) & 0xFF;
    header[1] = currentImgId & 0xFF;
    header[2] = (fragId >> 8) & 0xFF;
    header[3] = fragId & 0xFF;
    header[4] = (totalFragments >> 8) & 0xFF;
    header[5] = totalFragments & 0xFF;
    header[6] = (totalSize >> 8) & 0xFF;
    header[7] = totalSize & 0xFF;

    // Создание пакета
    uint8_t packet[8 + CHUNK_SIZE];
    memcpy(packet, header, 8);
    memcpy(packet + 8, fb->buf + offset, fragmentSize);

    // Отправка UDP пакета
    udp.beginPacket(udpAddress, udpPort);
    udp.write(packet, 8 + fragmentSize);
    udp.endPacket();

    // Минимальная задержка для предотвращения переполнения буфера
    delayMicroseconds(100);
  }

  // Обновление FPS каждую секунду
  frameCount++;
  unsigned long currentTime = millis();
  if (currentTime - lastFrameTime >= 1000) {
    fps = frameCount;
    frameCount = 0;
    lastFrameTime = currentTime;
    Serial.printf("📊 FPS: %d | Frame size: %d bytes | Fragments: %d\n", 
                  fps, totalSize, totalFragments);
  }
}

// ===========================================
// SETUP
// ===========================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n\n🚀 ESP32-CAM Stream Starting...");

  // Подключение к WiFi
  WiFi.begin(ssid, password);
  Serial.print("📡 Connecting to WiFi");
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ WiFi Connected!");
    Serial.printf("   IP Address: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("   Target PC: %s:%d\n", udpAddress, udpPort);
  } else {
    Serial.println("\n❌ WiFi connection failed!");
    return;
  }

  // Инициализация камеры
  initCamera();

  Serial.println("\n🎥 Starting video stream...");
  lastFrameTime = millis();
}

// ===========================================
// LOOP
// ===========================================
void loop() {
  // Захват кадра
  camera_fb_t * fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("❌ Frame buffer acquisition failed");
    delay(100);
    return;
  }

  // Отправка кадра
  sendFrameUDP(fb);

  // Освобождение буфера
  esp_camera_fb_return(fb);

  // Минимальная задержка для целевого FPS ~15
  // При 15 FPS период = 1000/15 ≈ 67ms
  // Учитываем время отправки, поэтому задержка меньше
  delay(10);
}
