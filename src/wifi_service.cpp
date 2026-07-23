#include "wifi_service.h"

#ifdef OPENDISPLAY_HAS_WIFI

#include "communication.h"
#include "encryption.h"
#include "structs.h"
#include "ble_init.h"   // NimBLE-Arduino + BLE* aliases (NimBLEDevice for the advertised MAC)
#include <Arduino.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <string.h>

#include "mbedtls/ssl.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl_ciphersuites.h"

#ifndef COMM_MODE_WIFI
#define COMM_MODE_WIFI (1 << 2)
#endif

extern struct GlobalConfig globalConfig;
extern char wifiSsid[33];
extern char wifiPassword[33];
extern uint8_t wifiEncryptionType;
extern bool wifiConfigured;
extern bool wifiConnected;
extern bool wifiInitialized;
extern uint16_t wifiServerPort;
extern WiFiServer wifiServer;
extern WiFiClient wifiClient;
extern bool wifiServerConnected;
extern uint8_t tcpReceiveBuffer[16384];
extern uint32_t tcpReceiveBufferPos;
extern uint8_t msd_payload[16];

// Command origin marker (F4): the shared dispatcher (imageDataWritten) reads this
// to decide whether to run the app-layer AES-CCM gate. Defined in communication.cpp;
// enum CommandOrigin comes from communication.h.
// ORIGIN_LAN_TLS frames are already secured by TLS, so CCM MUST be bypassed.
extern volatile uint8_t g_commandOrigin;

void writeSerial(String message, bool newLine = true);
String getChipIdHex();
uint8_t getFirmwareMajor();
uint8_t getFirmwareMinor();

typedef void* BLEConnHandle;
typedef void* BLECharPtr;
void imageDataWritten(BLEConnHandle conn_hdl, BLECharPtr chr, uint8_t* data, uint16_t len);

// ------------------------------------------------------------------ TLS-PSK ---
// One TLS session at a time, driven cooperatively from handleWiFiServer(). The
// PSK identity string "opendisplay" must match the py-opendisplay client; the
// PSK bytes come from deriveTlsPsk() (AES-CMAC over the master key).
static const char* kTlsPskIdentity = "opendisplay";
static const int kTlsCiphersuites[] = { MBEDTLS_TLS_ECDHE_PSK_WITH_AES_128_CBC_SHA256, 0 };

static bool tlsMode = false;            // true when the ACTIVE channel is TLS-PSK
static bool tlsInited = false;          // mbedTLS config objects built once
static bool tlsSessionActive = false;   // an mbedtls_ssl_context is live for wifiClient
static bool tlsHandshakeDone = false;
static uint8_t tlsPsk[16];

static mbedtls_ssl_context   tlsSsl;
static mbedtls_ssl_config    tlsConf;
static mbedtls_ctr_drbg_context tlsDrbg;
static mbedtls_entropy_context  tlsEntropy;

static uint32_t lastLanActivityMs = 0;  // for the OD_LAN_READ_TIMEOUT_S idle drop

// --------------------------------------------------- transfer power save ---
// Sticky across the whole transfer, unlike g_commandOrigin (per-frame, restored to
// ORIGIN_BLE right after each dispatch). A transfer can be torn down from a loop()
// timeout, disconnect cleanup, or an error path -- none of which know the origin --
// so suspend is origin-gated at START and restore is unconditional everywhere else.
static bool lanPsSuspended = false;

// INTENTIONAL NO-OP -- do NOT re-enable WIFI_PS_NONE here.
//
// This was meant to dodge a DTIM ack-ladder stall by dropping modem sleep for the
// duration of a LAN transfer. On this hardware WiFi and BLE share ONE radio and
// software coexistence is compiled in (CONFIG_SW_COEXIST_ENABLE). The coex arbiter
// relies on WiFi's modem-sleep (WIFI_PS_MIN_MODEM) windows to time-share the antenna
// with the always-on BLE advertiser. WIFI_PS_NONE tells the AP "I never sleep, send
// anytime", which is a lie under coex: BLE still periodically steals the radio, so the
// AP fires downlink into windows where the device is off-channel. Those frames are
// dropped at the PHY -> TCP retransmit + backoff on nearly every packet -> throughput
// collapses to a crawl (measured on hardware, 2026-07-23). It hits WiFi in BOTH
// directions (inbound data AND outbound acks), even though the transfer never touches
// BLE -- BLE only has to EXIST for coex to be active.
//
// The DTIM stall it was chasing was measured negligible, so there is nothing to trade
// off: the radio stays at the default WIFI_PS_MIN_MODEM for the whole session. The
// function and its restore/safety-net machinery are retained as harmless no-ops
// (lanPsSuspended never flips true) so the call sites and hook stay in place.
void lanPowerSaveSuspend(void) {
    // Deliberately does not touch esp_wifi_set_ps(). See the note above.
}

