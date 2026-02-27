#include "app_httpd.h"
#include "esp_timer.h"
#include "esp_camera.h"
#include "img_converters.h"
#include "fb_gfx.h"
#include "driver/ledc.h"
#include "driver/i2s.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "board_config.h"

static const char* TAG = "app_httpd";

static TaskHandle_t video_task_handle = NULL;
static TaskHandle_t audio_task_handle = NULL;

typedef struct {
    httpd_req_t *req;
} stream_ctx_t;

#if defined(ARDUINO_ARCH_ESP32) && defined(CONFIG_ARDUHAL_ESP_LOG)
#include "esp32-hal-log.h"
#include "esp32-hal.h"
#endif

// LED FLASH setup
#if defined(LED_GPIO_NUM)
#define CONFIG_LED_MAX_INTENSITY 255
#define LED_LEDC_CHANNEL 0
int led_duty = 0;
bool isStreaming = false;
#endif

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *_STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *_STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *_STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\nX-Timestamp: %d.%06d\r\n\r\n";

httpd_handle_t stream_httpd = NULL;

typedef struct {
	size_t size;   //number of values used for filtering
	size_t index;  //current value index
	size_t count;  //value count
	int sum;
	int *values;  //array to be filled with values
} ra_filter_t;

static ra_filter_t ra_filter;

static ra_filter_t *ra_filter_init(ra_filter_t *filter, size_t sample_size) {
	memset(filter, 0, sizeof(ra_filter_t));

	filter->values = (int *)malloc(sample_size * sizeof(int));
	if (!filter->values) {
		return NULL;
	}
	memset(filter->values, 0, sample_size * sizeof(int));

	filter->size = sample_size;
	return filter;
}

#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
static int ra_filter_run(ra_filter_t *filter, int value) {
	if (!filter->values) {
		return value;
	}
	filter->sum -= filter->values[filter->index];
	filter->values[filter->index] = value;
	filter->sum += filter->values[filter->index];
	filter->index++;
	filter->index = filter->index % filter->size;
	if (filter->count < filter->size) {
		filter->count++;
	}
	return filter->sum / filter->count;
}
#endif

#if defined(LED_GPIO_NUM)
void enable_led(bool en) {  // Turn LED On or Off
	int duty = en ? led_duty : 0;
	if (en && isStreaming && (led_duty > CONFIG_LED_MAX_INTENSITY)) {
		duty = CONFIG_LED_MAX_INTENSITY;
	}
	ledcWrite(LED_GPIO_NUM, duty);
	//ledc_set_duty(CONFIG_LED_LEDC_SPEED_MODE, CONFIG_LED_LEDC_CHANNEL, duty);
	//ledc_update_duty(CONFIG_LED_LEDC_SPEED_MODE, CONFIG_LED_LEDC_CHANNEL);
	log_i("Set LED intensity to %d", duty);
}
#endif

