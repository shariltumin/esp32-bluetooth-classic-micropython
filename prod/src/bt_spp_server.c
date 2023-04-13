/*
   MIT License

   Copyright (c) 2023 Sharil Tumin

   Permission is hereby granted, free of charge, to any person obtaining a copy
   of this software and associated documentation files (the "Software"), to deal
   in the Software without restriction, including without limitation the rights
   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
   copies of the Software, and to permit persons to whom the Software is
   furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
   SOFTWARE.

   This the server, slave, or peripheral of Bluetooth Classic serving SPP

*/

#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
// -include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_bt_device.h"
#include "esp_spp_api.h"

#include "py/obj.h"
#include "py/runtime.h"

// -define TAG "SPP_SERVER"

#define NON_BLOCKING 0  
#define DEFAULT_PIPE_SIZE 1024

typedef struct _pipe_obj_t {
    char *buffer;
    int head;
    int tail;
    int size;
    SemaphoreHandle_t lock;
} pipe_obj_t;

pipe_obj_t *pipe; /* will get value at bts.init() */

#define SPP_DATA_LEN ESP_SPP_MAX_MTU
static uint8_t spp_data[SPP_DATA_LEN];  /* ESP_SPP_MAX_MTU = 990 bytes */

static const esp_spp_mode_t esp_spp_mode = ESP_SPP_MODE_CB;
static const esp_spp_sec_t sec_mask = ESP_SPP_SEC_AUTHENTICATE;
// static const esp_spp_sec_t sec_mask = ESP_SPP_SEC_AUTHORIZE;
static const esp_spp_role_t role_slave = ESP_SPP_ROLE_SLAVE;

typedef struct _slave_obj_t {
   char name[ESP_BT_GAP_MAX_BDNAME_LEN + 1];
   uint8_t pin_code[17];
   /* esp_bd_addr_t master_addr; */
   bool ready;
   uint32_t handle; /* current write handle */
} slave_obj_t;

slave_obj_t *slave; /* will get value at bts.init() */

static bool slave_storage = false; /* slave and pipe storage allocation flag */

static bool slave_up = false; /* slave not up, can do init */

static bool slave_auth = false; /* slave not autenticated */

static void esp_spp_cb(esp_spp_cb_event_t event, esp_spp_cb_param_t *param)
{
    uint8_t *items;
    int count;

    switch (event) {
    case ESP_SPP_INIT_EVT:
        break;
    case ESP_SPP_DISCOVERY_COMP_EVT:
        break;
    case ESP_SPP_OPEN_EVT:
        break;
    case ESP_SPP_CLOSE_EVT:
        slave->ready = false;
        slave->handle = NULL;
        // now waiting for new connection 
        esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
        break;
    case ESP_SPP_START_EVT:
        break;
    case ESP_SPP_CL_INIT_EVT:
        break;
    case ESP_SPP_DATA_IND_EVT:
        items = param->data_ind.data;
        count = param->data_ind.len;
        int i = 0;
        if (xSemaphoreTake(pipe->lock, (TickType_t) NON_BLOCKING) == pdTRUE) {
           for (i = 0; i < count; i++) {
               int next_tail = (pipe->tail + 1) % pipe->size;
               if (next_tail == pipe->head) {
                   break;
               }
               pipe->buffer[pipe->tail] = (char) *items;
               pipe->tail = next_tail;
               items++;
               // added++;
           }
           xSemaphoreGive(pipe->lock);
        }
        slave->handle = param->data_ind.handle;
        slave->ready = true;  // master MUST send message slave first
        break;
    case ESP_SPP_CONG_EVT:
        break;
    case ESP_SPP_WRITE_EVT:
        break;
    case ESP_SPP_SRV_OPEN_EVT:
        // make the slave stop responding to discorery request
        esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
        break;
    default:
        break;
    }
}

