/*
 * ESPH
 * 
 * Copyright 2020 ESPH
 *  
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <esp/uart.h>
//#include <esp8266.h>
//#include <FreeRTOS.h>
//#include <espressif/esp_wifi.h>
#include <espressif/esp_common.h>
#include <rboot-api.h>
#include <sysparam.h>
//#include <task.h>
#include <math.h>

//#include <etstimer.h>
#include <esplibs/libmain.h>

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"

#include <homekit/homekit.h>
#include <homekit/characteristics.h>
#include <adv_button.h>
#include <ping.h>

#include <multipwm/multipwm.h>

#include <dht.h>
#include <ds18b20/ds18b20.h>

#include <cJSON.h>

#include "setup_mode/include/wifi_config.h"
#include "../common/ir_code.h"
#include "header.h"
#include "types.h"

uint8_t wifi_status = WIFI_STATUS_CONNECTED;
uint8_t wifi_channel = 0;
int8_t setup_mode_toggle_counter = INT8_MIN;
int8_t setup_mode_toggle_counter_max = SETUP_MODE_DEFAULT_ACTIVATE_COUNT;
uint8_t led_gpio = 255;
uint16_t setup_mode_time = 0;
ETSTimer *setup_mode_toggle_timer;
ETSTimer save_states_timer, wifi_watchdog_timer;
bool used_gpio[17];
bool led_inverted = true;
bool enable_homekit_server = true;
bool allow_insecure = false;
bool log_output = false;

bool ir_tx_is_running = false;
uint8_t ir_tx_freq = 13;
uint8_t ir_tx_gpio = 255;
bool ir_tx_inv = false;
char *ir_protocol = NULL;

bool setpwm_is_running = false;
bool setpwm_bool_semaphore = true;
ETSTimer *pwm_timer;
pwm_info_t *pwm_info;
uint16_t multipwm_duty[MULTIPWM_MAX_CHANNELS];
uint16_t pwm_freq = 0;

char name_value[11];
char serial_value[13];

ETSTimer *ping_task_timer;

last_state_t *last_states = NULL;
ch_group_t *ch_groups = NULL;
lightbulb_group_t *lightbulb_groups = NULL;
ping_input_t *ping_inputs = NULL;

#ifdef ESPH_DEBUG
ETSTimer free_heap_timer;
uint32_t free_heap = 0;
void free_heap_watchdog() {
    uint32_t new_free_heap = xPortGetFreeHeapSize();
    if (new_free_heap != free_heap) {
        free_heap = new_free_heap;
        INFO2("Free Heap: %d", free_heap);
    }
}
#endif  // ESPH_DEBUG

ch_group_t *ch_group_find(homekit_characteristic_t *ch) {
    ch_group_t *ch_group = ch_groups;
    while (ch_group &&
           ch_group->ch0 != ch &&
           ch_group->ch1 != ch &&
           ch_group->ch2 != ch &&
           ch_group->ch3 != ch &&
           ch_group->ch4 != ch &&
           ch_group->ch5 != ch &&
           ch_group->ch6 != ch &&
           ch_group->ch7 != ch &&
           ch_group->ch_child != ch &&
           ch_group->ch_sec != ch) {
        ch_group = ch_group->next;
    }

    return ch_group;
}

ch_group_t *ch_group_find_by_acc(uint8_t accessory) {
    ch_group_t *ch_group = ch_groups;
    while (ch_group &&
           ch_group->accessory != accessory) {
        ch_group = ch_group->next;
    }

    return ch_group;
}

lightbulb_group_t *lightbulb_group_find(homekit_characteristic_t *ch) {
    lightbulb_group_t *lightbulb_group = lightbulb_groups;
    while (lightbulb_group &&
           lightbulb_group->ch0 != ch) {
        lightbulb_group = lightbulb_group->next;
    }

    return lightbulb_group;
}

void led_task(void *pvParameters) {
    const uint8_t times = (int) pvParameters;
    
    for (uint8_t i=0; i<times; i++) {
        gpio_write(led_gpio, true ^ led_inverted);
        vTaskDelay(MS_TO_TICK(30));
        gpio_write(led_gpio, false ^ led_inverted);
        vTaskDelay(MS_TO_TICK(130));
    }
    
    vTaskDelete(NULL);
}

void led_blink(const int blinks) {
    if (led_gpio != 255) {
        xTaskCreate(led_task, "led_task", LED_TASK_SIZE, (void *) blinks, 1, NULL);
    }
}

void wifi_watchdog() {
    if (wifi_status == WIFI_STATUS_CONNECTED) {
        uint8_t current_channel = sdk_wifi_get_channel();
        if (wifi_channel != current_channel) {
            wifi_status = WIFI_STATUS_PRECONNECTED;
            INFO2("WiFi new Ch%i", current_channel);
            wifi_channel = current_channel;
            homekit_mdns_announce();
        }
    } else if (wifi_status == WIFI_STATUS_DISCONNECTED) {
        wifi_status = WIFI_STATUS_CONNECTING;
        INFO2("WiFi connecting...");
        wifi_config_connect();
        
    } else if (sdk_wifi_station_get_connect_status() == STATION_GOT_IP) {
        if (wifi_status == WIFI_STATUS_CONNECTING) {
            wifi_status = WIFI_STATUS_PRECONNECTED;
            INFO2("WiFi connected");
            homekit_mdns_announce();
            
        } else if (wifi_status == WIFI_STATUS_PRECONNECTED) {
            wifi_status = WIFI_STATUS_CONNECTED;
            wifi_channel = sdk_wifi_get_channel();
            INFO2("mDNS reannounced");
            homekit_mdns_announce();
        }
        
    } else {
        sdk_wifi_station_disconnect();
        led_blink(8);
        ERROR2("WiFi disconnected");

        wifi_status = WIFI_STATUS_DISCONNECTED;
        
        wifi_config_reset();
    }
}

ping_input_t *ping_input_find_by_host(char *host) {
    ping_input_t *ping_input = ping_inputs;
    while (ping_input &&
           strcmp(ping_input->host, host) != 0) {
        ping_input = ping_input->next;
    }

    return ping_input;
}

void ping_task() {
    INFO2("Ping...");
    
    void ping_input_run_callback_fn(ping_input_callback_fn_t *callbacks) {
        ping_input_callback_fn_t *ping_input_callback_fn = callbacks;
        
        while (ping_input_callback_fn) {
            ping_input_callback_fn->callback(0, ping_input_callback_fn->ch, ping_input_callback_fn->param);
            ping_input_callback_fn = ping_input_callback_fn->next;
        }
    }
    
    ping_input_t *ping_input = ping_inputs;
    while (ping_input) {
        bool ping_result = false;
        uint8_t i = 0;
        do {
            i++;
            
            ip_addr_t target_ip;
            const struct addrinfo hints = {
                .ai_family = AF_UNSPEC,
                .ai_socktype = SOCK_RAW
                
            };
            struct addrinfo *res;

            int err = getaddrinfo(ping_input->host, NULL, &hints, &res);

            if (err == 0 && res != NULL) {
                struct sockaddr *sa = res->ai_addr;
                if (sa->sa_family == AF_INET) {
                    struct in_addr ipv4_inaddr = ((struct sockaddr_in*) sa)->sin_addr;
                    memcpy(&target_ip, &ipv4_inaddr, sizeof(target_ip));
                }
#if LWIP_IPV6
                if (sa->sa_family == AF_INET6) {
                    struct in_addr ipv6_inaddr = ((struct sockaddr_in6 *)sa)->sin6_addr;
                    memcpy(&target_ip, &ipv6_inaddr, sizeof(target_ip));
                }
#endif
                ping_result = ping(target_ip);
            }
            
            if (res) {
                freeaddrinfo(res);
            }
    
            vTaskDelay(MS_TO_TICK(40));
        } while (i < PING_RETRIES && !ping_result);
        
        if (ping_result && !ping_input->last_response) {
            ping_input->last_response = true;
            INFO2("Ping %s", ping_input->host);
            ping_input_run_callback_fn(ping_input->callback_1);

        } else if (!ping_result && ping_input->last_response) {
            ping_input->last_response = false;
            ERROR2("Ping %s", ping_input->host);
            ping_input_run_callback_fn(ping_input->callback_0);
        }
        
        ping_input = ping_input->next;
    }
    
    vTaskDelete(NULL);
}

void ping_task_timer_worker() {
    xTaskCreate(ping_task, "ping_task", PING_TASK_SIZE, NULL, PING_TASK_PRIORITY, NULL);
}

// -----
void reboot_task() {
    led_blink(5);
    printf("\nRebooting...\n\n");
    vTaskDelay(MS_TO_TICK(2900));
    sdk_system_restart();
}

void setup_mode_call(const uint8_t gpio, void *args, const uint8_t param) {
    INFO2("Setup mode call");
    
    if (setup_mode_time == 0 || xTaskGetTickCountFromISR() < setup_mode_time * 1000 / portTICK_PERIOD_MS) {
        sysparam_set_int8("setup", 1);
        xTaskCreate(reboot_task, "reboot_task", REBOOT_TASK_SIZE, NULL, 1, NULL);
    } else {
        ERROR2("Not allowed after %i secs since boot", setup_mode_time);
    }
}

void setup_mode_toggle_upcount() {
    if (setup_mode_toggle_counter_max > 0) {
        setup_mode_toggle_counter++;
        sdk_os_timer_arm(setup_mode_toggle_timer, SETUP_MODE_TOGGLE_TIME_MS, 0);
    }
}

void setup_mode_toggle() {
    if (setup_mode_toggle_counter >= setup_mode_toggle_counter_max) {
        setup_mode_call(0, NULL, 0);
    }
    
    setup_mode_toggle_counter = 0;
}

void exit_emergency_setup_mode_task() {
    vTaskDelay(MS_TO_TICK(EXIT_EMERGENCY_SETUP_MODE_TIME));
    
    INFO2("Disarming Emergency Setup Mode");
    sysparam_set_int8("setup", 0);

    // vTaskDelay(MS_TO_TICK(8000)); sdk_wifi_station_disconnect(); sdk_wifi_station_connect();      // Emulates a WiFi disconnection. Keep comment for releases
    
    vTaskDelete(NULL);
}

// -----
void save_states() {
    INFO2("Saving states");
    last_state_t *last_state = last_states;
    sysparam_status_t status;
    
    while (last_state) {
        switch (last_state->ch_type) {
            case CH_TYPE_INT8:
                status = sysparam_set_int8(last_state->id, last_state->ch->value.int_value);
                break;
                
            case CH_TYPE_INT32:
                status = sysparam_set_int32(last_state->id, last_state->ch->value.int_value);
                break;
                
            case CH_TYPE_FLOAT:
                status = sysparam_set_int32(last_state->id, last_state->ch->value.float_value * 100);
                break;
                
            case CH_TYPE_STRING:
                status = sysparam_set_string(last_state->id, last_state->ch->value.string_value);
                break;
                
            default:    // case CH_TYPE_BOOL
                status = sysparam_set_bool(last_state->id, last_state->ch->value.bool_value);
                break;
        }
        
        if (status != SYSPARAM_OK) {
            ERROR2("Flash saving states");
        }
        
        last_state = last_state->next;
    }
}

void save_states_callback() {
    sdk_os_timer_arm(&save_states_timer, 4800, 0);
}

void hkc_group_notify(ch_group_t *ch_group) {
    if (ch_group->ch0) {
        homekit_characteristic_notify(ch_group->ch0, ch_group->ch0->value);
    }
    
    if (ch_group->ch1) {
        homekit_characteristic_notify(ch_group->ch1, ch_group->ch1->value);
        if (ch_group->ch2) {
            homekit_characteristic_notify(ch_group->ch2, ch_group->ch2->value);
            if (ch_group->ch3) {
                homekit_characteristic_notify(ch_group->ch3, ch_group->ch3->value);
                if (ch_group->ch4) {
                    homekit_characteristic_notify(ch_group->ch4, ch_group->ch4->value);
                    if (ch_group->ch5) {
                        homekit_characteristic_notify(ch_group->ch5, ch_group->ch5->value);
                        if (ch_group->ch6) {
                            homekit_characteristic_notify(ch_group->ch6, ch_group->ch6->value);
                            if (ch_group->ch7) {
                                homekit_characteristic_notify(ch_group->ch7, ch_group->ch7->value);
                            }
                        }
                    }
                }
            }
        }
    }
    
    if (ch_group->ch_child) {
        homekit_characteristic_notify(ch_group->ch_child, ch_group->ch_child->value);
    }
    if (ch_group->ch_sec) {
        homekit_characteristic_notify(ch_group->ch_sec, ch_group->ch_sec->value);
    }
}

void hkc_setter(homekit_characteristic_t *ch, const homekit_value_t value) {
    INFO2("Setter");
    ch->value = value;
    hkc_group_notify(ch_group_find(ch));
    
    save_states_callback();
}

void hkc_setter_with_setup(homekit_characteristic_t *ch, const homekit_value_t value) {
    hkc_setter(ch, value);
    
    setup_mode_toggle_upcount();
}

void hkc_autooff_setter_task(void *pvParameters);
void do_actions(ch_group_t *ch_group, const uint8_t int_action);
void do_wildcard_actions(ch_group_t *ch_group, uint8_t index, const float action_value);

// --- ON
void hkc_on_setter(homekit_characteristic_t *ch, const homekit_value_t value) {
    ch_group_t *ch_group = ch_group_find(ch);
    if (!ch_group->ch_sec || ch_group->ch_sec->value.bool_value) {
        if (ch->value.bool_value != value.bool_value) {
            led_blink(1);
            INFO2("Setter ON");
            
            ch->value = value;
            
            do_actions(ch_group, (uint8_t) ch->value.bool_value);
            
            if (ch->value.bool_value && ch_group->num[0] > 0) {
                    autooff_setter_params_t *autooff_setter_params = malloc(sizeof(autooff_setter_params_t));
                    autooff_setter_params->ch = ch;
                    autooff_setter_params->type = TYPE_ON;
                    autooff_setter_params->time = ch_group->num[0];
                    xTaskCreate(hkc_autooff_setter_task, "hkc_autooff_setter_task", AUTOOFF_SETTER_TASK_SIZE, autooff_setter_params, 1, NULL);
                }
            
            setup_mode_toggle_upcount();
            save_states_callback();
            
            if (ch_group->ch2) {
                if (value.bool_value) {
                    ch_group->ch2->value = ch_group->ch1->value;
                    sdk_os_timer_arm(ch_group->timer, 1000, 1);
                } else {
                    ch_group->ch2->value.int_value = 0;
                    sdk_os_timer_disarm(ch_group->timer);
                }
            }
        }
    }
    
    hkc_group_notify(ch_group);
}

void hkc_on_status_setter(homekit_characteristic_t *ch0, const homekit_value_t value) {
    if (ch0->value.bool_value != value.bool_value) {
        led_blink(1);
        INFO2("Setter Status ON");
        ch0->value = value;
        
        hkc_group_notify(ch_group_find(ch0));
    }
}

void on_timer_worker(void *args) {
    homekit_characteristic_t *ch = args;
    ch_group_t *ch_group = ch_group_find(ch);
    
    ch_group->ch2->value.int_value--;
    
    if (ch_group->ch2->value.int_value == 0) {
        sdk_os_timer_disarm(ch_group->timer);
        
        hkc_on_setter(ch, HOMEKIT_BOOL(false));
    }
}

// --- LOCK MECHANISM
void hkc_lock_setter(homekit_characteristic_t *ch, const homekit_value_t value) {
    ch_group_t *ch_group = ch_group_find(ch);
    if (!ch_group->ch_sec || ch_group->ch_sec->value.bool_value) {
        if (ch->value.int_value != value.int_value) {
            led_blink(1);
            INFO2("Setter LOCK");
            
            ch->value = value;
            ch_group->ch0->value = value;
            
            do_actions(ch_group, (uint8_t) ch->value.int_value);
            
            if (ch->value.int_value == 0 && ch_group->num[0] > 0) {
                    autooff_setter_params_t *autooff_setter_params = malloc(sizeof(autooff_setter_params_t));
                    autooff_setter_params->ch = ch;
                    autooff_setter_params->type = TYPE_LOCK;
                    autooff_setter_params->time = ch_group->num[0];
                    xTaskCreate(hkc_autooff_setter_task, "hkc_autooff_setter_task", AUTOOFF_SETTER_TASK_SIZE, autooff_setter_params, 1, NULL);
                }
            
            setup_mode_toggle_upcount();
            save_states_callback();
        }
    }
    
    hkc_group_notify(ch_group);
}

void hkc_lock_status_setter(homekit_characteristic_t *ch, const homekit_value_t value) {
    if (ch->value.int_value != value.int_value) {
        led_blink(1);
        INFO2("Setter Status LOCK");
        ch->value = value;
        ch_group_t *ch_group = ch_group_find(ch);
        ch_group->ch0->value = value;
        
        hkc_group_notify(ch_group);
    }
}

// --- BUTTON EVENT
void button_event(const uint8_t gpio, void *args, const uint8_t event_type) {
    homekit_characteristic_t *ch = args;
    
    ch_group_t *ch_group = ch_group_find(ch);
    if (!ch_group->ch_child || ch_group->ch_child->value.bool_value) {
        led_blink(event_type + 1);
        INFO2("Setter EVENT %i", event_type);
        
        homekit_characteristic_notify(ch, HOMEKIT_UINT8(event_type));
        
        do_actions(ch_group, event_type);
        
        setup_mode_toggle_upcount();
    }
}

// --- SENSORS
void sensor_1(const uint8_t gpio, void *args, const uint8_t type) {
    homekit_characteristic_t *ch = args;

    ch_group_t *ch_group = ch_group_find(ch);
    if (!ch_group->ch_sec || ch_group->ch_sec->value.bool_value) {
        if ((type == TYPE_SENSOR &&
            ch->value.int_value == 0) ||
            (type == TYPE_SENSOR_BOOL &&
            ch->value.bool_value == false)) {
            led_blink(1);
            INFO2("Sensor ON");
            
            if (type == TYPE_SENSOR) {
                ch->value = HOMEKIT_UINT8(1);
            } else {
                ch->value = HOMEKIT_BOOL(true);
            }

            do_actions(ch_group, 1);
            
            if (ch_group->num[0] > 0) {
               autooff_setter_params_t *autooff_setter_params = malloc(sizeof(autooff_setter_params_t));
               autooff_setter_params->ch = ch;
               autooff_setter_params->type = type;
               autooff_setter_params->time = ch_group->num[0];
               xTaskCreate(hkc_autooff_setter_task, "hkc_autooff_setter_task", AUTOOFF_SETTER_TASK_SIZE, autooff_setter_params, 1, NULL);
            } 
        }
    }
   
    hkc_group_notify(ch_group);
}

void sensor_status_1(const uint8_t gpio, void *args, const uint8_t type) {
    homekit_characteristic_t *ch = args;

    if ((type == TYPE_SENSOR &&
        ch->value.int_value == 0) ||
        (type == TYPE_SENSOR_BOOL &&
        ch->value.bool_value == false)) {
        led_blink(1);
        INFO2("Sensor Status ON");
        
        if (type == TYPE_SENSOR) {
            ch->value = HOMEKIT_UINT8(1);
        } else {
            ch->value = HOMEKIT_BOOL(true);
        }

        hkc_group_notify(ch_group_find(ch));
    }
}

void sensor_0(const uint8_t gpio, void *args, const uint8_t type) {
    homekit_characteristic_t *ch = args;
    
    ch_group_t *ch_group = ch_group_find(ch);
    if (!ch_group->ch_sec || ch_group->ch_sec->value.bool_value) {
        if ((type == TYPE_SENSOR &&
            ch->value.int_value == 1) ||
            (type == TYPE_SENSOR_BOOL &&
            ch->value.bool_value == true)) {
            led_blink(1);
            INFO2("Sensor OFF");
            
            if (type == TYPE_SENSOR) {
                ch->value = HOMEKIT_UINT8(0);
            } else {
                ch->value = HOMEKIT_BOOL(false);
            }
            
            do_actions(ch_group, 0);
        }
    }
    
    hkc_group_notify(ch_group);
}

void sensor_status_0(const uint8_t gpio, void *args, const uint8_t type) {
    homekit_characteristic_t *ch = args;

    if ((type == TYPE_SENSOR &&
        ch->value.int_value == 1) ||
        (type == TYPE_SENSOR_BOOL &&
        ch->value.bool_value == true)) {
        led_blink(1);
        INFO2("Sensor Status OFF");
        
        if (type == TYPE_SENSOR) {
            ch->value = HOMEKIT_UINT8(0);
        } else {
            ch->value = HOMEKIT_BOOL(false);
        }
        
        hkc_group_notify(ch_group_find(ch));
    }
}

// --- WATER VALVE
void hkc_valve_setter(homekit_characteristic_t *ch, const homekit_value_t value) {
    ch_group_t *ch_group = ch_group_find(ch);
    if (!ch_group->ch_sec || ch_group->ch_sec->value.bool_value) {
        if (ch->value.int_value != value.int_value) {
            led_blink(1);
            INFO2("Setter VALVE");
            
            ch->value = value;
            ch_group->ch1->value = value;
            
            do_actions(ch_group, (uint8_t) ch->value.int_value);
            
            if (ch->value.int_value == 1 && ch_group->num[0] > 0) {
                autooff_setter_params_t *autooff_setter_params = malloc(sizeof(autooff_setter_params_t));
                autooff_setter_params->ch = ch;
                autooff_setter_params->type = TYPE_VALVE;
                autooff_setter_params->time = ch_group->num[0];
                xTaskCreate(hkc_autooff_setter_task, "hkc_autooff_setter_task", AUTOOFF_SETTER_TASK_SIZE, autooff_setter_params, 1, NULL);
            }
         
            setup_mode_toggle_upcount();
            save_states_callback();
            
            if (ch_group->ch3) {
                if (value.int_value == 0) {
                    ch_group->ch3->value.int_value = 0;
                    sdk_os_timer_disarm(ch_group->timer);
                } else {
                    ch_group->ch3->value = ch_group->ch2->value;
                    sdk_os_timer_arm(ch_group->timer, 1000, 1);
                }
            }
        }
    }
    
    hkc_group_notify(ch_group);
}

void hkc_valve_status_setter(homekit_characteristic_t *ch, const homekit_value_t value) {
    if (ch->value.int_value != value.int_value) {
        led_blink(1);
        INFO2("Setter Status VALVE");
        
        ch->value = value;
        ch_group_t *ch_group = ch_group_find(ch);
        ch_group->ch1->value = value;
        
        hkc_group_notify(ch_group);
    }
}

void valve_timer_worker(void *args) {
    homekit_characteristic_t *ch = args;
    ch_group_t *ch_group = ch_group_find(ch);
    
    ch_group->ch3->value.int_value--;
    
    if (ch_group->ch3->value.int_value == 0) {
        sdk_os_timer_disarm(ch_group->timer);
        
        hkc_valve_setter(ch, HOMEKIT_UINT8(0));
    }
}

// --- THERMOSTAT
void update_th(homekit_characteristic_t *ch, const homekit_value_t value) {
    ch_group_t *ch_group = ch_group_find(ch);
    if (!ch_group->ch_sec || ch_group->ch_sec->value.bool_value) {
        led_blink(1);
        INFO2("Setter TH");
        
        ch->value = value;
        
        if (ch_group->ch1->value.int_value) {
            const float mid_target_temp = (ch_group->ch5->value.float_value + ch_group->ch6->value.float_value) / 2;
            
            switch (ch_group->ch4->value.int_value) {
                case THERMOSTAT_TARGET_MODE_HEATER:
                    if (ch_group->ch3->value.int_value <= THERMOSTAT_MODE_IDLE) {
                        if (ch_group->ch0->value.float_value < (ch_group->ch5->value.float_value - TH_DEADBAND)) {
                            ch_group->ch3->value.int_value = THERMOSTAT_MODE_HEATER;
                            do_actions(ch_group, THERMOSTAT_ACTION_HEATER_ON);
                        }
                    } else if (ch_group->ch0->value.float_value >= ch_group->ch5->value.float_value) {
                        ch_group->ch3->value.int_value = THERMOSTAT_MODE_IDLE;
                        do_actions(ch_group, THERMOSTAT_ACTION_HEATER_IDLE);
                    }
                    break;
                
                case THERMOSTAT_TARGET_MODE_COOLER:
                    if (ch_group->ch3->value.int_value <= THERMOSTAT_MODE_IDLE) {
                        if (ch_group->ch0->value.float_value > (ch_group->ch6->value.float_value + TH_DEADBAND)) {
                            ch_group->ch3->value.int_value = THERMOSTAT_MODE_COOLER;
                            do_actions(ch_group, THERMOSTAT_ACTION_COOLER_ON);
                        }
                    } else if (ch_group->ch0->value.float_value <= ch_group->ch6->value.float_value) {
                        ch_group->ch3->value.int_value = THERMOSTAT_MODE_IDLE;
                        do_actions(ch_group, THERMOSTAT_ACTION_COOLER_IDLE);
                    }
                    break;
                
                default:    // case THERMOSTAT_TARGET_MODE_AUTO:
                    switch (ch_group->ch3->value.int_value) {
                        case THERMOSTAT_MODE_HEATER:
                            if (ch_group->ch0->value.float_value >= mid_target_temp) {
                                ch_group->ch3->value.int_value = THERMOSTAT_MODE_IDLE;
                                do_actions(ch_group, THERMOSTAT_ACTION_HEATER_IDLE);
                            }
                            break;
                            
                        case THERMOSTAT_MODE_COOLER:
                            if (ch_group->ch0->value.float_value <= mid_target_temp) {
                                ch_group->ch3->value.int_value = THERMOSTAT_MODE_IDLE;
                                do_actions(ch_group, THERMOSTAT_ACTION_COOLER_IDLE);
                            }
                            break;
                            
                        default:    // cases THERMOSTAT_MODE_IDLE, THERMOSTAT_MODE_OFF:
                            if (ch_group->ch0->value.float_value < ch_group->ch5->value.float_value) {
                                ch_group->ch3->value.int_value = THERMOSTAT_MODE_HEATER;
                                do_actions(ch_group, THERMOSTAT_ACTION_HEATER_ON);
                            } else if (ch_group->ch0->value.float_value > ch_group->ch6->value.float_value) {
                                ch_group->ch3->value.int_value = THERMOSTAT_MODE_COOLER;
                                do_actions(ch_group, THERMOSTAT_ACTION_COOLER_ON);
                            }
                            break;
                    }
                    break;
            }
            
        } else {
            ch_group->ch3->value.int_value = THERMOSTAT_MODE_OFF;
            do_actions(ch_group, THERMOSTAT_ACTION_TOTAL_OFF);
        }
        
        save_states_callback();
    }
    
    hkc_group_notify(ch_group);
}

void hkc_th_target_setter(homekit_characteristic_t *ch, const homekit_value_t value) {
    setup_mode_toggle_upcount();
    
    update_th(ch, value);
}

void th_input(const uint8_t gpio, void *args, const uint8_t type) {
    homekit_characteristic_t *ch = args;
    ch_group_t *ch_group = ch_group_find(ch);
    if (!ch_group->ch_child || ch_group->ch_child->value.bool_value) {
        switch ((uint8_t) TH_TYPE) {
            case 0:
                ch_group->ch1->value.int_value = 0;
                break;
                
            case 1:
                ch_group->ch1->value.int_value = 1;
                break;
                
            case 5:
                ch_group->ch1->value.int_value = 1;
                ch_group->ch4->value.int_value = 2;
                break;
                
            case 6:
                ch_group->ch1->value.int_value = 1;
                ch_group->ch4->value.int_value = 1;
                break;
                
            case 7:
                ch_group->ch1->value.int_value = 1;
                ch_group->ch4->value.int_value = 0;
                break;
                
            default:    // case 9:  // Cyclic
                if (ch_group->ch1->value.int_value) {
                    if (TH_TYPE == THERMOSTAT_TYPE_HEATERCOOLER) {
                        if (ch_group->ch4->value.int_value > 0) {
                            ch_group->ch4->value.int_value--;
                        } else {
                            ch_group->ch1->value.int_value = 0;
                        }
                    } else {
                        ch_group->ch1->value.int_value = 0;
                    }
                } else {
                    ch_group->ch1->value.int_value = 1;
                    if (TH_TYPE == THERMOSTAT_TYPE_HEATERCOOLER) {
                        ch_group->ch4->value.int_value = THERMOSTAT_TARGET_MODE_COOLER;
                    }
                }
                break;
        }
        
        hkc_th_target_setter(ch_group->ch0, ch_group->ch0->value);
    }
}

void th_input_temp(const uint8_t gpio, void *args, const uint8_t type) {
    homekit_characteristic_t *ch = args;
    ch_group_t *ch_group = ch_group_find(ch);
    if (!ch_group->ch_child || ch_group->ch_child->value.bool_value) {
        float set_h_temp = ch_group->ch5->value.float_value;
        float set_c_temp = ch_group->ch6->value.float_value;  

        if (type == THERMOSTAT_TEMP_UP) {
            set_h_temp += 0.5;
            set_c_temp += 0.5;
            if (set_h_temp > TH_MAX_TEMP) {
                set_h_temp = TH_MAX_TEMP;
            }
            if (set_c_temp > TH_MAX_TEMP) {
                set_c_temp = TH_MAX_TEMP;
            }
        } else {    // type == THERMOSTAT_TEMP_DOWN
            set_h_temp -= 0.5;
            set_c_temp -= 0.5;
            if (set_h_temp < TH_MIN_TEMP) {
                set_h_temp = TH_MIN_TEMP;
            }
            if (set_c_temp < TH_MIN_TEMP) {
                set_c_temp = TH_MIN_TEMP;
            }
        }
        
        ch_group->ch5->value.float_value = set_h_temp;
        ch_group->ch6->value.float_value = set_c_temp;
        
        update_th(ch_group->ch0, ch_group->ch0->value);
    }
}

// --- TEMPERATURE
void temperature_timer_worker(void *args) {
   ch_group_t *ch_group = args;
    
    float humidity_value, temperature_value;
    bool get_temp = false;
    
    if (TH_SENSOR_TYPE != 3) {
        dht_sensor_type_t current_sensor_type = DHT_TYPE_DHT22; // TH_SENSOR_TYPE == 2
        
        if (TH_SENSOR_TYPE == 1) {
            current_sensor_type = DHT_TYPE_DHT11;
        } else if (TH_SENSOR_TYPE == 4) {
            current_sensor_type = DHT_TYPE_SI7021;
        }
        
        get_temp = dht_read_float_data(current_sensor_type, TH_SENSOR_GPIO, &humidity_value, &temperature_value);
    } else {    // TH_SENSOR_TYPE == 3
        ds18b20_addr_t ds18b20_addr[1];
        
        if (ds18b20_scan_devices(TH_SENSOR_GPIO, ds18b20_addr, 1) == 1) {
            float temps[1];
            ds18b20_measure_and_read_multi(TH_SENSOR_GPIO, ds18b20_addr, 1, temps);
            temperature_value = temps[0];
            humidity_value = 0.0;
            get_temp = true;
        }
    }
    
    //get_temp = true; temperature_value = 21;      // Only for tests. Keep comment for releases
    
    if (get_temp) {
        if (ch_group->ch0) {
            temperature_value += TH_SENSOR_TEMP_OFFSET;
            if (temperature_value < -100) {
                temperature_value = -100;
            } else if (temperature_value > 200) {
                temperature_value = 200;
            }
            
            if (temperature_value != ch_group->ch0->value.float_value) {
                ch_group->ch0->value = HOMEKIT_FLOAT(temperature_value);
                
                if (ch_group->ch5) {
                    update_th(ch_group->ch0, ch_group->ch0->value);
                }
                
                do_wildcard_actions(ch_group, 0, temperature_value);
            }
        }
        
        if (ch_group->ch1 && !ch_group->ch5) {
            humidity_value += TH_SENSOR_HUM_OFFSET;
            if (humidity_value < 0) {
                humidity_value = 0;
            } else if (humidity_value > 100) {
                humidity_value = 100;
            }

            if (humidity_value != ch_group->ch1->value.float_value) {
                ch_group->ch1->value = HOMEKIT_FLOAT(humidity_value);
                
                do_wildcard_actions(ch_group, 1, humidity_value);
            }
        }
        
        INFO2("TEMP %g, HUM %g", temperature_value, humidity_value);
    } else {
        led_blink(5);
        ERROR2("Sensor");
        
        if (ch_group->ch5) {
            ch_group->ch3->value = HOMEKIT_UINT8(THERMOSTAT_MODE_OFF);
            
            do_actions(ch_group, THERMOSTAT_ACTION_SENSOR_ERROR);
        }
    }
    
    hkc_group_notify(ch_group);
}

// --- LIGHTBULBS
//http://blog.saikoled.com/post/44677718712/how-to-convert-from-hsi-to-rgb-white
void hsi2rgbw(float h, float s, float i, lightbulb_group_t *lightbulb_group) {
    while (h < 0) {
        h += 360.0F;
    }
    while (h >= 360) {
        h -= 360.0F;
    }
    
    h = 3.141592653F * h / 180.0F;
    s /= 100.0F;
    i /= 100.0F;
    s = s > 0 ? (s < 1 ? s : 1) : 0;
    i = i > 0 ? (i < 1 ? i : 1) : 0;
    
    uint32_t r, g, b;
    
    if (h < 2.094393334) {
        r = lightbulb_group->factor_r * PWM_SCALE * i / 3 * (1 + s * cos(h) / cos(1.047196667 - h));
        g = lightbulb_group->factor_g * PWM_SCALE * i / 3 * (1 + s * (1 - cos(h) / cos(1.047196667 - h)));
        b = lightbulb_group->factor_b * PWM_SCALE * i / 3 * (1 - s);
    } else if (h < 4.188786668) {
        h = h - 2.094393334;
        g = lightbulb_group->factor_g * PWM_SCALE * i / 3 * (1 + s * cos(h) / cos(1.047196667 - h));
        b = lightbulb_group->factor_b * PWM_SCALE * i / 3 * (1 + s * (1 - cos(h) / cos(1.047196667 - h)));
        r = lightbulb_group->factor_r * PWM_SCALE * i / 3 * (1 - s);
    } else {
        h = h - 4.188786668;
        b = lightbulb_group->factor_b * PWM_SCALE * i / 3 * (1 + s * cos(h) / cos(1.047196667 - h));
        r = lightbulb_group->factor_r * PWM_SCALE * i / 3 * (1 + s * (1 - cos(h) / cos(1.047196667 - h)));
        g = lightbulb_group->factor_g * PWM_SCALE * i / 3 * (1 - s);
    }
    const uint32_t w = lightbulb_group->factor_w * PWM_SCALE * i * (1 - s);
    
    lightbulb_group->target_r = ((r > PWM_SCALE) ? PWM_SCALE : r);
    lightbulb_group->target_g = ((g > PWM_SCALE) ? PWM_SCALE : g);
    lightbulb_group->target_b = ((b > PWM_SCALE) ? PWM_SCALE : b);
    lightbulb_group->target_w = ((w > PWM_SCALE) ? PWM_SCALE : w);
}

void multipwm_set_all() {
    multipwm_stop(pwm_info);
    for (uint8_t i = 0; i < pwm_info->channels; i++) {
        multipwm_set_duty(pwm_info, i, multipwm_duty[i]);
    }
    multipwm_start(pwm_info);
}

void rgbw_set_timer_worker() {
    if (!setpwm_bool_semaphore) {
        setpwm_bool_semaphore = true;
        
        uint8_t channels_to_set = pwm_info->channels;
        lightbulb_group_t *lightbulb_group = lightbulb_groups;
        
        while (setpwm_is_running && lightbulb_group) {
            if (lightbulb_group->pwm_r != 255) {
                if (lightbulb_group->target_r - multipwm_duty[lightbulb_group->pwm_r] >= lightbulb_group->step) {
                    multipwm_duty[lightbulb_group->pwm_r] += lightbulb_group->step;
                } else if (multipwm_duty[lightbulb_group->pwm_r] - lightbulb_group->target_r >= lightbulb_group->step) {
                    multipwm_duty[lightbulb_group->pwm_r] -= lightbulb_group->step;
                } else {
                    multipwm_duty[lightbulb_group->pwm_r] = lightbulb_group->target_r;
                    channels_to_set--;
                }
            }
            
            if (lightbulb_group->pwm_g != 255) {
                if (lightbulb_group->target_g - multipwm_duty[lightbulb_group->pwm_g] >= lightbulb_group->step) {
                    multipwm_duty[lightbulb_group->pwm_g] += lightbulb_group->step;
                } else if (multipwm_duty[lightbulb_group->pwm_g] - lightbulb_group->target_g >= lightbulb_group->step) {
                    multipwm_duty[lightbulb_group->pwm_g] -= lightbulb_group->step;
                } else {
                    multipwm_duty[lightbulb_group->pwm_g] = lightbulb_group->target_g;
                    channels_to_set--;
                }
            }
            
            if (lightbulb_group->pwm_b != 255) {
                if (lightbulb_group->target_b - multipwm_duty[lightbulb_group->pwm_b] >= lightbulb_group->step) {
                    multipwm_duty[lightbulb_group->pwm_b] += lightbulb_group->step;
                } else if (multipwm_duty[lightbulb_group->pwm_b] - lightbulb_group->target_b >= lightbulb_group->step) {
                    multipwm_duty[lightbulb_group->pwm_b] -= lightbulb_group->step;
                } else {
                    multipwm_duty[lightbulb_group->pwm_b] = lightbulb_group->target_b;
                    channels_to_set--;
                }
            }
            
            if (lightbulb_group->pwm_w != 255) {
                if (lightbulb_group->target_w - multipwm_duty[lightbulb_group->pwm_w] >= lightbulb_group->step) {
                    multipwm_duty[lightbulb_group->pwm_w] += lightbulb_group->step;
                } else if (multipwm_duty[lightbulb_group->pwm_w] - lightbulb_group->target_w >= lightbulb_group->step) {
                    multipwm_duty[lightbulb_group->pwm_w] -= lightbulb_group->step;
                } else {
                    multipwm_duty[lightbulb_group->pwm_w] = lightbulb_group->target_w;
                    channels_to_set--;
                }
            }
            
            //INFO2("RGBW -> %i, %i, %i, %i", multipwm_duty[lightbulb_group->pwm_r], multipwm_duty[lightbulb_group->pwm_g], multipwm_duty[lightbulb_group->pwm_g], multipwm_duty[lightbulb_group->pwm_w]);

            if (channels_to_set == 0) {
                setpwm_is_running = false;
                sdk_os_timer_disarm(pwm_timer);
                INFO2("Color fixed");
            }
            
            lightbulb_group = lightbulb_group->next;
        }
        
        multipwm_set_all();

        setpwm_bool_semaphore = false;
    } else {
        ERROR2("MISSED Color set");
    }
}

void hkc_rgbw_setter(homekit_characteristic_t *ch, const homekit_value_t value) {
    ch_group_t *ch_group = ch_group_find(ch);
    if (ch_group->ch_sec && !ch_group->ch_sec->value.bool_value) {
        hkc_group_notify(ch_group);
        
    } else if (ch != ch_group->ch0 || value.bool_value != ch_group->ch0->value.bool_value) {
        lightbulb_group_t *lightbulb_group = lightbulb_group_find(ch_group->ch0);
        
        ch->value = value;
        
        if (ch_group->ch0->value.bool_value) {
            if (lightbulb_group->target_r == 0 &&
                lightbulb_group->target_g == 0 &&
                lightbulb_group->target_b == 0 &&
                lightbulb_group->target_w == 0) {
                setup_mode_toggle_upcount();
            }
            
            if (lightbulb_group->pwm_r != 255) {            // RGB/W
                hsi2rgbw(ch_group->ch2->value.float_value, ch_group->ch3->value.float_value, ch_group->ch1->value.int_value, lightbulb_group);
                
            } else if (lightbulb_group->pwm_b != 255) {     // Custom Color Temperature
                uint16_t target_color = 0;
                
                if (ch_group->ch2->value.int_value >= COLOR_TEMP_MAX - 5) {
                    target_color = PWM_SCALE;
                    
                } else if (ch_group->ch2->value.int_value > COLOR_TEMP_MIN + 1) { // Conversion based on @seritos curve
                    target_color = PWM_SCALE * (((0.09 + sqrt(0.18 + (0.1352 * (ch_group->ch2->value.int_value - COLOR_TEMP_MIN - 1)))) / 0.0676) - 1) / 100;
                }
                
                const uint32_t w = lightbulb_group->factor_w * target_color * ch_group->ch1->value.int_value / 100;
                const uint32_t b = lightbulb_group->factor_b * (PWM_SCALE - target_color) * ch_group->ch1->value.int_value / 100;
                lightbulb_group->target_w = ((w > PWM_SCALE) ? PWM_SCALE : w);
                lightbulb_group->target_b = ((b > PWM_SCALE) ? PWM_SCALE : b);
                
            } else {                                        // One Color Dimmer
                const uint32_t w = lightbulb_group->factor_w * PWM_SCALE * ch_group->ch1->value.int_value / 100;
                lightbulb_group->target_w = ((w > PWM_SCALE) ? PWM_SCALE : w);
            }
        } else {
            lightbulb_group->autodimmer = 0;
            lightbulb_group->target_r = 0;
            lightbulb_group->target_g = 0;
            lightbulb_group->target_b = 0;
            lightbulb_group->target_w = 0;
            
            setup_mode_toggle_upcount();
        }
        
        led_blink(1);
        INFO2("Target RGBW = %i, %i, %i, %i", lightbulb_group->target_r, lightbulb_group->target_g, lightbulb_group->target_b, lightbulb_group->target_w);
        
        if (lightbulb_group->is_pwm && !setpwm_is_running) {
            setpwm_is_running = true;
            sdk_os_timer_arm(pwm_timer, RGBW_PERIOD, true);
        }
        
        do_actions(ch_group, (uint8_t) ch_group->ch0->value.bool_value);
        
        if (ch_group->ch0->value.bool_value) {
            do_wildcard_actions(ch_group, 0, ch_group->ch1->value.int_value);
        }
        
        hkc_group_notify(ch_group);
        
        save_states_callback();

    } else {
        homekit_characteristic_notify(ch_group->ch0, ch_group->ch0->value);
    }
}

void rgbw_brightness(const uint8_t gpio, void *args, const uint8_t type) {
    homekit_characteristic_t *ch = args;
    ch_group_t *ch_group = ch_group_find(ch);
    if (!(ch_group->ch_child && !ch_group->ch_child->value.bool_value)) {
        if (type == LIGHTBULB_BRIGHTNESS_UP) {
            if (ch->value.int_value + 10 < 100) {
                ch->value.int_value += 10;
            } else {
                ch->value.int_value = 100;
            }
        } else {    // type == LIGHTBULB_BRIGHTNESS_DOWN
            if (ch->value.int_value - 10 > 0) {
                ch->value.int_value -= 10;
            } else {
                ch->value.int_value = 0;
            }
        }
        
        hkc_rgbw_setter(ch, ch->value);
    }
}

void autodimmer_task(void *args) {
    INFO2("AUTODimmer started");
    
    homekit_characteristic_t *ch = args;
    ch_group_t *ch_group = ch_group_find(ch);
    lightbulb_group_t *lightbulb_group = lightbulb_group_find(ch_group->ch0);
    
    lightbulb_group->autodimmer = 4 * 100 / lightbulb_group->autodimmer_task_step;
    while(lightbulb_group->autodimmer > 0) {
        lightbulb_group->autodimmer--;
        if (ch_group->ch1->value.int_value < 100) {
            if (ch_group->ch1->value.int_value + lightbulb_group->autodimmer_task_step < 100) {
                ch_group->ch1->value.int_value += lightbulb_group->autodimmer_task_step;
            } else {
                ch_group->ch1->value.int_value = 100;
            }
        } else {
            ch_group->ch1->value.int_value = lightbulb_group->autodimmer_task_step;
        }
        hkc_rgbw_setter(ch_group->ch1, ch_group->ch1->value);
        
        vTaskDelay(MS_TO_TICK(lightbulb_group->autodimmer_task_delay));
        
        if (ch_group->ch1->value.int_value == 100) {    // Double wait when brightness is 100%
            vTaskDelay(MS_TO_TICK(lightbulb_group->autodimmer_task_delay));
        }
    }
    
    INFO2("AUTODimmer stopped");
    
    vTaskDelete(NULL);
}

void no_autodimmer_called(void *args) {
    homekit_characteristic_t *ch0 = args;
    lightbulb_group_t *lightbulb_group = lightbulb_group_find(ch0);
    lightbulb_group->armed_autodimmer = false;
    hkc_rgbw_setter(ch0, HOMEKIT_BOOL(false));
}

void autodimmer_call(homekit_characteristic_t *ch0, const homekit_value_t value) {
    lightbulb_group_t *lightbulb_group = lightbulb_group_find(ch0);
    if (lightbulb_group->autodimmer_task_step == 0 || (value.bool_value && lightbulb_group->autodimmer == 0)) {
        hkc_rgbw_setter(ch0, value);
    } else if (lightbulb_group->autodimmer > 0) {
        lightbulb_group->autodimmer = 0;
    } else {
        lightbulb_group->autodimmer = 0;
        ch_group_t *ch_group = ch_group_find(ch0);
        if (lightbulb_group->armed_autodimmer) {
            lightbulb_group->armed_autodimmer = false;
            sdk_os_timer_disarm(ch_group->timer);
            xTaskCreate(autodimmer_task, "autodimmer_task", AUTODIMMER_TASK_SIZE, (void *) ch0, 1, NULL);
        } else {
            sdk_os_timer_arm(ch_group->timer, AUTODIMMER_DELAY, 0);
            lightbulb_group->armed_autodimmer = true;
        }
    }
}

// --- GARAGE DOOR
void garage_door_stop(const uint8_t gpio, void *args, const uint8_t type) {
    homekit_characteristic_t *ch0 = args;
    ch_group_t *ch_group = ch_group_find(ch0);
    
    if (ch0->value.int_value == GARAGE_DOOR_OPENING || ch0->value.int_value == GARAGE_DOOR_CLOSING) {
        led_blink(1);
        INFO2("GD stop");
        
        ch0->value.int_value = GARAGE_DOOR_STOPPED;
        
        sdk_os_timer_disarm(ch_group->timer);

        do_actions(ch_group, 10);
        
        hkc_group_notify(ch_group);
    }
}

void garage_door_obstruction(const uint8_t gpio, void *args, const uint8_t type) {
    homekit_characteristic_t *ch = args;
    ch_group_t *ch_group = ch_group_find(ch);
    
    led_blink(1);
    INFO2("GD obstr: %i", type);
    
    ch_group->ch2->value.bool_value = (bool) type;
    
    hkc_group_notify(ch_group);
    
    do_actions(ch_group, type + 8);
}

void garage_door_sensor(const uint8_t gpio, void *args, const uint8_t type) {
    homekit_characteristic_t *ch = args;
    ch_group_t *ch_group = ch_group_find(ch);
    
    led_blink(1);
    INFO2("GD sensor: %i", type);
    
    ch->value.int_value = type;
    
    if (type > 1) {
        ch_group->ch1->value.int_value = type - 2;
        sdk_os_timer_arm(ch_group->timer, 1000, 1);
    } else {
        ch_group->ch1->value.int_value = type;
        sdk_os_timer_disarm(ch_group->timer);
        
        if (type == 0) {
            GARAGE_DOOR_CURRENT_TIME = GARAGE_DOOR_WORKING_TIME - GARAGE_DOOR_TIME_MARGIN;
        } else {
            GARAGE_DOOR_CURRENT_TIME = GARAGE_DOOR_TIME_MARGIN;
        }
        
        if (ch_group->ch2->value.bool_value) {
            garage_door_obstruction(0, ch, 0);
        }
    }
    
     hkc_group_notify(ch_group);
    
    do_actions(ch_group, type + 4);
}

void hkc_garage_door_setter(homekit_characteristic_t *ch1, const homekit_value_t value) {
    ch_group_t *ch_group = ch_group_find(ch1);
    if ((!ch_group->ch_sec || ch_group->ch_sec->value.bool_value) && !ch_group->ch2->value.bool_value) {
        uint8_t current_door_state = ch_group->ch0->value.int_value;
        if (current_door_state == GARAGE_DOOR_STOPPED) {
            current_door_state = ch_group->ch1->value.int_value;
        } else if (current_door_state > 1) {
            current_door_state -= 2;
        }

        if (value.int_value != current_door_state) {
            led_blink(1);
            INFO2("Setter GD");
            
            ch1->value = value;

            do_actions(ch_group, (uint8_t) ch_group->ch0->value.int_value);
   
            if ((value.int_value == GARAGE_DOOR_OPENED && GARAGE_DOOR_HAS_F4 == 0) ||
                       ch_group->ch0->value.int_value == GARAGE_DOOR_CLOSING) {
                garage_door_sensor(0, ch_group->ch0, GARAGE_DOOR_OPENING);
            } else if ((value.int_value == GARAGE_DOOR_CLOSED && GARAGE_DOOR_HAS_F5 == 0) ||
                       ch_group->ch0->value.int_value == GARAGE_DOOR_OPENING) {
                garage_door_sensor(0, ch_group->ch0, GARAGE_DOOR_CLOSING);
            }
            
            setup_mode_toggle_upcount();
        }

    }
    
    hkc_group_notify(ch_group);
}

void garage_door_timer_worker(void *args) {
    homekit_characteristic_t *ch0 = args;
    ch_group_t *ch_group = ch_group_find(ch0);

    void halt_timer() {
        sdk_os_timer_disarm(ch_group->timer);
        if (GARAGE_DOOR_TIME_MARGIN > 0) {
            garage_door_obstruction(0, ch0, 1);
        }
    }
    
    if (ch0->value.int_value == GARAGE_DOOR_OPENING) {
        GARAGE_DOOR_CURRENT_TIME++;

        if (GARAGE_DOOR_CURRENT_TIME >= GARAGE_DOOR_WORKING_TIME - GARAGE_DOOR_TIME_MARGIN && GARAGE_DOOR_HAS_F2 == 0) {
            sdk_os_timer_disarm(ch_group->timer);
            garage_door_sensor(0, ch0, GARAGE_DOOR_OPENED);
            
        } else if (GARAGE_DOOR_CURRENT_TIME >= GARAGE_DOOR_WORKING_TIME && GARAGE_DOOR_HAS_F2 == 1) {
            halt_timer();
        }
        
    } else {    // GARAGE_DOOR_CLOSING
        GARAGE_DOOR_CURRENT_TIME -= GARAGE_DOOR_CLOSE_TIME_FACTOR;
        
        if (GARAGE_DOOR_CURRENT_TIME <= GARAGE_DOOR_TIME_MARGIN && GARAGE_DOOR_HAS_F3 == 0) {
            sdk_os_timer_disarm(ch_group->timer);
            garage_door_sensor(0, ch0, GARAGE_DOOR_CLOSED);
            
        } else if (GARAGE_DOOR_CURRENT_TIME <= 0 && GARAGE_DOOR_HAS_F3 == 1) {
            halt_timer();
        }
    }
}

// --- WINDOW COVER
void normalize_position(homekit_characteristic_t *ch) {
    ch_group_t *ch_group = ch_group_find(ch);
    
    if (WINDOW_COVER_POSITION < 0) {
        WINDOW_COVER_POSITION = 0;
    } else if (WINDOW_COVER_POSITION > 100) {
        WINDOW_COVER_POSITION = 100;
    }
    
    if (WINDOW_COVER_REAL_POSITION < 0) {
        WINDOW_COVER_REAL_POSITION = 0;
    } else if (WINDOW_COVER_REAL_POSITION > 100) {
        WINDOW_COVER_REAL_POSITION = 100;
    }
}

void window_cover_stop(homekit_characteristic_t *ch) {
    ch_group_t *ch_group = ch_group_find(ch);
    
    led_blink(1);
    INFO2("WC Stopped at %f, real %f", WINDOW_COVER_POSITION, WINDOW_COVER_REAL_POSITION);
    
    sdk_os_timer_disarm(ch_group->timer);
    normalize_position(ch);
    
    WINDOW_COVER_CH_CURRENT_POSITION->value.int_value = WINDOW_COVER_REAL_POSITION;
    WINDOW_COVER_CH_TARGET_POSITION->value = WINDOW_COVER_CH_CURRENT_POSITION->value;
    
    if (WINDOW_COVER_CH_STATE->value.int_value == WINDOW_COVER_CLOSING) {
        do_actions(ch_group, WINDOW_COVER_STOP_FROM_CLOSING);
    } else if (WINDOW_COVER_CH_STATE->value.int_value == WINDOW_COVER_OPENING) {
        do_actions(ch_group, WINDOW_COVER_STOP_FROM_OPENING);
    }
    
    do_actions(ch_group, WINDOW_COVER_STOP);
    
    WINDOW_COVER_CH_STATE->value.int_value = WINDOW_COVER_STOP;
    
    hkc_group_notify(ch_group);
    
    setup_mode_toggle_upcount();
    
    save_states_callback();
}

void window_cover_obstruction(const uint8_t gpio, void *args, const uint8_t type) {
    homekit_characteristic_t *ch = args;
    ch_group_t *ch_group = ch_group_find(ch);
    
    led_blink(1);
    INFO2("WC obstr: %i", type);
    
    ch_group->ch3->value.bool_value = (bool) type;
    
    hkc_group_notify(ch_group);
    
    do_actions(ch_group, type + WINDOW_COVER_OBSTRUCTION);
}

void hkc_window_cover_setter(homekit_characteristic_t *ch1, const homekit_value_t value) {
    ch_group_t *ch_group = ch_group_find(ch1);
    if (!ch_group->ch_sec || ch_group->ch_sec->value.bool_value) {
        led_blink(1);
        INFO2("Setter WC: Current: %i, Target: %i", WINDOW_COVER_CH_CURRENT_POSITION->value.int_value, value.int_value);
        
        ch1->value = value;

        normalize_position(ch1);
        
        if (value.int_value < WINDOW_COVER_CH_CURRENT_POSITION->value.int_value) {
            
            if (WINDOW_COVER_CH_STATE->value.int_value == WINDOW_COVER_OPENING) {
                do_actions(ch_group, WINDOW_COVER_CLOSING_FROM_MOVING);
                setup_mode_toggle_upcount();
            } else {
                do_actions(ch_group, WINDOW_COVER_CLOSING);
            }
            
            if (WINDOW_COVER_CH_STATE->value.int_value == WINDOW_COVER_STOP) {
                sdk_os_timer_arm(ch_group->timer, WINDOW_COVER_POLL_PERIOD_MS, 1);
            }
            
            WINDOW_COVER_CH_STATE->value.int_value = WINDOW_COVER_CLOSING;

        } else if (value.int_value > WINDOW_COVER_CH_CURRENT_POSITION->value.int_value) {

            if (WINDOW_COVER_CH_STATE->value.int_value == WINDOW_COVER_CLOSING) {
                do_actions(ch_group, WINDOW_COVER_OPENING_FROM_MOVING);
                setup_mode_toggle_upcount();
            } else {
                do_actions(ch_group, WINDOW_COVER_OPENING);
            }
            
            if (WINDOW_COVER_CH_STATE->value.int_value == WINDOW_COVER_STOP) {
                sdk_os_timer_arm(ch_group->timer, WINDOW_COVER_POLL_PERIOD_MS, 1);
            }
            
            WINDOW_COVER_CH_STATE->value.int_value = WINDOW_COVER_OPENING;

        } else {
            window_cover_stop(ch1);
        }
        
        do_wildcard_actions(ch_group, 0, value.int_value);
    
    }
    
    hkc_group_notify(ch_group);
}

void window_cover_timer_worker(void *args) {
    homekit_characteristic_t *ch0 = args;
    ch_group_t *ch_group = ch_group_find(ch0);
    
    uint8_t margin = 0;     // Used as covering offset to add extra time when target position completely closed or opened
    if (WINDOW_COVER_CH_TARGET_POSITION->value.int_value == 0 || WINDOW_COVER_CH_TARGET_POSITION->value.int_value == 100) {
        margin = WINDOW_COVER_MARGIN_SYNC;
    }
    
    void normalize_current_position() {
        if (WINDOW_COVER_POSITION < 0) {
            WINDOW_COVER_CH_CURRENT_POSITION->value.int_value = 0;
        } else if (WINDOW_COVER_POSITION > 100) {
            WINDOW_COVER_CH_CURRENT_POSITION->value.int_value = 100;
        } else {
            if ((WINDOW_COVER_CH_CURRENT_POSITION->value.int_value / 2) != (uint8_t) (WINDOW_COVER_POSITION / 2)) {
                INFO2("WC Moving at %f, real %f", WINDOW_COVER_POSITION, WINDOW_COVER_REAL_POSITION);
                WINDOW_COVER_CH_CURRENT_POSITION->value.int_value = WINDOW_COVER_POSITION;
                homekit_characteristic_notify(WINDOW_COVER_CH_CURRENT_POSITION, WINDOW_COVER_CH_CURRENT_POSITION->value);
            } else {
                WINDOW_COVER_CH_CURRENT_POSITION->value.int_value = WINDOW_COVER_POSITION;
            }
        }
    }
    
    switch (WINDOW_COVER_CH_STATE->value.int_value) {
        case WINDOW_COVER_CLOSING:
            WINDOW_COVER_POSITION -= WINDOW_COVER_STEP_TIME_DOWN;
            if (WINDOW_COVER_POSITION > 0) {
                WINDOW_COVER_REAL_POSITION = WINDOW_COVER_POSITION / (1 + ((100 - WINDOW_COVER_POSITION) * WINDOW_COVER_CORRECTION * 0.0002));
            } else {
                WINDOW_COVER_REAL_POSITION = WINDOW_COVER_POSITION;
            }
            
            normalize_current_position();

            if ((WINDOW_COVER_CH_TARGET_POSITION->value.int_value - margin) >= WINDOW_COVER_REAL_POSITION) {
                window_cover_stop(ch0);
            }
            break;
            
        case WINDOW_COVER_OPENING:
            WINDOW_COVER_POSITION += WINDOW_COVER_STEP_TIME_UP;
            if (WINDOW_COVER_POSITION < 100) {
                WINDOW_COVER_REAL_POSITION = WINDOW_COVER_POSITION / (1 + ((100 - WINDOW_COVER_POSITION) * WINDOW_COVER_CORRECTION * 0.0002));
            } else {
                WINDOW_COVER_REAL_POSITION = WINDOW_COVER_POSITION;
            }
        
            normalize_current_position();
            
            if ((WINDOW_COVER_CH_TARGET_POSITION->value.int_value + margin) <= WINDOW_COVER_REAL_POSITION) {
                window_cover_stop(ch0);
            }
            break;
            
        default:    // case WINDOW_COVER_STOP:
            window_cover_stop(ch0);
            break;
    }
}

// --- FAN
void hkc_fan_setter(homekit_characteristic_t *ch0, const homekit_value_t value) {
    ch_group_t *ch_group = ch_group_find(ch0);
    if (!ch_group->ch_sec || ch_group->ch_sec->value.bool_value) {
        if (ch0->value.bool_value != value.bool_value) {
            led_blink(1);
            INFO2("Setter FAN");
            
            ch0->value = value;
            
            do_actions(ch_group, (uint8_t) value.int_value);
            
            if (value.bool_value) {
                do_wildcard_actions(ch_group, 0, ch_group->ch1->value.float_value);
                
                if (ch0->value.bool_value && ch_group->num[0] > 0) {
                    autooff_setter_params_t *autooff_setter_params = malloc(sizeof(autooff_setter_params_t));
                    autooff_setter_params->ch = ch0;
                    autooff_setter_params->type = TYPE_FAN;
                    autooff_setter_params->time = ch_group->num[0];
                    xTaskCreate(hkc_autooff_setter_task, "hkc_autooff_setter_task", AUTOOFF_SETTER_TASK_SIZE, autooff_setter_params, 1, NULL);
                }
            } else {
                ch_group->last_wildcard_action[0] = NO_LAST_WILDCARD_ACTION;
            }

            setup_mode_toggle_upcount();
            
            save_states_callback();
        }
    }
    
    hkc_group_notify(ch_group);
}

void hkc_fan_speed_setter(homekit_characteristic_t *ch1, const homekit_value_t value) {
    ch_group_t *ch_group = ch_group_find(ch1);
    if (!ch_group->ch_sec || ch_group->ch_sec->value.bool_value) {
        if (ch1->value.float_value != value.float_value) {
            led_blink(1);
            INFO2("Setter Speed FAN");
            
            ch1->value = value;
            
            if (ch_group->ch0->value.bool_value) {
                do_wildcard_actions(ch_group, 0, value.float_value);
            }
            
            save_states_callback();
        }
    }
    
    hkc_group_notify(ch_group);
}

void hkc_fan_status_setter(homekit_characteristic_t *ch0, const homekit_value_t value) {
    if (ch0->value.int_value != value.int_value) {
        led_blink(1);
        INFO2("Setter Status FAN");
        
        ch0->value = value;
        
        hkc_group_notify(ch_group_find(ch0));
        
        save_states_callback();
    }
}

// --- TV
void hkc_tv_active(homekit_characteristic_t *ch0, const homekit_value_t value) {
    ch_group_t *ch_group = ch_group_find(ch0);
    if (!ch_group->ch_sec || ch_group->ch_sec->value.bool_value) {
        if (ch0->value.int_value != value.int_value) {
            led_blink(1);
            INFO2("Setter TV ON %i", value.int_value);
            
            ch0->value = value;
            
            do_actions(ch_group, value.int_value);
            
            setup_mode_toggle_upcount();
            save_states_callback();
        }
    }
    
    hkc_group_notify(ch_group);
}

void hkc_tv_status_active(homekit_characteristic_t *ch0, const homekit_value_t value) {
    if (ch0->value.int_value != value.int_value) {
        led_blink(1);
        INFO2("Setter Status TV ON %i", value.int_value);
        
        ch0->value = value;
        
        hkc_group_notify(ch_group_find(ch0));
        
        save_states_callback();
    }
}

void hkc_tv_active_identifier(homekit_characteristic_t *ch, const homekit_value_t value) {
    ch_group_t *ch_group = ch_group_find(ch);
    if (!ch_group->ch_sec || ch_group->ch_sec->value.bool_value) {
        if (ch->value.int_value != value.int_value) {
            led_blink(1);
            INFO2("Setter TV Input %i", value.int_value);
            
            ch->value = value;
            
            do_actions(ch_group, (MAX_ACTIONS - 1) + value.int_value);
        }
    }
    
    hkc_group_notify(ch_group);
}

void hkc_tv_key(homekit_characteristic_t *ch, const homekit_value_t value) {
    ch_group_t *ch_group = ch_group_find(ch);
    if (!ch_group->ch_sec || ch_group->ch_sec->value.bool_value) {
        led_blink(1);
        INFO2("Setter TV Key %i", value.int_value + 2);
        
        ch->value = value;
        
        do_actions(ch_group, value.int_value + 2);
    }
    
    hkc_group_notify(ch_group);
}

void hkc_tv_power_mode(homekit_characteristic_t *ch, const homekit_value_t value) {
    ch_group_t *ch_group = ch_group_find(ch);
    if (!ch_group->ch_sec || ch_group->ch_sec->value.bool_value) {
        led_blink(1);
        INFO2("Setter TV Settings %i", value.int_value + 30);
        
        ch->value = value;
        
        do_actions(ch_group, value.int_value + 30);
    }
    
    hkc_group_notify(ch_group);
}

void hkc_tv_mute(homekit_characteristic_t *ch, const homekit_value_t value) {
    ch_group_t *ch_group = ch_group_find(ch);
    if (!ch_group->ch_sec || ch_group->ch_sec->value.bool_value) {
        led_blink(1);
        INFO2("Setter TV Mute %i", value.int_value + 20);
        
        ch->value = value;
        
        do_actions(ch_group, value.int_value + 20);
    }
    
    hkc_group_notify(ch_group);
}

void hkc_tv_volume(homekit_characteristic_t *ch, const homekit_value_t value) {
    ch_group_t *ch_group = ch_group_find(ch);
    if (!ch_group->ch_sec || ch_group->ch_sec->value.bool_value) {
        led_blink(1);
        INFO2("Setter TV Volume %i", value.int_value + 22);
        
        ch->value = value;
        
        do_actions(ch_group, value.int_value + 22);
    }
    
    hkc_group_notify(ch_group);
}

void hkc_tv_configured_name(homekit_characteristic_t *ch1, const homekit_value_t value) {
    INFO2("Setter TV Name %s", value.string_value);
    
    char *new_name = strdup(value.string_value);
    
    homekit_value_destruct(&ch1->value);
    ch1->value = HOMEKIT_STRING(new_name);

    hkc_group_notify(ch_group_find(ch1));

    save_states_callback();
}

void hkc_tv_input_configured_name(homekit_characteristic_t *ch, const homekit_value_t value) {
    homekit_characteristic_notify(ch, ch->value);
}

// --- DIGITAL INPUTS
void window_cover_diginput(const uint8_t gpio, void *args, const uint8_t type) {
    homekit_characteristic_t *ch1 = args;
    
    ch_group_t *ch_group = ch_group_find(ch1);
    if (!ch_group->ch_child || ch_group->ch_child->value.bool_value) {
        switch (type) {
            case WINDOW_COVER_CLOSING:
                hkc_window_cover_setter(ch1, HOMEKIT_UINT8(0));
                break;
                
            case WINDOW_COVER_OPENING:
                hkc_window_cover_setter(ch1, HOMEKIT_UINT8(100));
                break;
                
            case (WINDOW_COVER_CLOSING + 3):
                if (WINDOW_COVER_CH_STATE->value.int_value == WINDOW_COVER_CLOSING) {
                    hkc_window_cover_setter(ch1, WINDOW_COVER_CH_CURRENT_POSITION->value);
                } else {
                    hkc_window_cover_setter(ch1, HOMEKIT_UINT8(0));
                }
                break;
                
            case (WINDOW_COVER_OPENING + 3):
                if (WINDOW_COVER_CH_STATE->value.int_value == WINDOW_COVER_OPENING) {
                    hkc_window_cover_setter(ch1, WINDOW_COVER_CH_CURRENT_POSITION->value);
                } else {
                    hkc_window_cover_setter(ch1, HOMEKIT_UINT8(100));
                }
                break;
                
            default:    // case WINDOW_COVER_STOP:
                hkc_window_cover_setter(ch1, WINDOW_COVER_CH_CURRENT_POSITION->value);
                break;
        }
    }
}

void diginput(const uint8_t gpio, void *args, const uint8_t type) {
    homekit_characteristic_t *ch = args;
    
    ch_group_t *ch_group = ch_group_find(ch);
    if (!ch_group->ch_child || ch_group->ch_child->value.bool_value) {
        switch (type) {
            case TYPE_LOCK:
                if (ch->value.int_value == 1) {
                    hkc_lock_setter(ch, HOMEKIT_UINT8(0));
                } else {
                    hkc_lock_setter(ch, HOMEKIT_UINT8(1));
                }
                break;
                
            case TYPE_VALVE:
                if (ch->value.int_value == 1) {
                    hkc_valve_setter(ch, HOMEKIT_UINT8(0));
                } else {
                    hkc_valve_setter(ch, HOMEKIT_UINT8(1));
                }
                break;
                
            case TYPE_LIGHTBULB:
                autodimmer_call(ch, HOMEKIT_BOOL(!ch->value.bool_value));
                break;
                
            case TYPE_GARAGE_DOOR:
                if (ch->value.int_value == 1) {
                    hkc_garage_door_setter(ch, HOMEKIT_UINT8(0));
                } else {
                    hkc_garage_door_setter(ch, HOMEKIT_UINT8(1));
                }
                break;
                
            case TYPE_WINDOW_COVER:
                if (ch->value.int_value == 0) {
                    hkc_window_cover_setter(ch, HOMEKIT_UINT8(100));
                } else {
                    hkc_window_cover_setter(ch, HOMEKIT_UINT8(0));
                }
                break;
                
            case TYPE_FAN:
                if (ch->value.int_value == 1) {
                    hkc_fan_setter(ch, HOMEKIT_BOOL(false));
                } else {
                    hkc_fan_setter(ch, HOMEKIT_BOOL(true));
                }
                break;
                
            case TYPE_TV:
                if (ch->value.int_value == 1) {
                    hkc_tv_active(ch, HOMEKIT_UINT8(0));
                } else {
                    hkc_tv_active(ch, HOMEKIT_UINT8(1));
                }
                break;
                
            default:    // case TYPE_ON:
                hkc_on_setter(ch, HOMEKIT_BOOL(!ch->value.bool_value));
                break;
        }
    }
}

void diginput_1(const uint8_t gpio, void *args, const uint8_t type) {
    homekit_characteristic_t *ch = args;
    ch_group_t *ch_group = ch_group_find(ch);
    if (!ch_group->ch_child || ch_group->ch_child->value.bool_value) {
        switch (type) {
            case TYPE_LOCK:
                if (ch->value.int_value == 0) {
                    hkc_lock_setter(ch, HOMEKIT_UINT8(1));
                }
                break;
                
            case TYPE_VALVE:
                if (ch->value.int_value == 0) {
                    hkc_valve_setter(ch, HOMEKIT_UINT8(1));
                }
                break;
                
            case TYPE_LIGHTBULB:
                if (ch->value.bool_value == false) {
                    autodimmer_call(ch, HOMEKIT_BOOL(true));
                }
                break;
                
            case TYPE_GARAGE_DOOR:
                if (ch->value.int_value == 0) {
                    hkc_garage_door_setter(ch, HOMEKIT_UINT8(1));
                }
                break;
                
            case TYPE_FAN:
                if (ch->value.int_value == 0) {
                    hkc_fan_setter(ch, HOMEKIT_BOOL(true));
                }
                break;
                
            case TYPE_TV:
                if (ch->value.int_value == 0) {
                    hkc_tv_active(ch, HOMEKIT_UINT8(1));
                }
                break;
                
            default:    // case TYPE_ON:
                if (ch->value.bool_value == false) {
                    hkc_on_setter(ch, HOMEKIT_BOOL(true));
                }
                break;
        }
    }
}

void digstate_1(const uint8_t gpio, void *args, const uint8_t type) {
    homekit_characteristic_t *ch = args;
    ch_group_t *ch_group = ch_group_find(ch);
    if (!ch_group->ch_child || ch_group->ch_child->value.bool_value) {
        switch (type) {
            case TYPE_LOCK:
                if (ch->value.int_value == 0) {
                    hkc_lock_status_setter(ch, HOMEKIT_UINT8(1));
                }
                break;
                
            case TYPE_VALVE:
                if (ch->value.int_value == 0) {
                    hkc_valve_status_setter(ch, HOMEKIT_UINT8(1));
                }
                break;
                
            case TYPE_FAN:
                if (ch->value.int_value == 0) {
                    hkc_fan_status_setter(ch, HOMEKIT_UINT8(1));
                }
                break;
                
            case TYPE_TV:
                if (ch->value.int_value == 0) {
                    hkc_tv_status_active(ch, HOMEKIT_UINT8(1));
                }
                break;
                
            default:    // case TYPE_ON:
                if (ch->value.bool_value == false) {
                    hkc_on_status_setter(ch, HOMEKIT_BOOL(true));
                }
                break;
        }
    }
}

void diginput_0(const uint8_t gpio, void *args, const uint8_t type) {
    homekit_characteristic_t *ch = args;
    ch_group_t *ch_group = ch_group_find(ch);
    if (!ch_group->ch_child || ch_group->ch_child->value.bool_value) {
        switch (type) {
            case TYPE_LOCK:
                if (ch->value.int_value == 1) {
                    hkc_lock_setter(ch, HOMEKIT_UINT8(0));
                }
                break;
                
            case TYPE_VALVE:
                if (ch->value.int_value == 1) {
                    hkc_valve_setter(ch, HOMEKIT_UINT8(0));
                }
                break;
                
            case TYPE_LIGHTBULB:
                if (ch->value.bool_value == true) {
                    autodimmer_call(ch, HOMEKIT_BOOL(false));
                }
                break;
                
            case TYPE_GARAGE_DOOR:
                if (ch->value.int_value == 1) {
                    hkc_garage_door_setter(ch, HOMEKIT_UINT8(0));
                }
                break;
                
            case TYPE_FAN:
                if (ch->value.int_value == 1) {
                    hkc_fan_setter(ch, HOMEKIT_BOOL(false));
                }
                break;
                
            case TYPE_TV:
                if (ch->value.int_value == 1) {
                    hkc_tv_active(ch, HOMEKIT_UINT8(0));
                }
                break;
                
            default:    // case TYPE_ON:
                if (ch->value.bool_value == true) {
                    hkc_on_setter(ch, HOMEKIT_BOOL(false));
                }
                break;
        }
    }
}

void digstate_0(const uint8_t gpio, void *args, const uint8_t type) {
    homekit_characteristic_t *ch = args;
    ch_group_t *ch_group = ch_group_find(ch);
    if (!ch_group->ch_child || ch_group->ch_child->value.bool_value) {
        switch (type) {
            case TYPE_LOCK:
                if (ch->value.int_value == 1) {
                    hkc_lock_status_setter(ch, HOMEKIT_UINT8(0));
                }
                break;
                
            case TYPE_VALVE:
                if (ch->value.int_value == 1) {
                    hkc_valve_status_setter(ch, HOMEKIT_UINT8(0));
                }
                break;
                
            case TYPE_FAN:
                if (ch->value.int_value == 1) {
                    hkc_fan_status_setter(ch, HOMEKIT_UINT8(0));
                }
                break;
                
            case TYPE_TV:
                if (ch->value.int_value == 1) {
                    hkc_tv_status_active(ch, HOMEKIT_UINT8(0));
                }
                break;
                
            default:    // case TYPE_ON:
                if (ch->value.bool_value == true) {
                    hkc_on_status_setter(ch, HOMEKIT_BOOL(false));
                }
                break;
        }
    }
}

// --- AUTO-OFF
void hkc_autooff_setter_task(void *pvParameters) {
    autooff_setter_params_t *autooff_setter_params = pvParameters;
    
    vTaskDelay(MS_TO_TICK(autooff_setter_params->time * 1000));
    
    switch (autooff_setter_params->type) {
        case TYPE_LOCK:
            hkc_lock_setter(autooff_setter_params->ch, HOMEKIT_UINT8(1));
            break;
            
        case TYPE_SENSOR:
        case TYPE_SENSOR_BOOL:
            sensor_0(0, autooff_setter_params->ch, autooff_setter_params->type);
            break;
            
        case TYPE_VALVE:
            hkc_valve_setter(autooff_setter_params->ch, HOMEKIT_UINT8(0));
            break;
            
        case TYPE_FAN:
            hkc_fan_setter(autooff_setter_params->ch, HOMEKIT_BOOL(false));
            break;
            
        default:    // case TYPE_ON:
            hkc_on_setter(autooff_setter_params->ch, HOMEKIT_BOOL(false));
            break;
    }
    
    free(autooff_setter_params);
    vTaskDelete(NULL);
}

// --- HTTP GET task
void http_get_task(void *pvParameters) {
    action_task_t *action_task = pvParameters;
    action_http_t *action_http = action_task->ch_group->action_http;
    
    while(action_http) {
        if (action_http->action == action_task->action) {
            INFO2("HTTP Action http://%s:%i/%s", action_http->host, action_http->port_n, action_http->url);
            
            const struct addrinfo hints = {
                .ai_family = AF_UNSPEC,
                .ai_socktype = SOCK_STREAM,
            };
            struct addrinfo *res;
            
            char port[5];
            itoa(action_http->port_n, port, 10);
            
            if (getaddrinfo(action_http->host, port, &hints, &res) == 0) {
                int s = socket(res->ai_family, res->ai_socktype, 0);
                if (s >= 0) {
                if (connect(s, res->ai_addr, res->ai_addrlen) == 0) {
                        uint16_t content_len_n = 0;
                        
                        char *method = "GET";
                        char *method_req = NULL;
                        if (action_http->method_n > 0) {
                            content_len_n = strlen(action_http->content);

                            char content_len[4];
                            itoa(content_len_n, content_len, 10);
                            method_req = malloc(48);
                            snprintf(method_req, 48, "Content-type: text/html\r\nContent-length: %s\r\n", content_len);
                            
                            if (action_http->method_n == 1) {
                                method = "PUT";
                            } else if (action_http->method_n == 2) {
                                method = "POST";
                            }
                        }
                    
                    uint16_t req_len = 69 + strlen(method) + ((method_req != NULL) ? strlen(method_req) : 0) + strlen(FIRMWARE_VERSION) + strlen(action_http->host) +  strlen(action_http->url) + content_len_n;
                        
                        char *req = malloc(req_len);
                        snprintf(req, req_len, "%s /%s HTTP/1.1\r\nHost: %s\r\nUser-Agent: ESPH/"FIRMWARE_VERSION" esp8266\r\nConnection: close\r\n%s\r\n%s",
                                 method,
                                 action_http->url,
                                 action_http->host,
                                 (method_req != NULL) ? method_req : "",
                                 action_http->content);
                        
                        if (write(s, req, strlen(req)) >= 0) {
                            INFO2("%s", req);
                            
                        } else {
                            ERROR2("HTTP");
                        }
                    
                    if (method_req != NULL) {
                        free(method_req);
                    }
                    
                    free(req);
                } else {
                    ERROR2("Connection");
                }
            } else {
                ERROR2("Socket");
            }
            
            close(s);
        } else {
            ERROR2("DNS");
        }
        
        freeaddrinfo(res);
        
        vTaskDelay(MS_TO_TICK(10));
    }
        
        action_http = action_http->next;
    }
    
    free(pvParameters);
    vTaskDelete(NULL);
}

// --- IR Send task
void ir_tx_task(void *pvParameters) {
    action_task_t *action_task = pvParameters;
    action_ir_tx_t *action_ir_tx = action_task->ch_group->action_ir_tx;
    
    while(action_ir_tx) {
        if (action_ir_tx->action == action_task->action) {
            uint16_t *ir_code = NULL;
            uint16_t ir_code_len = 0;
            
            uint8_t freq = ir_tx_freq;
            if (action_ir_tx->freq > 0) {
                freq = action_ir_tx->freq;
            }
            
            // Decoding protocol based IR code
            if (action_ir_tx->prot_code) {
                char *prot = NULL;
                
                if (action_ir_tx->prot) {
                    prot = action_ir_tx->prot;
                } else {
                    prot = ir_protocol;
                }
                
                // Decoding protocol based IR code length
                const uint16_t json_ir_code_len = strlen(action_ir_tx->prot_code);
                ir_code_len = 3;
                
                for (uint16_t i = 0; i < json_ir_code_len; i++) {
                    char *found = strchr(baseUC_dic, action_ir_tx->prot_code[i]);
                    if (found) {
                        ir_code_len += (1 + found - baseUC_dic) << 1;
                    } else {
                        found = strchr(baseLC_dic, action_ir_tx->prot_code[i]);
                        ir_code_len += (1 + found - baseLC_dic) << 1;
                    }
            }
            
            ir_code = malloc(sizeof(uint16_t) * ir_code_len);
            
            INFO2("IR Code Len: %i\nIR Protocol: %s", ir_code_len, prot);
            
            uint16_t bit0_mark = 0, bit0_space = 0, bit1_mark = 0, bit1_space = 0, packet;
            uint8_t index;
            for (uint8_t i=0; i<IR_ACTION_PROTOCOL_LEN / 2; i++) {
                index = i * 2;
                char *found = strchr(baseRaw_dic, prot[index]);
                packet = (found - baseRaw_dic) * IR_CODE_LEN * IR_CODE_SCALE;
                
                found = strchr(baseRaw_dic, prot[index + 1]);
                packet += (found - baseRaw_dic) * IR_CODE_SCALE;

                if (log_output)
                    printf("%s%5d ", i & 1 ? "-" : "+", packet);
                
                switch (i) {
                    case IR_CODE_HEADER_MARK_POS:
                        ir_code[0] = packet;
                        break;
                        
                    case IR_CODE_HEADER_SPACE_POS:
                        ir_code[1] = packet;
                        break;
                        
                    case IR_CODE_BIT0_MARK_POS:
                        bit0_mark = packet;
                        break;
                        
                    case IR_CODE_BIT0_SPACE_POS:
                        bit0_space = packet;
                        break;
                        
                    case IR_CODE_BIT1_MARK_POS:
                        bit1_mark = packet;
                        break;
                        
                    case IR_CODE_BIT1_SPACE_POS:
                        bit1_space = packet;
                        break;
                        
                    case IR_CODE_FOOTER_MARK_POS:
                        ir_code[ir_code_len - 1] = packet;
                        break;
                        
                    default:
                        // Do nothing
                        break;
                }
            }
            
            // Decoding BIT code part
            uint16_t ir_code_index = 2;
                for (uint16_t i = 0; i < json_ir_code_len; i++) {
                    char *found = strchr(baseUC_dic, action_ir_tx->prot_code[i]);
                    if (found) {
                        for (uint16_t j = 0; j < 1 + found - baseUC_dic; j++) {
                            ir_code[ir_code_index] = bit1_mark;
                            ir_code_index++;
                            ir_code[ir_code_index] = bit1_space;
                            ir_code_index++;
                        }
                    } else {
                        found = strchr(baseLC_dic, action_ir_tx->prot_code[i]);
                        for (uint16_t j = 0; j < 1 + found - baseLC_dic; j++) {
                            ir_code[ir_code_index] = bit0_mark;
                            ir_code_index++;
                            ir_code[ir_code_index] = bit0_space;
                            ir_code_index++;
                        }
                    }
                }
                
                if (log_output) {
                    printf("\nIR code: %s\n", action_ir_tx->prot_code);
                    for (uint16_t i = 0; i < ir_code_len; i++) {
                        printf("%s%5d ", i & 1 ? "-" : "+", ir_code[i]);
                        if (i % 16 == 15) {
                            printf("\n");
                        }

                }
                printf("\n");
            }
            
        } else {    // IR_ACTION_RAW_CODE
            const uint16_t json_ir_code_len = strlen(action_ir_tx->raw_code);
                ir_code_len = json_ir_code_len >> 1;
                
                ir_code = malloc(sizeof(uint16_t) * ir_code_len);
                
                INFO2("IR packet (%i)", ir_code_len);

                uint16_t index, packet;
                for (uint16_t i = 0; i < ir_code_len; i++) {
                    index = i << 1;
                    char *found = strchr(baseRaw_dic, action_ir_tx->raw_code[index]);
                    packet = (found - baseRaw_dic) * IR_CODE_LEN * IR_CODE_SCALE;
                    
                    found = strchr(baseRaw_dic, action_ir_tx->raw_code[index + 1]);
                    packet += (found - baseRaw_dic) * IR_CODE_SCALE;

                ir_code[i] = packet;

                if (log_output) {
                    printf("%s%5d ", i & 1 ? "-" : "+", packet);
                    if (i % 16 == 15) {
                        printf("\n");
                    }
                }
            }
            
            INFO2("");
        }
        
        // IR TRANSMITTER
        uint32_t start;
        const bool ir_true = true ^ ir_tx_inv;
        const bool ir_false = false ^ ir_tx_inv;
        
        while (ir_tx_is_running) {
            vTaskDelay(MS_TO_TICK(200));
        }
        
        ir_tx_is_running = true;
        
        for (uint8_t r = 0; r < action_ir_tx->repeats; r++) {
                for (uint16_t i = 0; i < ir_code_len; i++) {
                    if (ir_code[i] > 0) {
                        if (i & 1) {    // Space
                            gpio_write(ir_tx_gpio, ir_false);
                            sdk_os_delay_us(ir_code[i]);
                        } else {        // Mark
                            start = sdk_system_get_time();
                            while ((sdk_system_get_time() - start) < ir_code[i]) {
                                gpio_write(ir_tx_gpio, ir_true);
                                sdk_os_delay_us(freq);
                                gpio_write(ir_tx_gpio, ir_false);
                                sdk_os_delay_us(freq);
                            }
                        }
                    }
                }
                
                gpio_write(ir_tx_gpio, ir_false);
                
                INFO2("IR %i sent", r);
                
                if (action_ir_tx->pause > 0) {
                    sdk_os_delay_us(action_ir_tx->pause);
                } else {
                    vTaskDelay(MS_TO_TICK(100));
                }
                
            }       

            ir_tx_is_running = false;
        
        if (ir_code) {
            free(ir_code);
        }
            
            vTaskDelay(MS_TO_TICK(100));
        }
        
        action_ir_tx = action_ir_tx->next;
    }
    
    free(pvParameters);
    vTaskDelete(NULL);
}

// --- ACTIONS
void autoswitch_task(void *pvParameters) {
    autoswitch_params_t *autoswitch_params = pvParameters;

    vTaskDelay(MS_TO_TICK(autoswitch_params->time * 1000));
    
    gpio_write(autoswitch_params->gpio, autoswitch_params->value);
    INFO2("AutoSw digO GPIO %i -> %i", autoswitch_params->gpio, autoswitch_params->value);
    
    free(autoswitch_params);
    vTaskDelete(NULL);
}

void do_actions(ch_group_t *ch_group, uint8_t action) {
    INFO2("Exec action %i", action);
    
    // Copy actions
    action_copy_t *action_copy = ch_group->action_copy;
    while(action_copy) {
        if (action_copy->action == action) {
            action = action_copy->new_action;
            action_copy = NULL;
        } else {
             action_copy = action_copy->next;
        }
    }
    
    // Digital outputs
    action_relay_t *action_relay = ch_group->action_relay;
    while(action_relay) {
        if (action_relay->action == action) {
            gpio_write(action_relay->gpio, action_relay->value);
            INFO2("DigO GPIO %i -> %i", action_relay->gpio, action_relay->value);
            
            if (action_relay->inching > 0) {
                autoswitch_params_t *autoswitch_params = malloc(sizeof(autoswitch_params_t));
                autoswitch_params->gpio = action_relay->gpio;
                autoswitch_params->value = !action_relay->value;
                autoswitch_params->time = action_relay->inching;
                xTaskCreate(autoswitch_task, "autoswitch_task", AUTOSWITCH_TASK_SIZE, autoswitch_params, 1, NULL);
            }
        }
        
        action_relay = action_relay->next;
    }
    
    // Accessory Manager
    action_acc_manager_t *action_acc_manager = ch_group->action_acc_manager;
    while(action_acc_manager) {
        if (action_acc_manager->action == action) {
            ch_group_t *ch_group = ch_group_find_by_acc(action_acc_manager->accessory);
            if (ch_group) {
                if (action_acc_manager->is_kill_switch) {
                    switch ((uint8_t) action_acc_manager->value) {
                        case 1:
                            if (ch_group->ch_child) {
                                hkc_setter(ch_group->ch_child, HOMEKIT_BOOL(true));
                            }
                            break;
                            
                        case 2:
                            if (ch_group->ch_sec) {
                                hkc_setter(ch_group->ch_sec, HOMEKIT_BOOL(false));
                            }
                            break;
                            
                        case 3:
                            if (ch_group->ch_sec) {
                                hkc_setter(ch_group->ch_sec, HOMEKIT_BOOL(true));
                            }
                            break;
                            
                        default:    // case 0:
                            if (ch_group->ch_child) {
                                hkc_setter(ch_group->ch_child, HOMEKIT_BOOL(false));
                            }
                            break;
                    }
                    
                    INFO2("Kill Sw Manager %i -> %.2f", action_acc_manager->accessory, action_acc_manager->value);
                    
                } else {
                    switch (ch_group->acc_type) {
                        case ACC_TYPE_BUTTON:
                            button_event(0, ch_group->ch0, (uint8_t) action_acc_manager->value);
                            break;
                            
                        case ACC_TYPE_LOCK:
                            hkc_lock_setter(ch_group->ch1, HOMEKIT_UINT8((uint8_t) action_acc_manager->value));
                            break;
                            
                        case ACC_TYPE_CONTACT_SENSOR:
                            if ((bool) action_acc_manager->value) {
                                sensor_1(0, ch_group->ch0, TYPE_SENSOR);
                            } else {
                                sensor_0(0, ch_group->ch0, TYPE_SENSOR);
                            }
                            break;
                            
                        case ACC_TYPE_MOTION_SENSOR:
                            if ((bool) action_acc_manager->value) {
                                sensor_1(0, ch_group->ch0, TYPE_SENSOR_BOOL);
                            } else {
                                sensor_0(0, ch_group->ch0, TYPE_SENSOR_BOOL);
                            }
                            break;
                            
                        case ACC_TYPE_WATER_VALVE:
                            if (action_acc_manager->value == -1) {
                                if (ch_group->ch3) {
                                    ch_group->ch3->value = ch_group->ch2->value;
                                }
                            } else {
                                hkc_valve_setter(ch_group->ch0, HOMEKIT_UINT8((uint8_t) action_acc_manager->value));
                            }
                            break;
                            
                        case ACC_TYPE_THERMOSTAT:
                            if (action_acc_manager->value == 0.02 || action_acc_manager->value == 0.03) {
                                update_th(ch_group->ch1, HOMEKIT_UINT8((uint8_t) ((action_acc_manager->value - 0.02) * 100)));
                            } else if (action_acc_manager->value == 0.04 || action_acc_manager->value == 0.05 || action_acc_manager->value == 0.06) {
                                update_th(ch_group->ch4, HOMEKIT_UINT8((uint8_t) ((action_acc_manager->value - 0.04) * 100)));
                            } else {
                                if (((uint16_t) (action_acc_manager->value * 100) % 2) == 0) {
                                    update_th(ch_group->ch5, HOMEKIT_FLOAT(action_acc_manager->value));
                                } else {
                                    update_th(ch_group->ch6, HOMEKIT_FLOAT(action_acc_manager->value - 0.01));
                                }
                            }
                            break;
                            
                        case ACC_TYPE_GARAGE_DOOR:
                            if (action_acc_manager->value < 2) {
                                hkc_garage_door_setter(ch_group->ch1, HOMEKIT_UINT8((uint8_t) action_acc_manager->value));
                            } else if (action_acc_manager->value == 2) {
                                garage_door_stop(0, ch_group->ch0, 0);
                            } else {
                                garage_door_obstruction(0, ch_group->ch0, action_acc_manager->value - 3);
                            }
                            break;
                            
                        case ACC_TYPE_LIGHTBULB:
                            hkc_rgbw_setter(ch_group->ch0, HOMEKIT_BOOL((bool) action_acc_manager->value));
                            break;
                            
                        case ACC_TYPE_WINDOW_COVER:
                            if (action_acc_manager->value < 0) {
                                window_cover_obstruction(0, ch_group->ch0, action_acc_manager->value + 2);
                            } else if (action_acc_manager->value > 100) {
                                hkc_window_cover_setter(WINDOW_COVER_CH_TARGET_POSITION, WINDOW_COVER_CH_CURRENT_POSITION->value);
                            } else {
                                hkc_window_cover_setter(WINDOW_COVER_CH_TARGET_POSITION, HOMEKIT_UINT8(action_acc_manager->value));
                            }
                            break;
                            
                        case ACC_TYPE_FAN:
                            if (action_acc_manager->value == 0) {
                                hkc_fan_setter(ch_group->ch0, HOMEKIT_BOOL(false));
                            } else if (action_acc_manager->value > 100) {
                                hkc_fan_setter(ch_group->ch0, HOMEKIT_BOOL(true));
                            } else {
                                hkc_fan_speed_setter(ch_group->ch1, HOMEKIT_FLOAT(action_acc_manager->value));
                            }
                            break;
                            
                        case ACC_TYPE_TV:
                            if (action_acc_manager->value < 2 && action_acc_manager->value >= 0) {
                                hkc_tv_active(ch_group->ch0, HOMEKIT_UINT8(action_acc_manager->value));
                            } else if (action_acc_manager->value < 20) {
                                hkc_tv_key(ch_group->ch3, HOMEKIT_UINT8(action_acc_manager->value - 2));
                            } else if (action_acc_manager->value < 22) {
                                hkc_tv_mute(ch_group->ch5, HOMEKIT_BOOL((bool) (action_acc_manager->value - 20)));
                            } else if (action_acc_manager->value < 24) {
                                hkc_tv_volume(ch_group->ch7, HOMEKIT_UINT8(action_acc_manager->value - 22));
                            } else if (action_acc_manager->value < 32) {
                                hkc_tv_power_mode(ch_group->ch4, HOMEKIT_UINT8(action_acc_manager->value - 30));
                            } else if (action_acc_manager->value > 100) {
                                hkc_tv_active_identifier(ch_group->ch2, HOMEKIT_UINT8(action_acc_manager->value - 100));
                            }
                            break;
                            
                        default:    // ON Type ch
                            if (action_acc_manager->value == -1) {
                                if (ch_group->ch2) {
                                    ch_group->ch2->value = ch_group->ch1->value;
                                }
                            } else {
                                hkc_on_setter(ch_group->ch0, HOMEKIT_BOOL((bool) action_acc_manager->value));
                            }
                            break;
                    }
                    
                    INFO2("Acc Manager %i -> %.2f", action_acc_manager->accessory, action_acc_manager->value);
                }
            } else {
                ERROR2("No acc found: %i", action_acc_manager->accessory);
            }
        }

        action_acc_manager = action_acc_manager->next;
    }
    
    // System actions
    action_system_t *action_system = ch_group->action_system;
    while(action_system) {
        if (action_system->action == action) {
            INFO2("Sys Action %i", action_system->value);
            
            char *ota = NULL;
            
            switch (action_system->value) {
                case SYSTEM_ACTION_SETUP_MODE:
                    setup_mode_call(0, NULL, 0);
                    break;
                    
                case SYSTEM_ACTION_OTA_UPDATE:
                    if (sysparam_get_string("ota_repo", &ota) == SYSPARAM_OK) {
                        rboot_set_temp_rom(1);
                        xTaskCreate(reboot_task, "reboot_task", REBOOT_TASK_SIZE, NULL, 1, NULL);
                    }
                    break;
                    
                default:    // case SYSTEM_ACTION_REBOOT:
                    xTaskCreate(reboot_task, "reboot_task", REBOOT_TASK_SIZE, NULL, 1, NULL);
                    break;
            }
        }
        
        action_system = action_system->next;
    }
    
    // HTTP GET actions
    if (ch_group->action_http) {
        action_task_t *action_task = malloc(sizeof(action_task_t));
        memset(action_task, 0, sizeof(*action_task));
        action_task->action = action;
        action_task->ch_group = ch_group;
        
        xTaskCreate(http_get_task, "http_get_task", HTTP_GET_TASK_SIZE, action_task, 0, NULL);
    }
    
    // IR TX actions
    if (ch_group->action_ir_tx) {
        action_task_t *action_task = malloc(sizeof(action_task_t));
        memset(action_task, 0, sizeof(*action_task));
        action_task->action = action;
        action_task->ch_group = ch_group;
        
        xTaskCreate(ir_tx_task, "ir_tx_task", IR_TX_TASK_SIZE, action_task, IR_TX_TASK_PRIORITY, NULL);
    }
}

void do_wildcard_actions(ch_group_t *ch_group, uint8_t index, const float action_value) {
    INFO2("Wildcard %i %.2f", index, action_value);
    float last_value, last_diff = 10000;
    wildcard_action_t *wildcard_action = ch_group->wildcard_action;
    wildcard_action_t *last_wildcard_action = NULL;
    
    while (wildcard_action) {
        if (wildcard_action->index == index) {
            const float diff = action_value - wildcard_action->value;

            if (wildcard_action->value <= action_value && diff < last_diff) {
                last_value = wildcard_action->value;
                last_diff = diff;
                last_wildcard_action = wildcard_action;
            }
        }
        
        wildcard_action = wildcard_action->next;
    }
            
            if (last_wildcard_action != NULL) {
        if (ch_group->last_wildcard_action[index] != last_value || last_wildcard_action->repeat) {
            ch_group->last_wildcard_action[index] = last_value;
            INFO2("Wilcard Action %i %.2f", index, last_value);
            do_actions(ch_group, last_wildcard_action->target_action);
        }
    }
}

// --- IDENTIFY
void identify(homekit_value_t _value) {
    led_blink(6);
    INFO2("ID");
}

// ---------

void delayed_sensor_starter_task(void *args) {
    INFO2("Starting delayed sensor");
    ch_group_t *ch_group = args;
    
    vTaskDelay(ch_group->accessory * MS_TO_TICK(TH_SENSOR_POLL_PERIOD_MIN * 1000));
    
    temperature_timer_worker(ch_group);
    sdk_os_timer_arm(ch_group->timer, TH_SENSOR_POLL_PERIOD * 1000, 1);
    
    vTaskDelete(NULL);
}

homekit_characteristic_t name = HOMEKIT_CHARACTERISTIC_(NAME, NULL);
homekit_characteristic_t serial = HOMEKIT_CHARACTERISTIC_(SERIAL_NUMBER, NULL);
homekit_characteristic_t manufacturer = HOMEKIT_CHARACTERISTIC_(MANUFACTURER, "Wieslaw Weglowski");
homekit_characteristic_t model = HOMEKIT_CHARACTERISTIC_(MODEL, "ESPH-Device");
homekit_characteristic_t identify_function = HOMEKIT_CHARACTERISTIC_(IDENTIFY, identify);
homekit_characteristic_t firmware = HOMEKIT_CHARACTERISTIC_(FIRMWARE_REVISION, FIRMWARE_VERSION);

homekit_server_config_t config;

void run_homekit_server() {
    wifi_channel = sdk_wifi_get_channel();
    
    FREEHEAP();
    
    if (enable_homekit_server) {
        INFO2("Start HK Server");

        homekit_server_init(&config);
        FREEHEAP();
    }
    
    sdk_os_timer_setfn(&wifi_watchdog_timer, wifi_watchdog, NULL);
    sdk_os_timer_arm(&wifi_watchdog_timer, WIFI_WATCHDOG_POLL_PERIOD_MS, 1);

    if (ping_inputs) {
        ping_task_timer = malloc(sizeof(ETSTimer));
        memset(ping_task_timer, 0, sizeof(*ping_task_timer));
        sdk_os_timer_setfn(ping_task_timer, ping_task_timer_worker, NULL);
        sdk_os_timer_arm(ping_task_timer, PING_POLL_DELAY, 1);
    }
    
    led_blink(6);
}

void printf_header() {
    printf("\n\n\n");
    printf("ESPH v%s\n", FIRMWARE_VERSION);
    printf("Developed by wwns\n\n");
    
#ifdef ESPH_DEBUG
    INFO2("ESPH DEBUG ENABLED\n");
#endif  // ESPH_DEBUG
}

void normal_mode_init() {
    char *txt_config = NULL;
    sysparam_get_string("esph_conf", &txt_config);
    
    cJSON *json_esph = cJSON_Parse(txt_config);
    cJSON *json_config = cJSON_GetObjectItemCaseSensitive(json_esph, GENERAL_CONFIG);
    cJSON *json_accessories = cJSON_GetObjectItemCaseSensitive(json_esph, ACCESSORIES);
    
    const uint8_t total_accessories = cJSON_GetArraySize(json_accessories);
    
    if (total_accessories == 0) {
        uart_set_baud(0, 115200);
        printf("\n\n\n! Invalid JSON\n");
        sysparam_set_int8("total_ac", 0);
        sysparam_set_int8("setup", 2);
         xTaskCreate(reboot_task, "reboot_task", REBOOT_TASK_SIZE, NULL, 1, NULL);
        
        vTaskDelete(NULL);
    }
    
    xTaskCreate(exit_emergency_setup_mode_task, "exit_emergency_setup_mode_task", configMINIMAL_STACK_SIZE, NULL, 1, NULL);
    
    // Filling Used GPIO Array
    for (uint8_t g=0; g<18; g++) {
        used_gpio[g] = false;
    }
    
    // Buttons GPIO Setup function
    bool diginput_register(cJSON *json_buttons, void *callback, homekit_characteristic_t *hk_ch, const uint8_t param) {
        bool run_at_launch = false;
        
        for(uint8_t j=0; j<cJSON_GetArraySize(json_buttons); j++) {
            const uint8_t gpio = (uint8_t) cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(json_buttons, j), PIN_GPIO)->valuedouble;
            bool pullup_resistor = true;
            if (cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(json_buttons, j), PULLUP_RESISTOR) != NULL &&
                cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(json_buttons, j), PULLUP_RESISTOR)->valuedouble == 0) {
                pullup_resistor = false;
            }
            
            bool inverted = false;
            if (cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(json_buttons, j), INVERTED) != NULL &&
                cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(json_buttons, j), INVERTED)->valuedouble == 1) {
                inverted = true;
            }
            
            uint8_t button_type = 1;
            if (cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(json_buttons, j), BUTTON_PRESS_TYPE) != NULL) {
                button_type = (uint8_t) cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(json_buttons, j), BUTTON_PRESS_TYPE)->valuedouble;
            }
            
            if (!used_gpio[gpio]) {
                adv_button_create(gpio, pullup_resistor, inverted);
                used_gpio[gpio] = true;
            }
            adv_button_register_callback_fn(gpio, callback, button_type, (void *) hk_ch, param);
            
            INFO2("Digital input GPIO: %i, type: %i, inv: %i", gpio, button_type, inverted);
             
            if (gpio_read(gpio) == button_type) {
                run_at_launch = true;
            }
        }
        
        return run_at_launch;
    }
    
    // Ping Setup function
    void ping_register(cJSON *json_pings, void *callback, homekit_characteristic_t *ch, const uint8_t param) {
        for(uint8_t j = 0; j < cJSON_GetArraySize(json_pings); j++) {
            ping_input_t *ping_input = ping_input_find_by_host(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(json_pings, j), PING_HOST)->valuestring);
            
            if (!ping_input) {
                ping_input = malloc(sizeof(ping_input_t));
                memset(ping_input, 0, sizeof(*ping_input));
                
                ping_input->host = strdup(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(json_pings, j), PING_HOST)->valuestring);
                ping_input->last_response = false;
                
                ping_input->next = ping_inputs;
                ping_inputs = ping_input;
            }
            
            bool response_type = true;
            if (cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(json_pings, j), PING_RESPONSE_TYPE) != NULL &&
                cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(json_pings, j), PING_RESPONSE_TYPE)->valuedouble == 0) {
                response_type = false;
            }
            
            ping_input_callback_fn_t *ping_input_callback_fn;
            ping_input_callback_fn = malloc(sizeof(ping_input_callback_fn_t));
            memset(ping_input_callback_fn, 0, sizeof(*ping_input_callback_fn));
            
            ping_input_callback_fn->callback = callback;
            ping_input_callback_fn->ch = ch;
            ping_input_callback_fn->param = param;
            
            if (response_type) {
                ping_input_callback_fn->next = ping_input->callback_1;
                ping_input->callback_1 = ping_input_callback_fn;
            } else {
                ping_input_callback_fn->next = ping_input->callback_0;
                ping_input->callback_0 = ping_input_callback_fn;
            }
            
            INFO2("Ping input: %s, res: %i", ping_input->host, response_type);
        }
    }
    
    // Initial state function
    double set_initial_state(const uint8_t accessory, const uint8_t ch_number, cJSON *json_context, homekit_characteristic_t *ch, const uint8_t ch_type, const double default_value) {
        double state = default_value;
        INFO2("Set init state");
        if (cJSON_GetObjectItemCaseSensitive(json_context, INITIAL_STATE) != NULL) {
            const uint8_t initial_state = (uint8_t) cJSON_GetObjectItemCaseSensitive(json_context, INITIAL_STATE)->valuedouble;
            if (initial_state < INIT_STATE_LAST) {
                    state = initial_state;
            } else {
                char *saved_state_id = malloc(3);
                uint16_t int_saved_state_id = ((accessory + 10) * 10) + ch_number;
                
                itoa(int_saved_state_id, saved_state_id, 10);
                last_state_t *last_state = malloc(sizeof(last_state_t));
                memset(last_state, 0, sizeof(*last_state));
                last_state->id = saved_state_id;
                last_state->ch = ch;
                last_state->ch_type = ch_type;
                last_state->next = last_states;
                last_states = last_state;
                
                sysparam_status_t status;
                bool saved_state_bool = false;
                int8_t saved_state_int8;
                int32_t saved_state_int32;
                char *saved_state_string = NULL;
                
                switch (ch_type) {
                    case CH_TYPE_INT8:
                        status = sysparam_get_int8(saved_state_id, &saved_state_int8);
                        
                        if (status == SYSPARAM_OK) {
                            state = saved_state_int8;
                        }
                        break;
                        
                    case CH_TYPE_INT32:
                        status = sysparam_get_int32(saved_state_id, &saved_state_int32);
                        
                        if (status == SYSPARAM_OK) {
                            state = saved_state_int32;
                        }
                        break;
                        
                    case CH_TYPE_FLOAT:
                        status = sysparam_get_int32(saved_state_id, &saved_state_int32);
                        
                        if (status == SYSPARAM_OK) {
                            state = saved_state_int32 / 100.00f;
                        }
                        break;
                        
                    case CH_TYPE_STRING:
                        status = sysparam_get_string(saved_state_id, &saved_state_string);
                        
                        if (status == SYSPARAM_OK) {
                            state = (uint32_t) saved_state_string;
                        }
                        break;
                        
                    default:    // case CH_TYPE_BOOL
                        status = sysparam_get_bool(saved_state_id, &saved_state_bool);
                        
                        if (status == SYSPARAM_OK) {
                            if (initial_state == INIT_STATE_LAST) {
                                state = saved_state_bool;
                            } else if (ch_type == CH_TYPE_BOOL) {    // initial_state == INIT_STATE_INV_LAST
                                state = !saved_state_bool;
                            }
                        }
                        break;
                }
                
                if (status != SYSPARAM_OK) {
                    ERROR2("No saved state found");
                }
                
                if (ch_type == CH_TYPE_STRING && state > 0) {
                    INFO2("Init state = %s", (char *) (uint32_t) state);
                } else {
                    INFO2("Init state = %.2f", state);
                }
                
            }
        }
        
        return state;
    }
    
    // REGISTER ACTIONS
    // Copy actions
    void new_action_copy(ch_group_t *ch_group, cJSON *json_context, uint8_t fixed_action) {
        action_copy_t *last_action = ch_group->action_copy;
        
        void register_action(cJSON *json_accessory, uint8_t new_int_action) {
            char action[3];
            itoa(new_int_action, action, 10);
            if (cJSON_GetObjectItemCaseSensitive(json_accessory, action) != NULL) {
                cJSON *json_action = cJSON_GetObjectItemCaseSensitive(json_accessory, action);
                if (cJSON_GetObjectItemCaseSensitive(json_action, COPY_ACTIONS) != NULL) {
                    action_copy_t *action_copy = malloc(sizeof(action_copy_t));
                    memset(action_copy, 0, sizeof(*action_copy));
                    
                    action_copy->action = new_int_action;
                    action_copy->new_action = (uint8_t) cJSON_GetObjectItemCaseSensitive(json_action, COPY_ACTIONS)->valuedouble;
                    
                    action_copy->next = last_action;
                    last_action = action_copy;
                }
            }
        }
        
        if (fixed_action < MAX_ACTIONS) {
            for (uint8_t int_action = 0; int_action < MAX_ACTIONS; int_action++) {
                register_action(json_context, int_action);
            }
        } else {
            register_action(json_context, fixed_action);
        }
        
        ch_group->action_copy = last_action;
    }
    
    // Digital outputs
    void new_action_relay(ch_group_t *ch_group, cJSON *json_context, uint8_t fixed_action) {
        action_relay_t *last_action = ch_group->action_relay;
        
        void register_action(cJSON *json_accessory, uint8_t new_int_action) {
            char action[3];
            itoa(new_int_action, action, 10);
            if (cJSON_GetObjectItemCaseSensitive(json_accessory, action) != NULL) {
                if (cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(json_accessory, action), DIGITAL_OUTPUTS_ARRAY) != NULL) {
                    cJSON *json_relays = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(json_accessory, action), DIGITAL_OUTPUTS_ARRAY);
                    for(int16_t i = cJSON_GetArraySize(json_relays) - 1; i >= 0; i--) {
                        action_relay_t *action_relay = malloc(sizeof(action_relay_t));
                        memset(action_relay, 0, sizeof(*action_relay));
                        
                        cJSON *json_relay = cJSON_GetArrayItem(json_relays, i);
                        
                        action_relay->action = new_int_action;
                        
                        action_relay->gpio = (uint8_t) cJSON_GetObjectItemCaseSensitive(json_relay, PIN_GPIO)->valuedouble;
                        if (!used_gpio[action_relay->gpio]) {
                            gpio_enable(action_relay->gpio, GPIO_OUTPUT);
                            gpio_write(action_relay->gpio, false);
                            
                            used_gpio[action_relay->gpio] = true;
                            INFO2("DigO GPIO: %i", action_relay->gpio);
                        }
                        
                        action_relay->value = false;
                        if (cJSON_GetObjectItemCaseSensitive(json_relay, VALUE) != NULL) {
                            action_relay->value = (bool) cJSON_GetObjectItemCaseSensitive(json_relay, VALUE)->valuedouble;
                        }
                        
                        action_relay->inching = 0;
                        if (cJSON_GetObjectItemCaseSensitive(json_relay, AUTOSWITCH_TIME) != NULL) {
                            action_relay->inching = (float) cJSON_GetObjectItemCaseSensitive(json_relay, AUTOSWITCH_TIME)->valuedouble;
                        }
                        
                        action_relay->next = last_action;
                        last_action = action_relay;
                    }
                }
            }
        }
        
        if (fixed_action < MAX_ACTIONS) {
            for (uint8_t int_action = 0; int_action < MAX_ACTIONS; int_action++) {
                register_action(json_context, int_action);
            }
        } else {
            register_action(json_context, fixed_action);
        }
        
        ch_group->action_relay = last_action;
    }
    
    // Accessory Manager
    void new_action_acc_manager(ch_group_t *ch_group, cJSON *json_context, uint8_t fixed_action) {
        action_acc_manager_t *last_action = ch_group->action_acc_manager;
        
        void register_action(cJSON *json_accessory, uint8_t new_int_action) {
            char action[3];
            itoa(new_int_action, action, 10);
            if (cJSON_GetObjectItemCaseSensitive(json_accessory, action) != NULL) {
                if (cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(json_accessory, action), MANAGE_OTHERS_ACC_ARRAY) != NULL) {
                    cJSON *json_acc_managers = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(json_accessory, action), MANAGE_OTHERS_ACC_ARRAY);
                    
                    for(int16_t i = cJSON_GetArraySize(json_acc_managers) - 1; i >= 0; i--) {
                        action_acc_manager_t *action_acc_manager = malloc(sizeof(action_acc_manager_t));
                        memset(action_acc_manager, 0, sizeof(*action_acc_manager));
                        
                        cJSON *json_acc_manager = cJSON_GetArrayItem(json_acc_managers, i);
                        
                        action_acc_manager->action = new_int_action;
                        
                        action_acc_manager->is_kill_switch = false;
                        action_acc_manager->accessory = 1;
                        if (cJSON_GetObjectItemCaseSensitive(json_acc_manager, ACCESSORY_INDEX_KILL_SWITCH) != NULL) {
                            action_acc_manager->is_kill_switch = true;
                            action_acc_manager->accessory = (uint8_t) cJSON_GetObjectItemCaseSensitive(json_acc_manager, ACCESSORY_INDEX_KILL_SWITCH)->valuedouble;
                        } else if (cJSON_GetObjectItemCaseSensitive(json_acc_manager, ACCESSORY_INDEX) != NULL) {
                            action_acc_manager->accessory = (uint8_t) cJSON_GetObjectItemCaseSensitive(json_acc_manager, ACCESSORY_INDEX)->valuedouble;
                        }

                        action_acc_manager->value = 0;
                        if (cJSON_GetObjectItemCaseSensitive(json_acc_manager, VALUE) != NULL) {
                            action_acc_manager->value = (float) cJSON_GetObjectItemCaseSensitive(json_acc_manager, VALUE)->valuedouble;
                        }
                        
                        action_acc_manager->next = last_action;
                        last_action = action_acc_manager;
                    }
                }
            }
        }
        
        if (fixed_action < MAX_ACTIONS) {
            for (uint8_t int_action = 0; int_action < MAX_ACTIONS; int_action++) {
                register_action(json_context, int_action);
            }
        } else {
            register_action(json_context, fixed_action);
        }
        
        ch_group->action_acc_manager = last_action;
    }
    
    // System Actions
    void new_action_system(ch_group_t *ch_group, cJSON *json_context, uint8_t fixed_action) {
        action_system_t *last_action = ch_group->action_system;
        
        void register_action(cJSON *json_accessory, uint8_t new_int_action) {
            char action[3];
            itoa(new_int_action, action, 10);
            if (cJSON_GetObjectItemCaseSensitive(json_accessory, action) != NULL) {
                if (cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(json_accessory, action), SYSTEM_ACTIONS_ARRAY) != NULL) {
                    cJSON *json_action_systems = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(json_accessory, action), SYSTEM_ACTIONS_ARRAY);
                    for(int16_t i = cJSON_GetArraySize(json_action_systems) - 1; i >= 0; i--) {
                        action_system_t *action_system = malloc(sizeof(action_system_t));
                        memset(action_system, 0, sizeof(*action_system));
                        
                        cJSON *json_action_system = cJSON_GetArrayItem(json_action_systems, i);
                        
                        action_system->action = new_int_action;
                        
                        action_system->value = (uint8_t) cJSON_GetObjectItemCaseSensitive(json_action_system, SYSTEM_ACTION)->valuedouble;
                        
                        action_system->next = last_action;
                        last_action = action_system;
                    }
                }
            }
        }
        
        if (fixed_action < MAX_ACTIONS) {
            for (uint8_t int_action = 0; int_action < MAX_ACTIONS; int_action++) {
                register_action(json_context, int_action);
            }
        } else {
            register_action(json_context, fixed_action);
        }
        
        ch_group->action_system = last_action;
    }
    
    // HTTP GET Actions
    void new_action_http(ch_group_t *ch_group, cJSON *json_context, uint8_t fixed_action) {
        action_http_t *last_action = ch_group->action_http;
        
        void register_action(cJSON *json_accessory, uint8_t new_int_action) {
            char action[3];
            itoa(new_int_action, action, 10);
            if (cJSON_GetObjectItemCaseSensitive(json_accessory, action) != NULL) {
                if (cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(json_accessory, action), HTTP_ACTIONS_ARRAY) != NULL) {
                    cJSON *json_action_https = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(json_accessory, action), HTTP_ACTIONS_ARRAY);
                    for(int16_t i = cJSON_GetArraySize(json_action_https) - 1; i >= 0; i--) {
                        action_http_t *action_http = malloc(sizeof(action_http_t));
                        memset(action_http, 0, sizeof(*action_http));
                        
                        cJSON *json_action_http = cJSON_GetArrayItem(json_action_https, i);
                        
                        action_http->action = new_int_action;
                        
                        action_http->host = strdup(cJSON_GetObjectItemCaseSensitive(json_action_http, HTTP_ACTION_HOST)->valuestring);
                        
                        if (cJSON_GetObjectItemCaseSensitive(json_action_http, HTTP_ACTION_URL) != NULL) {
                            action_http->url = strdup(cJSON_GetObjectItemCaseSensitive(json_action_http, HTTP_ACTION_URL)->valuestring);
                        } else {
                            action_http->url = strdup("");
                        }
                        
                        action_http->port_n = 80;
                        if (cJSON_GetObjectItemCaseSensitive(json_action_http, HTTP_ACTION_PORT) != NULL) {
                            action_http->port_n = (uint16_t) cJSON_GetObjectItemCaseSensitive(json_action_http, HTTP_ACTION_PORT)->valuedouble;
                        }
                        
                        action_http->method_n = 0;
                        if (cJSON_GetObjectItemCaseSensitive(json_action_http, HTTP_ACTION_METHOD) != NULL) {
                            action_http->method_n = (uint8_t) cJSON_GetObjectItemCaseSensitive(json_action_http, HTTP_ACTION_METHOD)->valuedouble;
                        }
                        
                        if (cJSON_GetObjectItemCaseSensitive(json_action_http, HTTP_ACTION_CONTENT) != NULL) {
                            action_http->content = strdup(cJSON_GetObjectItemCaseSensitive(json_action_http, HTTP_ACTION_CONTENT)->valuestring);
                        } else {
                            action_http->content = strdup("");
                        }
                        
                        action_http->next = last_action;
                        last_action = action_http;
                    }
                }
            }
        }
        
        if (fixed_action < MAX_ACTIONS) {
            for (uint8_t int_action = 0; int_action < MAX_ACTIONS; int_action++) {
                register_action(json_context, int_action);
            }
        } else {
            register_action(json_context, fixed_action);
        }
        
        ch_group->action_http = last_action;
    }
    
    // IR TX Actions
    void new_action_ir_tx(ch_group_t *ch_group, cJSON *json_context, uint8_t fixed_action) {
        action_ir_tx_t *last_action = ch_group->action_ir_tx;
        
        void register_action(cJSON *json_accessory, uint8_t new_int_action) {
            char action[3];
            itoa(new_int_action, action, 10);
            if (cJSON_GetObjectItemCaseSensitive(json_accessory, action) != NULL) {
                if (cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(json_accessory, action), IR_ACTIONS_ARRAY) != NULL) {
                    cJSON *json_action_ir_txs = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(json_accessory, action), IR_ACTIONS_ARRAY);
                    for(int16_t i = cJSON_GetArraySize(json_action_ir_txs) - 1; i >= 0; i--) {
                        action_ir_tx_t *action_ir_tx = malloc(sizeof(action_ir_tx_t));
                        memset(action_ir_tx, 0, sizeof(*action_ir_tx));
                        
                        cJSON *json_action_ir_tx = cJSON_GetArrayItem(json_action_ir_txs, i);
                        
                        action_ir_tx->action = new_int_action;
                        
                        if (cJSON_GetObjectItemCaseSensitive(json_action_ir_tx, IR_ACTION_PROTOCOL) != NULL) {
                            action_ir_tx->prot = strdup(cJSON_GetObjectItemCaseSensitive(json_action_ir_tx, IR_ACTION_PROTOCOL)->valuestring);
                        }
                        
                        if (cJSON_GetObjectItemCaseSensitive(json_action_ir_tx, IR_ACTION_PROTOCOL_CODE) != NULL) {
                            action_ir_tx->prot_code = strdup(cJSON_GetObjectItemCaseSensitive(json_action_ir_tx, IR_ACTION_PROTOCOL_CODE)->valuestring);
                        }

                        if (cJSON_GetObjectItemCaseSensitive(json_action_ir_tx, IR_ACTION_RAW_CODE) != NULL) {
                            action_ir_tx->raw_code = strdup(cJSON_GetObjectItemCaseSensitive(json_action_ir_tx, IR_ACTION_RAW_CODE)->valuestring);
                        }
                        
                        action_ir_tx->freq = 0;
                        if (cJSON_GetObjectItemCaseSensitive(json_action_ir_tx, IR_ACTION_FREQ) != NULL) {
                            action_ir_tx->freq = 1000 / cJSON_GetObjectItemCaseSensitive(json_action_ir_tx, IR_ACTION_FREQ)->valuedouble / 2;
                        }
                        
                        action_ir_tx->repeats = 1;
                        if (cJSON_GetObjectItemCaseSensitive(json_action_ir_tx, IR_ACTION_REPEATS) != NULL) {
                            action_ir_tx->repeats = (uint8_t) cJSON_GetObjectItemCaseSensitive(json_action_ir_tx, IR_ACTION_REPEATS)->valuedouble;
                        }
                        
                        action_ir_tx->pause = 0;
                        if (cJSON_GetObjectItemCaseSensitive(json_action_ir_tx, IR_ACTION_REPEATS_PAUSE) != NULL) {
                            action_ir_tx->pause = (uint16_t) cJSON_GetObjectItemCaseSensitive(json_action_ir_tx, IR_ACTION_REPEATS_PAUSE)->valuedouble;
                        }
                        
                        action_ir_tx->next = last_action;
                        last_action = action_ir_tx;
                    }
                }
            }
        }
        
        if (fixed_action < MAX_ACTIONS) {
            for (uint8_t int_action = 0; int_action < MAX_ACTIONS; int_action++) {
                register_action(json_context, int_action);
            }
        } else {
            register_action(json_context, fixed_action);
        }
        
        ch_group->action_ir_tx = last_action;
    }
    
    void register_actions(ch_group_t *ch_group, cJSON *json_accessory, uint8_t fixed_action) {
        new_action_copy(ch_group, json_accessory, fixed_action);
        new_action_relay(ch_group, json_accessory, fixed_action);
        new_action_acc_manager(ch_group, json_accessory, fixed_action);
        new_action_system(ch_group, json_accessory, fixed_action);
        new_action_http(ch_group, json_accessory, fixed_action);
        new_action_ir_tx(ch_group, json_accessory, fixed_action);
    }
    
    void register_wildcard_actions(ch_group_t *ch_group, cJSON *json_accessory) {
        uint8_t global_index = MAX_ACTIONS;
        wildcard_action_t *last_action = ch_group->wildcard_action;
        
        for (uint8_t int_index = 0; int_index < MAX_WILDCARD_ACTIONS; int_index++) {
            char number[2];
            itoa(int_index, number, 10);
            
            char index[3];
            snprintf(index, 3, "%s%s", WILDCARD_ACTIONS_ARRAY_HEADER, number);
            
            cJSON *json_wilcard_actions = cJSON_GetObjectItemCaseSensitive(json_accessory, index);
            for (uint8_t i = 0; i < cJSON_GetArraySize(json_wilcard_actions); i++) {
                wildcard_action_t *wildcard_action = malloc(sizeof(wildcard_action_t));
                memset(wildcard_action, 0, sizeof(*wildcard_action));
                
                cJSON *json_wilcard_action = cJSON_GetArrayItem(json_wilcard_actions, i);
                
                wildcard_action->index = int_index;
                wildcard_action->value = (float) cJSON_GetObjectItemCaseSensitive(json_wilcard_action, VALUE)->valuedouble;
                
                wildcard_action->repeat = false;
                if (cJSON_GetObjectItemCaseSensitive(json_wilcard_action, WILDCARD_ACTION_REPEAT) != NULL) {
                    wildcard_action->repeat = (bool) cJSON_GetObjectItemCaseSensitive(json_wilcard_action, WILDCARD_ACTION_REPEAT)->valuedouble;
                }
                
                if (cJSON_GetObjectItemCaseSensitive(json_wilcard_action, "0") != NULL) {
                    char action[3];
                    itoa(global_index, action, 10);
                    cJSON *json_new_input_action = cJSON_CreateObject();
                    cJSON_AddItemReferenceToObject(json_new_input_action, action, cJSON_GetObjectItemCaseSensitive(json_wilcard_action, "0"));
                    register_actions(ch_group, json_new_input_action, global_index);
                    cJSON_Delete(json_new_input_action);
                }
                
                wildcard_action->target_action = global_index;
                global_index++;
                
                wildcard_action->next = last_action;
                last_action = wildcard_action;
            }
        }
        
        ch_group->wildcard_action = last_action;
    }
    
    // REGISTER ACCESSORY CONFIGURATION
    float autoswitch_time(cJSON *json_accessory) {
        if (cJSON_GetObjectItemCaseSensitive(json_accessory, AUTOSWITCH_TIME) != NULL) {
            return (float) cJSON_GetObjectItemCaseSensitive(json_accessory, AUTOSWITCH_TIME)->valuedouble;
        }
        return 0;
    }
    
    int8_t th_sensor_gpio(cJSON *json_accessory) {
        if (cJSON_GetObjectItemCaseSensitive(json_accessory, TEMPERATURE_SENSOR_GPIO) != NULL) {
            return (uint8_t) cJSON_GetObjectItemCaseSensitive(json_accessory, TEMPERATURE_SENSOR_GPIO)->valuedouble;
        }
        return -1;
    }
    
    uint8_t th_sensor_type(cJSON *json_accessory) {
        if (cJSON_GetObjectItemCaseSensitive(json_accessory, TEMPERATURE_SENSOR_TYPE) != NULL) {
            return (uint8_t) cJSON_GetObjectItemCaseSensitive(json_accessory, TEMPERATURE_SENSOR_TYPE)->valuedouble;
        }
        return 2;
    }
    
    float th_sensor_temp_offset(cJSON *json_accessory) {
        if (cJSON_GetObjectItemCaseSensitive(json_accessory, TEMPERATURE_OFFSET) != NULL) {
            return (float) cJSON_GetObjectItemCaseSensitive(json_accessory, TEMPERATURE_OFFSET)->valuedouble;
        }
        return 0;
    }
    
    float th_sensor_hum_offset(cJSON *json_accessory) {
        if (cJSON_GetObjectItemCaseSensitive(json_accessory, HUMIDITY_OFFSET) != NULL) {
            return (float) cJSON_GetObjectItemCaseSensitive(json_accessory, HUMIDITY_OFFSET)->valuedouble;
        }
        return 0;
    }
    
    float th_sensor_poll_period(cJSON *json_accessory) {
        float th_poll_period = TH_SENSOR_POLL_PERIOD_DEFAULT;
        
        if (cJSON_GetObjectItemCaseSensitive(json_accessory, TEMPERATURE_SENSOR_POLL_PERIOD) != NULL) {
            th_poll_period = (float) cJSON_GetObjectItemCaseSensitive(json_accessory, TEMPERATURE_SENSOR_POLL_PERIOD)->valuedouble;
            
            if (th_poll_period < TH_SENSOR_POLL_PERIOD_MIN) {
                th_poll_period = TH_SENSOR_POLL_PERIOD_MIN;
            }
        }
        
        return th_poll_period;
    }
    
    void th_sensor(ch_group_t *ch_group, cJSON *json_accessory) {
        TH_SENSOR_GPIO = th_sensor_gpio(json_accessory);
        TH_SENSOR_TYPE = th_sensor_type(json_accessory);
        TH_SENSOR_TEMP_OFFSET = th_sensor_temp_offset(json_accessory);
        TH_SENSOR_HUM_OFFSET = th_sensor_hum_offset(json_accessory);
        TH_SENSOR_POLL_PERIOD = th_sensor_poll_period(json_accessory);
    }
    
    void th_sensor_starter(ch_group_t *ch_group) {
        ch_group->timer = malloc(sizeof(ETSTimer));
        memset(ch_group->timer, 0, sizeof(*ch_group->timer));
        sdk_os_timer_setfn(ch_group->timer, temperature_timer_worker, ch_group);
        
        xTaskCreate(delayed_sensor_starter_task, "delayed_sensor_starter_task", DELAYED_SENSOR_START_TASK_SIZE, ch_group, 1, NULL);
    }
    
    // ----- CONFIG SECTION
    
    // Log output type
    if (cJSON_GetObjectItemCaseSensitive(json_config, LOG_OUTPUT) != NULL &&
        cJSON_GetObjectItemCaseSensitive(json_config, LOG_OUTPUT)->valuedouble == 1) {
        log_output = true;
        uart_set_baud(0, 115200);
        printf_header();
        INFO2("NORMAL MODE\n\nJSON:\n %s\n", txt_config);
    }
    
#ifdef ESPH_DEBUG
    log_output = true;
    uart_set_baud(0, 115200);
    printf_header();
    INFO2("NORMAL MODE\n\nJSON:\n %s\n", txt_config);
#endif  // ESPH_DEBUG

    free(txt_config);
    
    // Custom Hostname
    char *custom_hostname = name.value.string_value;
    if (cJSON_GetObjectItemCaseSensitive(json_config, CUSTOM_HOSTNAME) != NULL) {
        custom_hostname = strdup(cJSON_GetObjectItemCaseSensitive(json_config, CUSTOM_HOSTNAME)->valuestring);
        INFO2("Hostname: %s", custom_hostname);
    }
    
    // Status LED
    if (cJSON_GetObjectItemCaseSensitive(json_config, STATUS_LED_GPIO) != NULL) {
        led_gpio = (uint8_t) cJSON_GetObjectItemCaseSensitive(json_config, STATUS_LED_GPIO)->valuedouble;

        if (cJSON_GetObjectItemCaseSensitive(json_config, INVERTED) != NULL) {
                led_inverted = (bool) cJSON_GetObjectItemCaseSensitive(json_config, INVERTED)->valuedouble;
        }
        
        gpio_enable(led_gpio, GPIO_OUTPUT);
        used_gpio[led_gpio] = true;
        gpio_write(led_gpio, false ^ led_inverted);
        INFO2("Status LED GPIO: %i, inv: %i", led_gpio, led_inverted);
    }
    
    // IR TX LED Frequency
    if (cJSON_GetObjectItemCaseSensitive(json_config, IR_ACTION_FREQ) != NULL) {
        ir_tx_freq = 1000 / cJSON_GetObjectItemCaseSensitive(json_config, IR_ACTION_FREQ)->valuedouble / 2;
        INFO2("IR TX Freq: %i", ir_tx_freq);
    }
    
    // IR TX LED Inverted
    if (cJSON_GetObjectItemCaseSensitive(json_config, IR_ACTION_TX_GPIO_INVERTED) != NULL) {
        ir_tx_inv = (bool) cJSON_GetObjectItemCaseSensitive(json_config, IR_ACTION_TX_GPIO_INVERTED)->valuedouble;
        INFO2("IR TX Inv: %i", ir_tx_inv);
    }
    
    // IR TX LED GPIO
    if (cJSON_GetObjectItemCaseSensitive(json_config, IR_ACTION_TX_GPIO) != NULL) {
        ir_tx_gpio = (uint8_t) cJSON_GetObjectItemCaseSensitive(json_config, IR_ACTION_TX_GPIO)->valuedouble;
        gpio_enable(ir_tx_gpio, GPIO_OUTPUT);
        used_gpio[ir_tx_gpio] = true;
        gpio_write(ir_tx_gpio, false ^ ir_tx_inv);
        INFO2("IR TX GPIO: %i", ir_tx_gpio);
    }
    
    // IR Protocol
    if (cJSON_GetObjectItemCaseSensitive(json_config, IR_ACTION_PROTOCOL) != NULL) {
        ir_protocol = strdup(cJSON_GetObjectItemCaseSensitive(json_config, IR_ACTION_PROTOCOL)->valuestring);
        INFO2("IR Protocol: %s", ir_protocol);
    }
    
    // Button filter
    if (cJSON_GetObjectItemCaseSensitive(json_config, BUTTON_FILTER) != NULL) {
        uint8_t button_filter_value = (uint8_t) cJSON_GetObjectItemCaseSensitive(json_config, BUTTON_FILTER)->valuedouble;
        adv_button_set_evaluate_delay(button_filter_value);
        INFO2("Button filter: %i", button_filter_value);
    }
    
    // PWM Frequency
    if (cJSON_GetObjectItemCaseSensitive(json_config, PWM_FREQ) != NULL) {
        pwm_freq = (uint16_t) cJSON_GetObjectItemCaseSensitive(json_config, PWM_FREQ)->valuedouble;
        INFO2("PWM Freq: %i", pwm_freq);
    }
    
    // Allowed Setup Mode Time
    if (cJSON_GetObjectItemCaseSensitive(json_config, ALLOWED_SETUP_MODE_TIME) != NULL) {
        setup_mode_time = (uint16_t) cJSON_GetObjectItemCaseSensitive(json_config, ALLOWED_SETUP_MODE_TIME)->valuedouble;
        INFO2("Setup mode time: %i secs", setup_mode_time);
    }
    
    // Run HomeKit Server
    if (cJSON_GetObjectItemCaseSensitive(json_config, ENABLE_HOMEKIT_SERVER) != NULL) {
        enable_homekit_server = (bool) cJSON_GetObjectItemCaseSensitive(json_config, ENABLE_HOMEKIT_SERVER)->valuedouble;
        INFO2("Run HomeKit Server: %i", enable_homekit_server);
    }
    
    // Allow unsecure connections
    if (cJSON_GetObjectItemCaseSensitive(json_config, ALLOW_INSECURE_CONNECTIONS) != NULL) {
        bool allow_insecure = (bool) cJSON_GetObjectItemCaseSensitive(json_config, ALLOW_INSECURE_CONNECTIONS)->valuedouble;
        config.insecure = allow_insecure;
        INFO2("Unsecure connections: %i", allow_insecure);
    }
    
    // Times to toggle quickly an accessory status to enter setup mode
    if (cJSON_GetObjectItemCaseSensitive(json_config, SETUP_MODE_ACTIVATE_COUNT) != NULL) {
        setup_mode_toggle_counter_max = (int8_t) cJSON_GetObjectItemCaseSensitive(json_config, SETUP_MODE_ACTIVATE_COUNT)->valuedouble;
        INFO2("Toggles to enter setup mode: %i", setup_mode_toggle_counter_max);
    }
    
    if (setup_mode_toggle_counter_max > 0) {
        setup_mode_toggle_timer = malloc(sizeof(ETSTimer));
        memset(setup_mode_toggle_timer, 0, sizeof(*setup_mode_toggle_timer));
        sdk_os_timer_setfn(setup_mode_toggle_timer, setup_mode_toggle, NULL);
    }
    
    // Buttons to enter setup mode
    diginput_register(cJSON_GetObjectItemCaseSensitive(json_config, BUTTONS_ARRAY), setup_mode_call, NULL, 0);
    
    // ----- END CONFIG SECTION
    
    uint8_t hk_total_ac = 1;
    bool bridge_needed = false;

    for(uint8_t i=0; i<total_accessories; i++) {
        // Kill Switch Accessory count
        if (cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(json_accessories, i), KILL_SWITCH) != NULL) {
        const uint8_t kill_switch = (uint8_t) cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(json_accessories, i), KILL_SWITCH)->valuedouble;
            switch (kill_switch) {
                case 1:
                case 2:
                    hk_total_ac += 1;
                    break;
                    
                case 3:
                    hk_total_ac += 2;
                    break;
                    
                default:    // case 0:
                    break;
            }
        }
        
        // Accessory Type Accessory count
        uint8_t acc_type = ACC_TYPE_SWITCH;     // Default accessory type
        if (cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(json_accessories, i), ACCESSORY_TYPE) != NULL) {
            acc_type = (uint8_t) cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(json_accessories, i), ACCESSORY_TYPE)->valuedouble;
        }

        switch (acc_type) {
            default:
                hk_total_ac += 1;
                break;
        }
    }
    
    if (total_accessories > ACCESSORIES_WITHOUT_BRIDGE || bridge_needed) {
        // Bridge needed
        bridge_needed = true;
        hk_total_ac += 1;
    }
    
    // Saved States Timer Function
    sdk_os_timer_setfn(&save_states_timer, save_states, NULL);
    
    homekit_accessory_t **accessories = calloc(hk_total_ac, sizeof(homekit_accessory_t*));
    
    // Define services and characteristics
    uint8_t accessory_numerator = 1;
    
    void new_accessory(const uint8_t accessory, const uint8_t services) {
        accessories[accessory] = calloc(1, sizeof(homekit_accessory_t));
        accessories[accessory]->id = accessory + 1;
        accessories[accessory]->services = calloc(services, sizeof(homekit_service_t*));
        
        accessories[accessory]->services[0] = calloc(1, sizeof(homekit_service_t));
        accessories[accessory]->services[0]->id = 1;
        accessories[accessory]->services[0]->type = HOMEKIT_SERVICE_ACCESSORY_INFORMATION;
        accessories[accessory]->services[0]->characteristics = calloc(7, sizeof(homekit_characteristic_t*));
        accessories[accessory]->services[0]->characteristics[0] = &name;
        accessories[accessory]->services[0]->characteristics[1] = &manufacturer;
        accessories[accessory]->services[0]->characteristics[2] = &serial;
        accessories[accessory]->services[0]->characteristics[3] = &model;
        accessories[accessory]->services[0]->characteristics[4] = &firmware;
        accessories[accessory]->services[0]->characteristics[5] = &identify_function;
    }
    
    homekit_characteristic_t *new_kill_switch(const uint8_t accessory) {
        new_accessory(accessory, 3);
        
        homekit_characteristic_t *ch = NEW_HOMEKIT_CHARACTERISTIC(ON, false, .setter_ex=hkc_setter_with_setup);
        
        accessories[accessory]->services[1] = calloc(1, sizeof(homekit_service_t));
        accessories[accessory]->services[1]->id = 8;
        accessories[accessory]->services[1]->type = HOMEKIT_SERVICE_SWITCH;
        accessories[accessory]->services[1]->primary = true;
        accessories[accessory]->services[1]->characteristics = calloc(2, sizeof(homekit_characteristic_t*));
        accessories[accessory]->services[1]->characteristics[0] = ch;
        
        ch->value.bool_value = (bool) set_initial_state(accessory, 0, cJSON_Parse(INIT_STATE_LAST_STR), ch, CH_TYPE_BOOL, 0);
        
        return ch;
    }
    
    uint8_t build_kill_switches(const uint8_t accessory, ch_group_t *ch_group, cJSON *json_context) {
        if (cJSON_GetObjectItemCaseSensitive(json_context, KILL_SWITCH) != NULL) {
            const uint8_t kill_switch = (uint8_t) cJSON_GetObjectItemCaseSensitive(json_context, KILL_SWITCH)->valuedouble;
            
            if (kill_switch == 1) {
                INFO2("Secure Switch");
                ch_group->ch_sec = new_kill_switch(accessory);
                return accessory + 1;
                
            } else if (kill_switch == 2) {
                INFO2("Kids Switch");
                ch_group->ch_child = new_kill_switch(accessory);
                return accessory + 1;
                
            } else if (kill_switch == 3) {
                INFO2("Secure Switch");
                ch_group->ch_sec = new_kill_switch(accessory);
                INFO2("Kids Switch");
                ch_group->ch_child = new_kill_switch(accessory + 1);
                return accessory + 2;
            }
        }
        
        return accessory;
    }

    uint8_t new_switch(uint8_t accessory, cJSON *json_context, const uint8_t acc_type) {
        new_accessory(accessory, 3);
        
        homekit_characteristic_t *ch0 = NEW_HOMEKIT_CHARACTERISTIC(ON, false, .setter_ex=hkc_on_setter);
        
        uint32_t max_duration = 0;
        if (cJSON_GetObjectItemCaseSensitive(json_context, VALVE_MAX_DURATION) != NULL) {
            max_duration = (uint32_t) cJSON_GetObjectItemCaseSensitive(json_context, VALVE_MAX_DURATION)->valuedouble;
        }
        
        ch_group_t *ch_group = malloc(sizeof(ch_group_t));
        memset(ch_group, 0, sizeof(*ch_group));
        ch_group->accessory = accessory_numerator;
        accessory_numerator++;
        ch_group->acc_type = ACC_TYPE_SWITCH;
        ch_group->ch0 = ch0;
        register_actions(ch_group, json_context, 0);
        ch_group->num[0] = autoswitch_time(json_context);
        ch_group->next = ch_groups;
        ch_groups = ch_group;
        
        accessories[accessory]->services[1] = calloc(1, sizeof(homekit_service_t));
        accessories[accessory]->services[1]->id = 8;
        accessories[accessory]->services[1]->primary = true;
        
        uint8_t ch_calloc = 2;
        if (max_duration > 0) {
            ch_calloc += 2;
        }
        
        if (acc_type == ACC_TYPE_SWITCH) {
            accessories[accessory]->services[1]->type = HOMEKIT_SERVICE_SWITCH;
            accessories[accessory]->services[1]->characteristics = calloc(ch_calloc, sizeof(homekit_characteristic_t*));
            
        } else {    // acc_type == ACC_TYPE_OUTLET
            ch_calloc++;
            
            homekit_characteristic_t *ch3 = NEW_HOMEKIT_CHARACTERISTIC(OUTLET_IN_USE, true);
            
            accessories[accessory]->services[1]->characteristics = calloc(ch_calloc, sizeof(homekit_characteristic_t*));
            accessories[accessory]->services[1]->type = HOMEKIT_SERVICE_OUTLET;
            accessories[accessory]->services[1]->characteristics[1] = ch3;
            
            if (max_duration > 0) {
                ch_group->ch3 = ch3;
            } else {
                ch_group->ch1 = ch3;
            }
        }
        
        accessories[accessory]->services[1]->characteristics[0] = ch0;
        
        if (max_duration > 0) {
            homekit_characteristic_t *ch1 = NEW_HOMEKIT_CHARACTERISTIC(SET_DURATION, max_duration, .max_value=(float[]) {max_duration}, .setter_ex=hkc_setter);
            homekit_characteristic_t *ch2 = NEW_HOMEKIT_CHARACTERISTIC(REMAINING_DURATION, 0, .max_value=(float[]) {max_duration});
            
            ch_group->ch1 = ch1;
            ch_group->ch2 = ch2;
            
            accessories[accessory]->services[1]->characteristics[ch_calloc - 3] = ch1;
            accessories[accessory]->services[1]->characteristics[ch_calloc - 2] = ch2;
            
            const uint32_t initial_time = (uint32_t) set_initial_state(accessory, 1, cJSON_Parse(INIT_STATE_LAST_STR), ch1, CH_TYPE_INT32, max_duration);
            if (initial_time > max_duration) {
                ch1->value.int_value = max_duration;
            } else {
                ch1->value.int_value = initial_time;
            }
            
            ch_group->timer = malloc(sizeof(ETSTimer));
            memset(ch_group->timer, 0, sizeof(*ch_group->timer));
            sdk_os_timer_setfn(ch_group->timer, on_timer_worker, ch0);
        }
        
        diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, BUTTONS_ARRAY), diginput, ch0, TYPE_ON);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, PINGS_ARRAY), diginput, ch0, TYPE_ON);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_ARRAY_1), diginput_1, ch0, TYPE_ON);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_ARRAY_0), diginput_0, ch0, TYPE_ON);
        diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_STATUS_ARRAY_1), digstate_1, ch0, TYPE_ON);
        diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_STATUS_ARRAY_0), digstate_0, ch0, TYPE_ON);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_STATUS_ARRAY_1), digstate_1, ch0, TYPE_ON);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_STATUS_ARRAY_0), digstate_0, ch0, TYPE_ON);
        
        uint8_t initial_state = 0;
        if (cJSON_GetObjectItemCaseSensitive(json_context, INITIAL_STATE) != NULL) {
            initial_state = (uint8_t) cJSON_GetObjectItemCaseSensitive(json_context, INITIAL_STATE)->valuedouble;
        }
        
        if (initial_state != INIT_STATE_FIXED_INPUT) {
            diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_1), diginput_1, ch0, TYPE_ON);
            diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_0), diginput_0, ch0, TYPE_ON);
            
            ch0->value.bool_value = !((bool) set_initial_state(accessory, 0, json_context, ch0, CH_TYPE_BOOL, 0));
            hkc_on_setter(ch0, HOMEKIT_BOOL(!ch0->value.bool_value));
        } else {
            if (diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_1), diginput_1, ch0, TYPE_ON)) {
                diginput_1(0, ch0, TYPE_ON);
            }
            if (diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_0), diginput_0, ch0, TYPE_ON)) {
                ch0->value = HOMEKIT_BOOL(true);
                diginput_0(0, ch0, TYPE_ON);
            }
        }
        
        const uint8_t new_accessory_count = build_kill_switches(accessory + 1, ch_group, json_context);
        return new_accessory_count;
    }
    
    uint8_t new_button_event(uint8_t accessory, cJSON *json_context) {
        new_accessory(accessory, 3);
        
        homekit_characteristic_t *ch0 = NEW_HOMEKIT_CHARACTERISTIC(PROGRAMMABLE_SWITCH_EVENT, 0);
        
        ch_group_t *ch_group = malloc(sizeof(ch_group_t));
        memset(ch_group, 0, sizeof(*ch_group));
        ch_group->accessory = accessory_numerator;
        accessory_numerator++;
        ch_group->acc_type = ACC_TYPE_BUTTON;
        ch_group->ch0 = ch0;
        register_actions(ch_group, json_context, 0);
        ch_group->next = ch_groups;
        ch_groups = ch_group;
        
        accessories[accessory]->services[1] = calloc(1, sizeof(homekit_service_t));
        accessories[accessory]->services[1]->id = 8;
        accessories[accessory]->services[1]->type = HOMEKIT_SERVICE_STATELESS_PROGRAMMABLE_SWITCH;
        accessories[accessory]->services[1]->primary = true;
        accessories[accessory]->services[1]->characteristics = calloc(2, sizeof(homekit_characteristic_t*));
        accessories[accessory]->services[1]->characteristics[0] = ch0;
        
        diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_0), button_event, ch0, SINGLEPRESS_EVENT);
        diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_1), button_event, ch0, DOUBLEPRESS_EVENT);
        diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_2), button_event, ch0, LONGPRESS_EVENT);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_ARRAY_0), button_event, ch0, SINGLEPRESS_EVENT);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_ARRAY_1), button_event, ch0, DOUBLEPRESS_EVENT);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_ARRAY_2), button_event, ch0, LONGPRESS_EVENT);
        
        const uint8_t new_accessory_count = build_kill_switches(accessory + 1, ch_group, json_context);
        return new_accessory_count;
    }
    
    uint8_t new_lock(const uint8_t accessory, cJSON *json_context) {
        new_accessory(accessory, 3);
        
        homekit_characteristic_t *ch0 = NEW_HOMEKIT_CHARACTERISTIC(LOCK_CURRENT_STATE, 1);
        homekit_characteristic_t *ch1 = NEW_HOMEKIT_CHARACTERISTIC(LOCK_TARGET_STATE, 1, .setter_ex=hkc_lock_setter);
        
        ch_group_t *ch_group = malloc(sizeof(ch_group_t));
        memset(ch_group, 0, sizeof(*ch_group));
        ch_group->accessory = accessory_numerator;
        accessory_numerator++;
        ch_group->acc_type = ACC_TYPE_LOCK;
        ch_group->ch0 = ch0;
        ch_group->ch1 = ch1;
        register_actions(ch_group, json_context, 0);
        ch_group->num[0] = autoswitch_time(json_context);
        ch_group->next = ch_groups;
        ch_groups = ch_group;
        
        accessories[accessory]->services[1] = calloc(1, sizeof(homekit_service_t));
        accessories[accessory]->services[1]->id = 8;
        accessories[accessory]->services[1]->type = HOMEKIT_SERVICE_LOCK_MECHANISM;
        accessories[accessory]->services[1]->primary = true;
        accessories[accessory]->services[1]->characteristics = calloc(3, sizeof(homekit_characteristic_t*));
        accessories[accessory]->services[1]->characteristics[0] = ch0;
        accessories[accessory]->services[1]->characteristics[1] = ch1;
        
        diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, BUTTONS_ARRAY), diginput, ch1, TYPE_LOCK);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, PINGS_ARRAY), diginput, ch1, TYPE_LOCK);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_ARRAY_1), diginput_1, ch1, TYPE_LOCK);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_ARRAY_0), diginput_0, ch1, TYPE_LOCK);
        diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_STATUS_ARRAY_1), digstate_1, ch1, TYPE_LOCK);
        diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_STATUS_ARRAY_0), digstate_0, ch1, TYPE_LOCK);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_STATUS_ARRAY_1), digstate_1, ch1, TYPE_LOCK);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_STATUS_ARRAY_0), digstate_0, ch1, TYPE_LOCK);
        
        uint8_t initial_state = 0;
        if (cJSON_GetObjectItemCaseSensitive(json_context, INITIAL_STATE) != NULL) {
            initial_state = (uint8_t) cJSON_GetObjectItemCaseSensitive(json_context, INITIAL_STATE)->valuedouble;
        }
        
        if (initial_state != INIT_STATE_FIXED_INPUT) {
            diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_1), diginput_1, ch1, TYPE_LOCK);
            diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_0), diginput_0, ch1, TYPE_LOCK);
            
            ch1->value.int_value = !((uint8_t) set_initial_state(accessory, 0, json_context, ch1, CH_TYPE_INT8, 1));
            hkc_lock_setter(ch1, HOMEKIT_UINT8(!ch1->value.int_value));
        } else {
            if (diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_1), diginput_1, ch1, TYPE_LOCK)) {
                diginput_1(0, ch1, TYPE_LOCK);
            }
            if (diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_0), diginput_0, ch1, TYPE_LOCK)) {
                ch1->value = HOMEKIT_UINT8(0);
                diginput_0(0, ch1, TYPE_LOCK);
            }
        }
        
        const uint8_t new_accessory_count = build_kill_switches(accessory + 1, ch_group, json_context);
        return new_accessory_count;
    }
    
    uint8_t new_sensor(const uint8_t accessory, cJSON *json_context, const uint8_t acc_type) {
        new_accessory(accessory, 3);
        
        homekit_characteristic_t *ch0;
        
        accessories[accessory]->services[1] = calloc(1, sizeof(homekit_service_t));
        accessories[accessory]->services[1]->id = 8;
        accessories[accessory]->services[1]->primary = true;
        
        switch (acc_type) {
            case ACC_TYPE_OCCUPANCY_SENSOR:
                accessories[accessory]->services[1]->type = HOMEKIT_SERVICE_OCCUPANCY_SENSOR;
                ch0 = NEW_HOMEKIT_CHARACTERISTIC(OCCUPANCY_DETECTED, 0);
                break;
                
            case ACC_TYPE_LEAK_SENSOR:
                accessories[accessory]->services[1]->type = HOMEKIT_SERVICE_LEAK_SENSOR;
                ch0 = NEW_HOMEKIT_CHARACTERISTIC(LEAK_DETECTED, 0);
                break;
                
            case ACC_TYPE_SMOKE_SENSOR:
                accessories[accessory]->services[1]->type = HOMEKIT_SERVICE_SMOKE_SENSOR;
                ch0 = NEW_HOMEKIT_CHARACTERISTIC(SMOKE_DETECTED, 0);
                break;
                
            case ACC_TYPE_CARBON_MONOXIDE_SENSOR:
                accessories[accessory]->services[1]->type = HOMEKIT_SERVICE_CARBON_MONOXIDE_SENSOR;
                ch0 = NEW_HOMEKIT_CHARACTERISTIC(CARBON_MONOXIDE_DETECTED, 0);
                break;
                
            case ACC_TYPE_CARBON_DIOXIDE_SENSOR:
                accessories[accessory]->services[1]->type = HOMEKIT_SERVICE_CARBON_DIOXIDE_SENSOR;
                ch0 = NEW_HOMEKIT_CHARACTERISTIC(CARBON_DIOXIDE_DETECTED, 0);
                break;
                
            case ACC_TYPE_FILTER_CHANGE_SENSOR:
                accessories[accessory]->services[1]->type = HOMEKIT_SERVICE_FILTER_MAINTENANCE;
                ch0 = NEW_HOMEKIT_CHARACTERISTIC(FILTER_CHANGE_INDICATION, 0);
                break;
                
            case ACC_TYPE_MOTION_SENSOR:
                accessories[accessory]->services[1]->type = HOMEKIT_SERVICE_MOTION_SENSOR;
                ch0 = NEW_HOMEKIT_CHARACTERISTIC(MOTION_DETECTED, 0);
                break;
                
            default:    // case ACC_TYPE_CONTACT_SENSOR:
                accessories[accessory]->services[1]->type = HOMEKIT_SERVICE_CONTACT_SENSOR;
                ch0 = NEW_HOMEKIT_CHARACTERISTIC(CONTACT_SENSOR_STATE, 0);
                break;
        }
        
        accessories[accessory]->services[1]->characteristics = calloc(2, sizeof(homekit_characteristic_t*));
        accessories[accessory]->services[1]->characteristics[0] = ch0;
        
        ch_group_t *ch_group = malloc(sizeof(ch_group_t));
        memset(ch_group, 0, sizeof(*ch_group));
        ch_group->accessory = accessory_numerator;
        accessory_numerator++;
        ch_group->acc_type = ACC_TYPE_CONTACT_SENSOR;
        ch_group->ch0 = ch0;
        register_actions(ch_group, json_context, 0);
        ch_group->num[0] = autoswitch_time(json_context);
        ch_group->next = ch_groups;
        ch_groups = ch_group;
        
        if (acc_type == ACC_TYPE_MOTION_SENSOR) {
            ch_group->acc_type = ACC_TYPE_MOTION_SENSOR;
            
            ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_ARRAY_0), sensor_0, ch0, TYPE_SENSOR_BOOL);
            ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_ARRAY_1), sensor_1, ch0, TYPE_SENSOR_BOOL);
            diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_STATUS_ARRAY_1), sensor_status_1, ch0, TYPE_SENSOR_BOOL);
            diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_STATUS_ARRAY_0), sensor_status_0, ch0, TYPE_SENSOR_BOOL);
            ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_STATUS_ARRAY_1), sensor_status_1, ch0, TYPE_SENSOR_BOOL);
            ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_STATUS_ARRAY_0), sensor_status_0, ch0, TYPE_SENSOR_BOOL);
            
            if (diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_0), sensor_0, ch0, TYPE_SENSOR_BOOL)) {
                sensor_0(0, ch0, TYPE_SENSOR_BOOL);
            }
            
            if (diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_1), sensor_1, ch0, TYPE_SENSOR_BOOL)) {
                sensor_1(0, ch0, TYPE_SENSOR_BOOL);
            }
        } else {
            ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_ARRAY_0), sensor_0, ch0, TYPE_SENSOR);
            ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_ARRAY_1), sensor_1, ch0, TYPE_SENSOR);
            diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_STATUS_ARRAY_1), sensor_status_1, ch0, TYPE_SENSOR);
            diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_STATUS_ARRAY_0), sensor_status_0, ch0, TYPE_SENSOR);
            ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_STATUS_ARRAY_1), sensor_status_1, ch0, TYPE_SENSOR);
            ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_STATUS_ARRAY_0), sensor_status_0, ch0, TYPE_SENSOR);
            
            if (diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_0), sensor_0, ch0, TYPE_SENSOR)) {
                sensor_0(0, ch0, TYPE_SENSOR);
            }
            
            if (diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_1), sensor_1, ch0, TYPE_SENSOR)) {
                sensor_1(0, ch0, TYPE_SENSOR);
            }
        }
        
        const uint8_t new_accessory_count = build_kill_switches(accessory + 1, ch_group, json_context);
        return new_accessory_count;
    }
    
    uint8_t new_water_valve(uint8_t accessory, cJSON *json_context) {
        new_accessory(accessory, 3);
        
        uint8_t valve_type = VALVE_SYSTEM_TYPE_DEFAULT;
        if (cJSON_GetObjectItemCaseSensitive(json_context, VALVE_SYSTEM_TYPE) != NULL) {
            valve_type = (uint8_t) cJSON_GetObjectItemCaseSensitive(json_context, VALVE_SYSTEM_TYPE)->valuedouble;
        }
        
        uint32_t valve_max_duration = VALVE_MAX_DURATION_DEFAULT;
        if (cJSON_GetObjectItemCaseSensitive(json_context, VALVE_MAX_DURATION) != NULL) {
            valve_max_duration = (uint32_t) cJSON_GetObjectItemCaseSensitive(json_context, VALVE_MAX_DURATION)->valuedouble;
        }
        
        homekit_characteristic_t *ch0 = NEW_HOMEKIT_CHARACTERISTIC(ACTIVE, 0, .setter_ex=hkc_valve_setter);
        homekit_characteristic_t *ch1 = NEW_HOMEKIT_CHARACTERISTIC(IN_USE, 0);
        homekit_characteristic_t *ch2, *ch3;
        
        ch_group_t *ch_group = malloc(sizeof(ch_group_t));
        memset(ch_group, 0, sizeof(*ch_group));
        ch_group->accessory = accessory_numerator;
        accessory_numerator++;
        ch_group->acc_type = ACC_TYPE_WATER_VALVE;
        ch_group->ch0 = ch0;
        ch_group->ch1 = ch1;
        register_actions(ch_group, json_context, 0);
        ch_group->num[0] = autoswitch_time(json_context);
        ch_group->next = ch_groups;
        ch_groups = ch_group;
        
        accessories[accessory]->services[1] = calloc(1, sizeof(homekit_service_t));
        accessories[accessory]->services[1]->id = 8;
        accessories[accessory]->services[1]->primary = true;
        accessories[accessory]->services[1]->type = HOMEKIT_SERVICE_VALVE;
        
        if (valve_max_duration == 0) {
            accessories[accessory]->services[1]->characteristics = calloc(4, sizeof(homekit_characteristic_t*));
        } else {
            ch2 = NEW_HOMEKIT_CHARACTERISTIC(SET_DURATION, valve_max_duration, .max_value=(float[]) {valve_max_duration}, .setter_ex=hkc_setter);
            ch3 = NEW_HOMEKIT_CHARACTERISTIC(REMAINING_DURATION, 0, .max_value=(float[]) {valve_max_duration});
            
            ch_group->ch2 = ch2;
            ch_group->ch3 = ch3;
            
            accessories[accessory]->services[1]->characteristics = calloc(6, sizeof(homekit_characteristic_t*));
            accessories[accessory]->services[1]->characteristics[3] = ch2;
            accessories[accessory]->services[1]->characteristics[4] = ch3;
            
            const uint32_t initial_time = (uint32_t) set_initial_state(accessory, 2, cJSON_Parse(INIT_STATE_LAST_STR), ch2, CH_TYPE_INT32, 900);
            if (initial_time > valve_max_duration) {
                ch2->value.int_value = valve_max_duration;
            } else {
                ch2->value.int_value = initial_time;
            }
            
            ch_group->timer = malloc(sizeof(ETSTimer));
            memset(ch_group->timer, 0, sizeof(*ch_group->timer));
            sdk_os_timer_setfn(ch_group->timer, valve_timer_worker, ch0);
        }
        
        accessories[accessory]->services[1]->characteristics[0] = ch0;
        accessories[accessory]->services[1]->characteristics[1] = ch1;
        accessories[accessory]->services[1]->characteristics[2] = NEW_HOMEKIT_CHARACTERISTIC(VALVE_TYPE, valve_type);
        
        diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, BUTTONS_ARRAY), diginput, ch0, TYPE_VALVE);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, PINGS_ARRAY), diginput, ch0, TYPE_VALVE);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_ARRAY_1), diginput_1, ch0, TYPE_VALVE);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_ARRAY_0), diginput_0, ch0, TYPE_VALVE);
        diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_STATUS_ARRAY_1), digstate_1, ch0, TYPE_VALVE);
        diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_STATUS_ARRAY_0), digstate_0, ch0, TYPE_VALVE);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_STATUS_ARRAY_1), digstate_1, ch0, TYPE_VALVE);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_STATUS_ARRAY_0), digstate_0, ch0, TYPE_VALVE);
        
        uint8_t initial_state = 0;
        if (cJSON_GetObjectItemCaseSensitive(json_context, INITIAL_STATE) != NULL) {
            initial_state = (uint8_t) cJSON_GetObjectItemCaseSensitive(json_context, INITIAL_STATE)->valuedouble;
        }
        
        if (initial_state != INIT_STATE_FIXED_INPUT) {
            diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_1), diginput_1, ch0, TYPE_VALVE);
            diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_0), diginput_0, ch0, TYPE_VALVE);
            
            ch0->value.int_value = !((uint8_t) set_initial_state(accessory, 0, json_context, ch0, CH_TYPE_INT8, 0));
            hkc_valve_setter(ch0, HOMEKIT_UINT8(!ch0->value.int_value));
        } else {
            if (diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_1), diginput_1, ch0, TYPE_VALVE)) {
                diginput_1(0, ch0, TYPE_VALVE);
            }
            if (diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_0), diginput_0, ch0, TYPE_VALVE)) {
                ch0->value = HOMEKIT_UINT8(1);
                diginput_0(0, ch0, TYPE_VALVE);
            }
        }
        
        const uint8_t new_accessory_count = build_kill_switches(accessory + 1, ch_group, json_context);
        return new_accessory_count;
    }
    
    uint8_t new_thermostat(uint8_t accessory, cJSON *json_context) {
        new_accessory(accessory, 3);
        
        ch_group_t *ch_group = malloc(sizeof(ch_group_t));
        memset(ch_group, 0, sizeof(*ch_group));
        ch_group->accessory = accessory_numerator;
        accessory_numerator++;
        ch_group->acc_type = ACC_TYPE_THERMOSTAT;
        
        // Custom ranges of Target Temperatures
        TH_MIN_TEMP = THERMOSTAT_DEFAULT_MIN_TEMP;
        if (cJSON_GetObjectItemCaseSensitive(json_context, THERMOSTAT_MIN_TEMP) != NULL) {
            TH_MIN_TEMP = (float) cJSON_GetObjectItemCaseSensitive(json_context, THERMOSTAT_MIN_TEMP)->valuedouble;
        }
        
        if (TH_MIN_TEMP < -100) {
            TH_MIN_TEMP = -100;
        }
        
        TH_MAX_TEMP = THERMOSTAT_DEFAULT_MAX_TEMP;
        if (cJSON_GetObjectItemCaseSensitive(json_context, THERMOSTAT_MAX_TEMP) != NULL) {
            TH_MAX_TEMP = (float) cJSON_GetObjectItemCaseSensitive(json_context, THERMOSTAT_MAX_TEMP)->valuedouble;
        }
        
        if (TH_MAX_TEMP > 200) {
            TH_MAX_TEMP = 200;
        }
        
        const float default_target_temp = (TH_MIN_TEMP + TH_MAX_TEMP) / 2;
        
        // Temperature Deadband
        TH_DEADBAND = 0;
        if (cJSON_GetObjectItemCaseSensitive(json_context, THERMOSTAT_DEADBAND) != NULL) {
            TH_DEADBAND = (float) cJSON_GetObjectItemCaseSensitive(json_context, THERMOSTAT_DEADBAND)->valuedouble;
        }
        
        // Thermostat Type
        TH_TYPE = THERMOSTAT_TYPE_HEATER;
        if (cJSON_GetObjectItemCaseSensitive(json_context, THERMOSTAT_TYPE) != NULL) {
            TH_TYPE = (uint8_t) cJSON_GetObjectItemCaseSensitive(json_context, THERMOSTAT_TYPE)->valuedouble;
        }
        
        // HomeKit Characteristics
        homekit_characteristic_t *ch0 = NEW_HOMEKIT_CHARACTERISTIC(CURRENT_TEMPERATURE, 0, .min_value=(float[]) {-100}, .max_value=(float[]) {200});
        homekit_characteristic_t *ch1 = NEW_HOMEKIT_CHARACTERISTIC(ACTIVE, 0, .setter_ex=hkc_th_target_setter);
        homekit_characteristic_t *ch2 = NEW_HOMEKIT_CHARACTERISTIC(TEMPERATURE_DISPLAY_UNITS, 0, .setter_ex=hkc_setter);
        homekit_characteristic_t *ch3 = NEW_HOMEKIT_CHARACTERISTIC(CURRENT_HEATER_COOLER_STATE, 0);
        homekit_characteristic_t *ch5 = NEW_HOMEKIT_CHARACTERISTIC(HEATING_THRESHOLD_TEMPERATURE, default_target_temp -1, .min_value=(float[]) {TH_MIN_TEMP}, .max_value=(float[]) {TH_MAX_TEMP}, .setter_ex=update_th);
        homekit_characteristic_t *ch6 = NEW_HOMEKIT_CHARACTERISTIC(COOLING_THRESHOLD_TEMPERATURE, default_target_temp +1, .min_value=(float[]) {TH_MIN_TEMP}, .max_value=(float[]) {TH_MAX_TEMP}, .setter_ex=update_th);
        
        uint8_t ch_calloc = 7;
        if (TH_TYPE == THERMOSTAT_TYPE_HEATERCOOLER) {
            ch_calloc += 1;
        }
        accessories[accessory]->services[1] = calloc(1, sizeof(homekit_service_t));
        accessories[accessory]->services[1]->id = 8;
        accessories[accessory]->services[1]->primary = true;
        accessories[accessory]->services[1]->type = HOMEKIT_SERVICE_HEATER_COOLER;
        accessories[accessory]->services[1]->characteristics = calloc(ch_calloc, sizeof(homekit_characteristic_t*));
        accessories[accessory]->services[1]->characteristics[0] = ch1;
        accessories[accessory]->services[1]->characteristics[1] = ch0;
        accessories[accessory]->services[1]->characteristics[2] = ch2;
        accessories[accessory]->services[1]->characteristics[3] = ch3;
        
        homekit_characteristic_t *ch4;
        
        const float initial_h_target_temp = set_initial_state(accessory, 5, cJSON_Parse(INIT_STATE_LAST_STR), ch5, CH_TYPE_FLOAT, default_target_temp -1);
        if (initial_h_target_temp > TH_MAX_TEMP || initial_h_target_temp < TH_MIN_TEMP) {
            ch5->value.float_value = default_target_temp -1;
        } else {
            ch5->value.float_value = initial_h_target_temp;
        }
        
        const float initial_c_target_temp = set_initial_state(accessory, 6, cJSON_Parse(INIT_STATE_LAST_STR), ch6, CH_TYPE_FLOAT, default_target_temp +1);
        if (initial_c_target_temp > TH_MAX_TEMP || initial_c_target_temp < TH_MIN_TEMP) {
            ch6->value.float_value = default_target_temp +1;
        } else {
            ch6->value.float_value = initial_c_target_temp;
        }
        
        switch ((uint8_t) TH_TYPE) {
            case THERMOSTAT_TYPE_COOLER:
                ch4 = NEW_HOMEKIT_CHARACTERISTIC(TARGET_HEATER_COOLER_STATE, THERMOSTAT_TARGET_MODE_COOLER, .min_value=(float[]) {THERMOSTAT_TARGET_MODE_COOLER}, .max_value=(float[]) {THERMOSTAT_TARGET_MODE_COOLER}, .valid_values={.count=1, .values=(uint8_t[]) {THERMOSTAT_TARGET_MODE_COOLER}});
                
                accessories[accessory]->services[1]->characteristics[5] = ch6;
                break;
                
            case THERMOSTAT_TYPE_HEATERCOOLER:
                ch4 = NEW_HOMEKIT_CHARACTERISTIC(TARGET_HEATER_COOLER_STATE, THERMOSTAT_TARGET_MODE_AUTO, .setter_ex=update_th);
                
                accessories[accessory]->services[1]->characteristics[5] = ch5;
                accessories[accessory]->services[1]->characteristics[6] = ch6;
                break;
                
            default:        // case THERMOSTAT_TYPE_HEATER:
                ch4 = NEW_HOMEKIT_CHARACTERISTIC(TARGET_HEATER_COOLER_STATE, THERMOSTAT_TARGET_MODE_HEATER, .min_value=(float[]) {THERMOSTAT_TARGET_MODE_HEATER}, .max_value=(float[]) {THERMOSTAT_TARGET_MODE_HEATER}, .valid_values={.count=1, .values=(uint8_t[]) {THERMOSTAT_TARGET_MODE_HEATER}});

                accessories[accessory]->services[1]->characteristics[5] = ch5;
                break;
        }
        
        accessories[accessory]->services[1]->characteristics[4] = ch4;
        
        ch_group->ch0 = ch0;
        ch_group->ch1 = ch1;
        ch_group->ch2 = ch2;
        ch_group->ch3 = ch3;
        ch_group->ch4 = ch4;
        ch_group->ch5 = ch5;
        ch_group->ch6 = ch6;
        register_actions(ch_group, json_context, 0);
        register_wildcard_actions(ch_group, json_context);
        th_sensor(ch_group, json_context);
        ch_group->last_wildcard_action[0] = NO_LAST_WILDCARD_ACTION;
        ch_group->last_wildcard_action[1] = NO_LAST_WILDCARD_ACTION;
        ch_group->next = ch_groups;
        ch_groups = ch_group;
            
        if (TH_SENSOR_GPIO != -1) {
            th_sensor_starter(ch_group);
        }
        
        diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, BUTTONS_ARRAY), th_input, ch1, 9);
        diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_3), th_input_temp, ch0, THERMOSTAT_TEMP_UP);
        diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_4), th_input_temp, ch0, THERMOSTAT_TEMP_DOWN);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, PINGS_ARRAY), th_input, ch1, 9);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_ARRAY_3), th_input_temp, ch0, THERMOSTAT_TEMP_UP);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_ARRAY_4), th_input_temp, ch0, THERMOSTAT_TEMP_DOWN);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_ARRAY_1), th_input, ch0, 1);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_ARRAY_0), th_input, ch0, 0);
        
        if (TH_TYPE == THERMOSTAT_TYPE_HEATERCOOLER) {
            diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_5), th_input, ch0, 5);
            diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_6), th_input, ch0, 6);
            diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_7), th_input, ch0, 7);
            ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_ARRAY_5), th_input, ch0, 5);
            ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_ARRAY_6), th_input, ch0, 6);
            ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_ARRAY_7), th_input, ch0, 7);
            
            ch4->value.int_value = set_initial_state(accessory, 4, cJSON_Parse(INIT_STATE_LAST_STR), ch4, CH_TYPE_INT8, 0);
        }
        
        uint8_t initial_state = 0;
        if (cJSON_GetObjectItemCaseSensitive(json_context, INITIAL_STATE) != NULL) {
            initial_state = (uint8_t) cJSON_GetObjectItemCaseSensitive(json_context, INITIAL_STATE)->valuedouble;
        }
        
        if (initial_state != INIT_STATE_FIXED_INPUT) {
            diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_1), th_input, ch0, 1);
            diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_0), th_input, ch0, 0);
            
            ch1->value.int_value = !((uint8_t) set_initial_state(accessory, 1, json_context, ch1, CH_TYPE_INT8, 0));
            update_th(ch1, HOMEKIT_UINT8(!ch1->value.int_value));
        } else {
            if (diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_1), th_input, ch0, 1)) {
                th_input(0, ch1, 1);
            }
            if (diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_0), th_input, ch0, 0)) {
                ch1->value = HOMEKIT_UINT8(1);
                th_input(0, ch1, 0);
            }
        }
        
        const uint8_t new_accessory_count = build_kill_switches(accessory + 1, ch_group, json_context);
        return new_accessory_count;
    }
    
    uint8_t new_temp_sensor(uint8_t accessory, cJSON *json_context) {
        new_accessory(accessory, 3);
        
        homekit_characteristic_t *ch0 = NEW_HOMEKIT_CHARACTERISTIC(CURRENT_TEMPERATURE, 0, .min_value=(float[]) {-100}, .max_value=(float[]) {200});
        
        ch_group_t *ch_group = malloc(sizeof(ch_group_t));
        memset(ch_group, 0, sizeof(*ch_group));
        ch_group->accessory = accessory_numerator;
        accessory_numerator++;
        ch_group->ch0 = ch0;
        th_sensor(ch_group, json_context);
        register_wildcard_actions(ch_group, json_context);
        ch_group->last_wildcard_action[0] = NO_LAST_WILDCARD_ACTION;
        ch_group->next = ch_groups;
        ch_groups = ch_group;
        
        accessories[accessory]->services[1] = calloc(1, sizeof(homekit_service_t));
        accessories[accessory]->services[1]->id = 8;
        accessories[accessory]->services[1]->primary = true;
        accessories[accessory]->services[1]->type = HOMEKIT_SERVICE_TEMPERATURE_SENSOR;
        accessories[accessory]->services[1]->characteristics = calloc(2, sizeof(homekit_characteristic_t*));
        accessories[accessory]->services[1]->characteristics[0] = ch0;
            
        if (TH_SENSOR_GPIO != -1) {
            th_sensor_starter(ch_group);
        }
        
        return accessory + 1;
    }
    
    uint8_t new_hum_sensor(uint8_t accessory, cJSON *json_context) {
        new_accessory(accessory, 3);
        
        homekit_characteristic_t *ch1 = NEW_HOMEKIT_CHARACTERISTIC(CURRENT_RELATIVE_HUMIDITY, 0);
        
        ch_group_t *ch_group = malloc(sizeof(ch_group_t));
        memset(ch_group, 0, sizeof(*ch_group));
        ch_group->accessory = accessory_numerator;
        accessory_numerator++;
        ch_group->ch1 = ch1;
        th_sensor(ch_group, json_context);
        register_wildcard_actions(ch_group, json_context);
        ch_group->last_wildcard_action[1] = NO_LAST_WILDCARD_ACTION;
        ch_group->next = ch_groups;
        ch_groups = ch_group;
        
        accessories[accessory]->services[1] = calloc(1, sizeof(homekit_service_t));
        accessories[accessory]->services[1]->id = 8;
        accessories[accessory]->services[1]->primary = true;
        accessories[accessory]->services[1]->type = HOMEKIT_SERVICE_HUMIDITY_SENSOR;
        accessories[accessory]->services[1]->characteristics = calloc(2, sizeof(homekit_characteristic_t*));
        accessories[accessory]->services[1]->characteristics[0] = ch1;
        
        if (TH_SENSOR_GPIO != -1) {
            th_sensor_starter(ch_group);
        }
        
        return accessory + 1;
    }
    
    uint8_t new_th_sensor(uint8_t accessory, cJSON *json_context) {
        new_accessory(accessory, 4);
        
        homekit_characteristic_t *ch0 = NEW_HOMEKIT_CHARACTERISTIC(CURRENT_TEMPERATURE, 0, .min_value=(float[]) {-100}, .max_value=(float[]) {200});
        homekit_characteristic_t *ch1 = NEW_HOMEKIT_CHARACTERISTIC(CURRENT_RELATIVE_HUMIDITY, 0);
        
        ch_group_t *ch_group = malloc(sizeof(ch_group_t));
        memset(ch_group, 0, sizeof(*ch_group));
        ch_group->accessory = accessory_numerator;
        accessory_numerator++;
        ch_group->ch0 = ch0;
        ch_group->ch1 = ch1;
        th_sensor(ch_group, json_context);
        register_wildcard_actions(ch_group, json_context);
        ch_group->last_wildcard_action[0] = NO_LAST_WILDCARD_ACTION;
        ch_group->last_wildcard_action[1] = NO_LAST_WILDCARD_ACTION;
        ch_group->next = ch_groups;
        ch_groups = ch_group;
        
        accessories[accessory]->services[1] = calloc(1, sizeof(homekit_service_t));
        accessories[accessory]->services[1]->id = 8;
        accessories[accessory]->services[1]->primary = true;
        accessories[accessory]->services[1]->type = HOMEKIT_SERVICE_TEMPERATURE_SENSOR;
        accessories[accessory]->services[1]->characteristics = calloc(2, sizeof(homekit_characteristic_t*));
        accessories[accessory]->services[1]->characteristics[0] = ch0;
        
        accessories[accessory]->services[2] = calloc(1, sizeof(homekit_service_t));
        accessories[accessory]->services[2]->id = 11;
        accessories[accessory]->services[2]->primary = false;
        accessories[accessory]->services[2]->type = HOMEKIT_SERVICE_HUMIDITY_SENSOR;
        accessories[accessory]->services[2]->characteristics = calloc(2, sizeof(homekit_characteristic_t*));
        accessories[accessory]->services[2]->characteristics[0] = ch1;
        
        if (TH_SENSOR_GPIO != -1) {
            th_sensor_starter(ch_group);
        }
        
        return accessory + 1;
    }
    
    uint8_t new_lightbulb(uint8_t accessory, cJSON *json_context) {
        new_accessory(accessory, 3);
        
        bool is_pwm = true;
        if (cJSON_GetObjectItemCaseSensitive(json_context, LIGHTBULB_PWM_GPIO_R) == NULL &&
            cJSON_GetObjectItemCaseSensitive(json_context, LIGHTBULB_PWM_GPIO_W) == NULL) {
            is_pwm = false;
        }

        if (is_pwm && !lightbulb_groups) {
            INFO2("PWM Init");
            pwm_timer = malloc(sizeof(ETSTimer));
            memset(pwm_timer, 0, sizeof(*pwm_timer));
            sdk_os_timer_setfn(pwm_timer, rgbw_set_timer_worker, NULL);
            
            pwm_info = malloc(sizeof(pwm_info_t));
            memset(pwm_info, 0, sizeof(*pwm_info));
            
            multipwm_init(pwm_info);
            if (pwm_freq > 0) {
                multipwm_set_freq(pwm_info, pwm_freq);
            }
            pwm_info->channels = 0;
        }
        
        homekit_characteristic_t *ch0 = NEW_HOMEKIT_CHARACTERISTIC(ON, false, .setter_ex=hkc_rgbw_setter);
        homekit_characteristic_t *ch1 = NEW_HOMEKIT_CHARACTERISTIC(BRIGHTNESS, 100, .setter_ex=hkc_rgbw_setter);
        
        ch_group_t *ch_group = malloc(sizeof(ch_group_t));
        memset(ch_group, 0, sizeof(*ch_group));
        ch_group->accessory = accessory_numerator;
        accessory_numerator++;
        ch_group->acc_type = ACC_TYPE_LIGHTBULB;
        ch_group->ch0 = ch0;
        ch_group->ch1 = ch1;
        register_actions(ch_group, json_context, 0);
        register_wildcard_actions(ch_group, json_context);
        ch_group->last_wildcard_action[0] = NO_LAST_WILDCARD_ACTION;
        ch_group->next = ch_groups;
        ch_groups = ch_group;
        
        lightbulb_group_t *lightbulb_group = malloc(sizeof(lightbulb_group_t));
        memset(lightbulb_group, 0, sizeof(*lightbulb_group));
        lightbulb_group->ch0 = ch0;
        lightbulb_group->is_pwm = is_pwm;
        lightbulb_group->pwm_r = 255;
        lightbulb_group->pwm_g = 255;
        lightbulb_group->pwm_b = 255;
        lightbulb_group->pwm_w = 255;
        lightbulb_group->target_r = 0;
        lightbulb_group->target_g = 0;
        lightbulb_group->target_b = 0;
        lightbulb_group->target_w = 0;
        lightbulb_group->factor_r = 1;
        lightbulb_group->factor_g = 1;
        lightbulb_group->factor_b = 1;
        lightbulb_group->factor_w = 1;
        lightbulb_group->step = RGBW_STEP_DEFAULT;
        lightbulb_group->autodimmer = 0;
        lightbulb_group->armed_autodimmer = false;
        lightbulb_group->autodimmer_task_delay = AUTODIMMER_TASK_DELAY_DEFAULT;
        lightbulb_group->autodimmer_task_step = AUTODIMMER_TASK_STEP_DEFAULT;
        lightbulb_group->next = lightbulb_groups;
        lightbulb_groups = lightbulb_group;

        if (is_pwm) {
            if (cJSON_GetObjectItemCaseSensitive(json_context, LIGHTBULB_PWM_GPIO_R) != NULL && pwm_info->channels < MULTIPWM_MAX_CHANNELS) {
                lightbulb_group->pwm_r = pwm_info->channels;
                pwm_info->channels++;
                multipwm_set_pin(pwm_info, lightbulb_group->pwm_r, (uint8_t) cJSON_GetObjectItemCaseSensitive(json_context, LIGHTBULB_PWM_GPIO_R)->valuedouble);
            }
            
            if (cJSON_GetObjectItemCaseSensitive(json_context, LIGHTBULB_FACTOR_R) != NULL) {
                lightbulb_group->factor_r = (float) cJSON_GetObjectItemCaseSensitive(json_context, LIGHTBULB_FACTOR_R)->valuedouble;
            }

            if (cJSON_GetObjectItemCaseSensitive(json_context, LIGHTBULB_PWM_GPIO_G) != NULL && pwm_info->channels < MULTIPWM_MAX_CHANNELS) {
                lightbulb_group->pwm_g = pwm_info->channels;
                pwm_info->channels++;
                multipwm_set_pin(pwm_info, lightbulb_group->pwm_g, (uint8_t) cJSON_GetObjectItemCaseSensitive(json_context, LIGHTBULB_PWM_GPIO_G)->valuedouble);
            }
            
            if (cJSON_GetObjectItemCaseSensitive(json_context, LIGHTBULB_FACTOR_G) != NULL) {
                lightbulb_group->factor_g = (float) cJSON_GetObjectItemCaseSensitive(json_context, LIGHTBULB_FACTOR_G)->valuedouble;
            }

            if (cJSON_GetObjectItemCaseSensitive(json_context, LIGHTBULB_PWM_GPIO_B) != NULL && pwm_info->channels < MULTIPWM_MAX_CHANNELS) {
                lightbulb_group->pwm_b = pwm_info->channels;
                pwm_info->channels++;
                multipwm_set_pin(pwm_info, lightbulb_group->pwm_b, (uint8_t) cJSON_GetObjectItemCaseSensitive(json_context, LIGHTBULB_PWM_GPIO_B)->valuedouble);
            }
            
            if (cJSON_GetObjectItemCaseSensitive(json_context, LIGHTBULB_FACTOR_B) != NULL) {
                lightbulb_group->factor_b = (float) cJSON_GetObjectItemCaseSensitive(json_context, LIGHTBULB_FACTOR_B)->valuedouble;
            }

            if (cJSON_GetObjectItemCaseSensitive(json_context, LIGHTBULB_PWM_GPIO_W) != NULL && pwm_info->channels < MULTIPWM_MAX_CHANNELS) {
                lightbulb_group->pwm_w = pwm_info->channels;
                pwm_info->channels++;
                multipwm_set_pin(pwm_info, lightbulb_group->pwm_w, (uint8_t) cJSON_GetObjectItemCaseSensitive(json_context, LIGHTBULB_PWM_GPIO_W)->valuedouble);
            }
            
            if (cJSON_GetObjectItemCaseSensitive(json_context, LIGHTBULB_FACTOR_W) != NULL) {
                lightbulb_group->factor_w = (float) cJSON_GetObjectItemCaseSensitive(json_context, LIGHTBULB_FACTOR_W)->valuedouble;
            }
        }
        
        if (cJSON_GetObjectItemCaseSensitive(json_context, RGBW_STEP) != NULL) {
            lightbulb_group->step = (uint16_t) cJSON_GetObjectItemCaseSensitive(json_context, RGBW_STEP)->valuedouble;
        }
        
        if (cJSON_GetObjectItemCaseSensitive(json_context, AUTODIMMER_TASK_DELAY) != NULL) {
            lightbulb_group->autodimmer_task_delay = cJSON_GetObjectItemCaseSensitive(json_context, AUTODIMMER_TASK_DELAY)->valuedouble * (1000 / portTICK_PERIOD_MS);
        }
        
        if (cJSON_GetObjectItemCaseSensitive(json_context, AUTODIMMER_TASK_STEP) != NULL) {
            lightbulb_group->autodimmer_task_step = (uint8_t) cJSON_GetObjectItemCaseSensitive(json_context, AUTODIMMER_TASK_STEP)->valuedouble;
        }

        accessories[accessory]->services[1] = calloc(1, sizeof(homekit_service_t));
        accessories[accessory]->services[1]->id = 8;
        accessories[accessory]->services[1]->primary = true;
        accessories[accessory]->services[1]->type = HOMEKIT_SERVICE_LIGHTBULB;
        
        if (lightbulb_group->pwm_r != 255) {
            homekit_characteristic_t *ch2 = NEW_HOMEKIT_CHARACTERISTIC(HUE, 0, .setter_ex=hkc_rgbw_setter);
            homekit_characteristic_t *ch3 = NEW_HOMEKIT_CHARACTERISTIC(SATURATION, 0, .setter_ex=hkc_rgbw_setter);
            
            ch_group->ch2 = ch2;
            ch_group->ch3 = ch3;
            
            accessories[accessory]->services[1]->characteristics = calloc(5, sizeof(homekit_characteristic_t*));
            accessories[accessory]->services[1]->characteristics[0] = ch0;
            accessories[accessory]->services[1]->characteristics[1] = ch1;
            accessories[accessory]->services[1]->characteristics[2] = ch2;
            accessories[accessory]->services[1]->characteristics[3] = ch3;
            
            ch2->value.float_value = set_initial_state(accessory, 2, cJSON_Parse(INIT_STATE_LAST_STR), ch2, CH_TYPE_FLOAT, 0);
            ch3->value.float_value = set_initial_state(accessory, 3, cJSON_Parse(INIT_STATE_LAST_STR), ch3, CH_TYPE_FLOAT, 0);
        } else if (lightbulb_group->pwm_b != 255) {
            homekit_characteristic_t *ch2 = NEW_HOMEKIT_CHARACTERISTIC(COLOR_TEMPERATURE, 152, .setter_ex=hkc_rgbw_setter);
            
            ch_group->ch2 = ch2;
            
            accessories[accessory]->services[1]->characteristics = calloc(4, sizeof(homekit_characteristic_t*));
            accessories[accessory]->services[1]->characteristics[0] = ch0;
            accessories[accessory]->services[1]->characteristics[1] = ch1;
            accessories[accessory]->services[1]->characteristics[2] = ch2;
            
            ch2->value.int_value = set_initial_state(accessory, 2, cJSON_Parse(INIT_STATE_LAST_STR), ch2, CH_TYPE_INT32, 152);
        } else {
            accessories[accessory]->services[1]->characteristics = calloc(3, sizeof(homekit_characteristic_t*));
            accessories[accessory]->services[1]->characteristics[0] = ch0;
            accessories[accessory]->services[1]->characteristics[1] = ch1;
        }

        ch1->value.int_value = set_initial_state(accessory, 1, cJSON_Parse(INIT_STATE_LAST_STR), ch1, CH_TYPE_INT8, 100);

        diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, BUTTONS_ARRAY), diginput, ch0, TYPE_LIGHTBULB);
        diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_2), rgbw_brightness, ch1, LIGHTBULB_BRIGHTNESS_UP);
        diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_3), rgbw_brightness, ch1, LIGHTBULB_BRIGHTNESS_DOWN);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, PINGS_ARRAY), diginput, ch0, TYPE_LIGHTBULB);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_ARRAY_2), rgbw_brightness, ch1, LIGHTBULB_BRIGHTNESS_UP);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_ARRAY_3), rgbw_brightness, ch1, LIGHTBULB_BRIGHTNESS_DOWN);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_ARRAY_1), diginput_1, ch0, TYPE_LIGHTBULB);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_ARRAY_0), diginput_0, ch0, TYPE_LIGHTBULB);
        
        if (cJSON_GetObjectItemCaseSensitive(json_context, BUTTONS_ARRAY) != NULL ||
            cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_0) != NULL ||
            cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_1) != NULL) {
            if (lightbulb_group->autodimmer_task_step > 0) {
                ch_group->timer = malloc(sizeof(ETSTimer));
                memset(ch_group->timer, 0, sizeof(*ch_group->timer));
                sdk_os_timer_setfn(ch_group->timer, no_autodimmer_called, ch0);
            }
        } else {
            lightbulb_group->autodimmer_task_step = 0;
        }
        
        uint8_t initial_state = 0;
        if (cJSON_GetObjectItemCaseSensitive(json_context, INITIAL_STATE) != NULL) {
            initial_state = (uint8_t) cJSON_GetObjectItemCaseSensitive(json_context, INITIAL_STATE)->valuedouble;
        }
        
        if (initial_state != INIT_STATE_FIXED_INPUT) {
            diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_1), diginput_1, ch0, TYPE_LIGHTBULB);
            diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_0), diginput_0, ch0, TYPE_LIGHTBULB);
            
            ch0->value.bool_value = !((bool) set_initial_state(accessory, 0, json_context, ch0, CH_TYPE_BOOL, 0));
        } else {
            if (diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_1), diginput_1, ch0, TYPE_LIGHTBULB)) {
                ch0->value = HOMEKIT_BOOL(false);
            }
            if (diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_0), diginput_0, ch0, TYPE_LIGHTBULB)) {
                ch0->value = HOMEKIT_BOOL(true);
            }
        }
        
        const uint8_t new_accessory_count = build_kill_switches(accessory + 1, ch_group, json_context);
        return new_accessory_count;
    }
    
    uint8_t new_garage_door(uint8_t accessory, cJSON *json_context) {
        new_accessory(accessory, 3);
        
        homekit_characteristic_t *ch0 = NEW_HOMEKIT_CHARACTERISTIC(CURRENT_DOOR_STATE, 1);
        homekit_characteristic_t *ch1 = NEW_HOMEKIT_CHARACTERISTIC(TARGET_DOOR_STATE, 1, .setter_ex=hkc_garage_door_setter);
        homekit_characteristic_t *ch2 = NEW_HOMEKIT_CHARACTERISTIC(OBSTRUCTION_DETECTED, false);
        
        accessories[accessory]->services[1] = calloc(1, sizeof(homekit_service_t));
        accessories[accessory]->services[1]->id = 8;
        accessories[accessory]->services[1]->primary = true;
        accessories[accessory]->services[1]->type = HOMEKIT_SERVICE_GARAGE_DOOR_OPENER;
        accessories[accessory]->services[1]->characteristics = calloc(4, sizeof(homekit_characteristic_t*));
        accessories[accessory]->services[1]->characteristics[0] = ch0;
        accessories[accessory]->services[1]->characteristics[1] = ch1;
        accessories[accessory]->services[1]->characteristics[2] = ch2;
        
        ch_group_t *ch_group = malloc(sizeof(ch_group_t));
        memset(ch_group, 0, sizeof(*ch_group));
        ch_group->accessory = accessory_numerator;
        accessory_numerator++;
        ch_group->acc_type = ACC_TYPE_GARAGE_DOOR;
        ch_group->ch0 = ch0;
        ch_group->ch1 = ch1;
        ch_group->ch2 = ch2;
        register_actions(ch_group, json_context, 0);
        GARAGE_DOOR_CURRENT_TIME = GARAGE_DOOR_TIME_MARGIN_DEFAULT;
        GARAGE_DOOR_WORKING_TIME = GARAGE_DOOR_TIME_OPEN_DEFAULT;
        GARAGE_DOOR_TIME_MARGIN = GARAGE_DOOR_TIME_MARGIN_DEFAULT;
        GARAGE_DOOR_CLOSE_TIME_FACTOR = 1;
        ch_group->next = ch_groups;
        ch_groups = ch_group;
        
        ch_group->timer = malloc(sizeof(ETSTimer));
        memset(ch_group->timer, 0, sizeof(*ch_group->timer));
        sdk_os_timer_setfn(ch_group->timer, garage_door_timer_worker, ch0);
        
        if (cJSON_GetObjectItemCaseSensitive(json_context, GARAGE_DOOR_TIME_MARGIN_SET) != NULL) {
            GARAGE_DOOR_TIME_MARGIN = cJSON_GetObjectItemCaseSensitive(json_context, GARAGE_DOOR_TIME_MARGIN_SET)->valuedouble;
        }
        
        if (cJSON_GetObjectItemCaseSensitive(json_context, GARAGE_DOOR_TIME_OPEN_SET) != NULL) {
            GARAGE_DOOR_WORKING_TIME = cJSON_GetObjectItemCaseSensitive(json_context, GARAGE_DOOR_TIME_OPEN_SET)->valuedouble;
        }
        
        if (cJSON_GetObjectItemCaseSensitive(json_context, GARAGE_DOOR_TIME_CLOSE_SET) != NULL) {
            GARAGE_DOOR_CLOSE_TIME_FACTOR = GARAGE_DOOR_WORKING_TIME / cJSON_GetObjectItemCaseSensitive(json_context, GARAGE_DOOR_TIME_CLOSE_SET)->valuedouble;
        }
        
        GARAGE_DOOR_WORKING_TIME += GARAGE_DOOR_TIME_MARGIN * 2;
        
        diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, BUTTONS_ARRAY), diginput, ch1, TYPE_GARAGE_DOOR);
        diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_0), diginput_0, ch1, TYPE_GARAGE_DOOR);
        diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_1), diginput_1, ch1, TYPE_GARAGE_DOOR);
        diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_8), garage_door_stop, ch0, 0);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, PINGS_ARRAY), diginput, ch1, TYPE_GARAGE_DOOR);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_ARRAY_0), diginput_0, ch1, TYPE_GARAGE_DOOR);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_ARRAY_1), diginput_1, ch1, TYPE_GARAGE_DOOR);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_ARRAY_8), garage_door_stop, ch0, 0);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_ARRAY_5), garage_door_sensor, ch0, GARAGE_DOOR_CLOSING);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_ARRAY_4), garage_door_sensor, ch0, GARAGE_DOOR_OPENING);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_ARRAY_3), garage_door_sensor, ch0, GARAGE_DOOR_CLOSED);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_ARRAY_2), garage_door_sensor, ch0, GARAGE_DOOR_OPENED);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_ARRAY_6), garage_door_obstruction, ch0, 0);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_ARRAY_7), garage_door_obstruction, ch0, 1);
        
        ch0->value.int_value = (uint8_t) set_initial_state(accessory, 0, json_context, ch0, CH_TYPE_INT8, 1);
        if (ch0->value.int_value > 1) {
            ch1->value.int_value = ch0->value.int_value - 2;
        } else {
            ch1->value.int_value = ch0->value.int_value;
        }
        
        if (ch0->value.int_value == 0) {
            GARAGE_DOOR_CURRENT_TIME = GARAGE_DOOR_WORKING_TIME - GARAGE_DOOR_TIME_MARGIN;
        }

        GARAGE_DOOR_HAS_F5 = 0;
        if (cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_5) != NULL) {
            GARAGE_DOOR_HAS_F5 = 1;
        }
        
        GARAGE_DOOR_HAS_F4 = 0;
        if (cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_4) != NULL) {
            GARAGE_DOOR_HAS_F4 = 1;
        }
        
        GARAGE_DOOR_HAS_F3 = 0;
        if (cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_3) != NULL) {
            GARAGE_DOOR_HAS_F3 = 1;
        }
        
        GARAGE_DOOR_HAS_F2 = 0;
        if (cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_2) != NULL) {
            GARAGE_DOOR_HAS_F2 = 1;
        }
        
        if (diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_5), garage_door_sensor, ch0, GARAGE_DOOR_CLOSING)) {
            garage_door_sensor(0, ch0, GARAGE_DOOR_OPENING);
        }
        
        if (diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_4), garage_door_sensor, ch0, GARAGE_DOOR_OPENING)) {
            garage_door_sensor(0, ch0, GARAGE_DOOR_OPENING);
        }
        
        if (diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_3), garage_door_sensor, ch0, GARAGE_DOOR_CLOSED)) {
            garage_door_sensor(0, ch0, GARAGE_DOOR_CLOSED);
        }

        if (diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_2), garage_door_sensor, ch0, GARAGE_DOOR_OPENED)) {
            garage_door_sensor(0, ch0, GARAGE_DOOR_OPENED);
        }
        
        if (diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_6), garage_door_obstruction, ch0, 0)) {
            garage_door_obstruction(0, ch0, 0);
        }
        if (diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_7), garage_door_obstruction, ch0, 1)) {
            garage_door_obstruction(0, ch0, 1);
        }
        
        const uint8_t new_accessory_count = build_kill_switches(accessory + 1, ch_group, json_context);
        return new_accessory_count;
    }
    
    uint8_t new_window_cover(uint8_t accessory, cJSON *json_context) {
        new_accessory(accessory, 3);
        
        uint8_t cover_type = WINDOW_COVER_TYPE_DEFAULT;
        if (cJSON_GetObjectItemCaseSensitive(json_context, WINDOW_COVER_TYPE) != NULL) {
            cover_type = (uint8_t) cJSON_GetObjectItemCaseSensitive(json_context, WINDOW_COVER_TYPE)->valuedouble;
        }
        
        homekit_characteristic_t *ch0 = NEW_HOMEKIT_CHARACTERISTIC(CURRENT_POSITION, 0);
        homekit_characteristic_t *ch1 = NEW_HOMEKIT_CHARACTERISTIC(TARGET_POSITION, 0, .setter_ex=hkc_window_cover_setter);
        homekit_characteristic_t *ch2 = NEW_HOMEKIT_CHARACTERISTIC(POSITION_STATE, WINDOW_COVER_STOP);
        homekit_characteristic_t *ch3 = NEW_HOMEKIT_CHARACTERISTIC(OBSTRUCTION_DETECTED, false);
        
        accessories[accessory]->services[1] = calloc(1, sizeof(homekit_service_t));
        accessories[accessory]->services[1]->id = 8;
        accessories[accessory]->services[1]->primary = true;
        
        switch (cover_type) {
            case 1:
                accessories[accessory]->services[1]->type = HOMEKIT_SERVICE_WINDOW;
                break;
                
            case 2:
                accessories[accessory]->services[1]->type = HOMEKIT_SERVICE_DOOR;
                break;
                
            default:    // case 0:
                accessories[accessory]->services[1]->type = HOMEKIT_SERVICE_WINDOW_COVERING;
                break;
        }

        accessories[accessory]->services[1]->characteristics = calloc(5, sizeof(homekit_characteristic_t*));
        accessories[accessory]->services[1]->characteristics[0] = ch0;
        accessories[accessory]->services[1]->characteristics[1] = ch1;
        accessories[accessory]->services[1]->characteristics[2] = ch2;
        accessories[accessory]->services[1]->characteristics[3] = ch3;
        
        ch_group_t *ch_group = malloc(sizeof(ch_group_t));
        memset(ch_group, 0, sizeof(*ch_group));
        ch_group->accessory = accessory_numerator;
        accessory_numerator++;
        ch_group->acc_type = ACC_TYPE_WINDOW_COVER;
        WINDOW_COVER_CH_CURRENT_POSITION = ch0;
        WINDOW_COVER_CH_TARGET_POSITION = ch1;
        WINDOW_COVER_CH_STATE = ch2;
        WINDOW_COVER_CH_OBSTRUCTION = ch3;
        WINDOW_COVER_STEP_TIME_UP = WINDOW_COVER_STEP_TIME(WINDOW_COVER_TIME_OPEN_DEFAULT);
        WINDOW_COVER_STEP_TIME_DOWN = WINDOW_COVER_STEP_TIME(WINDOW_COVER_TIME_OPEN_DEFAULT);
        WINDOW_COVER_POSITION = 0;
        WINDOW_COVER_REAL_POSITION = 0;
        WINDOW_COVER_CORRECTION = WINDOW_COVER_CORRECTION_DEFAULT;
        register_actions(ch_group, json_context, 0);
        register_wildcard_actions(ch_group, json_context);
        ch_group->last_wildcard_action[0] = NO_LAST_WILDCARD_ACTION;
        ch_group->next = ch_groups;
        ch_groups = ch_group;
        
        ch_group->timer = malloc(sizeof(ETSTimer));
        memset(ch_group->timer, 0, sizeof(*ch_group->timer));
        sdk_os_timer_setfn(ch_group->timer, window_cover_timer_worker, ch0);
        
        if (cJSON_GetObjectItemCaseSensitive(json_context, WINDOW_COVER_TIME_OPEN_SET) != NULL) {
            WINDOW_COVER_STEP_TIME_UP = WINDOW_COVER_STEP_TIME(cJSON_GetObjectItemCaseSensitive(json_context, WINDOW_COVER_TIME_OPEN_SET)->valuedouble);
        }
        
        if (cJSON_GetObjectItemCaseSensitive(json_context, WINDOW_COVER_TIME_CLOSE_SET) != NULL) {
            WINDOW_COVER_STEP_TIME_DOWN = WINDOW_COVER_STEP_TIME(cJSON_GetObjectItemCaseSensitive(json_context, WINDOW_COVER_TIME_CLOSE_SET)->valuedouble);
        }
        
        if (cJSON_GetObjectItemCaseSensitive(json_context, WINDOW_COVER_CORRECTION_SET) != NULL) {
            WINDOW_COVER_CORRECTION = cJSON_GetObjectItemCaseSensitive(json_context, WINDOW_COVER_CORRECTION_SET)->valuedouble;
        }
        
        WINDOW_COVER_POSITION = set_initial_state(accessory, 0, cJSON_Parse(INIT_STATE_LAST_STR), ch0, CH_TYPE_INT8, 0);
        ch0->value.int_value = (uint8_t) WINDOW_COVER_POSITION;
        ch1->value.int_value = ch0->value.int_value;
        
        diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, BUTTONS_ARRAY), diginput, ch1, TYPE_WINDOW_COVER);
        diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_0), window_cover_diginput, ch1, WINDOW_COVER_CLOSING);
        diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_1), window_cover_diginput, ch1, WINDOW_COVER_OPENING);
        diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_2), window_cover_diginput, ch1, WINDOW_COVER_STOP);
        diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_3), window_cover_diginput, ch1, WINDOW_COVER_CLOSING + 3);
        diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_4), window_cover_diginput, ch1, WINDOW_COVER_OPENING + 3);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, PINGS_ARRAY), diginput, ch1, TYPE_WINDOW_COVER);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_ARRAY_0), window_cover_diginput, ch1, WINDOW_COVER_CLOSING);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_ARRAY_1), window_cover_diginput, ch1, WINDOW_COVER_OPENING);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_ARRAY_2), window_cover_diginput, ch1, WINDOW_COVER_STOP);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_ARRAY_3), window_cover_diginput, ch1, WINDOW_COVER_CLOSING + 3);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_ARRAY_4), window_cover_diginput, ch1, WINDOW_COVER_OPENING + 3);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_ARRAY_5), window_cover_obstruction, ch0, 0);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_ARRAY_6), window_cover_obstruction, ch0, 1);
        
        if (diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_5), window_cover_obstruction, ch0, 0)) {
            window_cover_obstruction(0, ch0, 0);
        }
        if (diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_6), window_cover_obstruction, ch0, 1)) {
            window_cover_obstruction(0, ch0, 1);
        }

        const uint8_t new_accessory_count = build_kill_switches(accessory + 1, ch_group, json_context);
        return new_accessory_count;
    }
    
    uint8_t new_fan(uint8_t accessory, cJSON *json_context) {
        new_accessory(accessory, 3);
        
        uint8_t max_speed = 100;
        if (cJSON_GetObjectItemCaseSensitive(json_context, FAN_SPEED_STEPS) != NULL) {
            max_speed = (uint8_t) cJSON_GetObjectItemCaseSensitive(json_context, FAN_SPEED_STEPS)->valuedouble;
        }
        
        homekit_characteristic_t *ch0 = NEW_HOMEKIT_CHARACTERISTIC(ON, false, .setter_ex=hkc_fan_setter);
        homekit_characteristic_t *ch1 = NEW_HOMEKIT_CHARACTERISTIC(ROTATION_SPEED, max_speed, .max_value=(float[]) {max_speed}, .setter_ex=hkc_fan_speed_setter);
        
        ch_group_t *ch_group = malloc(sizeof(ch_group_t));
        memset(ch_group, 0, sizeof(*ch_group));
        ch_group->accessory = accessory_numerator;
        accessory_numerator++;
        ch_group->acc_type = ACC_TYPE_FAN;
        ch_group->ch0 = ch0;
        ch_group->ch1 = ch1;
        register_actions(ch_group, json_context, 0);
        register_wildcard_actions(ch_group, json_context);
        ch_group->last_wildcard_action[0] = NO_LAST_WILDCARD_ACTION;
        ch_group->num[0] = autoswitch_time(json_context);
        ch_group->next = ch_groups;
        ch_groups = ch_group;
        
        accessories[accessory]->services[1] = calloc(1, sizeof(homekit_service_t));
        accessories[accessory]->services[1]->id = 8;
        accessories[accessory]->services[1]->primary = true;
        accessories[accessory]->services[1]->type = HOMEKIT_SERVICE_FAN;
        accessories[accessory]->services[1]->characteristics = calloc(3, sizeof(homekit_characteristic_t*));
        accessories[accessory]->services[1]->characteristics[0] = ch0;
        accessories[accessory]->services[1]->characteristics[1] = ch1;
        
        float saved_max_speed = set_initial_state(accessory, 1, cJSON_Parse(INIT_STATE_LAST_STR), ch1, CH_TYPE_FLOAT, max_speed);
        if (saved_max_speed > max_speed) {
            saved_max_speed = max_speed;
        }
        ch1->value.float_value = saved_max_speed;
        
        diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, BUTTONS_ARRAY), diginput, ch0, TYPE_FAN);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, PINGS_ARRAY), diginput, ch0, TYPE_FAN);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_ARRAY_1), diginput_1, ch0, TYPE_FAN);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_ARRAY_0), diginput_0, ch0, TYPE_FAN);
        diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_STATUS_ARRAY_1), digstate_1, ch0, TYPE_FAN);
        diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_STATUS_ARRAY_0), digstate_0, ch0, TYPE_FAN);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_STATUS_ARRAY_1), digstate_1, ch0, TYPE_FAN);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_STATUS_ARRAY_0), digstate_0, ch0, TYPE_FAN);
        
        uint8_t initial_state = 0;
        if (cJSON_GetObjectItemCaseSensitive(json_context, INITIAL_STATE) != NULL) {
            initial_state = (uint8_t) cJSON_GetObjectItemCaseSensitive(json_context, INITIAL_STATE)->valuedouble;
        }
        
        if (initial_state != INIT_STATE_FIXED_INPUT) {
            diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_1), diginput_1, ch0, TYPE_FAN);
            diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_0), diginput_0, ch0, TYPE_FAN);
            
            ch0->value.bool_value = !((uint8_t) set_initial_state(accessory, 0, json_context, ch0, CH_TYPE_BOOL, false));
            hkc_fan_setter(ch0, HOMEKIT_UINT8(!ch0->value.bool_value));
        } else {
            if (diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_1), diginput_1, ch0, TYPE_FAN)) {
                diginput_1(0, ch0, TYPE_FAN);
            }
            if (diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_0), diginput_0, ch0, TYPE_FAN)) {
                ch0->value = HOMEKIT_UINT8(1);
                diginput_0(0, ch0, TYPE_FAN);
            }
        }
        
        const uint8_t new_accessory_count = build_kill_switches(accessory + 1, ch_group, json_context);
        return new_accessory_count;
    }
    
    uint8_t new_tv(uint8_t accessory, cJSON *json_context) {
        cJSON *json_inputs = cJSON_GetObjectItemCaseSensitive(json_context, TV_INPUTS_ARRAY);
        uint8_t inputs = cJSON_GetArraySize(json_inputs);
        
        if (inputs == 0) {
            inputs = 1;
        }
        
        new_accessory(accessory, 4 + inputs);

        homekit_characteristic_t *ch0 = NEW_HOMEKIT_CHARACTERISTIC(ACTIVE, 0, .setter_ex=hkc_tv_active);
        homekit_characteristic_t *ch1 = NEW_HOMEKIT_CHARACTERISTIC(CONFIGURED_NAME, "ESPH TV", .setter_ex=hkc_tv_configured_name);
        homekit_characteristic_t *ch2 = NEW_HOMEKIT_CHARACTERISTIC(ACTIVE_IDENTIFIER, 1, .setter_ex=hkc_tv_active_identifier);
        homekit_characteristic_t *ch3 = NEW_HOMEKIT_CHARACTERISTIC(REMOTE_KEY, .setter_ex=hkc_tv_key);
        homekit_characteristic_t *ch4 = NEW_HOMEKIT_CHARACTERISTIC(POWER_MODE_SELECTION, 0, .setter_ex=hkc_tv_power_mode);
        
        homekit_characteristic_t *ch5 = NEW_HOMEKIT_CHARACTERISTIC(MUTE, false, .setter_ex=hkc_tv_mute);
        homekit_characteristic_t *ch6 = NEW_HOMEKIT_CHARACTERISTIC(ACTIVE, true);
        homekit_characteristic_t *ch7 = NEW_HOMEKIT_CHARACTERISTIC(VOLUME_SELECTOR, .setter_ex=hkc_tv_volume);

        ch_group_t *ch_group = malloc(sizeof(ch_group_t));
        memset(ch_group, 0, sizeof(*ch_group));
        ch_group->accessory = accessory_numerator;
        accessory_numerator++;
        ch_group->acc_type = ACC_TYPE_TV;
        ch_group->ch0 = ch0;
        ch_group->ch1 = ch1;
        ch_group->ch2 = ch2;
        ch_group->ch3 = ch3;
        ch_group->ch4 = ch4;
        ch_group->ch5 = ch5;
        ch_group->ch6 = ch6;
        ch_group->ch7 = ch7;
        register_actions(ch_group, json_context, 0);
        ch_group->next = ch_groups;
        ch_groups = ch_group;
        
        homekit_service_t *new_tv_input_service(const uint8_t service_number, char *name) {
            INFO2("TV Input: %s", name);
            
            homekit_service_t **service = calloc(1, sizeof(homekit_service_t*));
            
            service[0] = calloc(1, sizeof(homekit_service_t));
            service[0]->id = 26 + (service_number * 8);
            service[0]->primary = false;
            service[0]->type = HOMEKIT_SERVICE_INPUT_SOURCE;
            service[0]->characteristics = calloc(7, sizeof(homekit_characteristic_t*));
            service[0]->characteristics[0] = NEW_HOMEKIT_CHARACTERISTIC(NAME, "I");
            service[0]->characteristics[1] = NEW_HOMEKIT_CHARACTERISTIC(IDENTIFIER, service_number);
            service[0]->characteristics[2] = NEW_HOMEKIT_CHARACTERISTIC(CONFIGURED_NAME, name, .setter_ex=hkc_tv_input_configured_name);
            service[0]->characteristics[3] = NEW_HOMEKIT_CHARACTERISTIC(INPUT_SOURCE_TYPE, HOMEKIT_INPUT_SOURCE_TYPE_HDMI);
            service[0]->characteristics[4] = NEW_HOMEKIT_CHARACTERISTIC(IS_CONFIGURED, true);
            service[0]->characteristics[5] = NEW_HOMEKIT_CHARACTERISTIC(CURRENT_VISIBILITY_STATE, HOMEKIT_CURRENT_VISIBILITY_STATE_SHOWN);
            
            return *service;
        }
        
        accessories[accessory]->services[1] = calloc(1, sizeof(homekit_service_t));
        accessories[accessory]->services[1]->id = 18;
        accessories[accessory]->services[1]->primary = true;
        accessories[accessory]->services[1]->type = HOMEKIT_SERVICE_TELEVISION;
        accessories[accessory]->services[1]->characteristics = calloc(9, sizeof(homekit_characteristic_t*));
        accessories[accessory]->services[1]->characteristics[0] = ch0;
        accessories[accessory]->services[1]->characteristics[1] = ch1;
        accessories[accessory]->services[1]->characteristics[2] = ch2;
        accessories[accessory]->services[1]->characteristics[3] = NEW_HOMEKIT_CHARACTERISTIC(NAME, "TV");
        accessories[accessory]->services[1]->characteristics[4] = NEW_HOMEKIT_CHARACTERISTIC(SLEEP_DISCOVERY_MODE, HOMEKIT_SLEEP_DISCOVERY_MODE_ALWAYS_DISCOVERABLE);
        accessories[accessory]->services[1]->characteristics[5] = ch3;
        accessories[accessory]->services[1]->characteristics[6] = NEW_HOMEKIT_CHARACTERISTIC(PICTURE_MODE, HOMEKIT_PICTURE_MODE_STANDARD);
        accessories[accessory]->services[1]->characteristics[7] = ch4;
        
        accessories[accessory]->services[1]->linked = calloc(inputs + 1, sizeof(homekit_service_t*));
        
        for (uint8_t i = 0; i < inputs; i++) {
            cJSON *json_input = cJSON_GetArrayItem(json_inputs, i);
            
            char *name = strdup("TV");
            if (cJSON_GetObjectItemCaseSensitive(json_input, TV_INPUT_NAME) != NULL) {
                free(name);
                name = strdup(cJSON_GetObjectItemCaseSensitive(json_input, TV_INPUT_NAME)->valuestring);
                if (cJSON_GetObjectItemCaseSensitive(json_input, "0") != NULL) {
                    uint8_t int_action = MAX_ACTIONS + i;
                    char action[3];
                    itoa(int_action, action, 10);
                    cJSON *json_new_input_action = cJSON_CreateObject();
                    cJSON_AddItemReferenceToObject(json_new_input_action, action, cJSON_GetObjectItemCaseSensitive(json_input, "0"));
                    register_actions(ch_group, json_new_input_action, int_action);
                    cJSON_Delete(json_new_input_action);
                }
            }
            
            accessories[accessory]->services[1]->linked[i] = new_tv_input_service(i + 1, name);
            accessories[accessory]->services[i + 3] = accessories[accessory]->services[1]->linked[i];
        }
        
        accessories[accessory]->services[2] = calloc(1, sizeof(homekit_service_t));
        accessories[accessory]->services[2]->id = 28;
        accessories[accessory]->services[2]->primary = false;
        accessories[accessory]->services[2]->type = HOMEKIT_SERVICE_TELEVISION_SPEAKER;
        accessories[accessory]->services[2]->characteristics = calloc(5, sizeof(homekit_characteristic_t*));
        accessories[accessory]->services[2]->characteristics[0] = ch5;
        accessories[accessory]->services[2]->characteristics[1] = ch6;
        accessories[accessory]->services[2]->characteristics[2] = NEW_HOMEKIT_CHARACTERISTIC(VOLUME_CONTROL_TYPE, HOMEKIT_VOLUME_CONTROL_TYPE_RELATIVE);
        accessories[accessory]->services[2]->characteristics[3] = ch7;
        
        uint32_t configured_name = set_initial_state(accessory, 1, cJSON_Parse(INIT_STATE_LAST_STR), ch1, CH_TYPE_STRING, 0);
        if (configured_name > 0) {
            homekit_value_destruct(&ch1->value);
            ch1->value = HOMEKIT_STRING((char *) configured_name);
        }
        
        diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, BUTTONS_ARRAY), diginput, ch0, TYPE_TV);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, PINGS_ARRAY), diginput, ch0, TYPE_TV);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_ARRAY_1), diginput_1, ch0, TYPE_TV);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_ARRAY_0), diginput_0, ch0, TYPE_TV);
        diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_STATUS_ARRAY_1), digstate_1, ch0, TYPE_TV);
        diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_STATUS_ARRAY_0), digstate_0, ch0, TYPE_TV);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_STATUS_ARRAY_1), digstate_1, ch0, TYPE_TV);
        ping_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_PINGS_STATUS_ARRAY_0), digstate_0, ch0, TYPE_TV);
        
        uint8_t initial_state = 0;
        if (cJSON_GetObjectItemCaseSensitive(json_context, INITIAL_STATE) != NULL) {
            initial_state = (uint8_t) cJSON_GetObjectItemCaseSensitive(json_context, INITIAL_STATE)->valuedouble;
        }
        
        if (initial_state != INIT_STATE_FIXED_INPUT) {
            diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_1), diginput_1, ch0, TYPE_TV);
            diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_0), diginput_0, ch0, TYPE_TV);
            
            ch0->value.int_value = !((uint8_t) set_initial_state(accessory, 0, json_context, ch0, CH_TYPE_INT8, 0));
            hkc_tv_active(ch0, HOMEKIT_UINT8(!ch0->value.int_value));
        } else {
            if (diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_1), diginput_1, ch0, TYPE_TV)) {
                diginput_1(0, ch0, TYPE_TV);
            }
            if (diginput_register(cJSON_GetObjectItemCaseSensitive(json_context, FIXED_BUTTONS_ARRAY_0), diginput_0, ch0, TYPE_TV)) {
                ch0->value = HOMEKIT_UINT8(1);
                diginput_0(0, ch0, TYPE_TV);
            }
        }
        
        const uint8_t new_accessory_count = build_kill_switches(accessory + 1, ch_group, json_context);
        return new_accessory_count;
    }
    
    uint8_t acc_count = 0;
    
    // Accessory Builder
    if (bridge_needed) {
        INFO2("BRIDGE CREATED");
        new_accessory(0, 2);
        acc_count++;
    }
    
    for(uint8_t i = 0; i < total_accessories; i++) {
        INFO2("\nACCESSORY %i", accessory_numerator);
        
        uint8_t acc_type = ACC_TYPE_SWITCH;
        if (cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(json_accessories, i), ACCESSORY_TYPE) != NULL) {
            acc_type = (uint8_t) cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(json_accessories, i), ACCESSORY_TYPE)->valuedouble;
        }
        
        cJSON *json_accessory = cJSON_GetArrayItem(json_accessories, i);

// Creating HomeKit Accessory
        INFO2("Type %i", acc_type);
        if (acc_type == ACC_TYPE_BUTTON) {
            acc_count = new_button_event(acc_count, json_accessory);
            
        } else if (acc_type == ACC_TYPE_LOCK) {
            acc_count = new_lock(acc_count, json_accessory);
            
        } else if (acc_type >= ACC_TYPE_CONTACT_SENSOR && acc_type < ACC_TYPE_WATER_VALVE) {
            acc_count = new_sensor(acc_count, json_accessory, acc_type);
            
        } else if (acc_type == ACC_TYPE_WATER_VALVE) {
            acc_count = new_water_valve(acc_count, json_accessory);
        
        } else if (acc_type == ACC_TYPE_THERMOSTAT) {
            acc_count = new_thermostat(acc_count, json_accessory);
            
        } else if (acc_type == ACC_TYPE_TEMP_SENSOR) {
            acc_count = new_temp_sensor(acc_count, json_accessory);
            
        } else if (acc_type == ACC_TYPE_HUM_SENSOR) {
            acc_count = new_hum_sensor(acc_count, json_accessory);
            
        } else if (acc_type == ACC_TYPE_TH_SENSOR) {
            acc_count = new_th_sensor(acc_count, json_accessory);
            
        } else if (acc_type == ACC_TYPE_LIGHTBULB) {
            acc_count = new_lightbulb(acc_count, json_accessory);
            
        } else if (acc_type == ACC_TYPE_GARAGE_DOOR) {
            acc_count = new_garage_door(acc_count, json_accessory);
            
        } else if (acc_type == ACC_TYPE_WINDOW_COVER) {
            acc_count = new_window_cover(acc_count, json_accessory);

        } else if (acc_type == ACC_TYPE_LIGHT_SENSOR) {
            //acc_count = new_analog_sensor(acc_count, json_accessory);
            
        } else if (acc_type == ACC_TYPE_TV) {
            acc_count = new_tv(acc_count, json_accessory);
            
        } else if (acc_type == ACC_TYPE_FAN) {
            acc_count = new_fan(acc_count, json_accessory);
        
        } else {    // acc_type == ACC_TYPE_SWITCH || acc_type == ACC_TYPE_OUTLET
            acc_count = new_switch(acc_count, json_accessory, acc_type);
        }
        
        setup_mode_toggle_counter = INT8_MIN;
        
        vTaskDelay(MS_TO_TICK(ACC_CREATION_DELAY));
    }
    
    sysparam_set_int8("total_ac", hk_total_ac);
    
    INFO2("");
    
    // --- LIGHTBULBS INIT
    if (lightbulb_groups) {
        INFO2("Init Lights");
        
        setpwm_bool_semaphore = false;
        
        lightbulb_group_t *lightbulb_group = lightbulb_groups;
        while (lightbulb_group) {
            ch_group_t *ch_group = ch_group_find(lightbulb_group->ch0);
            bool kill_switch = false;
            if (ch_group->ch_sec && !ch_group->ch_sec->value.bool_value) {
                ch_group->ch_sec->value = HOMEKIT_BOOL(true);
                kill_switch = true;
            }
            
            hkc_rgbw_setter(lightbulb_group->ch0, HOMEKIT_BOOL(!lightbulb_group->ch0->value.bool_value));
            
            if (kill_switch) {
                ch_group->ch_sec->value = HOMEKIT_BOOL(false);
            }
            
            lightbulb_group = lightbulb_group->next;
        }
        
        INFO2("");
    }
    
    // --- HOMEKIT SET CONFIG
    serial.value = name.value;
    config.accessories = accessories;
    config.setupId = "WWNS";
    config.category = homekit_accessory_category_other;
    config.config_number = FIRMWARE_VERSION_OCTAL;
    config.log_output = log_output;
    
    setup_mode_toggle_counter = 0;

    cJSON_Delete(json_esph);
    
    wifi_config_init("ESPH", NULL, run_homekit_server, custom_hostname);
    
    vTaskDelete(NULL);
}

void user_init(void) {
#ifdef ESPH_DEBUG
    log_output = true;
    sdk_os_timer_setfn(&free_heap_timer, free_heap_watchdog, NULL);
    sdk_os_timer_arm(&free_heap_timer, 2000, 1);
#endif // ESPH_DEBUG
    
    sdk_wifi_station_set_auto_connect(false);
    sdk_wifi_set_opmode(STATION_MODE);
    sdk_wifi_station_disconnect();

    printf("\n\n\n\n");
    
    // Sysparam starter
    sysparam_status_t status;
    status = sysparam_init(SYSPARAMSECTOR, 0);
    if (status == SYSPARAM_NOTFOUND) {
        status = sysparam_init(SYSPARAMOLDSECTOR, 0);
        if (status == SYSPARAM_NOTFOUND) {
            printf("Creating new sysparam\n");
            sysparam_create_area(SYSPARAMSECTOR, SYSPARAMSIZE, true);
            sysparam_init(SYSPARAMSECTOR, 0);
        } else if (status == SYSPARAM_OK) {
            printf("Old sysparam ready\n");
        }
    } else if (status == SYSPARAM_OK) {
        printf("New sysparam ready\n");
    }

    uint8_t macaddr[6];
    sdk_wifi_get_macaddr(STATION_IF, macaddr);
    
    snprintf(name_value, 11, "ESPH-%02X%02X%02X", macaddr[3], macaddr[4], macaddr[5]);
    name.value = HOMEKIT_STRING(name_value);
    
    int8_t esph_setup = 0;
    
    //sysparam_set_int8("setup", 2);            // Force to enter always in setup mode. Only for tests. Keep comment for releases
    //sysparam_set_string("ota_repo", "1");     // Simulates Installation with OTA. Only for tests. Keep comment for releases
    
    sysparam_get_int8("setup", &esph_setup);
    if (esph_setup > 0) {
        uart_set_baud(0, 115200);
        printf_header();
        printf("SETUP MODE\n");
        wifi_config_init("ESPH", NULL, NULL, name.value.string_value);
        
    } else {
        // Arming emergency Setup Mode
        sysparam_set_int8("setup", 1);
        
        xTaskCreate(normal_mode_init, "normal_mode_init", INITIAL_SETUP_TASK_SIZE, NULL, 1, NULL);
    }
}