static esp_err_t video_stream_handler(httpd_req_t *req) {
	camera_fb_t *fb = NULL;
	struct timeval _timestamp;
	esp_err_t res = ESP_OK;
	size_t _jpg_buf_len = 0;
	uint8_t *_jpg_buf = NULL;
	char *part_buf[128];

	static int64_t last_frame = 0;
	if (!last_frame) {
		last_frame = esp_timer_get_time();
	}

	res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
	if (res != ESP_OK) {
		return res;
	}

	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	httpd_resp_set_hdr(req, "X-Framerate", "60");

#if defined(LED_GPIO_NUM)
	isStreaming = true;
	enable_led(true);
#endif

	while (true) {
		fb = esp_camera_fb_get();
		if (!fb) {
		log_e("Camera capture failed");
		res = ESP_FAIL;
		} else {
		_timestamp.tv_sec = fb->timestamp.tv_sec;
		_timestamp.tv_usec = fb->timestamp.tv_usec;
		if (fb->format != PIXFORMAT_JPEG) {
			bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
			esp_camera_fb_return(fb);
			fb = NULL;
			if (!jpeg_converted) {
			log_e("JPEG compression failed");
			res = ESP_FAIL;
			}
		} else {
			_jpg_buf_len = fb->len;
			_jpg_buf = fb->buf;
		}
		}
		if (res == ESP_OK) {
		res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
		}
		if (res == ESP_OK) {
		size_t hlen = snprintf((char *)part_buf, 128, _STREAM_PART, _jpg_buf_len, _timestamp.tv_sec, _timestamp.tv_usec);
		res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
		}
		if (res == ESP_OK) {
		res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
		}
		if (fb) {
		esp_camera_fb_return(fb);
		fb = NULL;
		_jpg_buf = NULL;
		} else if (_jpg_buf) {
		free(_jpg_buf);
		_jpg_buf = NULL;
		}
		if (res != ESP_OK) {
		log_e("Send frame failed");
		break;
		}
		int64_t fr_end = esp_timer_get_time();

		int64_t frame_time = fr_end - last_frame;
		last_frame = fr_end;

		frame_time /= 1000;
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
    	uint32_t avg_frame_time = ra_filter_run(&ra_filter, frame_time);
#endif
		log_i(
		"MJPG: %uB %ums (%.1ffps), AVG: %ums (%.1ffps)", (uint32_t)(_jpg_buf_len), (uint32_t)frame_time, 1000.0 / (uint32_t)frame_time, avg_frame_time,
		1000.0 / avg_frame_time
		);
  	}

#if defined(LED_GPIO_NUM)
	isStreaming = false;
	enable_led(false);
#endif
  	return res;
}

static esp_err_t audio_stream_handler(httpd_req_t *req) {
    esp_err_t res = ESP_OK;
    size_t bytes_read = 0;
    int32_t i2s_in[512];
    int16_t pcm_out[512];
    static int32_t dc_sum = 0;

    // Set header to send raw binary data
    res = httpd_resp_set_type(req, "application/octet-stream");
    if (res != ESP_OK) return res;
    
    // Allow browsers to access this stream
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    // Infinite loop to stream audio as long as the client is connected
    while (true) {
        i2s_read(I2S_NUM_0, (void*)i2s_in, sizeof(i2s_in), &bytes_read, portMAX_DELAY);
        int samples = bytes_read / sizeof(int32_t);
        
        // Apply your high-pass filter
        for (int i = 0; i < samples; i++) {
            dc_sum = (dc_sum * 15 + i2s_in[i]) >> 4; 
            int32_t filtered = i2s_in[i] - dc_sum;
            pcm_out[i] = (int16_t)(filtered >> 14);
        }
        
        // Send the filtered 16-bit audio chunk over Wi-Fi
        res = httpd_resp_send_chunk(req, (const char *)pcm_out, samples * sizeof(int16_t));
        
        // If the chunk fails to send (client disconnected), break the loop
        if (res != ESP_OK) {
            break;
        }
    }
    return res;
}

void startServer() {
	httpd_config_t config = HTTPD_DEFAULT_CONFIG();
	config.max_uri_handlers = 4; //apparently reduces memory overhead

	httpd_uri_t video_stream_uri = {
		.uri = "/video_stream",
		.method = HTTP_GET,
		.handler = video_stream_handler,
		.user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
		,
		.is_websocket = true,
		.handle_ws_control_frames = false,
		.supported_subprotocol = NULL
#endif
  	};

	httpd_uri_t audio_stream_uri = {
		.uri = "/audio_stream",
		.method = HTTP_GET,
		.handler = audio_stream_handler,
		.user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
		,
		.is_websocket = true,
		.handle_ws_control_frames = false,
		.supported_subprotocol = NULL
#endif
  	};

	ra_filter_init(&ra_filter, 20);

	log_i("Starting stream server on port: '%d'", config.server_port);
	if (httpd_start(&stream_httpd, &config) == ESP_OK) {
		httpd_register_uri_handler(stream_httpd, &video_stream_uri);
		httpd_register_uri_handler(stream_httpd, &audio_stream_uri);
	}
}

void setupLedFlash() {
#if defined(LED_GPIO_NUM)
	ledcSetup(LED_LEDC_CHANNEL, 5000, 8);
	ledcAttachPin(LED_GPIO_NUM, LED_LEDC_CHANNEL);
#else
  	log_i("LED flash is disabled -> LED_GPIO_NUM undefined");
#endif
}
