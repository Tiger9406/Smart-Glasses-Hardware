#include "app_httpd.h"
#include "esp_timer.h"
#include "esp_camera.h"
#include "esp_log.h"

// #include "sdkconfig.h"
// #include "fb_gfx.h"

static const char* TAG = "app_httpd";

static httpd_handle_t server = NULL;
static int ws_fd = -1;
static TaskHandle_t ws_send_task_handle = NULL;

void websocket_send_task(void *pvParameters){
	video_frame_t video_frame;
    audio_buffer_t audio_buf;
    httpd_ws_frame_t ws_pkt;
    uint8_t *send_buf = NULL;

	ESP_LOGI(TAG, "WebSocket send task started on core %d", xPortGetCoreID());

	while(1){
		if (xQueueReceive(audio_tx_queue, &audio_buf, pdMS_TO_TICKS(5)) == pdTRUE) {
            if (ws_fd >= 0) {
                size_t audio_size = audio_buf.count * sizeof(int16_t);
                send_buf = (uint8_t*)malloc(1 + audio_size);
                
                if (send_buf) {
                    send_buf[0] = WS_FRAME_AUDIO_TX;
                    memcpy(&send_buf[1], audio_buf.samples, audio_size);
                    
                    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
                    ws_pkt.type = HTTPD_WS_TYPE_BINARY;
                    ws_pkt.payload = send_buf;
                    ws_pkt.len = 1 + audio_size;
                    
                    esp_err_t ret = httpd_ws_send_frame_async(server, ws_fd, &ws_pkt);
                    if (ret != ESP_OK) {
                        ESP_LOGE(TAG, "Failed to send audio frame: %s", esp_err_to_name(ret));
                        ws_fd = -1; // Connection lost
                    }
                    
                    free(send_buf);
                } else {
                    ESP_LOGE(TAG, "Failed to allocate memory for audio send");
                }
            }
        }

        if (xQueueReceive(video_queue, &video_frame, 0) == pdTRUE) {
            if (ws_fd >= 0) {
                send_buf = (uint8_t*)malloc(1 + video_frame.len);
                
                if (send_buf) {
                    send_buf[0] = WS_FRAME_VIDEO;
                    memcpy(&send_buf[1], video_frame.data, video_frame.len);
                    
                    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
                    ws_pkt.type = HTTPD_WS_TYPE_BINARY;
                    ws_pkt.payload = send_buf;
                    ws_pkt.len = 1 + video_frame.len;
                    
                    esp_err_t ret = httpd_ws_send_frame_async(server, ws_fd, &ws_pkt);
                    if (ret != ESP_OK) {
                        ESP_LOGE(TAG, "Failed to send video frame: %s", esp_err_to_name(ret));
                        ws_fd = -1; // Connection lost
                    }
                    
                    free(send_buf);
                } else {
                    ESP_LOGE(TAG, "Failed to allocate memory for video send");
                }
            }
            
            // Always free the video data after processing
            free(video_frame.data);
        }

        // Small delay to prevent task starvation
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

static esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        // WebSocket handshake
        ESP_LOGI(TAG, "WebSocket client connected");
        ws_fd = httpd_req_to_sockfd(req);
        
        return ESP_OK;
    }
    
    // For send-only, we don't process incoming frames
    // But we need to handle control frames (ping/pong/close)
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        return ret;
    }
    
    // Just acknowledge control frames, ignore data
    if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        ESP_LOGI(TAG, "WebSocket client disconnected");
        ws_fd = -1;
    }
    
    return ESP_OK;
}

void startServer(){
	httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 3;
    config.lru_purge_enable = true;
    config.stack_size = 4096;
    
    ESP_LOGI(TAG, "Starting server on port %d", config.server_port);
    
    if (httpd_start(&server, &config) == ESP_OK) {
        // WebSocket endpoint
        httpd_uri_t ws_uri = {
            .uri = "/ws",
            .method = HTTP_GET,
            .handler = ws_handler,
            .user_ctx = NULL,
            .is_websocket = true
        };
        httpd_register_uri_handler(server, &ws_uri);
        
        
        ESP_LOGI(TAG, "Server started");
        ESP_LOGI(TAG, "WebSocket: ws://192.168.4.1/ws");
        
        // Start send task on Core 0
        xTaskCreatePinnedToCore(
            websocket_send_task,
            "ws_send",
            4096,
            NULL,
            6,
            &ws_send_task_handle,
            0  // Core 0
        );
        
        ESP_LOGI(TAG, "WebSocket send task created");
    } else {
        ESP_LOGE(TAG, "Failed to start server");
    }
}