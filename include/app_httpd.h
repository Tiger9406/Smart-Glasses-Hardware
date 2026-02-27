#ifndef APP_HTTPD_H
#define APP_HTTPD_H

#include "esp_http_server.h"

// Frame types
#define WS_FRAME_VIDEO 0x01
#define WS_FRAME_AUDIO_RX 0x02

typedef struct {
    uint8_t *data;
    size_t len;
} video_frame_t;

typedef struct {
    int16_t samples[512];
    size_t count;
} audio_buffer_t;

extern QueueHandle_t video_queue;
extern QueueHandle_t audio_tx_queue;

// Function declarations
void startServer();
void setupLedFlash();

#endif // APP_HTTPD_H