void lanPowerSaveRestore(void) {
    if (!lanPsSuspended) return;
    lanPsSuspended = false;
    // Only touch the radio if it is still associated: after a disconnect the
    // reconnect path re-applies MIN_MODEM itself (and would fail here anyway).
    if (wifiConnected) esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    writeSerial("LAN: power save restored (WIFI_PS_MIN_MODEM)");
}

bool lanPowerSaveSuspended(void) { return lanPsSuspended; }

static uint16_t lanBasePort(void) {
    return (wifiServerPort != 0) ? wifiServerPort : (uint16_t)OD_LAN_TCP_PORT;
}
// Active LAN port: plaintext -> base; TLS -> base+1 (derived, no config field).
uint16_t lanActivePort(void) {
    uint16_t base = lanBasePort();
    return isEncryptionEnabled() ? (uint16_t)(base + 1) : base;
}

bool lanTlsEnabled(void) { return isEncryptionEnabled(); }

// mbedTLS BIO shims over the accepted WiFiClient (non-blocking cooperative model).
static int tls_bio_send(void* ctx, const unsigned char* buf, size_t len) {
    WiFiClient* c = static_cast<WiFiClient*>(ctx);
    if (c == nullptr || !c->connected()) return MBEDTLS_ERR_NET_CONN_RESET;
    int w = c->write(buf, len);
    if (w <= 0) return MBEDTLS_ERR_SSL_WANT_WRITE;
    return w;
}
static int tls_bio_recv(void* ctx, unsigned char* buf, size_t len) {
    WiFiClient* c = static_cast<WiFiClient*>(ctx);
    if (c == nullptr || !c->connected()) return MBEDTLS_ERR_NET_CONN_RESET;
    if (c->available() <= 0) return MBEDTLS_ERR_SSL_WANT_READ;
    int r = c->read(buf, len);
    if (r <= 0) return MBEDTLS_ERR_SSL_WANT_READ;
    return r;
}

