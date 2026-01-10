/*
  rtc_sync_pi.ino
  Raspberry Pi Pico W / Pico 2 W 用スケッチ
  - シリアルからSSIDとパスワードを入力
  - WiFi接続 -> NTP取得
  - DS3231(RTC)へNTP時刻を同期

  前提ライブラリ:
  - RTClib (Adafruit)
  - Pico W用のWiFiライブラリ（Arduino coreのWiFi/WiFiUDP）

  シリアルモニタ: 115200, 改行を送信
*/

#include <Wire.h>
#include <RTClib.h>
#include <WiFi.h>
#include <WiFiUdp.h>

RTC_DS3231 rtc;
WiFiUDP udp;

const char *ntpServer = "pool.ntp.org";
const int NTP_PACKET_SIZE = 48;
byte packetBuffer[NTP_PACKET_SIZE];

void sendNTPPacket(const char *address)
{
    memset(packetBuffer, 0, NTP_PACKET_SIZE);
    packetBuffer[0] = 0b11100011; // LI, Version, Mode
    packetBuffer[1] = 0;          // Stratum
    packetBuffer[2] = 6;          // Polling
    packetBuffer[3] = 0xEC;       // Precision
    packetBuffer[12] = 49;
    packetBuffer[13] = 0x4E;
    packetBuffer[14] = 49;
    packetBuffer[15] = 52;

    udp.beginPacket(address, 123); // NTP requests are to port 123
    udp.write(packetBuffer, NTP_PACKET_SIZE);
    udp.endPacket();
}

bool getNtpTime(uint32_t &result, uint32_t timeout_ms = 3000)
{
    // UDPローカルポートを初期化
    udp.begin(2390);
    sendNTPPacket(ntpServer);
    uint32_t start = millis();
    while (millis() - start < timeout_ms)
    {
        int size = udp.parsePacket();
        if (size >= NTP_PACKET_SIZE)
        {
            udp.read(packetBuffer, NTP_PACKET_SIZE);
            uint32_t secsSince1900 = ((uint32_t)packetBuffer[40] << 24) | ((uint32_t)packetBuffer[41] << 16) | ((uint32_t)packetBuffer[42] << 8) | ((uint32_t)packetBuffer[43]);
            const uint32_t seventyYears = 2208988800UL;
            result = secsSince1900 - seventyYears; // UNIX epoch
            return true;
        }
        delay(10);
    }
    return false;
}

// 非ブロッキングでシリアル行を受け取るための状態管理
enum InputState { IDLE, AWAIT_SSID, AWAIT_PASS };
InputState inputState = IDLE;
String lineBuffer = "";
String ssid = "";
String pass = "";

// 非同期で受信した1行を処理する
void handleLine(const String &ln)
{
    if (inputState == AWAIT_SSID)
    {
        ssid = ln;
        Serial.println();
        Serial.print("Enter password: ");
        inputState = AWAIT_PASS;
    }
    else if (inputState == AWAIT_PASS)
    {
        pass = ln;
        Serial.println();
        // SSIDとパスワードが揃ったので接続＆同期を開始
        Serial.print("Connecting to WiFi '");
        Serial.print(ssid);
        Serial.println("' ...");
        // 接続とNTP同期を行う
        WiFi.begin(ssid.c_str(), pass.c_str());
        unsigned long start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 20000)
        {
            delay(500);
            Serial.print('.');
        }
        Serial.println();
        if (WiFi.status() != WL_CONNECTED)
        {
            Serial.println("Failed to connect to WiFi");
        }
        else
        {
            Serial.print("Connected, IP: ");
            Serial.println(WiFi.localIP());
            uint32_t epoch;
            if (getNtpTime(epoch))
            {
                Serial.print("NTP epoch (UTC): ");
                Serial.println(epoch);
                // 日本時間 (JST = UTC+9)
                const uint32_t JST_OFFSET = 9UL * 3600UL;
                uint32_t epoch_jst = epoch + JST_OFFSET;
                DateTime nowJst(epoch_jst);
                Serial.println("NTP time (JST):");
                printDateTime(nowJst);

                // RTCに同期（DS3231にはJST時刻を書き込む）
                rtc.adjust(nowJst);
                Serial.println("RTC updated. RTC time now:");
                DateTime rtcNow = rtc.now();
                printDateTime(rtcNow);
            }
            else
            {
                Serial.println("Failed to get NTP time");
            }
        }

        // 終了後は次のSSID入力待ちへ戻す
        ssid = "";
        pass = "";
        inputState = AWAIT_SSID;
        Serial.print("Enter SSID: ");
    }
}

void printDateTime(const DateTime &dt)
{
    char buf[32];
    sprintf(buf, "%04d/%02d/%02d %02d:%02d:%02d", dt.year(), dt.month(), dt.day(), dt.hour(), dt.minute(), dt.second());
    Serial.println(buf);
}

void setup()
{
    Serial.begin(115200);
    delay(100);
    Serial.println();
    Serial.println("RTC sync for Pico W / Pico 2 W");

    Wire.begin();
    if (!rtc.begin())
    {
        Serial.println("Couldn't find RTC (DS3231)");
    }
    else
    {
        if (rtc.lostPower())
        {
            Serial.println("RTC lost power - will set from NTP if available");
        }
    }

    // 待機状態。シリアルから 'sync' を受け取ると同期フローを開始する
    Serial.println("Ready. Send 'sync' (followed by newline) to start RTC sync.");
    inputState = IDLE;
}

void loop()
{
    // シリアル受信を非同期に処理。改行(\n)で1行完了とする
    while (Serial.available())
    {
        char c = Serial.read();
        if (c == '\r')
            continue;
        if (c == '\n')
        {
            String ln = lineBuffer;
            ln.trim();
            if (ln.length() > 0)
            {
                // IDLE 時に 'sync' を受け取ったら同期フローを開始
                if (inputState == IDLE && ln.equalsIgnoreCase("sync"))
                {
                    Serial.println();
                    Serial.print("Enter SSID: ");
                    inputState = AWAIT_SSID;
                }
                else if (inputState != IDLE)
                {
                    handleLine(ln);
                }
                else
                {
                    Serial.println();
                    Serial.println("Unknown command. Send 'sync' to start.");
                }
            }
            lineBuffer = "";
        }
        else
        {
            lineBuffer += c;
            Serial.print(c); // エコー
        }
    }
    delay(5);
}