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

   This is client, master, or central of Bluetooth Classic using SPP

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

// -define TAG "SPP_CLIENT"

#define NON_BLOCKING 0
#define DEFAULT_PIPE_SIZE 1024

typedef struct _pipe_obj_t {
    char *buffer;
    int head;
    int tail;
    int size;
    SemaphoreHandle_t lock;
} pipe_obj_t;

pipe_obj_t *pipe; /* will get value at btm.init() */

static const esp_spp_mode_t esp_spp_mode = ESP_SPP_MODE_CB;
static const esp_spp_sec_t sec_mask = ESP_SPP_SEC_AUTHENTICATE;
static const esp_spp_role_t role_master = ESP_SPP_ROLE_MASTER;
static const esp_bt_inq_mode_t inq_mode = ESP_BT_INQ_MODE_GENERAL_INQUIRY;
static const uint8_t inq_len = 30;
static const uint8_t inq_num_rsps = 0;

// use to get slave after discovery
static char slave_device_name[ESP_BT_GAP_MAX_BDNAME_LEN + 1];
static uint8_t slave_device_name_len;  

// use for data in
static uint8_t spp_data[ESP_SPP_MAX_MTU]; /* ESP_SPP_MAX_MTU = 990 bytes */
// static char msg_in[ESP_SPP_MAX_MTU];

typedef struct _master_obj_t {
   char name[ESP_BT_GAP_MAX_BDNAME_LEN + 1];
   char slave_name[ESP_BT_GAP_MAX_BDNAME_LEN + 1];
   uint8_t slave_name_len;
   uint8_t slave_pin_code[17];
   esp_bd_addr_t slave_addr;
   bool ready;
   uint32_t handle; /* current write handle */
   uint32_t c_handle; /* connection handle */
} master_obj_t;

master_obj_t *master; /* will get value at btm.init() */

static bool master_storage = false; /* master and pipe storage allocation flag */

static bool master_up = false;   /* master not up, can do init */
static bool master_auth = false; /* master not authenticated */

static bool get_name_from_eir(uint8_t *eir, char *bdname, uint8_t *bdname_len)
{
    uint8_t *rmt_bdname = NULL;
    uint8_t rmt_bdname_len = 0;

    if (!eir) {
        return false;
    }

    rmt_bdname = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &rmt_bdname_len);
    if (!rmt_bdname) {
        rmt_bdname = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &rmt_bdname_len);
    }

    if (rmt_bdname) {
        if (rmt_bdname_len > ESP_BT_GAP_MAX_BDNAME_LEN) {
            rmt_bdname_len = ESP_BT_GAP_MAX_BDNAME_LEN;
        }

        if (bdname) {
            memcpy(bdname, rmt_bdname, rmt_bdname_len);
            bdname[rmt_bdname_len] = '\0';
        }
        if (bdname_len) {
            *bdname_len = rmt_bdname_len;
        }
        return true;
    }

    return false;
}

static void esp_spp_cb(esp_spp_cb_event_t event, esp_spp_cb_param_t *param)
{
    uint8_t *items;
    int count;

    switch (event) {
    case ESP_SPP_INIT_EVT:
        break;
    case ESP_SPP_DISCOVERY_COMP_EVT:
        if (param->disc_comp.status == ESP_SPP_SUCCESS) {
            esp_spp_connect(sec_mask, role_master, param->disc_comp.scn[0], master->slave_addr);
        }
        break;
    case ESP_SPP_OPEN_EVT:
        master->handle = param->srv_open.handle;
        master->c_handle = param->srv_open.handle;
        master->ready = true;
        break;
    case ESP_SPP_CLOSE_EVT:
        master->ready = false;
        master->handle = NULL;
        master->c_handle = NULL;
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
           }
           xSemaphoreGive(pipe->lock);
        }
        master->handle = param->data_ind.handle;
        break;
    case ESP_SPP_CONG_EVT:
        master->handle = param->cong.handle;
        break;
    case ESP_SPP_WRITE_EVT:
        master->handle = param->write.handle;
        break;
    case ESP_SPP_SRV_OPEN_EVT:
        break;
    default:
        break;
    }
}

static void esp_bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch(event){
    case ESP_BT_GAP_DISC_RES_EVT:
        for (int i = 0; i < param->disc_res.num_prop; i++){
            if (param->disc_res.prop[i].type == ESP_BT_GAP_DEV_PROP_EIR
                && get_name_from_eir(param->disc_res.prop[i].val, slave_device_name, &slave_device_name_len)){
                if (strlen(slave_device_name) == master->slave_name_len
                    && strncmp(master->slave_name, slave_device_name, master->slave_name_len) == 0) {
                    memcpy(master->slave_addr, param->disc_res.bda, ESP_BD_ADDR_LEN);
                    esp_spp_start_discovery(master->slave_addr);
                    esp_bt_gap_cancel_discovery();
                }
            }
        }
        break;
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        break;
    case ESP_BT_GAP_RMT_SRVCS_EVT:
        break;
    case ESP_BT_GAP_RMT_SRVC_REC_EVT:
        break;
    case ESP_BT_GAP_AUTH_CMPL_EVT:{
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            master_auth = true;
        } else {
            master_auth = false;
        }
        break;
    }
    case ESP_BT_GAP_PIN_REQ_EVT:{
        if (param->pin_req.min_16_digit) {
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 16, master->slave_pin_code);
        } else {
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, master->slave_pin_code);
        }
        break;
    }

    default:
        break;
    }
}

