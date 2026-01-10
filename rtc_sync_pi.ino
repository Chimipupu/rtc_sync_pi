/**
 * @file rtc_sync_pi.ino.c
 * @author Chimipupu(https://github.com/Chimipupu)
 * @brief NTPとRTCの時刻同期治具F/W(For Raspberry Pi Pico W / Pico 2 W)
 * @version 0.1
 * @date 2026-01-11
 * @note RTC: DS3231, EEPROM: 24C32
 * @note NTPの時刻はUTCで取得、RTCにはJST(UTC+9)に変換
 * @copyright Copyright (c) 2025 Chimipupu All Rights Reserved.
 * 
 */

// C/C++の標準ライブラリ
#include <stdint.h>

// Arduino IDEの標準ライブラリ
#include <Wire.h>
#include <WiFi.h>
#include <WiFiUdp.h>

// 追加Arduinoライブラリ
#include <RTClib.h> // AdafruitのRTClibライブラリ(要インストール！)

//---------------------------------------------------------------------------
// [定数]
#define E2P_I2C_ADDR                 0x57 // 24C32 I2Cアドレス 0x57
#define E2P_PAGE_BYTE_SIZE           32   // 24C32 ページサイズ 32バイト
#define E2P_WIFI_CONFIG_MAKER_LEN    30   // WiFi設定保存用マーカーの文字列長
#define NTP_PACKET_SIZE              48   // NTPパケットサイズ
#define ENTER_REQUEST_PARAM_CNT      2    // シリアル入力WiFi設定パラメータ数
//---------------------------------------------------------------------------
// [グローバル変数]
// const char *p_ntp_primary_server = "pool.ntp.org";
// const char *p_ntp_primary_server = "jp.pool.ntp.org";
const char *p_ntp_primary_server = "ntp.nict.jp";
const char *p_ntp_secondary_server = "ntp.jst.mfeed.ad.jp";
const char *p_e2p_wifi_config_maker = "WIFI_CONFIG_SSID_AND_PASSWORD";
//---------------------------------------------------------------------------
// [Static変数]
static byte s_packet_buf[NTP_PACKET_SIZE];
static RTC_DS3231 s_rtc;
static WiFiUDP s_udp;

typedef struct {
    const char *p_enter_str;
    String *p_wifi_config_pram_str;
} serial_enter_config_pram_t;
typedef enum {
    STATE_INIT = 0,
    STATE_AWAIT_WIFI_CONFIG,
    STATE_WIFI_CONFIG_E2P_SAVE,
    STATE_IDLE,
} E_WIFI_CONFIG_DATA_INPUT_STATE;

static E_WIFI_CONFIG_DATA_INPUT_STATE s_state = STATE_IDLE;
static String s_line = "";
static String s_tmp_ssid = "";
static String s_tmp_pass = "";
static String e2p_stored_ssid = "";
static String e2p_stored_password = "";
static bool s_is_enter_request = false;
static bool s_is_e2p_wifi_config = false;
static bool s_is_wifi_config_data = false;
static uint8_t s_await_param = 0;

const serial_enter_config_pram_t g_serial_enter_config_pram[] = {
    {"Enter SSID: ",     &s_tmp_ssid},
    {"Enter Password: ", &s_tmp_pass},
};

