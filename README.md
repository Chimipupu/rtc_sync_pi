# NTPとRTCの時刻同期治具F/W

RPI Pico 2 WでWiFiからNTPに接続してRTC(DS3231)とNTPの時刻を同期

## (TBD) 仕様 ※詳細設計中

- 初期化
  - I2CにEEPROMが接続されている？(24C32、アドレス0x57、1ページ32Byte)
    - no):  リセット(マイコンをS/Wリセット)
  - I2CでEEPROMにWiFiのSSIDとパスワードが保存されている？(EEPROMの1ページ目が"RTC_SYNC")
    - yes): ページ2からSSID、ページ3からパスワードをReadしてStatic変数に保存
- メインループ
  - シリアルコマンド "sync"
    - WiFiのSSIDとパスワードのStatic変数にデータがある? (変数が初期値のNULLではない)
      - no): シリアルでSSIDとパスワードを入力してもらう
            - 1): EEPROMにSSIDとパスワードを書き込んで保存
            - 2): Static変数にSSIDとパスワードを保存
            - 3): フラグを降ろす
    - WiFiに接続
    - シリアルにIPアドレス表示
    - NTPから時刻取得（日本時刻）、時刻をシリアルに表示
    - RTCとNTPを時刻同期
    - RTCの時刻をシリアルで表示
    - WiFi切断
  - シリアルコマンド "e2p"
    - Static変数のEEPROMのSSIDとパスワードをシリアルで表示
  - シリアルコマンド "rtc"
    - RTCの時刻を読み出してシリアルで表示
