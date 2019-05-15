#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event_loop.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "time.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "cJSON.h"


static const char *TAG = "MQTT_EXAMPLE";

static EventGroupHandle_t wifi_event_group;
static EventGroupHandle_t mqtt_event_group;

//TimerHandle_t RountripTimer;
unsigned long lastMillis = 0;   // time since last publish
unsigned long Timeout = 10000;  // timeout is set to 10 seconds
bool Start;                     // Start check bit
int value = 0;                  //number to send
bool GotAck;                    // acknowledgment check bit
int period = 1000;         //specify perdiod duration
int payload_size=0;
int qos =0;               //QOS level
cJSON *Jmessage;
cJSON *init;
cJSON *freq;
cJSON *pay_size;
cJSON *qualityof;
char* payload;

const static int CONNECTED_BIT = BIT0;
static esp_mqtt_client_handle_t mqtt_client = NULL;



char *gen_string(size_t length) {       
    
    char *varString = NULL;

    if (length) {
        varString = malloc(sizeof(char) * (length +1));

        if (varString) {            
            for (int n = 0;n < length;n++) {            
                varString[n] = 'S';
            }

            varString[length] = '\0';
        }
    }

    return varString;
}


static esp_err_t mqtt_event_handler(esp_mqtt_event_handle_t event)
{
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    // your_context_t *context = event->context;
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
            xEventGroupSetBits(mqtt_event_group, CONNECTED_BIT);
            msg_id = esp_mqtt_client_publish(client, "ESP32/Status", "Connected", 0, 0, 0);
            ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
            msg_id = esp_mqtt_client_subscribe(client, "ESP32/ack", 0);
            ESP_LOGI(TAG, "subscribe ack successful, msg_id=%d", msg_id);
            msg_id = esp_mqtt_client_subscribe(client, "ESP32/start", 0);
            ESP_LOGI(TAG, "subscribe start successful, msg_id=%d", msg_id);
            msg_id = esp_mqtt_client_subscribe(client, "ESP32/kill", 0);
            ESP_LOGI(TAG, "Subscribe kill successful, msg_id=%d", msg_id);
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
            break;
        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
            msg_id = esp_mqtt_client_publish(client, "ESP32/Status", "Subscribed", 0, 0, 0);
            ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
            break;
        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "MQTT_EVENT_DATA");
            printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
            printf("DATA=%.*s\r\n", event->data_len, event->data);
            ESP_LOGI(TAG, "Free memory after recieving: %d bytes", esp_get_free_heap_size());
            if (strncmp((event->topic), "ESP32/ack" , event->topic_len) == 0 && Start == 1){
            char message[12];
            cJSON_DeleteItemFromObject(Jmessage, "roundtrip_time");
            cJSON_AddStringToObject(Jmessage, "roundtrip_time", itoa((esp_log_timestamp() - lastMillis),message,10));
            msg_id = esp_mqtt_client_publish(client, "ESP32/finaldata", cJSON_Print(Jmessage), 0, 0, 0);
            GotAck = 1; //enable ack bit to verify
            ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
            printf("Roundtrip=%lu\n", esp_log_timestamp() - lastMillis);
            }
            else if (strncmp((event->topic), "ESP32/start" , event->topic_len) == 0 && Start == 0 ){
                ESP_LOGI(TAG, "Start_Code, msg_id=%d", event->msg_id);
                init = cJSON_Parse(event->data);
                freq = cJSON_GetObjectItemCaseSensitive(init, "frequency");
                pay_size = cJSON_GetObjectItemCaseSensitive(init, "payload");
                qualityof = cJSON_GetObjectItemCaseSensitive(init, "qos");
                period = freq->valueint;
                payload_size = pay_size->valueint;
                qos = qualityof->valueint;
                payload = gen_string(payload_size);
                cJSON_AddStringToObject(Jmessage, "message", payload);
                ESP_LOGI(TAG, "init data is %d %d %d %s", period, payload_size, qos,payload);
                Start = 1; // enable Start/tansmission bit
                ESP_LOGI(TAG, "transmit is %d", event->msg_id);

            }
            else if (strncmp((event->topic), "ESP32/kill" , event->topic_len) == 0 && Start == 1){
                ESP_LOGI(TAG, "Kill_Code, msg_id=%d", event->msg_id);
                Start = 0; // disable start/tranmsission bit
                free(payload);
                vTaskDelay(100);
                esp_restart();
            }
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
            break;
        default:
            ESP_LOGI(TAG, "Other event id:%d", event->event_id);
            break;
    }
    return ESP_OK;
}