static bool e2p_write_byte(uint16_t memAddr, const uint8_t *data, uint16_t len);
static bool e2p_read_byte(uint16_t memAddr, uint8_t *buf, uint16_t len);
static bool e2p_read_page(uint8_t pageNo, uint8_t *buf32);
static bool e2p_write_page(uint8_t pageNo, const uint8_t *buf32);
static bool e2p_save_wifi_config(const String &ssid, const String &password);
static bool e2p_check_wifi_config(String &ssid, String &password);
static void send_ntp_packet(const char *address);
static bool get_ntp_time(uint32_t &result, uint32_t timeout_ms);
static bool rtc_and_ntp_sync(const String &ssid, const String &password);
static void get_wifi_config_at_serial_line(const String &input_line_str);
static void print_time_date(const DateTime &dt);
//---------------------------------------------------------------------------
// [Static関数]
//---------------------------------------------------------------------------
static bool e2p_write_byte(uint16_t memAddr, const uint8_t *data, uint16_t len)
{
    for (uint16_t i = 0; i < len; ++i)
    {
        Wire.beginTransmission(E2P_I2C_ADDR);
        Wire.write((uint8_t)(memAddr >> 8));
        Wire.write((uint8_t)(memAddr & 0xFF));
        Wire.write(data[i]);
        if (Wire.endTransmission() != 0)
            return false;
        memAddr++;
        delay(5);
    }
    return true;
}

static bool e2p_read_byte(uint16_t memAddr, uint8_t *buf, uint16_t len)
{
    uint16_t got = 0;

    Wire.beginTransmission(E2P_I2C_ADDR);
    Wire.write((uint8_t)(memAddr >> 8));
    Wire.write((uint8_t)(memAddr & 0xFF));
    if (Wire.endTransmission() != 0)
        return false;
    Wire.requestFrom(E2P_I2C_ADDR, (int)len);
    while (Wire.available() && got < len)
        buf[got++] = Wire.read();

    return got == len;
}

static bool e2p_read_page(uint8_t pageNo, uint8_t *buf32)
{
    uint16_t addr = (uint16_t)pageNo * E2P_PAGE_BYTE_SIZE;
    return e2p_read_byte(addr, buf32, E2P_PAGE_BYTE_SIZE);
}

static bool e2p_write_page(uint8_t pageNo, const uint8_t *buf32)
{
    uint16_t addr = (uint16_t)pageNo * E2P_PAGE_BYTE_SIZE;
    return e2p_write_byte(addr, buf32, E2P_PAGE_BYTE_SIZE);
}

static bool e2p_save_wifi_config(const String &ssid, const String &password)
{
    uint8_t tmp_page[E2P_PAGE_BYTE_SIZE] = {0};

    // Page[0]: check marker
    if (!e2p_write_page(0, (const uint8_t *)p_e2p_wifi_config_maker))
        return false;

    // page[1]: SSID
    size_t slen = min((size_t)31, (size_t)ssid.length());
    memcpy(tmp_page, ssid.c_str(), slen);
    if (!e2p_write_page(1, tmp_page))
        return false;
    memset(&tmp_page[0], 0x00, E2P_PAGE_BYTE_SIZE);

    // page[2]: Password
    size_t plen = min((size_t)31, (size_t)password.length());
    memcpy(tmp_page, password.c_str(), plen);
    if (!e2p_write_page(2, tmp_page))
        return false;

    return true;
}

static bool e2p_check_wifi_config(String &ssid, String &password)
{
    uint8_t tmp_page[E2P_PAGE_BYTE_SIZE] = {0};

    // Page[0]: check marker
    if (!e2p_read_page(0, tmp_page))
        return false;
    if (memcmp(tmp_page, p_e2p_wifi_config_maker, E2P_WIFI_CONFIG_MAKER_LEN) != 0)
        return false;
    memset(&tmp_page[0], 0x00, E2P_PAGE_BYTE_SIZE);

    // page[1]: SSID
    if (!e2p_read_page(1, tmp_page))
        return false;
    tmp_page[E2P_PAGE_BYTE_SIZE - 1] = 0;
    ssid = String((const char *)tmp_page);
    memset(&tmp_page[0], 0x00, E2P_PAGE_BYTE_SIZE);

    // page[2]: Password
    if (!e2p_read_page(2, tmp_page))
        return false;
    tmp_page[E2P_PAGE_BYTE_SIZE - 1] = 0;
    password = String((const char *)tmp_page);

    return true;
}

