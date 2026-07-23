#ifndef COMMUNICATION_H
#define COMMUNICATION_H

#include <stdint.h>

void sendResponseUnencrypted(uint8_t* response, uint16_t len);
void sendResponse(uint8_t* response, uint16_t len);
uint16_t calculateCRC16CCITT(uint8_t* data, uint32_t len);
uint8_t getFirmwareMajor();
uint8_t getFirmwareMinor();
const char* getFirmwareShaString();
void handleFirmwareVersion();
void handleReadMSD();
void handleReadConfig();
void handleWriteConfig(uint8_t* data, uint16_t len);
void handleWriteConfigChunk(uint8_t* data, uint16_t len);

// Origin of the command currently being dispatched: 0 = BLE, 1 = LAN plaintext,
// 2 = LAN TLS. Set by the LAN listener around each dispatch and ORIGIN_BLE at all
// other times. Multi-frame transfers use it to reject frames from a transport that
// does not own the in-flight session.
uint8_t commandOrigin(void);

#endif
