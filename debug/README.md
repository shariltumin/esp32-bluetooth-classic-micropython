
This is a debug version of the firmware. Verbose logging is enabled. This increases the size of the firmware. The sdkconfig.board is shown below:

```
# DEBUGGING
CONFIG_LOG_DEFAULT_LEVEL_VERBOSE=y

# Override some defaults so BT stack is enabled
CONFIG_BT_ENABLED=y
# CONFIG_BTDM_CONTROLLER_MODE_BLE_ONLY=
CONFIG_BTDM_CONTROLLER_MODE_BR_EDR_ONLY=y
# CONFIG_BTDM_CONTROLLER_MODE_BTDM=
CONFIG_CLASSIC_BT_ENABLED=y
CONFIG_BT_SPP_ENABLED=y
# disable Secure Simple Pairing
CONFIG_BT_SSP_ENABLED=
# Support ONE connection at a time
# don't work --> CONFIG_BTDM_CTRL_BR_EDR_MAX_ACL_CONN=1
# need to change esp_bt_gap_set_scan_mode to
# ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE
# and WiFi is disabled
CONFIG_WIFI_ENABLED=n
```

Logging is very verbose. Every event is reported to the console. To disable disable logging in REPL, do this

``` 
import esp
esp.osdebug(None)

```

For production use, please use the prod version of the firmware as it is smaller in size.

This is a "hobby" grade software. It is not guaranteed to work or even be useful in any way. The use of the firmware is entirely at the user's own risk.

