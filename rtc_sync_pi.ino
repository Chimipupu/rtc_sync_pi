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
#define WIFI_CONNECT_TIMEOUT_MS      30000 // WiFi接続タイムアウト時間(ms)
#define WIFI_POLL_DELAY_MS           100   // WiFi接続ポーリング間隔(ms) - 短くすると接続待ちが細かくなる
#define NTP_PACKET_SIZE              48   // NTPパケットサイズ
#define NTP_LOCAL_PORT               2390  // UDPローカルポート (s_udp.begin)
#define NTP_POLL_DELAY_MS            5     // NTP受信ポーリング間隔(ms)
#define NTP_TIMEOUT_MS               5000  // get_ntp_time のタイムアウト既定値(ms)
#define SEC_1970_TO_1900             2208988800UL // 1970年から1900年までの秒数
#define JST_UTC_OFFSET_SEC           (9 * 3600) // JSTのUTCオフセット秒数(+9時間 * 3600秒)
#define E2P_I2C_ADDR                 0x57 // 24C32 I2Cアドレス 0x57
#define RTC_I2C_ADDR                 0x68 // DS3231 I2Cアドレス 0x68
#define E2P_PAGE_BYTE_SIZE           32   // 24C32 ページサイズ 32バイト
#define E2P_WIFI_CONFIG_MAKER_LEN    30   // WiFi設定保存用マーカーの文字列長
#define E2P_WRITE_DELAY_MS           5    // EEPROM バイト書き込み後の待ち(ms)
#define SERIAL_LOOP_DELAY_MS         5    // メインループ末尾の待ち(ms)
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
static String s_line = "";
static String e2p_stored_ssid = "";
static String e2p_stored_password = "";
static bool s_is_e2p_wifi_config = false;
static bool s_is_wifi_config_data = false;
//---------------------------------------------------------------------------
// [Static関数プロトタイプ宣言]
static bool e2p_write_byte(uint16_t memAddr, const uint8_t *data, uint16_t len);
static bool e2p_read_byte(uint16_t memAddr, uint8_t *buf, uint16_t len);
static bool e2p_read_page(uint8_t pageNo, uint8_t *buf32);
static bool e2p_write_page(uint8_t pageNo, const uint8_t *buf32);
static bool e2p_save_wifi_config(const String &ssid, const String &password);
static bool e2p_check_wifi_config(String &ssid, String &password);
static void send_ntp_packet(const char *address);
static bool get_ntp_time(uint32_t &result, uint32_t timeout_ms);
static bool rtc_and_ntp_sync(const String &ssid, const String &password);
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
        delay(E2P_WRITE_DELAY_MS);
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
    bool is_wifi_connet_timeout = false;
    unsigned long wait_time_ms = millis();

    Serial.println();
    Serial.print("WiFi Connecting...\n");
    Serial.print("SSID: ");
    Serial.println(ssid);
    Serial.print("Password: ");
    Serial.println(password);
    WiFi.begin(ssid.c_str(), password.c_str());

    // WiFIを接続(タイムアウトあり)
    wait_time_ms = millis();
    while (WiFi.status() != WL_CONNECTED)
    {
        delay(WIFI_POLL_DELAY_MS);
        Serial.print('.');

        // WiFI接続タイムアウト
        if( millis() - wait_time_ms > WIFI_CONNECT_TIMEOUT_MS ) {
            Serial.println();
            Serial.println("WiFi Connect Timeout!");
            is_wifi_connet_timeout = true;
            ret = false;
            break;
        }
    }

    if(is_wifi_connet_timeout != true) {
        Serial.println();
        Serial.print("WiFi Connecting...\n");
        if (WiFi.status() == WL_CONNECTED) {
            Serial.print("WiFi Connected! IP: ");
            Serial.println(WiFi.localIP());
            uint32_t epoch;
            if (get_ntp_time(epoch, NTP_TIMEOUT_MS)) {
                uint32_t epoch_jst = epoch + JST_UTC_OFFSET_SEC;
                DateTime nowJst(epoch_jst);
                s_rtc.adjust(nowJst);
                Serial.println("NTP and RTC Synced!");
                Serial.print("NTP time (JST):");
                print_time_date(nowJst);
                Serial.print("RTC time (JST):");
                print_time_date(s_rtc.now());
                ret = true;
            } else {
                Serial.println("Failed to get NTP time");
            }
            Serial.print("WiFi Disconnecting...\n");
            WiFi.disconnect(true);
            Serial.print("WiFi Disconnected.\n");
        } else {
            Serial.println("WiFI Connect Failed");
        }
    }

    return ret;
}

static void send_ntp_packet(const char *address)
{
    memset(s_packet_buf, 0, NTP_PACKET_SIZE);
    s_packet_buf[0] = 0xE3; // LI=11(警告), VN=100(Ver4), Mode=011(クライアント) -> 0xE3
    s_udp.beginPacket(address, 123); // NTP requests are to port 123
    s_udp.write(s_packet_buf, NTP_PACKET_SIZE);
    s_udp.endPacket();
}