void esp_bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:{
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            slave_auth = true;
        } else {
            slave_auth = false;
        }
        break;
    }
    case ESP_BT_GAP_PIN_REQ_EVT:{
        break;
    }

    // These are for CONFIG_BT_SSP_ENABLED
    case ESP_BT_GAP_CFM_REQ_EVT:
        break;
    case ESP_BT_GAP_KEY_NOTIF_EVT:
        break;
    case ESP_BT_GAP_KEY_REQ_EVT:
        break;
    // These are for CONFIG_BT_SSP_ENABLED

    default: {
        break;
    }
    }
    return;
}

void bts_start()
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK( ret );

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if ((ret = esp_bt_controller_init(&bt_cfg)) != ESP_OK) {
        return;
    }

    if ((ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT)) != ESP_OK) {
        return;
    }

    if ((ret = esp_bluedroid_init()) != ESP_OK) {
        return;
    }

    if ((ret = esp_bluedroid_enable()) != ESP_OK) {
        return;
    }

    if ((ret = esp_bt_gap_register_callback(esp_bt_gap_cb)) != ESP_OK) {
        return;
    }

    if ((ret = esp_spp_register_callback(esp_spp_cb)) != ESP_OK) {
        return;
    }

    if ((ret = esp_spp_init(esp_spp_mode)) != ESP_OK) {
        return;
    }

    /*
     * Set default parameters for Legacy Pairing
     * Use variable pin, input pin code when pairing
     */

    esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_FIXED;
    esp_bt_gap_set_pin(pin_type, 4, slave->pin_code);

    esp_bt_dev_set_device_name(slave->name);
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    esp_spp_start_srv(sec_mask, role_slave, 0, slave->name);
}

