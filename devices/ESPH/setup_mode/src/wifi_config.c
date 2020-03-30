#include <stdio.h>
#include <string.h>
//#include <stdint.h>
#include <sysparam.h>
#include <etstimer.h>
#include <esplibs/libmain.h>
#include <espressif/esp_common.h>
#include <lwip/sockets.h>
#include <lwip/dhcp.h>

#include <semphr.h>

#include <http-parser/http_parser.h>
#include <dhcpserver.h>

#include "form_urlencoded.h"

#include <rboot-api.h>
#include <homekit/homekit.h>

#define WIFI_CONFIG_SERVER_PORT         80

#ifndef AUTO_REBOOT_TIMEOUT
#define AUTO_REBOOT_TIMEOUT             90000
#endif

#define INFO(message, ...)              printf(message "\n", ##__VA_ARGS__);
#define ERROR(message, ...)             printf("! " message "\n", ##__VA_ARGS__);

typedef enum {
    ENDPOINT_UNKNOWN = 0,
    ENDPOINT_INDEX,
    ENDPOINT_SETTINGS,
    ENDPOINT_SETTINGS_UPDATE,
} endpoint_t;

typedef struct {
    char *ssid_prefix;
    char *password;
    char *custom_hostname;
    void (*on_wifi_ready)();

    ETSTimer sta_connect_timeout;
    TaskHandle_t http_task_handle;
    TaskHandle_t dns_task_handle;
} wifi_config_context_t;

static wifi_config_context_t *context;

typedef struct _client {
    int fd;

    http_parser parser;
    endpoint_t endpoint;
    uint8_t *body;
    size_t body_length;
} client_t;

ETSTimer auto_reboot_timer;

static void wifi_config_station_connect();
static void wifi_config_softap_start();
static void wifi_config_softap_stop();

static client_t *client_new() {
    client_t *client = malloc(sizeof(client_t));
    memset(client, 0, sizeof(client_t));

    http_parser_init(&client->parser, HTTP_REQUEST);
    client->parser.data = client;

    return client;
}

static void client_free(client_t *client) {
    if (client->body) {
        free(client->body);
    }

    free(client);
}

static void client_send(client_t *client, const char *payload, size_t payload_size) {
    lwip_write(client->fd, payload, payload_size);
}

static void client_send_chunk(client_t *client, const char *payload) {
    int len = strlen(payload);
    char buffer[10];
    int buffer_len = snprintf(buffer, sizeof(buffer), "%x\r\n", len);
    client_send(client, buffer, buffer_len);
    client_send(client, payload, len);
    client_send(client, "\r\n", 2);
}

static void client_send_redirect(client_t *client, int code, const char *redirect_url) {
    INFO("Redirecting to %s", redirect_url);
    char buffer[128];
    size_t len = snprintf(buffer, sizeof(buffer), "HTTP/1.1 %d \r\nLocation: %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", code, redirect_url);
    client_send(client, buffer, len);
}

typedef struct _wifi_network_info {
    char ssid[33];
    char bssid[7];
    char rssi[4];
    char channel[3];
    bool secure;

    struct _wifi_network_info *next;
} wifi_network_info_t;

void wifi_config_reset() {
    struct sdk_station_config sta_config;
    memset(&sta_config, 0, sizeof(sta_config));

    strncpy((char *)sta_config.ssid, "none", sizeof(sta_config.ssid));
    sta_config.ssid[sizeof(sta_config.ssid) - 1] = 0;
    sdk_wifi_station_set_config(&sta_config);
    sdk_wifi_station_connect();
    sdk_wifi_station_set_auto_connect(true);
}

wifi_network_info_t *wifi_networks = NULL;
SemaphoreHandle_t wifi_networks_mutex;

