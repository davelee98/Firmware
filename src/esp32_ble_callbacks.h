#ifndef ESP32_BLE_CALLBACKS_H
#define ESP32_BLE_CALLBACKS_H

#ifdef TARGET_ESP32

#include <Arduino.h>
#include <BLECharacteristic.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <string.h>
#include "ble_init.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifndef COMMAND_QUEUE_SIZE
#define COMMAND_QUEUE_SIZE 16
#endif
#ifndef MAX_COMMAND_SIZE
#define MAX_COMMAND_SIZE 512
#endif
#ifndef CMD_QUEUE_PUSH_TIMEOUT_MS
#define CMD_QUEUE_PUSH_TIMEOUT_MS 100
#endif

struct CommandQueueItem {
    uint8_t data[MAX_COMMAND_SIZE];
    uint16_t len;
    bool pending;
};

extern QueueHandle_t cmdQueue;
static inline bool cmdQueuePush(const uint8_t* data, uint16_t len) {
    if (!cmdQueue || len == 0 || len > MAX_COMMAND_SIZE) return false;
    CommandQueueItem item;
    memcpy(item.data, data, len);
    item.len = len;
    item.pending = true;  // vestigial, kept for struct compatibility
    return xQueueSend(cmdQueue, &item, pdMS_TO_TICKS(CMD_QUEUE_PUSH_TIMEOUT_MS)) == pdTRUE;
}
static inline bool cmdQueuePop(CommandQueueItem* out) {
    return cmdQueue && xQueueReceive(cmdQueue, out, 0) == pdTRUE;
}
static inline bool cmdQueueHasItems() {
    return cmdQueue && uxQueueMessagesWaiting(cmdQueue) > 0;
}
extern uint8_t rebootFlag;
extern volatile bool esp32BleNotifySubscribed;

void updatemsdata();
void cleanupDirectWriteState(bool refreshDisplay);
void touchResumeAfterEpdRefresh(void);
extern volatile bool epdRefreshInProgress;
extern bool directWriteActive;

class MyBLEServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
        (void)pServer;
        writeSerial("=== BLE CLIENT CONNECTED (ESP32) ===");
        rebootFlag = 0;
        esp32BleNotifySubscribed = false;
        updatemsdata();
    }
    void onDisconnect(BLEServer* pServer) {
        (void)pServer;
        writeSerial("=== BLE CLIENT DISCONNECTED (ESP32) ===");
        esp32BleNotifySubscribed = false;
        if (epdRefreshInProgress) {
            writeSerial("EPD refresh in progress — deferring cleanup/advertising to main loop");
        } else if (directWriteActive) {
            cleanupDirectWriteState(true);
        }
        bleRestartAdvertisingPending = true;
    }
};

class MyBLECharacteristicCallbacks : public BLECharacteristicCallbacks {
public:
#if defined(CONFIG_NIMBLE_ENABLED)
    void onSubscribe(BLECharacteristic* pCharacteristic, ble_gap_conn_desc* desc, uint16_t subValue) {
        (void)pCharacteristic;
        (void)desc;
        esp32BleNotifySubscribed = (subValue & 0x0001) != 0;
        writeSerial("BLE notify subscription: " + String(esp32BleNotifySubscribed ? "enabled" : "disabled"));
    }
#endif
    void onWrite(BLECharacteristic* pCharacteristic) {
        writeSerial("=== BLE WRITE RECEIVED (ESP32) ===");
        String value = pCharacteristic->getValue();
        writeSerial("Received data length: " + String(value.length()) + " bytes");
        if (value.length() > 0 && value.length() <= MAX_COMMAND_SIZE) {
            uint8_t* data = (uint8_t*)value.c_str();
            uint16_t len = value.length();
            String hexDump = "Data: ";
            for (int i = 0; i < len && i < 16; i++) {
                if (data[i] < 16) hexDump += "0";
                hexDump += String(data[i], HEX) + " ";
            }
            writeSerial(hexDump);
            if (cmdQueuePush(data, len))
                writeSerial("ESP32: Command queued for processing");
            else
                writeSerial("ERROR: Command queue full, dropping command");
        } else if (value.length() > MAX_COMMAND_SIZE) {
            writeSerial("WARNING: Command too large, dropping");
        } else {
            writeSerial("WARNING: Empty data received");
        }
    }
};

#endif

#endif
