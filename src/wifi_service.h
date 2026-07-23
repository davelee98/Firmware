#ifndef WIFI_SERVICE_H
#define WIFI_SERVICE_H

#include <stdint.h>

// OPENDISPLAY_HAS_WIFI gates the entire WiFi/LAN transport surface (mDNS, TCP
// server, TLS-PSK listener, RX reassembly buffer, LAN response framing). It is
// defined only on ESP32 targets built with -DOPENDISPLAY_ENABLE_WIFI, which is
// applied to the S3, C6, and C3 platformio envs. The classic esp32-N4 builds
// without it, so it does not compile the WiFi surface and reclaims the 8 KB RX
// buffer + WiFiServer/WiFiClient RAM. Call sites in main.cpp /
// communication.cpp / display_service.cpp are #ifdef-guarded on this macro.
#if defined(TARGET_ESP32) && defined(OPENDISPLAY_ENABLE_WIFI)
#define OPENDISPLAY_HAS_WIFI 1
#endif

#ifdef OPENDISPLAY_HAS_WIFI

void initWiFi(bool waitForConnection = true);
void disconnectWiFiServer();
void handleWiFiServer();
void restartWiFiLanAfterReconnect();
/// Publish MSD (bytes after company ID) as mDNS TXT key ``msd`` (28 hex chars). No-op if Wi-Fi down.
void opendisplay_mdns_update_msd_txt(void);
/// Tear down the LAN session, TLS context, TCP server, and WiFi before a reboot.
void opendisplay_lan_teardown(void);
/// Frame [len:2 LE][payload] and write it over the ACTIVE LAN channel (TLS or
/// plaintext). Used by communication.cpp to route LAN-origin responses.
void opendisplay_lan_send_frame(const uint8_t* payload, uint16_t len);
/// True while a LAN client is connected (plaintext or TLS).
bool wifiLanClientConnected(void);
/// Port the LAN listener binds: WifiConfig.server_port (or OD_LAN_TCP_PORT when 0),
/// +1 when the TLS-PSK channel is active. Derived, never configured directly.
uint16_t lanActivePort(void);
/// True when the LAN channel is TLS-PSK rather than plaintext (= isEncryptionEnabled()).
bool lanTlsEnabled(void);

#endif

#endif