void btm_start()
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

    // set others
    esp_bt_dev_set_device_name(master->name);
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    return;
}

STATIC mp_obj_t btm_init(mp_obj_t name){
    if (master_up == true) {
       return mp_const_false;
    }
    char *mn = mp_obj_str_get_str(name);
    if (master_storage == false) {
       // create master object
       master_obj_t *mo = m_new_obj(master_obj_t);
       memcpy(mo->name, mn, strlen(mn));  // master name
       master = mo;
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
       master_storage = true;  // ready with storage
    } else {
       memcpy(master->name, mn, strlen(mn));  // master name
       master->ready = false;
       master->handle = NULL;
       master->c_handle = NULL;
       pipe->head = 0;
       pipe->tail = 0;
    }
    btm_start();
    master_up = true;  // master is up, can deinit
    return mp_const_true;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(btm_init_obj, btm_init);

STATIC mp_obj_t btm_data() {
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
STATIC MP_DEFINE_CONST_FUN_OBJ_0(btm_data_obj, btm_data);

STATIC mp_obj_t btm_get_str(const mp_obj_t what) {
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
STATIC MP_DEFINE_CONST_FUN_OBJ_1(btm_get_str_obj, btm_get_str);

STATIC mp_obj_t btm_get_bin(const mp_obj_t what) {
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
STATIC MP_DEFINE_CONST_FUN_OBJ_1(btm_get_bin_obj, btm_get_bin);

STATIC mp_obj_t btm_send_str(mp_obj_t data) {
    if (master->ready == true) {
       char *str = mp_obj_str_get_str(data);
       memcpy(spp_data, str, strlen(str));  // convert char to uint8_t
       esp_spp_write(master->handle, strlen(str), spp_data);
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(btm_send_str_obj, btm_send_str);

STATIC mp_obj_t btm_send_bin(mp_obj_t data) {
    size_t len;
    if (master->ready == true) {
       char *bin = mp_obj_str_get_data(data, &len);
       memcpy(spp_data, bin, len); // convert char to uint8_t
       esp_spp_write(master->handle, len, spp_data);
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(btm_send_bin_obj, btm_send_bin);

STATIC mp_obj_t btm_open(mp_obj_t name, mp_obj_t pin) {
    char *sn = mp_obj_str_get_str(name);
    char *sp = mp_obj_str_get_str(pin);
    memcpy(master->slave_name, sn, strlen(sn));     // slave name
    master->slave_name_len = strlen(sn);            // slave name length
    memcpy(master->slave_pin_code, sp, strlen(sp)); // binding PIN
    esp_bt_gap_start_discovery(inq_mode, inq_len, inq_num_rsps);
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(btm_open_obj, btm_open);

STATIC mp_obj_t btm_close(){
    if (master->ready == true) {
       esp_spp_disconnect(master->c_handle);
       master->ready = false;
       master->handle = NULL;
       master->c_handle = NULL;
    }
    return mp_const_true;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(btm_close_obj, btm_close);

STATIC mp_obj_t btm_ready(){
     return mp_obj_new_bool(master->ready);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(btm_ready_obj, btm_ready);

STATIC mp_obj_t btm_up(){
     return mp_obj_new_bool(master_up);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(btm_up_obj, btm_up);

STATIC mp_obj_t btm_deinit(){
    if (master_up == false) {
       return mp_const_false;
    }
    esp_spp_deinit();
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    master->ready = false;
    master->handle = NULL;
    master->c_handle = NULL;
    master_up = false;  // can do init
    return mp_const_true;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(btm_deinit_obj, btm_deinit);

STATIC const mp_rom_map_elem_t btm_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_btm) },
    { MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&btm_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_up), MP_ROM_PTR(&btm_up_obj) },
    { MP_ROM_QSTR(MP_QSTR_data), MP_ROM_PTR(&btm_data_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_str), MP_ROM_PTR(&btm_get_str_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_bin), MP_ROM_PTR(&btm_get_bin_obj) },
    { MP_ROM_QSTR(MP_QSTR_send_str), MP_ROM_PTR(&btm_send_str_obj) },
    { MP_ROM_QSTR(MP_QSTR_send_bin), MP_ROM_PTR(&btm_send_bin_obj) },
    { MP_ROM_QSTR(MP_QSTR_open), MP_ROM_PTR(&btm_open_obj) },
    { MP_ROM_QSTR(MP_QSTR_ready), MP_ROM_PTR(&btm_ready_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&btm_close_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&btm_deinit_obj) },
};

STATIC MP_DEFINE_CONST_DICT(btm_module_globals, btm_module_globals_table);

const mp_obj_module_t mp_module_btm = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t*)&btm_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_btm, mp_module_btm);