// Build the shared server config once (RNG + PSK + one ECDHE-PSK ciphersuite).
static bool tlsEnsureConfig(void) {
    if (tlsInited) return true;
    if (!deriveTlsPsk(tlsPsk)) {
        writeSerial("ERROR: TLS PSK derivation failed (no master key)");
        return false;
    }
    mbedtls_ssl_config_init(&tlsConf);
    mbedtls_ctr_drbg_init(&tlsDrbg);
    mbedtls_entropy_init(&tlsEntropy);
    const char* pers = "opendisplay-tls";
    if (mbedtls_ctr_drbg_seed(&tlsDrbg, mbedtls_entropy_func, &tlsEntropy,
                              reinterpret_cast<const unsigned char*>(pers), strlen(pers)) != 0) {
        writeSerial("ERROR: TLS RNG seed failed");
        return false;
    }
    if (mbedtls_ssl_config_defaults(&tlsConf, MBEDTLS_SSL_IS_SERVER,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        writeSerial("ERROR: TLS config defaults failed");
        return false;
    }
    mbedtls_ssl_conf_rng(&tlsConf, mbedtls_ctr_drbg_random, &tlsDrbg);
    mbedtls_ssl_conf_ciphersuites(&tlsConf, kTlsCiphersuites);
    if (mbedtls_ssl_conf_psk(&tlsConf, tlsPsk, sizeof(tlsPsk),
                             reinterpret_cast<const unsigned char*>(kTlsPskIdentity),
                             strlen(kTlsPskIdentity)) != 0) {
        writeSerial("ERROR: TLS conf_psk failed");
        return false;
    }
    tlsInited = true;
    return true;
}

static void tlsCloseSession(void) {
    if (tlsSessionActive) {
        mbedtls_ssl_close_notify(&tlsSsl);
        mbedtls_ssl_free(&tlsSsl);
    }
    tlsSessionActive = false;
    tlsHandshakeDone = false;
}

static bool tlsBeginSession(void) {
    if (!tlsEnsureConfig()) return false;
    mbedtls_ssl_init(&tlsSsl);
    if (mbedtls_ssl_setup(&tlsSsl, &tlsConf) != 0) {
        writeSerial("ERROR: TLS ssl_setup failed");
        mbedtls_ssl_free(&tlsSsl);
        return false;
    }
    mbedtls_ssl_set_bio(&tlsSsl, &wifiClient, tls_bio_send, tls_bio_recv, nullptr);
    tlsSessionActive = true;
    tlsHandshakeDone = false;
    return true;
}

// Staging buffer so a frame leaves as ONE write. Two writes cost an extra packet
// (and, before setNoDelay(), a 40-200 ms Nagle/delayed-ACK stall on every frame);
// on TLS they also cost a second record header + MAC, which on a 2-byte ACK is more
// overhead than payload. Sized past the largest response the device produces
// (encrypted_response[600] in communication.cpp); anything larger falls back to the
// two-write path rather than growing static RAM for a case that does not occur.
static uint8_t lanTxFrame[2 + 640];

// Write one [len:2 LE][payload] frame over the active LAN channel (TLS or plain).
// Called by communication.cpp for LAN-origin responses (send_tls_lan_frame / plain).
void opendisplay_lan_send_frame(const uint8_t* payload, uint16_t len) {
    if (!wifiServerConnected || !wifiClient.connected() || len == 0) {
        return;
    }
    if (tlsMode && (!tlsSessionActive || !tlsHandshakeDone)) return;

    if ((uint32_t)len + 2u <= sizeof(lanTxFrame)) {
        lanTxFrame[0] = (uint8_t)(len & 0xFF);
        lanTxFrame[1] = (uint8_t)((len >> 8) & 0xFF);
        memcpy(lanTxFrame + 2, payload, len);
        const uint16_t total = (uint16_t)(len + 2u);
        if (tlsMode) {
            if (mbedtls_ssl_write(&tlsSsl, lanTxFrame, total) < 0) {
                writeSerial("ERROR: TLS LAN response write failed", true);
            }
        } else if (wifiClient.write(lanTxFrame, total) != total) {
            writeSerial("ERROR: LAN response write incomplete", true);
        }
        return;
    }

    // Oversized fallback: header then payload. A failure between the two leaves the
    // peer waiting on a length prefix whose payload never arrives.
    uint8_t hdr[2] = { (uint8_t)(len & 0xFF), (uint8_t)((len >> 8) & 0xFF) };
    if (tlsMode) {
        if (mbedtls_ssl_write(&tlsSsl, hdr, 2) < 0 ||
            mbedtls_ssl_write(&tlsSsl, payload, len) < 0) {
            writeSerial("ERROR: TLS LAN response write failed", true);
        }
        return;
    }
    if (wifiClient.write(hdr, 2) != 2 || wifiClient.write(payload, len) != len) {
        writeSerial("ERROR: LAN response write incomplete", true);
    }
}

bool wifiLanClientConnected(void) {
    return wifiServerConnected && wifiClient.connected();
}

static void hex14_lower(const uint8_t* src, char* out29) {
    static const char* h = "0123456789abcdef";
    for (int i = 0; i < 14; i++) {
        out29[i * 2] = h[(src[i] >> 4) & 0x0F];
        out29[i * 2 + 1] = h[src[i] & 0x0F];
    }
    out29[28] = '\0';
}

void opendisplay_mdns_update_msd_txt(void) {
    if (!wifiConnected || WiFi.status() != WL_CONNECTED) {
        return;
    }
    static uint8_t last_msd[14];
    static uint32_t last_ms = 0;
    static bool have_last = false;
    uint8_t cur[14];
    memcpy(cur, &msd_payload[2], sizeof(cur));
    uint32_t now = millis();
    if (have_last && memcmp(cur, last_msd, sizeof(cur)) == 0 && (now - last_ms) < 400) {
        return;
    }
    have_last = true;
    memcpy(last_msd, cur, sizeof(cur));
    last_ms = now;
    char hex[29];
    hex14_lower(cur, hex);
    // const char* overload (void); char* overload (bool) — avoid ambiguous resolution with char hex[].
    MDNS.addServiceTxt("opendisplay", "tcp", "msd", static_cast<const char*>(hex));
}

// The advertised BLE address, lowercase colon-separated (SECTION 9 rule 6, key
// `mac`). This is a genuinely new call: prior identity used getChipIdHex()
// (eFuse), which is NOT what HA stores as the device unique_id. HARDWARE
// VALIDATION REQUIRED: confirm NimBLEDevice::getAddress() == the advertised AdvA
// HA sees (public vs static-random) on BOTH S3 and C6.
static String advertisedBleMacLower(void) {
    auto s = NimBLEDevice::getAddress().toString();
    String out(s.c_str());
    out.toLowerCase();
    return out;
}

static void restartLanService(void) {
    String deviceName = "OD" + getChipIdHex();
    if (!MDNS.begin(deviceName.c_str())) {
        writeSerial("ERROR: mDNS responder failed");
        return;
    }
    uint16_t port = lanActivePort();
    writeSerial("mDNS: " + deviceName + ".local");
    MDNS.addService("opendisplay", "tcp", port);
    // F1 -- identity/capability TXT keys (SECTION 9 rule 6, ADDITIVE to `msd`).
    String mac = advertisedBleMacLower();
    MDNS.addServiceTxt("opendisplay", "tcp", "mac", mac.c_str());        // REQUIRED
    MDNS.addServiceTxt("opendisplay", "tcp", "tls", isEncryptionEnabled() ? "1" : "0"); // REQUIRED
    // const char* overload (value) vs char* overload (key/value) — cast char[] to
    // const char* to disambiguate, matching opendisplay_mdns_update_msd_txt().
    char fw[12];
    snprintf(fw, sizeof(fw), "%u.%u", (unsigned)getFirmwareMajor(), (unsigned)getFirmwareMinor());
    MDNS.addServiceTxt("opendisplay", "tcp", "fw", static_cast<const char*>(fw));  // RECOMMENDED
    char cm[3];
    snprintf(cm, sizeof(cm), "%02x", (unsigned)globalConfig.system_config.communication_modes);
    MDNS.addServiceTxt("opendisplay", "tcp", "cm", static_cast<const char*>(cm));  // RECOMMENDED
    uint8_t did[4];
    getAuthDeviceIdBytes(did);
    char idhex[9];
    snprintf(idhex, sizeof(idhex), "%02x%02x%02x%02x", did[0], did[1], did[2], did[3]);
    MDNS.addServiceTxt("opendisplay", "tcp", "id", static_cast<const char*>(idhex));  // OPTIONAL
    MDNS.addServiceTxt("opendisplay", "tcp", "pv", OD_PROTOCOL_VERSION_STR); // OPTIONAL
    writeSerial("mDNS: _opendisplay._tcp port " + String(port) +
                " tls=" + (isEncryptionEnabled() ? "1" : "0") + " mac=" + mac);
    opendisplay_mdns_update_msd_txt();
}

static void startLanServer(void) {
    tlsMode = isEncryptionEnabled();
    uint16_t port = lanActivePort();
    wifiServer.begin(port);
    writeSerial(String(tlsMode ? "TLS-PSK" : "Plaintext") +
                " LAN server listening on port " + String(port));
    restartLanService();
}

// WiFi.status() collapses every association failure into WL_DISCONNECTED (6), which
// is useless for field diagnosis. Log the 802.11/ESP reason code instead: 201
// (NO_AP_FOUND) means the SSID was never seen -- typically a 5 GHz-only or hidden
// network; 15 (4WAY_HANDSHAKE_TIMEOUT) / 202 (AUTH_FAIL) mean a bad password; 200
// (BEACON_TIMEOUT) / 4 (ASSOC_EXPIRE) point at range or BLE coexistence.
static bool wifiDiagEventsRegistered = false;

static void onWiFiDiagEvent(arduino_event_id_t event, arduino_event_info_t info) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_CONNECTED:
            writeSerial("WiFi event: associated (channel " +
                        String(info.wifi_sta_connected.channel) + ")");
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            writeSerial("WiFi event: disconnected, reason " +
                        String(info.wifi_sta_disconnected.reason));
            break;
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            writeSerial("WiFi event: got IP " + IPAddress(info.got_ip.ip_info.ip.addr).toString());
            break;
        default:
            break;
    }
}