static void wifi_scan_done_cb(void *arg, sdk_scan_status_t status) {
    if (status != SCAN_OK) {
        ERROR("WiFi scan failed");
        return;
    }

    xSemaphoreTake(wifi_networks_mutex, portMAX_DELAY);

    wifi_network_info_t *wifi_network = wifi_networks;
    while (wifi_network) {
        wifi_network_info_t *next = wifi_network->next;
        free(wifi_network);
        wifi_network = next;
    }
    wifi_networks = NULL;

    struct sdk_bss_info *bss = (struct sdk_bss_info *)arg;
    // first one is invalid
    bss = bss->next.stqe_next;

    while (bss) {
        //INFO("%s (%i) Ch %i - %02x%02x%02x%02x%02x%02x", bss->ssid, bss->rssi, bss->channel, bss->bssid[0], bss->bssid[1], bss->bssid[2], bss->bssid[3], bss->bssid[4], bss->bssid[5]);

        wifi_network_info_t *net = wifi_networks;
        while (net) {
            if (!strncmp(net->bssid, (char *)bss->bssid, sizeof(net->bssid)))
                break;
            net = net->next;
        }
        if (!net) {
            wifi_network_info_t *net = malloc(sizeof(wifi_network_info_t));
            memset(net, 0, sizeof(*net));
            strncpy(net->ssid, (char *)bss->ssid, sizeof(net->ssid));
            strncpy(net->bssid, (char *)bss->bssid, sizeof(net->bssid));
            itoa(bss->rssi, net->rssi, 10);
            itoa(bss->channel, net->channel, 10);
            net->secure = bss->authmode != AUTH_OPEN;
            net->next = wifi_networks;

            wifi_networks = net;
        }

        bss = bss->next.stqe_next;
    }

    xSemaphoreGive(wifi_networks_mutex);
}

static void wifi_scan_task(void *arg) {
    INFO("Start WiFi scan");

    while(context != NULL) {
        sdk_wifi_station_scan(NULL, wifi_scan_done_cb);
        vTaskDelay(10000 / portTICK_PERIOD_MS);
    }
    
    xSemaphoreTake(wifi_networks_mutex, portMAX_DELAY);

    wifi_network_info_t *wifi_network = wifi_networks;
    while (wifi_network) {
        wifi_network_info_t *next = wifi_network->next;
        free(wifi_network);
        wifi_network = next;
    }
    wifi_networks = NULL;

    xSemaphoreGive(wifi_networks_mutex);
    
    vTaskDelete(NULL);
}

#include "index.html.h"

static void wifi_config_server_on_settings(client_t *client) {
    sdk_os_timer_disarm(&auto_reboot_timer);
    
    static const char http_prologue[] =
        "HTTP/1.1 200 \r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Cache-Control: no-store\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: close\r\n"
        "\r\n";

    client_send(client, http_prologue, sizeof(http_prologue)-1);
    client_send_chunk(client, html_settings_header);
    
    char *json = NULL;
    sysparam_status_t status;
    status = sysparam_get_string("esph_conf", &json);
    if (status == SYSPARAM_OK) {
        client_send_chunk(client, json);
        free(json);
    }

    client_send_chunk(client, html_settings_middle);
    
    char *ota = NULL;
    status = sysparam_get_string("ota_repo", &ota);
    if (status == SYSPARAM_OK) {
        client_send_chunk(client, html_settings_ota);
        
        if (strlen(ota) < 10) {
            client_send_chunk(client, "(ESPH OTA v");
            client_send_chunk(client, ota);
            client_send_chunk(client, ")");
        }
        
        free(ota);
        
        client_send_chunk(client, html_settings_otaversion);
        
        bool auto_ota = false;
        status = sysparam_get_bool("aota", &auto_ota);
        if (status == SYSPARAM_OK && auto_ota) {
            client_send_chunk(client, "checked");
        }
        
        client_send_chunk(client, html_settings_autoota);
    }
    
    client_send_chunk(client, html_settings_postota);
    
    int8_t wifi_mode = 0;
    sysparam_get_int8("wifi_mode", &wifi_mode);
    if (wifi_mode == 0) {
        client_send_chunk(client, "selected");
    }
    client_send_chunk(client, html_wifi_mode_0);
    
    if (wifi_mode == 1) {
        client_send_chunk(client, "selected");
    }
    client_send_chunk(client, html_wifi_mode_1);
    
    // WiFi Networks
    char buffer[120];
    char bssid[13];
    if (xSemaphoreTake(wifi_networks_mutex, 5000 / portTICK_PERIOD_MS)) {
        wifi_network_info_t *net = wifi_networks;
        while (net) {
            snprintf(bssid, 13, "%02x%02x%02x%02x%02x%02x", net->bssid[0], net->bssid[1], net->bssid[2], net->bssid[3], net->bssid[4], net->bssid[5]);
            snprintf(
                buffer, sizeof(buffer),
                html_network_item,
                net->secure ? "secure" : "unsecure", bssid, net->ssid, net->ssid, net->rssi, net->channel, bssid
            );
            client_send_chunk(client, buffer);

            net = net->next;
        }

        xSemaphoreGive(wifi_networks_mutex);
    }

    client_send_chunk(client, html_settings_footer);
    client_send_chunk(client, "");
}