static bool get_ntp_time(uint32_t &result, uint32_t timeout_ms)
{
    // UDPローカルポートを初期化
    s_udp.begin(NTP_LOCAL_PORT);
    send_ntp_packet(p_ntp_primary_server);
    uint32_t start = millis();
    while (millis() - start < timeout_ms)
    {
        int size = s_udp.parsePacket();
        if (size >= NTP_PACKET_SIZE)
        {
            s_udp.read(s_packet_buf, NTP_PACKET_SIZE);

            // 1900年1月1日からの経過秒数を取得
            // NOTE: NTPパケットのバイト40～43が秒数のビッグエンディアン表現
            uint32_t secsSince1900 = ((uint32_t)s_packet_buf[40] << 24) | \
                                    ((uint32_t)s_packet_buf[41] << 16) | \
                                    ((uint32_t)s_packet_buf[42] << 8) | \
                                    ((uint32_t)s_packet_buf[43]);

#if 1
            // NTPからパケットが来るまでの遅延対策
            // NOTE: バイト44は小数部の最上位バイト
            // NOTE: 0x80 (128) は 0.5秒 に相当
            if (s_packet_buf[44] >= 0x80) {
                secsSince1900++; // 0.5秒以上なら1秒進める（四捨五入）
            }
#endif

            // result = (1900年1月1日からの経過秒数) - (1970年1月1日からの経過秒数)
            result = secsSince1900 - SEC_1970_TO_1900;
            return true;
        }
        // delay(NTP_POLL_DELAY_MS);
    }
    return false;
}

static void print_time_date(const DateTime &dt)
{
    Serial.printf("%04d/%02d/%02d %02d:%02d:%02d\n",
                    dt.year(), dt.month(), dt.day(), dt.hour(), dt.minute(), dt.second());
}

//---------------------------------------------------------------------------
void setup()
{
    bool is_e2p_connect = false;
    bool is_rtc_connect = false;
    bool is_rtc_begin = false;

    Serial.begin(115200);

    // I2CでEEPROMとRTCの接続確認
    Wire.begin();
    Wire.beginTransmission(E2P_I2C_ADDR);
    if (Wire.endTransmission() == 0)
        is_e2p_connect = true;

    Wire.beginTransmission(RTC_I2C_ADDR);
    if (Wire.endTransmission() == 0)
        is_rtc_connect = true;

    if ((is_e2p_connect != true) || (is_rtc_connect != true)) {
        rp2040.reboot();
    }

    // EEPROMからWiFi設定読み込み
    s_is_e2p_wifi_config = e2p_check_wifi_config(e2p_stored_ssid, e2p_stored_password);
    if (s_is_e2p_wifi_config != false) {
        s_is_wifi_config_data = true;
    }

    // RTC初期化
    is_rtc_begin = s_rtc.begin();
    if (is_rtc_begin != true) {
        Serial.println("RTC DS3231 not found!");
        // rp2040.reboot();
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
                    // WiFiのSSIDとパスワードがあるなら同期開始
                    if (s_is_wifi_config_data != false) {
                        rtc_and_ntp_sync(e2p_stored_ssid, e2p_stored_password);
                    } else {
                        Serial.println();
                        Serial.println("No WiFi Config Data. Please set SSID and Password!\nUse command: e2p w <ssid> <password>\n");
                    }
                }
                // コマンド: "e2p w ssid password" -> コマンドのssidとpasswordをEEPROMに保存
                else if (line_str.length() >= 4 && line_str.substring(0,4).equalsIgnoreCase("e2p "))
                {
                    // 形式: e2p w <ssid> <password>
                    int firstSpace = line_str.indexOf(' ');
                    int secondSpace = line_str.indexOf(' ', firstSpace + 1);
                    if (secondSpace <= 0) {
                        Serial.println();
                        Serial.println("Invalid e2p command. Usage: e2p w <ssid> <password>");
                    } else {
                        String subcmd = line_str.substring(firstSpace + 1, secondSpace);
                        if (subcmd.equalsIgnoreCase("w")) {
                            int thirdSpace = line_str.indexOf(' ', secondSpace + 1);
                            if (thirdSpace <= 0) {
                                Serial.println();
                                Serial.println("Usage: e2p w <ssid> <password>");
                            } else {
                                String ssid = line_str.substring(secondSpace + 1, thirdSpace);
                                String pass = line_str.substring(thirdSpace + 1);
                                ssid.trim(); pass.trim();
                                if (ssid.length() == 0 || pass.length() == 0) {
                                    Serial.println();
                                    Serial.println("SSID or Password empty. Usage: e2p w <ssid> <password>");
                                } else {
                                    if (e2p_save_wifi_config(ssid, pass)) {
                                        s_is_e2p_wifi_config = true;
                                        e2p_stored_ssid = ssid;
                                        e2p_stored_password = pass;
                                        s_is_wifi_config_data = true;
                                        Serial.println();
                                        Serial.println("Success! SSID and Password Saved to EEPROM!");
                                    } else {
                                        Serial.println();
                                        Serial.println("Failed! SSID and Password save to EEPROM!");
                                    }
                                }
                            }
                        } else {
                            Serial.println();
                            Serial.println("Unknown e2p subcommand. Supported: w");
                        }
                    }
                }
                // コマンド: "e2p" -> 保存内容を表示(Static変数)
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
                    Serial.print("RTC: ");
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
            }
            s_line = "";
        } else {
            s_line += c;
            Serial.print(c); // ローカルエコー
        }
    }
    delay(SERIAL_LOOP_DELAY_MS);
}