static void registerWiFiDiagEvents(void) {
    if (wifiDiagEventsRegistered) return;
    WiFi.onEvent(onWiFiDiagEvent);
    wifiDiagEventsRegistered = true;
}

void initWiFi(bool waitForConnection) {
    writeSerial("=== Initializing WiFi ===");

    // WiFi is NOT gated on power_mode: if COMM_MODE_WIFI is enabled the radio comes
    // up on battery too. Radio cost on battery is managed by WIFI_PS_MIN_MODEM
    // (below) and by deep sleep, not by refusing to associate.
    if (!(globalConfig.system_config.communication_modes & COMM_MODE_WIFI)) {
        writeSerial("WiFi not enabled in communication_modes, skipping");
        wifiInitialized = false;
        return;
    }
    if (!wifiConfigured) {
        writeSerial("WiFi: system_config has WiFi mode on, but wifi_config TLV (0x26) is not in saved "
                    "configuration (or failed to parse). Enable Wi-Fi in config, set SSID, and write full "
                    "config to the device.");
        wifiInitialized = false;
        return;
    }
    if (wifiSsid[0] == '\0' || strlen(wifiSsid) == 0) {
        writeSerial("WiFi: wifi_config packet present but SSID field is empty.");
        wifiInitialized = false;
        return;
    }
    // Do not log the SSID or password (credentials); log only presence/length.
    writeSerial("WiFi: connecting to configured SSID (len " + String(strlen(wifiSsid)) + ")");
    registerWiFiDiagEvents();
    WiFi.setAutoReconnect(true);
    wifiSsid[32] = '\0';
    wifiPassword[32] = '\0';
    writeSerial("Encryption type: 0x" + String(wifiEncryptionType, HEX));
    wifiConnected = false;
    wifiInitialized = true;
    WiFi.begin(wifiSsid, wifiPassword);
    // Tx power can only be set once the STA is started, i.e. after begin(); the
    // pre-begin() call this replaces failed with ESP_ERR_WIFI_NOT_START.
    WiFi.setTxPower(WIFI_POWER_15dBm);
    if (!waitForConnection) {
        writeSerial("WiFi: STA started (non-blocking; LAN starts when associated)");
        return;
    }
    writeSerial("Waiting for WiFi connection...");
    const int maxRetries = 3;
    const unsigned long timeoutPerRetry = 10000;
    bool connected = false;
    for (int retry = 0; retry < maxRetries && !connected; retry++) {
        unsigned long startAttempt = millis();
        bool abortCurrentRetry = false;
        while (WiFi.status() != WL_CONNECTED && (millis() - startAttempt < timeoutPerRetry)) {
            delay(500);
            wl_status_t status = WiFi.status();
            writeSerial("WiFi status: " + String(status));
            if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) {
                writeSerial("Connection failed immediately (Status: " + String(status) + ")");
                abortCurrentRetry = true;
                break;
            }
        }
        if (WiFi.status() == WL_CONNECTED) {
            connected = true;
            break;
        }
        if (!abortCurrentRetry) {
            writeSerial("WiFi attempt " + String(retry + 1) + " timed out");
        }
        if (retry < maxRetries - 1) {
            delay(2000);
        }
    }
    if (WiFi.status() == WL_CONNECTED) {
        wifiConnected = true;
        writeSerial("=== WiFi connected ===");
        writeSerial("IP: " + WiFi.localIP().toString());
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);  // F5: modem sleep between beacons
        startLanServer();
    } else {
        wifiConnected = false;
        writeSerial("=== WiFi connection failed ===");
    }
}