static void wifi_config_server_on_settings_update(client_t *client) {
    INFO("Update settings, body = %s", client->body);
    
    form_param_t *form = form_params_parse((char *)client->body);
    if (!form) {
        client_send_redirect(client, 302, "/settings");
        return;
    }

    form_param_t *conf_param = form_params_find(form, "conf");
    form_param_t *reset_param = form_params_find(form, "reset");
    form_param_t *nowifi_param = form_params_find(form, "nowifi");
    form_param_t *ota_param = form_params_find(form, "ota");
    form_param_t *autoota_param = form_params_find(form, "autoota");
    form_param_t *wifimode_param = form_params_find(form, "wifimode");
    form_param_t *ssid_param = form_params_find(form, "ssid");
    form_param_t *bssid_param = form_params_find(form, "bssid");
    form_param_t *password_param = form_params_find(form, "password");
    
    static const char payload[] = "HTTP/1.1 204 \r\nContent-Type: text/html\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    client_send(client, payload, sizeof(payload) - 1);
    
    // Remove saved states
    int8_t hk_total_ac = 0;
    sysparam_get_int8("total_ac", &hk_total_ac);
    char saved_state_id[5];
    for (uint16_t int_saved_state_id = 100; int_saved_state_id <= hk_total_ac * 100; int_saved_state_id++) {
        itoa(int_saved_state_id, saved_state_id, 10);
        sysparam_set_data(saved_state_id, NULL, 0, false);
    }

    if (conf_param->value) {
        sysparam_set_string("esph_conf", conf_param->value);
    }
    
    if (autoota_param) {
        sysparam_set_bool("aota", true);
    } else {
        sysparam_set_bool("aota", false);
    }
    
    if (ota_param) {
        rboot_set_temp_rom(1);
    }
    
    if (nowifi_param) {
        sysparam_set_data("wifi_ssid", NULL, 0, false);
        sysparam_set_data("wifi_password", NULL, 0, false);
    }
    
    if (reset_param) {
        homekit_server_reset();
    }
    
    if (ssid_param->value) {
        sysparam_set_string("wifi_ssid", ssid_param->value);

        if (bssid_param->value && strlen(bssid_param->value) == 12) {
            char *bssid = malloc(7);
            char *hex = malloc(3);
            
            for (uint8_t i = 0; i < 6; i++) {
                hex[0] = bssid_param->value[(i * 2)];
                hex[1] = bssid_param->value[(i * 2) + 1];
                bssid[i] = (char) strtol(hex, NULL, 16);
            }

            sysparam_set_string("wifi_bssid", bssid);
            
            free(hex);
            free(bssid);
        } else {
            sysparam_set_string("wifi_bssid", "");
        }
        
        if (password_param->value) {
            sysparam_set_string("wifi_password", password_param->value);
        } else {
            sysparam_set_string("wifi_password", "");
        }
    }
    
    sysparam_compact();
    
    if (wifimode_param->value) {
        int8_t current_wifi_mode = 0;
        int8_t new_wifi_mode = strtol(wifimode_param->value, NULL, 10);
        sysparam_get_int8("wifi_mode", &current_wifi_mode);
        sysparam_set_int8("wifi_mode", new_wifi_mode);
        
        if (current_wifi_mode != new_wifi_mode) {
            wifi_config_reset();
        }
    }
    
    INFO("Rebooting...");
    vTaskDelay(250 / portTICK_PERIOD_MS);
    sdk_system_restart();
}

