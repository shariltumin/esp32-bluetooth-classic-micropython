# BLUETOOTH CLASSIC

For some time now, vanilla MicroPython firmware for the ESP32 family of boards has provided us with Bluetooth Low Energy (BLE). I have yet to come across MicroPython firmware that supports Bluetooth Classic.

I require RFCOMM and SPP Bluetooth support for my Bluetooth remote control robot car. This resulted in the development of MicroPython firmware for the ESP32. 

I am aware of several external Bluetooth modules that connect to the microcontroller UART, such as the JDY-31 and HC-05. I have several of these modules, and they all work fine. 

However, Bluetooth Classic support for ESP32 in MicroPython would be useful.
Apart from reducing the hardware count by one, we can have full Bluetooth control in our MicroPython programs. We can avoid configuring a Bluetooth device using AT commands and save two GPIO pins. 

Bluetooth Classic can only be provided by the ESP32. Other variants, such as the ESP32-C3, ESP32-S2, and ESP32-S3, only support Bluetooth Low Energy. 

This custom firmware allows an ESP32 board to function as either a Bluetooth Slave or a Bluetooth Master device. A slave acts as a server, waiting for connections, whereas a master acts as a client, initiating connections. 

The firmware only allows one connection at a time, whether as a slave or master device. This simplifies the implementation and provides us with a more user-friendly interface. Because Bluetooth Classic requires a lot of resources, the firmware does not support WIFI. Adding WIFI, on the other hand, will result in a build error. 

| Master             | Slave                    | Function                                 |
|:-------------------|:-------------------------|:----------------------------------------|
| import btm         | import bts               | **btm** for Master. **bts** for Slave device.   |
| btm.init("MTR-1")  |                          | Set up a master device. Set device name |
|                    |                          | as "MTR-1".                             |
|                    | bts.init("SLV-1", "2761")| Set up a slave device. Set device name  |
|                    |                          | as "SLV-1" and pairing PIN as "2761".   |
| btm.up()           | bts.up()                 | Initialization is successful if True.   |
|                    |                          | False if Bluetooth is not ready.        |
| btm.open("SLV-1", "2761") |                   | Master connecting to salve, "SLV-1" using |
|                    |                          | "2761" and pairing PIN.                 |
| btm.ready()        | bts.ready()              | Device is ready to send data across a   |
|                    |                          | connection if True.                     |
| btm.send_str("Hei")| bts.send_str("Hei")      | Send a string message to the recipient. |
|                    |                          | The maximum character count is 990.     |
| btm.send_bin(b'ok')| bts.send_bin(b'ok')      | Send a bytearray data to the recipient. |
|                    |                          | The maximum byte count is 990.          |
| btm.data()         | bts.data()               | Return the amount of data in the buffer.|
|                    |                          | 0 if no data else n <= 1024.            |
| w=btm.get_str(100) | w=bts.get_str(100)       | Read at most 100 bytes of data as string|
|                    |                          | from the buffer.
|                    |                          | The parameter n is 0 < n < 1024         | 
|                    |                          | btx.get_str(btx.data()) will read all   |
|                    |                          | from the buffer.
| w=btm.get_bin(103) | w=bts.get_bin(103)       | Read at most 103 bytes of data as bytes |
|                    |                          | from the buffer.
|                    |                          | The same as for string read. If btx.data()|
|                    |                          | is 200 and n is 50 then 50 bytes is read. |
|                    |                          | Next btx.data() will give 150.
| btm.close()        | bts.close()              | Close the current connection. Either master||                    |                          | or slave can initiate close. btx.ready()
|                    |                          | will return False after close.
| btm.deinit()       | bts.deinit()             | Take down and disable Bluetooth. 
|                    |                          | Not necessary under normal running, might |
|                    |                          | be useful before deep-sleep.              |


The firmware disables Secure Simple Pairing (SSP). To connect, the master must enter a valid 4-digit PIN. When a slave is connected to a master, it stops listening for 'discover' packets. When the connection is terminated, the slave will reconfigure itself to listen for any 'discover' packets. A new connection with the slave can be established with a valid PIN provided by the master. 

We can test the functionality of the Bluetooth Serial Port Profile (SPP) on two ESP32 boards.
One will serve as a slave device, while the other will serve as the master. To use it as a slave, start the first board and 'import bts'. Start the second board and then 'import btm'. First, initialize the slave. After that, initialize the master and connect to the slave with 'bt,.open()'. When the devices are ready, try sending and receiving messages. 

We can test an SPP slave on an ESP32 board and a master from a smart phone. Boot the ESP32 and initialize it as a slave. Enable Bluetooth on the phone and try to pair it with the "SLV-1" if that is the name given to the ESP32. Start an SPP application on the smart phone and try to send and receive messages.

We can also try ESP32 as a master and connect it to a JDY-31 or HC-05. Due to some problems in the in the Bluetooth stack library, we will get a lot of warning messages. We cannot do anything about it. Since these warnings come from a binary blob of the Bluetooth stack library, there is no way to disable them. This will clutter our REPL with warnings and make it useless for interactive testing.

The input data buffer is implemented as a ring buffer. If the data is received too fast and the buffer is full, incoming data is simply ignored. There is no provision for traffic congestion control.

The ring buffer used by the Bluetooth module is protected by a lock. Since Bluetooth Classic is implemented as an event-driven system using callback, this lock is necessary. If the 'data-in' event callback tries to acquire the lock but fails, the data will be lost.

Naturally, this firmware was not built with network and socket. The uasyncio was not included as a frozen modules. For preemptive multitasking we can use _thread module. For cooperative multitasking we can use worker module ( see - https://github.com/shariltumin/workers-framework-micropython). 

As mentioned earlier, this firmware is the result of a need for a Bluetooth Classic slave device on an ESP32 board for a robot car that can be remotely controlled by a smartphone. As such, the firmware serves its purpose. 

I provide two versions of custom MicroPython. The firmware under **prod** is for normal use. The one under **debug** is a firmware compiled with verbose logging enabled. The debug version is for those who want to know more about how the event-driven Bluetooth driver works. The debug firmware version is 1513216 bytes long, while the prod version is 1333696 bytes long.

I hope some of you will find it useful. Good luck.

At the monthly [Melbourne meeting](https://www.youtube.com/watch?v=nThCxRihyes), I received an honorary mention from the core MicroPython developers. Thanks!