void disconnectWiFiServer() {
    tlsCloseSession();
    if (wifiClient.connected()) {
        writeSerial("Closing LAN client");
        clearEncryptionSession();
        wifiClient.stop();
    }
    wifiServerConnected = false;
    tcpReceiveBufferPos = 0;
    // The client is gone, so any transfer it owned is dead. Restore here rather than
    // relying on the deferred cleanup below: serviceBleDisconnectCleanup() skips
    // teardown while an EPD refresh is in flight or when the other transport still
    // owns the session, either of which would leave the radio stuck at full power.
    lanPowerSaveRestore();
    // F4: abort any in-flight direct-write / pipe / partial transfer + tear down a
    // mid-transfer panel session, DEFERRED to loop() (serviceBleDisconnectCleanup)
    // so cleanup never races an in-progress EPD refresh. Reuses the BLE path's flag.
    bleDisconnectCleanupPending = true;
}

// Drain readable bytes from the active channel into tcpReceiveBuffer. Returns the
// number of bytes appended (>=0), or -1 on a fatal channel error (caller drops).
static int lanReadIntoBuffer(void) {
    int space = (int)sizeof(tcpReceiveBuffer) - (int)tcpReceiveBufferPos;
    if (space <= 0) {
        writeSerial("LAN RX buffer full, dropping connection");
        return -1;
    }
    if (tlsMode) {
        int r = mbedtls_ssl_read(&tlsSsl, &tcpReceiveBuffer[tcpReceiveBufferPos], (size_t)space);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) return 0;
        if (r <= 0) return -1;   // close-notify, reset, or fatal
        tcpReceiveBufferPos += (uint32_t)r;
        return r;
    }
    int available = wifiClient.available();
    if (available <= 0) return 0;
    int bytesToRead = (available > space) ? space : available;
    int bytesRead = wifiClient.read(&tcpReceiveBuffer[tcpReceiveBufferPos], bytesToRead);
    if (bytesRead > 0) tcpReceiveBufferPos += (uint32_t)bytesRead;
    return (bytesRead > 0) ? bytesRead : 0;
}

