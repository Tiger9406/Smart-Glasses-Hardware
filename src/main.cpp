#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include "driver/i2s.h"
#include "driver/ledc.h"
#include "camera_pins.h"
#include "app_httpd.h"

//led flash configs
#if defined(LED_GPIO_NUM)
#define LED_LEDC_CHANNEL 2
#define LED_LEDC_FREQUENCY 5000
#define LED_LEDC_RESOLUTION 8
#define LED_BRIGHTNESS 128  // 0-255, adjust brightness here
#endif

#define I2S_BCLK   14
#define I2S_LRCLK  15
#define I2S_DOUT   13

//TODO: to define speaker pins here

static const int SAMPLE_RATE = 16000;
static const int DMA_BUF_LEN = 512;
static const int DMA_BUF_CNT = 4;

const char* ssid = "ESP32-CAM-S3";
const char* password = "12345678";

// tasks are threads in esp32
TaskHandle_t camera_task_handle = NULL;
TaskHandle_t audio_tx_task_handle = NULL; //rx for microphone

QueueHandle_t video_queue = NULL;
QueueHandle_t audio_tx_queue = NULL;

#if defined(LED_GPIO_NUM)
void setupLED() {
    ledcSetup(LED_LEDC_CHANNEL, LED_LEDC_FREQUENCY, LED_LEDC_RESOLUTION);
    ledcAttachPin(LED_GPIO_NUM, LED_LEDC_CHANNEL);
    ledcWrite(LED_LEDC_CHANNEL, 0);  // Start with LED off
    Serial.println("LED flash initialized");
}

void setLED(bool on) {
    ledcWrite(LED_LEDC_CHANNEL, on ? LED_BRIGHTNESS : 0);
}

void blinkLED(int times, int delayMs) {
    for (int i = 0; i < times; i++) {
        ledcWrite(LED_LEDC_CHANNEL, LED_BRIGHTNESS);
        delay(delayMs);
        ledcWrite(LED_LEDC_CHANNEL, 0);
        delay(delayMs);
    }
}
#else
void setupLED() {
    Serial.println("LED flash disabled (LED_GPIO_NUM not defined)");
}
void setLED(bool on) {}
void blinkLED(int times, int delayMs) {}
#endif

void setupI2S() {
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = DMA_BUF_CNT,
        .dma_buf_len = DMA_BUF_LEN,
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0
    };
    
    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_BCLK,
        .ws_io_num = I2S_LRCLK,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_DOUT
    };
    
    i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_NUM_0, &pin_config);
    i2s_zero_dma_buffer(I2S_NUM_0);

    //TODO: I2S0 for Speaker (tx)
}

//assign to core 1
void camera_task(void *pvParameters){
    camera_fb_t *fb = NULL;
    video_frame_t frame;

#if defined(LED_GPIO_NUM)
    setLED(true);  // Turn on LED when streaming starts
#endif
    Serial.println("Camera task started");
    
    while (1) {
        fb = esp_camera_fb_get();
        if (fb) {
            if (fb->format == PIXFORMAT_JPEG) {
                // Allocate memory for frame copy
                frame.data = (uint8_t*)malloc(fb->len);
                if (frame.data) {
                    memcpy(frame.data, fb->buf, fb->len);
                    frame.len = fb->len;
                    
                    // Send to queue (non-blocking)
                    if (xQueueSend(video_queue, &frame, 0) != pdTRUE) {
                        free(frame.data); // Queue full, drop frame
                        Serial.println("Video queue full, dropping frame");
                    }
                }
            }
            esp_camera_fb_return(fb);
        }
        
        vTaskDelay(pdMS_TO_TICKS(33)); // ~30fps
    }
}

//assign to core 1
void audio_tx_task(void *pvParameters) {
    size_t bytes_read;
    int32_t i2s_in[512];
    audio_buffer_t audio_buf;
    static int32_t dc_sum = 0;

    Serial.println("Audio TX task started");
    
    while (1) {
        // Read from microphone
        i2s_read(I2S_NUM_0, (void*)i2s_in, sizeof(i2s_in), 
                 &bytes_read, portMAX_DELAY);
        
        int samples = bytes_read / sizeof(int32_t);
        audio_buf.count = samples;
        
        // High-pass filter and convert to 16-bit
        for (int i = 0; i < samples; i++) {
            dc_sum = (dc_sum * 15 + i2s_in[i]) >> 4;
            int32_t filtered = i2s_in[i] - dc_sum;
            audio_buf.samples[i] = (int16_t)(filtered >> 14);
        }
        
        // Send to queue
        xQueueSend(audio_tx_queue, &audio_buf, portMAX_DELAY);
    }
}

void setup() {
    Serial.begin(115200);
    Serial.setDebugOutput(true);
    Serial.println();
    
    setupI2S();
    
    // Camera configuration
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
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000;
    config.frame_size = FRAMESIZE_UXGA;
    config.pixel_format = PIXFORMAT_JPEG;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.jpeg_quality = 12;
    config.fb_count = 1;
    
    // If PSRAM IC present, init with UXGA resolution
    if(psramFound()){
        config.jpeg_quality = 10;
        config.fb_count = 2;
        config.grab_mode = CAMERA_GRAB_LATEST;
    } else {
        config.frame_size = FRAMESIZE_SVGA;
        config.fb_location = CAMERA_FB_IN_DRAM;
    }
    
    // Camera init
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("Camera init failed with error 0x%x", err);
        return;
    }
    
    sensor_t * s = esp_camera_sensor_get();
    s->set_vflip(s, 1);
    s->set_brightness(s, 1);
    s->set_saturation(s, 0);

    video_queue=xQueueCreate(2, sizeof(video_frame_t));
    audio_tx_queue = xQueueCreate(4, sizeof(audio_buffer_t));

    if (!video_queue || !audio_tx_queue) {
        Serial.println("Failed to create queues!");
        return;
    }
    
    // Start WiFi AP
    WiFi.softAP(ssid, password);
    IPAddress IP = WiFi.softAPIP();
    Serial.print("AP IP address: ");
    Serial.println(IP);
    
    startServer();
    xTaskCreatePinnedToCore(
        camera_task,           // Task function
        "camera",              // Name
        4096,                  // Stack size
        NULL,                  // Parameters
        4,                     // Priority
        &camera_task_handle,   // Handle
        1                      // Core 1 (App CPU)
    );

    xTaskCreatePinnedToCore(
        audio_tx_task,
        "audio_tx",
        4096,
        NULL,
        5,                     // Higher priority for audio
        &audio_tx_task_handle,
        1                      // Core 1
    );
    
    Serial.print("Camera Ready! Use 'http://");
    Serial.print(IP);
    Serial.println("' to connect");
}

void loop() {
    delay(10000);
}