static int wifi_config_server_on_url(http_parser *parser, const char *data, size_t length) {
    client_t *client = (client_t*) parser->data;

    client->endpoint = ENDPOINT_UNKNOWN;
    if (parser->method == HTTP_GET) {
        if (!strncmp(data, "/settings", length)) {
            client->endpoint = ENDPOINT_SETTINGS;
        } else if (!strncmp(data, "/", length)) {
            client->endpoint = ENDPOINT_INDEX;
        }
    } else if (parser->method == HTTP_POST) {
        if (!strncmp(data, "/settings", length)) {
            client->endpoint = ENDPOINT_SETTINGS_UPDATE;
        }
    }

    if (client->endpoint == ENDPOINT_UNKNOWN) {
        char *url = strndup(data, length);
        ERROR("Unknown: %s %s", http_method_str(parser->method), url);
        free(url);
    }

    return 0;
}

static int wifi_config_server_on_body(http_parser *parser, const char *data, size_t length) {
    client_t *client = parser->data;
    client->body = realloc(client->body, client->body_length + length + 1);
    memcpy(client->body + client->body_length, data, length);
    client->body_length += length;
    client->body[client->body_length] = 0;

    return 0;
}

static void wifi_config_context_free(wifi_config_context_t *context) {
    if (context->ssid_prefix)
        free(context->ssid_prefix);

    if (context->password)
        free(context->password);

    free(context);
    context = NULL;
}

static int wifi_config_server_on_message_complete(http_parser *parser) {
    client_t *client = parser->data;

    switch(client->endpoint) {
        case ENDPOINT_INDEX: {
            client_send_redirect(client, 301, "/settings");
            break;
        }
        case ENDPOINT_SETTINGS: {
            wifi_config_server_on_settings(client);
            break;
        }
        case ENDPOINT_SETTINGS_UPDATE: {
            wifi_config_context_free(context);
            wifi_config_server_on_settings_update(client);
            break;
        }
        case ENDPOINT_UNKNOWN: {
            INFO("Unknown");
            client_send_redirect(client, 302, "http://192.168.4.1/settings");
            break;
        }
    }

    if (client->body) {
        free(client->body);
        client->body = NULL;
        client->body_length = 0;
    }

    return 0;
}

static http_parser_settings wifi_config_http_parser_settings = {
    .on_url = wifi_config_server_on_url,
    .on_body = wifi_config_server_on_body,
    .on_message_complete = wifi_config_server_on_message_complete,
};