STATIC mp_obj_t bts_init(mp_obj_t name, mp_obj_t pin){
    if (slave_up == true) {
       return mp_const_false;
    }
    char *sn = mp_obj_str_get_str(name);
    char *sp = mp_obj_str_get_str(pin);
    if (slave_storage == false) {
       // create slave object
       slave_obj_t *so = m_new_obj(slave_obj_t);
       memcpy(so->name, sn, strlen(sn));     // slave name
       memcpy(so->pin_code, sp, strlen(sp)); // PIN
       slave = so;
       slave->ready = false;
       // create pipe object
       int size = DEFAULT_PIPE_SIZE;
       pipe_obj_t *po = m_new_obj(pipe_obj_t);
       po->buffer = NULL;
       po->head = 0;
       po->tail = 0;
       po->lock = xSemaphoreCreateMutex();
       char *buff = malloc(sizeof(char) * size);
       if (buff == NULL) {
          po->buffer = NULL;
          po->size = 0;
       } else {
          po->buffer = buff;
          po->size = size+1;
       }
       pipe = po;
       slave_storage = true;  // ready with storage
    } else {
       memcpy(slave->name, sn, strlen(sn));     // slave name
       memcpy(slave->pin_code, sp, strlen(sp)); // PIN
       slave->ready = false;
       slave->handle = NULL;
       pipe->head = 0;
       pipe->tail = 0;
    }   
    bts_start();
    slave_up = true;  // slave is up, can deinit
    return mp_const_true;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(bts_init_obj, bts_init);

STATIC mp_obj_t bts_data() {
    int size = 0;
    if (xSemaphoreTake(pipe->lock, (TickType_t) NON_BLOCKING) == pdTRUE) {
        if (pipe->tail >= pipe->head) {
            size = pipe->tail - pipe->head;
        } else {
            size = pipe->size - pipe->head + pipe->tail;
        }
        xSemaphoreGive(pipe->lock);
    }
    return mp_obj_new_int(size);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(bts_data_obj, bts_data);

STATIC mp_obj_t bts_get_str(const mp_obj_t what) {
    const int count = mp_obj_get_int(what);
    if (count > 0) {
       if (xSemaphoreTake(pipe->lock, (TickType_t) NON_BLOCKING) == pdTRUE) {
          char items[count];
          int i, removed = 0;
          for (i = 0; i < count; i++) {
              if (pipe->head == pipe->tail) {
                  break;
              }
              items[i] = (char) pipe->buffer[pipe->head];
              pipe->head = (pipe->head + 1) % pipe->size;
              removed++;
          }
          xSemaphoreGive(pipe->lock);
          if (removed > 0) {
             mp_obj_t data = mp_obj_new_str(items, removed);
             return data;
          }
       }
    }
    return mp_const_none; // count<=0 or can't get lock or empty pipe
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(bts_get_str_obj, bts_get_str);

STATIC mp_obj_t bts_get_bin(const mp_obj_t what) {
    const int count = mp_obj_get_int(what);
    if (count > 0) {
       if (xSemaphoreTake(pipe->lock, (TickType_t) NON_BLOCKING) == pdTRUE) {
          uint8_t items[count];
          int i, removed = 0;
          for (i = 0; i < count; i++) {
              if (pipe->head == pipe->tail) {
                  break;
              }
              items[i] = (uint8_t) pipe->buffer[pipe->head];
              pipe->head = (pipe->head + 1) % pipe->size;
              removed++;
          }
          xSemaphoreGive(pipe->lock);
          if (removed > 0) {
             mp_obj_t data = mp_obj_new_bytes(items, removed);
             return data;
          }
       }
    }
    return mp_const_none; // count<=0 or can't get lock or empty pipe
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(bts_get_bin_obj, bts_get_bin);


STATIC mp_obj_t bts_send_str(mp_obj_t data) {
    if (slave->ready == true) {
       char *str = mp_obj_str_get_str(data);
       memcpy(spp_data, str, strlen(str));  // convert char to uint8_t
       esp_spp_write(slave->handle, strlen(str), spp_data);
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(bts_send_str_obj, bts_send_str);

STATIC mp_obj_t bts_send_bin(mp_obj_t data) {
    size_t len;
    if (slave->ready == true) {
       char *bin = mp_obj_str_get_data(data, &len);
       memcpy(spp_data, bin, len); // convert char to uint8_t
       esp_spp_write(slave->handle, len, spp_data);
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(bts_send_bin_obj, bts_send_bin);

STATIC mp_obj_t bts_close(){
    if (slave->ready == true) {
       esp_spp_disconnect(slave->handle);
       slave->ready = false;
       slave->handle = NULL;
    }
    return mp_const_true;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(bts_close_obj, bts_close);

STATIC mp_obj_t bts_ready(){
     return mp_obj_new_bool(slave->ready);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(bts_ready_obj, bts_ready);

STATIC mp_obj_t bts_up(){
     return mp_obj_new_bool(slave_up);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(bts_up_obj, bts_up);

STATIC mp_obj_t bts_deinit(){
    if (slave_up == false) {
       return mp_const_false;
    }
    esp_spp_deinit();
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    slave->ready = false;
    slave->handle = NULL;
    slave_up = false;  // can do init
    return mp_const_true;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(bts_deinit_obj, bts_deinit);

STATIC const mp_rom_map_elem_t bts_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_bts) },
    { MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&bts_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_up), MP_ROM_PTR(&bts_up_obj) },
    { MP_ROM_QSTR(MP_QSTR_data), MP_ROM_PTR(&bts_data_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_str), MP_ROM_PTR(&bts_get_str_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_bin), MP_ROM_PTR(&bts_get_bin_obj) },
    { MP_ROM_QSTR(MP_QSTR_send_str), MP_ROM_PTR(&bts_send_str_obj) },
    { MP_ROM_QSTR(MP_QSTR_send_bin), MP_ROM_PTR(&bts_send_bin_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&bts_close_obj) },
    { MP_ROM_QSTR(MP_QSTR_ready), MP_ROM_PTR(&bts_ready_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&bts_deinit_obj) },
};

STATIC MP_DEFINE_CONST_DICT(bts_module_globals, bts_module_globals_table);

const mp_obj_module_t mp_module_bts = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t*)&bts_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_bts, mp_module_bts);