static esp_err_t wifi_event_handler(void *ctx, system_event_t *event)
{
    switch (event->event_id) {
        case SYSTEM_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case SYSTEM_EVENT_STA_GOT_IP:
            xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
            //tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ipInfo);
            //printf(str, "%x", ipInfo.ip.addr);

            break;
        case SYSTEM_EVENT_STA_DISCONNECTED:
            esp_wifi_connect();
            xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
            break;
        default:
            break;
    }
    return ESP_OK;
}

static void wifi_init(void)
{
    tcpip_adapter_init();
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_loop_init(wifi_event_handler, NULL));
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = "Shams",
            .password = "testpass",
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_LOGI(TAG, "start the WIFI SSID:[%s]", CONFIG_WIFI_SSID);
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Waiting for wifi");
    xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, portMAX_DELAY);
}

static void mqtt_app_start(void)
{
    mqtt_event_group = xEventGroupCreate();
    esp_mqtt_client_config_t mqtt_cfg = {
        .uri = "mqtt://192.168.43.12",
        .event_handle = mqtt_event_handler,
        // .user_context = (void *)your_context
    };

#if CONFIG_BROKER_URL_FROM_STDIN
    char line[128];

    if (strcmp(mqtt_cfg.uri, "FROM_STDIN") == 0) {
        int count = 0;
        printf("Please enter url of mqtt broker\n");
        while (count < 128) {
            int c = fgetc(stdin);
            if (c == '\n') {
                line[count] = '\0';
                break;
            } else if (c > 0 && c < 127) {
                line[count] = c;
                ++count;
            }
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
        mqtt_cfg.uri = line;
        printf("Broker url: %s\n", line);
    } else {
        ESP_LOGE(TAG, "Configuration mismatch: wrong broker url");
        abort();
    }
#endif /* CONFIG_BROKER_URL_FROM_STDIN */

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    //esp_mqtt_client_start(client);
    
}


void app_main()
{
    
    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("MQTT_CLIENT", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT_TCP", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT_SSL", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT", ESP_LOG_VERBOSE);
    esp_log_level_set("OUTBOX", ESP_LOG_VERBOSE);
    Jmessage = cJSON_CreateObject();
    nvs_flash_init();
    wifi_init();
    mqtt_app_start();
    xEventGroupClearBits(mqtt_event_group, CONNECTED_BIT);
    esp_mqtt_client_start(mqtt_client);
    xEventGroupWaitBits(mqtt_event_group, CONNECTED_BIT, false, true, portMAX_DELAY);
    vTaskDelay(1000);
    char message[12];
    cJSON_AddStringToObject(Jmessage, "sensor_data", "");
    cJSON_AddStringToObject(Jmessage, "roundtrip_time", "");
    Start = 0; //initilize Start bit to 0;
    GotAck = 1;//initilize ack bit to 1;

    while (1){
    if (Start == 1 && (esp_log_timestamp() - lastMillis) > period && (GotAck == 1 || ((esp_log_timestamp() - lastMillis) > Timeout))){
    GotAck = 0; //reset ack to 0 (wait for ack before next tranmission)
    cJSON_DeleteItemFromObject(Jmessage, "sensor_data");                 //delete current "text" element
    cJSON_AddStringToObject(Jmessage, "sensor_data", itoa(value, message, 10)); //add text element with new value
    int msg_id = esp_mqtt_client_publish(mqtt_client, "ESP32/data", itoa(value,message,10), 0, qos, 0);
    value++;
    lastMillis = esp_log_timestamp(); //capture timestamp of las publish
    ESP_LOGI(TAG, "[%d] Publishing...", msg_id);
    ESP_LOGI(TAG, "Free memory after publishing: %d bytes", esp_get_free_heap_size());
    //vTaskDelay(freq);
    }
    else {}
    }
}