static void http_task(void *arg) {
    INFO("Start HTTP server");

    struct sockaddr_in serv_addr;
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    memset(&serv_addr, '0', sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(WIFI_CONFIG_SERVER_PORT);
    int flags;
    if ((flags = lwip_fcntl(listenfd, F_GETFL, 0)) < 0) {
        ERROR("Get HTTP socket flags");
        lwip_close(listenfd);
        vTaskDelete(NULL);
        return;
    };
    if (lwip_fcntl(listenfd, F_SETFL, flags | O_NONBLOCK) < 0) {
        ERROR("Set HTTP socket flags");
        lwip_close(listenfd);
        vTaskDelete(NULL);
        return;
    }
    bind(listenfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
    listen(listenfd, 2);

    char data[64];

    bool running = true;
    while (running) {
        uint32_t task_value = 0;
        if (xTaskNotifyWait(0, 1, &task_value, 0) == pdTRUE) {
            if (task_value) {
                running = false;
                break;
            }
        }

        int fd = accept(listenfd, (struct sockaddr *)NULL, (socklen_t *)NULL);
        if (fd < 0) {
            vTaskDelay(500 / portTICK_PERIOD_MS);
            continue;
        }

        const struct timeval timeout = { 2, 0 }; /* 2 second timeout */
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        client_t *client = client_new();
        client->fd = fd;

        for (;;) {
            int data_len = lwip_read(client->fd, data, sizeof(data));
            if (data_len == 0) {
                break;
            }

            if (data_len > 0) {
                INFO("Got %d bytes", data_len);

                http_parser_execute(
                    &client->parser, &wifi_config_http_parser_settings,
                    data, data_len
                );
            }

            if (xTaskNotifyWait(0, 1, &task_value, 0) == pdTRUE) {
                if (task_value) {
                    running = false;
                    break;
                }
            }
        }

        INFO("Client disconnected");

        lwip_close(client->fd);
        client_free(client);
    }

    INFO("Stop HTTP server");

    lwip_close(listenfd);
    vTaskDelete(NULL);
}

static void http_start() {
    xTaskCreate(http_task, "http_task", 512, NULL, 2, &context->http_task_handle);
}

static void http_stop() {
    if (! context->http_task_handle) {
        return;
    }

    xTaskNotify(context->http_task_handle, 1, eSetValueWithOverwrite);
}

static void dns_task(void *arg)
{
    INFO("Start DNS server");

    ip4_addr_t server_addr;
    IP4_ADDR(&server_addr, 192, 168, 4, 1);

    struct sockaddr_in serv_addr;
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    memset(&serv_addr, '0', sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(53);
    bind(fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));

    const struct timeval timeout = { 2, 0 }; /* 2 second timeout */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    const struct ifreq ifreq1 = { "en1" };
    setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, &ifreq1, sizeof(ifreq1));

    for (;;) {
        char buffer[96];
        struct sockaddr src_addr;
        socklen_t src_addr_len = sizeof(src_addr);
        size_t count = recvfrom(fd, buffer, sizeof(buffer), 0, (struct sockaddr*)&src_addr, &src_addr_len);

        /* Drop messages that are too large to send a response in the buffer */
        if (count > 0 && count <= sizeof(buffer) - 16 && src_addr.sa_family == AF_INET) {
            size_t qname_len = strlen(buffer + 12) + 1;
            uint32_t reply_len = 2 + 10 + qname_len + 16 + 4;

            char *head = buffer + 2;
            *head++ = 0x80; // Flags
            *head++ = 0x00;
            *head++ = 0x00; // Q count
            *head++ = 0x01;
            *head++ = 0x00; // A count
            *head++ = 0x01;
            *head++ = 0x00; // Auth count
            *head++ = 0x00;
            *head++ = 0x00; // Add count
            *head++ = 0x00;
            head += qname_len;
            *head++ = 0x00; // Q type
            *head++ = 0x01;
            *head++ = 0x00; // Q class
            *head++ = 0x01;
            *head++ = 0xC0; // LBL offs
            *head++ = 0x0C;
            *head++ = 0x00; // Type
            *head++ = 0x01;
            *head++ = 0x00; // Class
            *head++ = 0x01;
            *head++ = 0x00; // TTL
            *head++ = 0x00;
            *head++ = 0x00;
            *head++ = 0x78;
            *head++ = 0x00; // RD len
            *head++ = 0x04;
            *head++ = ip4_addr1(&server_addr);
            *head++ = ip4_addr2(&server_addr);
            *head++ = ip4_addr3(&server_addr);
            *head++ = ip4_addr4(&server_addr);

            sendto(fd, buffer, reply_len, 0, &src_addr, src_addr_len);
        }

        uint32_t task_value = 0;
        if (xTaskNotifyWait(0, 1, &task_value, 0) == pdTRUE) {
            if (task_value)
                break;
        }
    }

    INFO("Stop DNS server");

    lwip_close(fd);

    vTaskDelete(NULL);
}

static void dns_start() {
    xTaskCreate(dns_task, "dns_task", 384, NULL, 2, &context->dns_task_handle);
}

static void dns_stop() {
    if (!context->dns_task_handle)
        return;

    xTaskNotify(context->dns_task_handle, 1, eSetValueWithOverwrite);
}

static void wifi_config_softap_start() {
    sdk_wifi_set_opmode(STATIONAP_MODE);

    uint8_t macaddr[6];
    sdk_wifi_get_macaddr(SOFTAP_IF, macaddr);

    struct sdk_softap_config softap_config;
    softap_config.ssid_len = snprintf(
        (char *)softap_config.ssid, sizeof(softap_config.ssid),
        "%s-%02X%02X%02X", context->ssid_prefix, macaddr[3], macaddr[4], macaddr[5]
    );
    softap_config.ssid_hidden = 0;
    softap_config.channel = 6;
    if (context->password) {
        softap_config.authmode = AUTH_WPA_WPA2_PSK;
        strncpy((char *)softap_config.password,
                context->password, sizeof(softap_config.password));
    } else {
        softap_config.authmode = AUTH_OPEN;
    }
    softap_config.max_connection = 2;
    softap_config.beacon_interval = 100;

    INFO("Start AP SSID=%s", softap_config.ssid);

    struct ip_info ap_ip;
    IP4_ADDR(&ap_ip.ip, 192, 168, 4, 1);
    IP4_ADDR(&ap_ip.netmask, 255, 255, 255, 0);
    IP4_ADDR(&ap_ip.gw, 0, 0, 0, 0);
    sdk_wifi_set_ip_info(SOFTAP_IF, &ap_ip);

    sdk_wifi_softap_set_config(&softap_config);

    ip4_addr_t first_client_ip;
    first_client_ip.addr = ap_ip.ip.addr + htonl(1);

    wifi_networks_mutex = xSemaphoreCreateBinary();
    xSemaphoreGive(wifi_networks_mutex);

    xTaskCreate(wifi_scan_task, "wifi_scan_task", 384, NULL, 2, NULL);

    INFO("Start DHCP server");
    dhcpserver_start(&first_client_ip, 4);
    dhcpserver_set_router(&ap_ip.ip);
    dhcpserver_set_dns(&ap_ip.ip);

    dns_start();
    http_start();
}

static void wifi_config_softap_stop() {
    LOCK_TCPIP_CORE();
    dhcpserver_stop();
    dns_stop();
    sdk_wifi_set_opmode(STATION_MODE);
    UNLOCK_TCPIP_CORE();
}

static void wifi_config_sta_connect_timeout_callback(void *arg) {
    struct netif *netif = sdk_system_get_netif(STATION_IF);
    if (netif && !netif->hostname && context->custom_hostname) {
        LOCK_TCPIP_CORE();
        dhcp_release_and_stop(netif);
        netif->hostname = context->custom_hostname;
        dhcp_start(netif);
        UNLOCK_TCPIP_CORE();
        INFO("Hostname: %s", context->custom_hostname);
    }
    
    if (sdk_wifi_station_get_connect_status() == STATION_GOT_IP) {
        // Connected to station, all is dandy
        sdk_os_timer_disarm(&context->sta_connect_timeout);
        
        wifi_config_softap_stop();
        
        if (context->on_wifi_ready) {
            http_stop();
            context->on_wifi_ready();
            
            wifi_config_context_free(context);
        }
    }
}

static void auto_reboot_run() {
    bool auto_ota = false;
    sysparam_get_bool("aota", &auto_ota);
    if (auto_ota) {
        INFO("OTA Update");
        rboot_set_temp_rom(1);
    }

    INFO("Auto Reboot");
    vTaskDelay(150 / portTICK_PERIOD_MS);
    
    sdk_system_restart();
}

bool wifi_config_connect() {
    char *wifi_ssid = NULL;
    sysparam_get_string("wifi_ssid", &wifi_ssid);
    
    if (wifi_ssid) {
        struct sdk_station_config sta_config;
               
        memset(&sta_config, 0, sizeof(sta_config));
        strncpy((char *)sta_config.ssid, wifi_ssid, sizeof(sta_config.ssid));
        sta_config.ssid[sizeof(sta_config.ssid) - 1] = 0;

        char *wifi_password = NULL;
        sysparam_get_string("wifi_password", &wifi_password);
        if (wifi_password) {
           strncpy((char *)sta_config.password, wifi_password, sizeof(sta_config.password));
        }

        int8_t wifi_mode = 0;
        sysparam_get_int8("wifi_mode", &wifi_mode);
        if (wifi_mode == 1) {
            char *wifi_bssid = NULL;
            sysparam_get_string("wifi_bssid", &wifi_bssid);
            if (wifi_bssid) {
                sta_config.bssid_set = 1;
                strncpy((char *)sta_config.bssid, wifi_bssid, sizeof(sta_config.bssid));
                printf("WiFi Mode: Forced BSSID %02x%02x%02x%02x%02x%02x\n", sta_config.bssid[0], sta_config.bssid[1], sta_config.bssid[2], sta_config.bssid[3], sta_config.bssid[4], sta_config.bssid[5]);
               
                free(wifi_bssid);
            }
        } else {
            INFO("WiFi Mode: Normal");
            sta_config.bssid_set = 0;
        }

        sdk_wifi_station_set_config(&sta_config);
        sdk_wifi_station_connect();
        sdk_wifi_station_set_auto_connect(true);
        
        free(wifi_ssid);
        
        if (wifi_password) {
            free(wifi_password);
        }
        
        return true;
    } else {
        INFO("No WiFi config found");
    }
    
    return false;
}

static void wifi_config_station_connect() {
    if (wifi_config_connect()) {
        wifi_config_sta_connect_timeout_callback(context);
        
        sdk_os_timer_setfn(&context->sta_connect_timeout, wifi_config_sta_connect_timeout_callback, context);
        sdk_os_timer_arm(&context->sta_connect_timeout, 1000, 1);
        
        if (!context->on_wifi_ready) {
            INFO("ESPH Setup");
            
            int8_t mode = 0;
            sysparam_get_int8("setup", &mode);
            
            sysparam_set_int8("setup", 0);

            if (mode == 1) {
                INFO("Enabling auto reboot");
                sdk_os_timer_setfn(&auto_reboot_timer, auto_reboot_run, NULL);
                sdk_os_timer_arm(&auto_reboot_timer, AUTO_REBOOT_TIMEOUT, 0);
            }
            
            wifi_config_softap_start();
        }
    } else {
        wifi_config_softap_start();
    }
}

void wifi_config_init(const char *ssid_prefix, const char *password, void (*on_wifi_ready)(), const char *custom_hostname) {
    INFO("WiFi config init");
    if (password && strlen(password) < 8) {
        ERROR("Password must be at least 8 characters");
        return;
    }

    context = malloc(sizeof(wifi_config_context_t));
    memset(context, 0, sizeof(*context));

    context->ssid_prefix = strndup(ssid_prefix, 33-7);
    if (password)
        context->password = strdup(password);
    
    if (custom_hostname)
        context->custom_hostname = strdup(custom_hostname);

    context->on_wifi_ready = on_wifi_ready;

    wifi_config_station_connect();
}
