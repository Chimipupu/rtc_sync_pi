# NTPとRTCの時刻同期治具F/W

RPI Pico 2 WでWiFiからNTPに接続してRTC(DS3231)とNTPの時刻を同期するF/W

## (TBD)仕様 ※詳細設計中

### 初期化シーケンス

- 初期化
  - I2CにRTCとEEPROMが接続されている？
    - no): リセット(マイコンをS/Wリセット)

  - EEPROMのページ0にマーカーがある?
    - yes):ページ1からSSID、ページ2からパスワードをReadしてStatic変数に保存

### シリアルコマンド

- `sync`
  - EEPROM に SSID/Password が保存済み？
    - no): `e2p w <ssid> <password>` でSSIDとパスワードを設定するようprintf()だけ
    - yes): WiFi 接続 → NTP 取得 → RTC 書き込み
      - NTP サーバ: `ntp.nict.jp`、UDP ローカルポートは `2390`
      - NTP の 時刻はUNIXから日本時間のJSTに変換してRTC に設定
      - WiFi 接続のタイムアウトは決め打ちで`30000` ms（30秒）
      - 同期処理が完了したら `WiFi.disconnect(true)` でWiFiを切断

- `e2p`
  - EEPROMに保存されているSSID / Password をシリアルに表示
  - ただしこのとき表示するSSID / Password は内部の static 変数(EEPROMは読み出しが遅すぎるから)

  - `e2p w <ssid> <password>`
    - 引数のSSID と Password を EEPROM に書き込み、内部の static 変数にも反映

- `rtc`
  - RTCのDS3231に現在時刻をシリアルで表示する（ドライバは`RTClib`を使用）

### EEPROMメモリマップ

- 型番: AT24C32
- I2C アドレス: `0x57`
- ページサイズ: `32` バイト
- ページ割り当て:
  - page 0: マーカー文字列 `"WIFI_CONFIG_SSID_AND_PASSWORD"`
  - page 1: SSID（最大 31 バイトまで保存）
  - page 2: Password（最大 31 バイトまで保存）
- マーカー比較でWiFiのSSIDとパスワードが保存されているか判定
- EEPROMの読み取り・書き込みはページ単位
- 今はすべて暗号化なしの平文で保存(TBD: 暗号化予定)