static bool rtc_and_ntp_sync(const String &ssid, const String &password)
{
    bool ret = false;

    Serial.print("Wifi Connectting! SSID: ");
    Serial.println(ssid);
    // connect, sync, disconnect
    WiFi.begin(ssid.c_str(), password.c_str());
    unsigned long startConn = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startConn < 20000)
    {
        delay(500);
        Serial.print('.');
    }

    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
        Serial.print("WiFi Connected! IP: ");
        Serial.println(WiFi.localIP());
        uint32_t epoch;
        if (get_ntp_time(epoch, 5000)) {
            const uint32_t JST_OFFSET = 9UL * 3600UL;
            uint32_t epoch_jst = epoch + JST_OFFSET;
            DateTime nowJst(epoch_jst);
            Serial.println("NTP time (JST):");
            print_time_date(nowJst);
            s_rtc.adjust(nowJst);
            Serial.println("RTC updated. RTC time now:");
            print_time_date(s_rtc.now());
            ret = true;
        } else {
            Serial.println("Failed to get NTP time");
        }
        WiFi.disconnect(true);
    } else {
        Serial.println("WiFI Connect Failed");
    }

    return ret;
}

static void send_ntp_packet(const char *address)
{
    memset(s_packet_buf, 0, NTP_PACKET_SIZE);
    s_packet_buf[0] = 0b11100011; // LI, Version, Mode
    s_packet_buf[1] = 0;          // Stratum
    s_packet_buf[2] = 6;          // Polling
    s_packet_buf[3] = 0xEC;       // Precision
    s_packet_buf[12] = 49;
    s_packet_buf[13] = 0x4E;
    s_packet_buf[14] = 49;
    s_packet_buf[15] = 52;

    s_udp.beginPacket(address, 123); // NTP requests are to port 123
    s_udp.write(s_packet_buf, NTP_PACKET_SIZE);
    s_udp.endPacket();
}

static bool get_ntp_time(uint32_t &result, uint32_t timeout_ms)
{
    // UDPローカルポートを初期化
    s_udp.begin(2390);
    send_ntp_packet(p_ntp_primary_server);
    uint32_t start = millis();
    while (millis() - start < timeout_ms)
    {
        int size = s_udp.parsePacket();
        if (size >= NTP_PACKET_SIZE)
        {
            s_udp.read(s_packet_buf, NTP_PACKET_SIZE);
            uint32_t secsSince1900 = ((uint32_t)s_packet_buf[40] << 24) | ((uint32_t)s_packet_buf[41] << 16) | ((uint32_t)s_packet_buf[42] << 8) | ((uint32_t)s_packet_buf[43]);
            const uint32_t seventyYears = 2208988800UL;
            result = secsSince1900 - seventyYears; // UNIX epoch
            return true;
        }
        delay(10);
    }
    return false;
}

static void get_wifi_config_at_serial_line(const String &input_line_str)
{
    serial_enter_config_pram_t pram;

    switch (s_state)
    {
        case STATE_INIT:
            s_tmp_ssid = "";
            s_tmp_pass = "";
            s_await_param = 0;
            s_state = STATE_AWAIT_WIFI_CONFIG;
            break;

        case STATE_AWAIT_WIFI_CONFIG:
            if (s_await_param < ENTER_REQUEST_PARAM_CNT)
            {
                pram = g_serial_enter_config_pram[s_await_param];
                if (s_is_enter_request != true)
                {
                    Serial.print(pram.p_enter_str);
                } else {
                    *(pram.p_wifi_config_pram_str) = input_line_str;
                    s_await_param++;
                }
            } else {
                s_state = STATE_WIFI_CONFIG_E2P_SAVE;
            }
            break;

        case STATE_WIFI_CONFIG_E2P_SAVE:
            if (e2p_save_wifi_config(s_tmp_ssid, s_tmp_pass)) {
                s_is_e2p_wifi_config = true;
                Serial.println("Success! SSID and Password Saved to EEPROM!");
            } else {
                Serial.println("Failed! SSID and Password save to EEPROM!");
            }
            e2p_stored_ssid = s_tmp_ssid;
            e2p_stored_password = s_tmp_pass;
            s_is_wifi_config_data = true;
            s_is_enter_request = false;
            s_state = STATE_IDLE;
            break;

        case STATE_IDLE:
        default:
            // NOP
            break;
    }
}