void handleWiFiServer() {
    if (wifiInitialized && WiFi.status() == WL_CONNECTED && !wifiConnected) {
        wifiConnected = true;
        writeSerial("=== WiFi connected ===");
        writeSerial("IP: " + WiFi.localIP().toString());
        // Re-association: any transfer that suspended power save died with the old
        // link, so clear the flag here rather than leaving it stuck across the
        // reconnect (the teardown funnels may not run if the client vanished).
        lanPowerSaveRestore();
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        startLanServer();
    }
    if (!wifiConnected || WiFi.status() != WL_CONNECTED) {
        if (wifiServerConnected || wifiClient.connected()) {
            writeSerial("WiFi lost, closing LAN session");
            disconnectWiFiServer();
        }
        return;
    }

    WiFiClient incoming = wifiServer.accept();
    if (incoming) {
        if (wifiClient.connected()) {
            writeSerial("LAN: new client, replacing previous");
            tlsCloseSession();
            clearEncryptionSession();
            wifiClient.stop();
        }
        wifiClient = incoming;
        // TCP_NODELAY: every LAN write is a complete, self-delimited frame, so there
        // is never a following write for Nagle to coalesce it with -- it can only
        // hold a small frame until the peer's delayed ACK fires (40-200 ms). With
        // per-chunk direct-write ACKs that lands on every frame of a transfer.
        wifiClient.setNoDelay(true);
        wifiClient.setTimeout(30000);
        tcpReceiveBufferPos = 0;
        wifiServerConnected = true;
        lastLanActivityMs = millis();
        writeSerial("LAN client connected from " + wifiClient.remoteIP().toString());
        if (tlsMode) {
            if (!tlsBeginSession()) {
                writeSerial("LAN: TLS session start failed, dropping");
                disconnectWiFiServer();
                return;
            }
        }
    }

    if (!wifiServerConnected || !wifiClient.connected()) {
        if (wifiServerConnected) {
            writeSerial("LAN client disconnected");
            disconnectWiFiServer();
        }
        return;
    }

    // Drive the TLS handshake incrementally; return until it completes.
    if (tlsMode && tlsSessionActive && !tlsHandshakeDone) {
        int hs = mbedtls_ssl_handshake(&tlsSsl);
        if (hs == 0) {
            tlsHandshakeDone = true;
            lastLanActivityMs = millis();
            writeSerial("LAN: TLS handshake complete");
        } else if (hs == MBEDTLS_ERR_SSL_WANT_READ || hs == MBEDTLS_ERR_SSL_WANT_WRITE) {
            // still handshaking; but honor the idle timeout below
        } else {
            writeSerial("LAN: TLS handshake failed (" + String(hs) + "), dropping");
            disconnectWiFiServer();
            return;
        }
    }

    // Bounded drain: keep reading and dispatching until the channel is dry or a
    // per-tick byte budget (one full tcpReceiveBuffer) is spent. Parsing INSIDE
    // the loop frees buffer space before the next read -- essential on TLS, where
    // one mbedtls_ssl_read can return a whole max-size record. The budget bounds
    // LAN's hold on loop() so BLE drain, buttons/touch/buzzer, and the deep-sleep
    // gate still run under a saturating streaming client.
    uint32_t drainedBytes = 0;
    int got;
    do {
        got = lanReadIntoBuffer();
        if (got < 0) {
            writeSerial("LAN: channel read error, dropping");
            disconnectWiFiServer();
            return;
        }
        if (got > 0) {
            lastLanActivityMs = millis();
            drainedBytes += (uint32_t)got;
        } else if (drainedBytes == 0) {
            // No traffic at all this tick: drop only after OD_LAN_READ_TIMEOUT_S
            // of silence (persistent client is otherwise kept). Any valid frame
            // below resets the timer.
            if ((millis() - lastLanActivityMs) > (uint32_t)OD_LAN_READ_TIMEOUT_S * 1000UL) {
                writeSerial("LAN: idle timeout, dropping client");
                disconnectWiFiServer();
            }
            return;
        }

        while (tcpReceiveBufferPos >= 2) {
            uint16_t flen = (uint16_t)(tcpReceiveBuffer[0] | (tcpReceiveBuffer[1] << 8));
            if (flen == 0 || flen > OD_LAN_MAX_PAYLOAD) {
                writeSerial("LAN: invalid frame length, closing");
                disconnectWiFiServer();
                return;
            }
            if (tcpReceiveBufferPos < (uint32_t)(2 + flen)) {
                break;
            }
            // F4: tag the frame's origin so the dispatcher bypasses app-layer CCM on
            // TLS (already-secure) and routes the response back over LAN only.
            g_commandOrigin = tlsMode ? ORIGIN_LAN_TLS : ORIGIN_LAN_PLAIN;
            lastLanActivityMs = millis();
            imageDataWritten(NULL, NULL, tcpReceiveBuffer + 2, flen);
            g_commandOrigin = ORIGIN_BLE;   // restore default for any subsequent BLE drain
            uint32_t consumed = 2u + (uint32_t)flen;
            uint32_t rem = tcpReceiveBufferPos - consumed;
            if (rem > 0) {
                memmove(tcpReceiveBuffer, tcpReceiveBuffer + consumed, rem);
            }
            tcpReceiveBufferPos = rem;
        }
        // A dispatched command may have torn the session down (reboot, power-off,
        // config-driven LAN restart). Never read from a dead client.
        if (!wifiServerConnected || !wifiClient.connected()) {
            return;
        }
    } while (got > 0 && drainedBytes < sizeof(tcpReceiveBuffer));
}

void restartWiFiLanAfterReconnect() {
    if (!wifiConnected || WiFi.status() != WL_CONNECTED) {
        return;
    }
    disconnectWiFiServer();
    startLanServer();
}

// F5 -- reboot teardown: drop the LAN client + TLS context, stop the TCP server,
// and disconnect WiFi so esp_restart() does not leave the radio/socket half-up.
// Extends PR #114's BLE-only teardown (device_control.cpp) to the WiFi surface.
void opendisplay_lan_teardown(void) {
    tlsCloseSession();
    if (wifiClient.connected()) {
        wifiClient.stop();
    }
    wifiServer.end();
    wifiServerConnected = false;
    tcpReceiveBufferPos = 0;
    if (tlsInited) {
        mbedtls_ssl_config_free(&tlsConf);
        mbedtls_ctr_drbg_free(&tlsDrbg);
        mbedtls_entropy_free(&tlsEntropy);
        tlsInited = false;
    }
    WiFi.disconnect(true);
    writeSerial("LAN/WiFi torn down before restart", true);
}

#endif  // OPENDISPLAY_HAS_WIFI