static void print_time_date(const DateTime &dt)
{
    char buf[32];
    sprintf(buf, "%04d/%02d/%02d %02d:%02d:%02d",
            dt.year(), dt.month(), dt.day(), dt.hour(), dt.minute(), dt.second());
    Serial.println(buf);
}

//---------------------------------------------------------------------------
void setup()
{
    Serial.begin(115200);
    delay(100);
    Serial.println();
    Serial.println("RTC sync for Pico W / Pico 2 W");

    Wire.begin();
    bool eeprom_present = false;
    Wire.beginTransmission(E2P_I2C_ADDR);
    if (Wire.endTransmission() == 0)
        eeprom_present = true;

    if (!eeprom_present) {
        Serial.println("EEPROM (24C32) not found at Addr 0x57 -> rebooting");
        rp2040.reboot();
    }

    s_is_e2p_wifi_config = e2p_check_wifi_config(e2p_stored_ssid, e2p_stored_password);
    if (s_is_e2p_wifi_config != false) {
        s_is_wifi_config_data = true;
        Serial.println("EEPROM, Found Stored WiFi Config Data.");
    } else {
        Serial.println("EEPROM, Not Stored WiFi Config Data.");
    }

    if (!s_rtc.begin()) {
        Serial.println("Couldn't find RTC (DS3231)");
    } else {
        if (s_rtc.lostPower()) {
            Serial.println("RTC lost power - will set from NTP if available");
        }
    }
}

void loop()
{
    // シリアル受信を非同期に処理。改行(\n)で1行完了とする
    while (Serial.available())
    {
        char c = Serial.read();
        if (c == '\r')
            continue;

        if (c == '\n') {
            String line_str = s_line;
            line_str.trim();
            if (line_str.length() > 0) {
                String input_line_str = line_str;
                // コマンド: "sync" -> NTPとDS3231を時刻同期
                if (line_str.equalsIgnoreCase("sync"))
                {
                    Serial.println();
                    if (s_is_wifi_config_data != false) {
                        rtc_and_ntp_sync(e2p_stored_ssid, e2p_stored_password);
                    } else {
                        s_is_enter_request = true;
                        s_state = STATE_INIT;
                    }
                }
                // コマンド: "e2p" -> EEPROMの認証表示, rtc -> DS3231時刻表示
                else if (line_str.equalsIgnoreCase("e2p"))
                {
                    Serial.println();
                    if (s_is_e2p_wifi_config)
                    {
                        Serial.print("SSID: "); Serial.println(e2p_stored_ssid);
                        Serial.print("Password: "); Serial.println(e2p_stored_password);
                    } else {
                        Serial.println("EEPROM, Not Stored WiFi Config Data.");
                    }
                }
                // コマンド: "rtc" -> DS3231時刻表示
                else if (line_str.equalsIgnoreCase("rtc")) {
                    Serial.println();
                    Serial.println("RTC time:");
                    print_time_date(s_rtc.now());
                }
                // Unknown command
                else {
                    Serial.println();
                    Serial.println("Unknown command. Available commands:");
                    Serial.println("  sync - Start WiFi setup");
                    Serial.println("  e2p - Show stored credentials");
                    Serial.println("  rtc - Show RTC time");
                }

                // WiFi設定入力待ち状態の場合
                if((s_is_enter_request != false) && (s_await_param < ENTER_REQUEST_PARAM_CNT)) {
                    get_wifi_config_at_serial_line(line_str);
                }
            }
            s_line = "";
        } else {
            s_line += c;
            Serial.print(c); // ローカルエコー
        }
    }
    delay(5);
}