/**
 * @file men-g2-atoms3-hello.ino
 * @brief M5AtomS3 を使って Even G2 スマートグラスへ BLE 接続し、
 *        カウンターヘルパーアプリを動作させるスケッチ。
 *
 * 概要:
 *   - BLE スキャンにより Even G2 の左右レンズを発見・接続する
 *   - 所定のブートシーケンスを実行して認証・初期化を行う
 *   - G2 グラスにカウンター文字を表示し、タップまたは M5AtomS3 のボタンでカウントする
 *   - 定期ハートビートで BLE セッションを維持し、切断時は自動再接続する
 */
#include <Arduino.h>
#include <BLEDevice.h>
#include <M5Unified.h>
#include <esp_log.h>

#include <string>
#include <time.h>
#include <vector>

#include "G2MentraProtocol.h"

/**
 * @brief スケッチ全体の調整式パラメータ。
 *
 * コンパイル時定数にまとめてあり、スケッチ本体と G2 グラスの動作を変更する
 * 場合はここだけを編集する。
 */
namespace config {

/// BLE 広告時に使用するローカルデバイス名。
constexpr char kLocalClientName[] = "M5AtomS3-G2";
/// 接続対象をシリアル番号の部分一致で絞り込むフラグメント。空文字列の場合は任意の G2 に接続する。
constexpr char kTargetSerialFragment[] = "";
/// ハードコードで左レンズの BLE アドレスを指定する場合に設定する (スキャンをスキップする)。
constexpr char kKnownLeftAddress[] = "";
/// ハードコードで右レンズの BLE アドレスを指定する場合に設定する (スキャンをスキップする)。
constexpr char kKnownRightAddress[] = "";
/// G2 グラスに表示するテキストコンテナの ID。
constexpr uint32_t kCounterTextContainerId = 1;

/// BLE スキャンのタイムアウト (ミリ秒)。
constexpr uint32_t kDiscoveryTimeoutMs = 30000;
/// BLE スキャン 1 回当たりのスキャン持続時間 (秒)。
constexpr uint32_t kScanChunkSeconds = 4;
/// BLE 接続タイムアウト (ミリ秒)。
constexpr uint32_t kConnectTimeoutMs = 10000;
/// 接続直後の落ち着き待機時間 (ミリ秒)。この間にサービスディスカバリが安定する。
constexpr uint32_t kPostConnectSettleMs = 300;
/// サービスディスカバリの最大リトライ回数。
constexpr uint8_t kServiceDiscoveryRetries = 3;
/// サービスディスカバリリトライ間の待機時間 (ミリ秒)。
constexpr uint32_t kServiceDiscoveryRetryDelayMs = 400;
/// ブートシーケンス各ステップ間の待機時間 (ミリ秒)。
constexpr uint32_t kStepGapMs = 200;
/// 認証完了を待つタイムアウト (ミリ秒)。
constexpr uint32_t kAuthWaitTimeoutMs = 2000;
/// EvenHub / DevSettings ハートビート送信間隔 (ミリ秒)。
constexpr uint32_t kHeartbeatIntervalMs = 5000;
/// 時刻同期時のタイムゾーンオフセット (時間)。JST = 9。
constexpr uint8_t kTimezoneHours = 9;
/// 0 以外の値を設定すると RTC の代わりにその値を起点時刻として使用する (デバッグ用)。
constexpr int32_t kManualUnixTime = 0;
/// セッション失敗後の再試行待機時間 (ミリ秒)。
constexpr uint32_t kRetryDelayMs = 5000;
/// G2 タップのデバウンス時間 (ミリ秒)。短時間に重複したイベントを無視する。
constexpr uint32_t kRemoteTapDebounceMs = 250;
/// マルチパケット送信時のパケット間延遅 (ミリ秒)。BLE ラジオ通信負荷軽減用。
constexpr uint8_t kBleBurstGapMs = 8;
/// true の場合、ESP-IDF の VERBOSE ログを有効化する (デバッグ用)。
constexpr bool kEnableEspVerboseLogs = false;

}  // namespace config

/**
 * @brief 片方のレンズとの BLE 接続に関するすべての状態を保持する構造体。
 *
 * gLeft / gRight の 2 インスタンスをグローバルに保持する。
 */
struct LensConnection {
  const char* sideLabel;              ///< デバッグ表示用の識別名 ("LEFT" または "RIGHT")
  bool isLeft;                        ///< true = 左レンズ、false = 右レンズ
  String name;                        ///< BLE 広告名 ("G2_XXXX_L_..." 形式)
  String address;                     ///< BLE MAC アドレス文字列
  uint8_t addressType = 0xFF;         ///< BLE アドレスタイプ (0=public, 1=random, 0xFF=未知)
  BLEAdvertisedDevice advertisedDevice; ///< スキャンで取得した最新の広告オブジェクト
  bool hasAdvertisedDevice = false;   ///< advertisedDevice が有効なスキャン結果かどうか
  BLEClient* client = nullptr;        ///< BLE クライアントオブジェクト (接続後に確保)
  BLERemoteCharacteristic* writeChar = nullptr;  ///< コマンド送信用 WRITE キャラクタリスティック
  BLERemoteCharacteristic* notifyChar = nullptr; ///< 応答受信用 NOTIFY キャラクタリスティック
  g2mentra::TransportAssembler assembler; ///< マルチパケット再組み立て器
  bool connected = false;             ///< BLE 接続中かどうか
  bool ready = false;                 ///< キャラクタリスティック取得完了で送信可能かどうか
  bool authenticated = false;         ///< 認証完了かどうか
};

/**
 * @brief 用途間にアドレス・名前を保持する簡易キャッシュ構造体。
 *
 * スキャンなしで再接続するため、前回のセッションから左右レンズの情報を保持する。
 */
struct CachedLensInfo {
  String name;                 ///< 前回接続時の BLE 広告名
  String address;              ///< 前回接続時の BLE MAC アドレス
  uint8_t addressType = 0xFF;  ///< 前回接続時のアドレスタイプ
};

/**
 * @brief グラス接続後のブートシーケンスの進行ステップ。
 *
 * processBootSequence() がこの列挙型に従って順に各コマンドを送信する。
 * 各ステップは所定時間待機後に実行され、Ready に達すると通常運用に移行する。
 */
enum class BootStep {
  Idle,               ///< 初期状態。セッション未開始。
  SendAuthLeft,       ///< 左レンズへ認証コマンドを送信する。
  SendAuthRight,      ///< 右レンズへ認証コマンドを送信する。
  SendPipeRoleChange, ///< 右レンズへパイプ役割変更コマンドを送信する。
  SendTimeSync,       ///< 時刻同期コマンドを送信する。
  WaitAuth,           ///< 左右レンズからの認証完了通知を待つ。
  SendOnboardingSkip, ///< 初回セットアップスキップコマンドを送信する。
  SendUniverseSettings,  ///< ユニバース設定を送信する。
  SendGestureInit,       ///< ジェスチャーコントロール初期化コマンドを送信する。
  SendUiSettingQuery,    ///< UI 設定照会コマンドを送信する。
  SendEvenHubCtrlInit,   ///< EvenHub コントロール初期化コマンドを送信する。
  SendDashboardInit,     ///< ダッシュボード初期化コマンドを送信する。
  SendGestureControlList,   ///< ジェスチャーリストを送信する。
  SendDashboardNewsRequest, ///< ダッシュボードニュース情報をリクエストする。
  SendDashboardAppNewsRequest, ///< ダッシュボードアプリニュース情報をリクエストする。
  ShowHelloPage, ///< カウンターページを作成して表示する。
  Ready,         ///< ブート完了。通常運用中。
};

/// BLE NOTIFY コールバックの型エイリアス。
using NotifyHandler = void (*)(BLERemoteCharacteristic*, uint8_t*, size_t, bool);

// ─── グローバル状態変数 ───────────────────────────────────────────────────────────────

// 左右レンズのセッション情報。
LensConnection gLeft{"LEFT", true};
LensConnection gRight{"RIGHT", false};

// セッション跨ぎのアドレスキャッシュ (スキャンなしで再接続するため)。
CachedLensInfo gCachedLeft;
CachedLensInfo gCachedRight;

/// 現在のセッションで使用中のシリアル番号。
String gSelectedSerial;
/// キャッシュ済みのシリアル番号 (セッション跨ぎに保持)。
String gCachedSerial;
/// 現在のブートシーケンスステップ。
BootStep gBootStep = BootStep::Idle;
/// 次のブートアクションを実行してよい時刻 (millis() 基準値)。
uint32_t gNextBootActionAtMs = 0;
/// 認証シーケンス開始時刻。WaitAuth タイムアウト算出に使用する。
uint32_t gAuthSequenceStartedAtMs = 0;
/// 前回の EvenHub ハートビート送信時刻。
uint32_t gLastEvenHubHeartbeatAtMs = 0;
/// 前回の DevSettings ハートビート送信時刻。
uint32_t gLastDevSettingsHeartbeatAtMs = 0;
/// 前回のセッション開始試み時刻。再接続間隔制御に使用する。
uint32_t gLastStartAttemptAtMs = 0;
/// カウンターの現在値。
uint32_t gCounterValue = 0;

/// 送信パケットに付与する同期 ID カウンター (0–255 でラップアラウンド)。
uint8_t gNextSyncId = 0;
/// コマンドの一意性を保つための 8 ビットアップカウンター。
uint8_t gNextMagicRandom = 0;

/// setup() が正常完了したかどうか。loop() の再接続ロジックの制御に使用する。
bool gSetupComplete = false;
/// ブートシーケンス完了し、通常運用中かどうか。
bool gBootReady = false;
/// 直前のセッション開始が失敗したかどうか。再試行トリガーに使用する。
bool gSessionStartFailed = false;
/// カウンター値が変更されたがグラスにまだ忘れているかどうか。
bool gCounterDirty = true;

/// G2 タップでインクリメントする保留カウント (NOTIFY コールバックから書き込む)。volatile 必須。
volatile uint32_t gPendingRemoteTapCount = 0;
/// 直前の G2 タップ時刻 (millis())。デバウンス用。volatile 必須。
volatile uint32_t gLastRemoteTapAtMs = 0;
/// gPendingRemoteTapCount / gLastRemoteTapAtMs を保護するクリティカルセクション用スピンロック。
portMUX_TYPE gInputMux = portMUX_INITIALIZER_UNLOCKED;

void resetSessionState();
bool startSession();
void handleNotifyPacket(LensConnection& lens, uint8_t* data, size_t length);
void onLensDisconnected(LensConnection& lens);
void rememberLens(const LensConnection& lens);
uint8_t nextMagicRandom();
void sendEvenHub(const std::vector<uint8_t>& payload);

/// G2 グラスへ送るカウンターテキストを生成する。
String buildCounterText() {
  return String("Count: ") + String(gCounterValue);
}

/// M5AtomS3 ディスプレイに現在のカウンター値を表示する。
void renderLocalCounter() {
  M5.Display.fillScreen(0x0000);
  M5.Display.setTextColor(0xFFFF, 0x0000);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(8, 12);
  M5.Display.println("Counter");
  M5.Display.setTextSize(4);
  M5.Display.setCursor(8, 48);
  M5.Display.println(gCounterValue);
}

/**
 * @brief カウンターを指定量分加算し、ローカル画面を更新する。
 *
 * @param amount 加算する値。0 の場合は何もしない。
 * @param source ログ表示用の入力ソース文字列。
 */
void incrementCounter(uint32_t amount, const char* source) {
  if (amount == 0) {
    return;
  }

  gCounterValue += amount;
  gCounterDirty = true;
  renderLocalCounter();
  Serial.printf("Counter +%lu via %s -> %lu\n",
                static_cast<unsigned long>(amount),
                source,
                static_cast<unsigned long>(gCounterValue));
}

/**
 * @brief G2 タップイベントを保留キューに追加する (NOTIFY コールバックから呼び出される)。
 *
 * デバウンス時間内の重複イベントは無視する。
 * クリティカルセクションでカウンタを保護する。
 */
void queueRemoteTap() {
  const uint32_t nowMs = millis();
  portENTER_CRITICAL(&gInputMux);
  if ((nowMs - gLastRemoteTapAtMs) >= config::kRemoteTapDebounceMs) {
    gLastRemoteTapAtMs = nowMs;
    ++gPendingRemoteTapCount;
  }
  portEXIT_CRITICAL(&gInputMux);
}

/**
 * @brief M5AtomS3 のボタンと G2 タップイベントをポーリングし、カウンターを更新する。
 *
 * loop() 毎回呼び出す。両方の入力が存在する場合は合算して一度にインクリメントする。
 */
void pollInputs() {
  M5.update();

  const bool buttonPressed = M5.BtnA.wasPressed();
  uint32_t remoteTapCount = 0;
  portENTER_CRITICAL(&gInputMux);
  remoteTapCount = gPendingRemoteTapCount;
  gPendingRemoteTapCount = 0;
  portEXIT_CRITICAL(&gInputMux);

  const uint32_t totalIncrement = (buttonPressed ? 1U : 0U) + remoteTapCount;
  if (totalIncrement == 0) {
    return;
  }

  const char* source = "G2 tap";
  if (buttonPressed && remoteTapCount > 0) {
    source = "AtomS3 button + G2 tap";
  } else if (buttonPressed) {
    source = "AtomS3 button";
  }

  incrementCounter(totalIncrement, source);
}

/**
 * @brief カウンター値が変更されており、グラスが接続中ならば、
 *        グラスのテキストコンテナを更新する。
 *
 * loop() 毎回呼び出し、gCounterDirty フラグで遠隔送信を制御する。
 */
void syncCounterToGlassesIfNeeded() {
  if (!gCounterDirty || !gBootReady || !gRight.connected) {
    return;
  }

  sendEvenHub(g2mentra::buildUpdateTextCommand(nextMagicRandom(),
                                               buildCounterText(),
                                               config::kCounterTextContainerId));
  gCounterDirty = false;
}

/// 右レンズのアドレスが判明しているかどうかを返す。
bool isRightLensAvailable() {
  return !gRight.address.isEmpty();
}

/// 左レンズが接続済みの場合、ブート完了を左レンズの認証完了にも依存させるかどうか。
bool isLeftLensRequiredForBoot() {
  return gLeft.connected;
}

// 左レンズ用の BLE クライアントコールバック。
// onConnect / onDisconnect はコールバックスレッドから呼ばれるため、
// グローバル変数の読み込み/書き込みは最小限にとどめること。
class LeftClientCallbacks : public BLEClientCallbacks {
  void onConnect(BLEClient* client) override {
    gLeft.connected = true;
    Serial.println("[LEFT] Connected");
  }

  void onDisconnect(BLEClient* client) override {
    onLensDisconnected(gLeft);
  }
};

// 右レンズ用の BLE クライアントコールバック。
class RightClientCallbacks : public BLEClientCallbacks {
  void onConnect(BLEClient* client) override {
    gRight.connected = true;
    Serial.println("[RIGHT] Connected");
  }

  void onDisconnect(BLEClient* client) override {
    onLensDisconnected(gRight);
  }
};

LeftClientCallbacks gLeftClientCallbacks;
RightClientCallbacks gRightClientCallbacks;

// 左レンズからの NOTIFY データを handleNotifyPacket() に渡すコールバック。
void leftNotifyCallback(BLERemoteCharacteristic* characteristic,
                        uint8_t* data,
                        size_t length,
                        bool isNotify) {
  handleNotifyPacket(gLeft, data, length);
}

// 右レンズからの NOTIFY データを handleNotifyPacket() に渡すコールバック。
void rightNotifyCallback(BLERemoteCharacteristic* characteristic,
                         uint8_t* data,
                         size_t length,
                         bool isNotify) {
  handleNotifyPacket(gRight, data, length);
}

/// 専用の syncId カウンタから次の値を取得する (0–255 でラップアラウンド)。
uint8_t nextSyncId() {
  const uint8_t value = gNextSyncId;
  ++gNextSyncId;
  return value;
}

/// 専用の magicRandom カウンタから次の値を取得する (0–255 でラップアラウンド)。
uint8_t nextMagicRandom() {
  const uint8_t value = gNextMagicRandom;
  ++gNextMagicRandom;
  return value;
}

/**
 * @brief 現在の Unix 時刻 (秒単位) を返す。
 *
 * kManualUnixTime が 0 以外ならそれを基準に millis() で追加する。
 * それ以外は time() を試み、対応する値が得られなければ millis()/1000 を代用する。
 */
int32_t currentUnixSeconds() {
  if (config::kManualUnixTime > 0) {
    return config::kManualUnixTime + static_cast<int32_t>(millis() / 1000UL);
  }

  const time_t now = time(nullptr);
  if (now > 1700000000) {
    return static_cast<int32_t>(now);
  }

  return static_cast<int32_t>(millis() / 1000UL);
}

/**
 * @brief BLE メーカーデータの先頭 14 バイトから ASCII シリアル番号を抽出する。
 *
 * @param manufacturerData BLE 広告の manufacturer data 文字列
 * @return 抽出したシリアル番号、または失敗時は空文字列
 */
String extractSerialFromManufacturerData(const String& manufacturerData) {
  if (manufacturerData.length() < 14) {
    return "";
  }

  String serial;
  serial.reserve(14);
  for (size_t index = 0; index < 14; ++index) {
    const char value = manufacturerData.charAt(index);
    if (value >= 32 && value <= 126) {
      serial += value;
    }
  }
  return serial;
}

/**
 * @brief BLE 広告名から "G2_<serial>_" パターンでシリアル番号を抽出する。
 *
 * @param name BLE 広告名
 * @return 抽出したシリアル番号、または失敗時は空文字列
 */
String extractSerialFromName(const String& name) {
  const int start = name.indexOf("G2_");
  if (start < 0) {
    return "";
  }

  const int serialStart = start + 3;
  const int serialEnd = name.indexOf('_', serialStart);
  if (serialEnd <= serialStart) {
    return "";
  }

  return name.substring(serialStart, serialEnd);
}

/**
 * @brief 指定シリアル番号または広告名が設定済みの接続対象と一致するか判定する。
 *
 * kTargetSerialFragment が空文字列の場合は常に true を返す。
 */
bool matchesConfiguredTarget(const String& serial, const String& name) {
  if (strlen(config::kTargetSerialFragment) == 0) {
    return true;
  }

  return serial.indexOf(config::kTargetSerialFragment) >= 0 ||
         name.indexOf(config::kTargetSerialFragment) >= 0;
}

/**
 * @brief レンズ接続をクリーンアップする。
 *
 * 認証状態・接続状態をリセットし、接続中の場合は BLE 切断も行う。
 */
void closeLens(LensConnection& lens) {
  lens.authenticated = false;
  lens.ready = false;
  lens.connected = false;
  if (lens.client != nullptr) {
    if (lens.client->isConnected()) {
      lens.client->disconnect();
    }
  }
  lens.writeChar = nullptr;
  lens.notifyChar = nullptr;
}

/**
 * @brief グローバルのセッション状態を全部初期化する。
 *
 * 再接続前に呼び出し、左右レンズの接続情報・アドレス・ブート状態をリセットする。
 * キャッシュしたアドレス情報は消去しない。
 */
void resetSessionState() {
  gSelectedSerial = "";
  gBootStep = BootStep::Idle;
  gNextBootActionAtMs = 0;
  gAuthSequenceStartedAtMs = 0;
  gLastEvenHubHeartbeatAtMs = 0;
  gLastDevSettingsHeartbeatAtMs = 0;
  gNextSyncId = 0;
  gNextMagicRandom = 0;
  gBootReady = false;

  closeLens(gLeft);
  closeLens(gRight);
  gLeft.name = "";
  gRight.name = "";
  gLeft.address = "";
  gRight.address = "";
  gLeft.addressType = 0xFF;
  gRight.addressType = 0xFF;
  gLeft.hasAdvertisedDevice = false;
  gRight.hasAdvertisedDevice = false;
}

/**
 * @brief キャッシュ情報を LensConnection に復元する。
 *
 * スキャンなしで前回のアドレスで再接続する場合に呼び出す。
 */
void restoreCachedLens(LensConnection& lens, const CachedLensInfo& cached) {
  if (cached.address.isEmpty()) {
    return;
  }
  lens.name = cached.name;
  lens.address = cached.address;
  lens.addressType = cached.addressType;
}

/**
 * @brief レンズの名前・アドレス・アドレスタイプをキャッシュに保存する。
 *
 * セッション途中で切断した場合に再接続を高速化するため。
 */
void rememberLens(const LensConnection& lens) {
  CachedLensInfo& cached = lens.isLeft ? gCachedLeft : gCachedRight;
  cached.name = lens.name;
  cached.address = lens.address;
  cached.addressType = lens.addressType;
}

/**
 * @brief Even G2 レンズをスキャンまたはキャッシュから発見する。
 *
 * 実行順序:
 *   1. kKnownLeftAddress / kKnownRightAddress が設定されていればそのまま利用する。
 *   2. キャッシュがあればスキャンをスキップする。
 *   3. BLE スキャンを kDiscoveryTimeoutMs まで実行する。
 *
 * @return true  少なくとも右レンズのアドレスが判明した
 * @return false タイムアウトのため右レンズを発見できなかった
 */
bool discoverTargets() {
  if (strlen(config::kKnownLeftAddress) > 0 && strlen(config::kKnownRightAddress) > 0) {
    gLeft.address = config::kKnownLeftAddress;
    gRight.address = config::kKnownRightAddress;
    gLeft.addressType = 0;
    gRight.addressType = 0;
    gSelectedSerial = "manual";
    Serial.println("Using manually configured lens addresses");
    return true;
  }

  restoreCachedLens(gLeft, gCachedLeft);
  restoreCachedLens(gRight, gCachedRight);
  if (!gCachedSerial.isEmpty()) {
    gSelectedSerial = gCachedSerial;
  }

  if (!gLeft.address.isEmpty()) {
    Serial.printf("Reusing cached LEFT lens at %s (type=%u)\n",
                  gLeft.address.c_str(),
                  gLeft.addressType);
  }
  if (!gRight.address.isEmpty()) {
    Serial.printf("Reusing cached RIGHT lens at %s (type=%u)\n",
                  gRight.address.c_str(),
                  gRight.addressType);
  }
  if (isRightLensAvailable()) {
    Serial.println("Using cached lens addresses without scanning");
    return true;
  }

  BLEScan* scan = BLEDevice::getScan();
  scan->setActiveScan(true);

  const uint32_t startedAtMs = millis();
  while ((millis() - startedAtMs) < config::kDiscoveryTimeoutMs) {
    Serial.println("Scanning for Even G2 lenses...");
    BLEScanResults* results = scan->start(config::kScanChunkSeconds, false);
    if (results == nullptr) {
      continue;
    }

    for (int index = 0; index < results->getCount(); ++index) {
      BLEAdvertisedDevice device = results->getDevice(index);
      const String name = device.getName().c_str();
      if (name.indexOf("G2") < 0) {
        continue;
      }

      String serial = extractSerialFromManufacturerData(device.getManufacturerData());
      if (serial.isEmpty()) {
        serial = extractSerialFromName(name);
      }
      if (serial.isEmpty() || !matchesConfiguredTarget(serial, name)) {
        continue;
      }

      if (gSelectedSerial.isEmpty()) {
        gSelectedSerial = serial;
        gCachedSerial = serial;
        Serial.printf("Selected serial: %s\n", gSelectedSerial.c_str());
      }
      if (serial != gSelectedSerial) {
        continue;
      }

      const String address = device.getAddress().toString();
      const uint8_t addressType = device.getAddressType();
      Serial.printf("Found %s lens %s at %s (type=%u, connectable=%s, scannable=%s, advType=%u, legacy=%s)\n",
                    name.c_str(),
                    serial.c_str(),
                    address.c_str(),
                    addressType,
                    device.isConnectable() ? "true" : "false",
                    device.isScannable() ? "true" : "false",
                    device.getAdvType(),
                    device.isLegacyAdvertisement() ? "true" : "false");

      if (name.indexOf("_L_") >= 0 && gLeft.address.isEmpty()) {
        gLeft.name = name;
        gLeft.address = address;
        gLeft.addressType = addressType;
        gLeft.advertisedDevice = device;
        gLeft.hasAdvertisedDevice = true;
        rememberLens(gLeft);
      } else if (name.indexOf("_R_") >= 0 && gRight.address.isEmpty()) {
        gRight.name = name;
        gRight.address = address;
        gRight.addressType = addressType;
        gRight.advertisedDevice = device;
        gRight.hasAdvertisedDevice = true;
        rememberLens(gRight);
      }
    }

    scan->clearResults();
    if (!gLeft.address.isEmpty() && !gRight.address.isEmpty()) {
      scan->stop();
      return true;
    }
  }

  scan->stop();
  return isRightLensAvailable();
}

/**
 * @brief 指定レンズへ BLE 接続し、WRITE / NOTIFY キャラクタリスティックを取得する。
 *
 *   1. BLEClient 作成 (2 回目以降は再利用)
 *   2. connectTimeout または connect で接続
 *   3. サービスディスカバリを最大 kServiceDiscoveryRetries 回リトライ
 *   4. WRITE / NOTIFY キャラクタリスティックのマッチング
 *   5. NOTIFY 登録
 *
 * @param lens           接続対象の LensConnection
 * @param notifyHandler  NOTIFY コールバック関数ポインタ
 * @return true  接続成功かつキャラクタリスティック取得完了
 * @return false 接続またはサービスディスカバリに失敗
 */
bool connectLens(LensConnection& lens, NotifyHandler notifyHandler) {
  if (lens.address.isEmpty()) {
    return false;
  }

  Serial.printf("Connecting %s lens at %s (type=%u)\n",
                lens.sideLabel,
                lens.address.c_str(),
                lens.addressType);
  if (lens.client == nullptr) {
    lens.client = BLEDevice::createClient();
    if (lens.client == nullptr) {
      Serial.printf("Failed to create %s BLE client\n", lens.sideLabel);
      return false;
    }
    lens.client->setClientCallbacks(lens.isLeft ? static_cast<BLEClientCallbacks*>(&gLeftClientCallbacks)
                                                : static_cast<BLEClientCallbacks*>(&gRightClientCallbacks));
  }

  bool connected = false;
  if (lens.hasAdvertisedDevice) {
    Serial.printf("Attempting %s connection using fresh advertisement object (connectable=%s, advType=%u)\n",
                  lens.sideLabel,
                  lens.advertisedDevice.isConnectable() ? "true" : "false",
                  lens.advertisedDevice.getAdvType());
    connected = lens.client->connectTimeout(&lens.advertisedDevice, config::kConnectTimeoutMs);
  } else {
    Serial.printf("Attempting %s connection using cached address only\n", lens.sideLabel);
    const BLEAddress peerAddress(lens.address.c_str(), lens.addressType == 0xFF ? 0 : lens.addressType);
    connected = lens.client->connect(peerAddress,
                                     lens.addressType == 0xFF ? 0 : lens.addressType,
                                     config::kConnectTimeoutMs);
  }

  if (!connected) {
    Serial.printf("Failed to connect %s lens within %lu ms\n",
                  lens.sideLabel,
                  static_cast<unsigned long>(config::kConnectTimeoutMs));
    closeLens(lens);
    return false;
  }

  lens.connected = true;
  delay(config::kPostConnectSettleMs);

  lens.writeChar = nullptr;
  lens.notifyChar = nullptr;
  const BLEUUID writeUuid(g2mentra::kWriteCharUuid);
  const BLEUUID notifyUuid(g2mentra::kNotifyCharUuid);
  for (uint8_t attempt = 1;
       attempt <= config::kServiceDiscoveryRetries &&
       (lens.writeChar == nullptr || (!lens.isLeft && lens.notifyChar == nullptr));
       ++attempt) {
    if (attempt > 1) {
      delay(config::kServiceDiscoveryRetryDelayMs);
    }

    auto* services = lens.client->getServices();
    const size_t serviceCount = services != nullptr ? services->size() : 0;
    Serial.printf("%s lens service discovery attempt %u: count=%u\n",
                  lens.sideLabel,
                  attempt,
                  static_cast<unsigned>(serviceCount));

    if (services != nullptr) {
      for (const auto& entry : *services) {
        const String discoveredUuid = entry.first.c_str();
        Serial.printf("%s lens discovered service: %s\n",
                      lens.sideLabel,
                      discoveredUuid.c_str());

        BLERemoteService* discoveredService = entry.second;
        if (discoveredService == nullptr) {
          continue;
        }

        if (lens.writeChar == nullptr) {
          BLERemoteCharacteristic* writeCandidate = discoveredService->getCharacteristic(writeUuid);
          if (writeCandidate != nullptr) {
            lens.writeChar = writeCandidate;
            Serial.printf("%s lens matched WRITE characteristic in service %s\n",
                          lens.sideLabel,
                          discoveredUuid.c_str());
          }
        }

        if (!lens.isLeft && lens.notifyChar == nullptr) {
          BLERemoteCharacteristic* notifyCandidate = discoveredService->getCharacteristic(notifyUuid);
          if (notifyCandidate != nullptr) {
            lens.notifyChar = notifyCandidate;
            Serial.printf("%s lens matched NOTIFY characteristic in service %s\n",
                          lens.sideLabel,
                          discoveredUuid.c_str());
          }
        }
      }
    }
  }

  if (lens.writeChar == nullptr) {
    Serial.printf("Write characteristic discovery failed on %s lens\n", lens.sideLabel);
    closeLens(lens);
    return false;
  }

  if (lens.notifyChar != nullptr && lens.notifyChar->canNotify()) {
    lens.notifyChar->registerForNotify(notifyHandler);
  } else if (!lens.isLeft) {
    Serial.println("Right lens notify characteristic missing");
    closeLens(lens);
    return false;
  }

  lens.ready = lens.isLeft ? (lens.writeChar != nullptr) : (lens.writeChar != nullptr && lens.notifyChar != nullptr);
  Serial.printf("%s lens ready=%s\n", lens.sideLabel, lens.ready ? "true" : "false");
  return lens.ready;
}

/**
 * @brief レンズの WRITE キャラクタリスティックへ BLE パケットを書く。
 *
 * @param lens   送信先の LensConnection
 * @param packet 送信する BLE パケット
 * @return true  送信成功
 * @return false 接続切断またはキャラクタリスティックなし
 */
bool writePacket(const LensConnection& lens, const std::vector<uint8_t>& packet) {
  if (!lens.connected || lens.writeChar == nullptr) {
    return false;
  }
  return lens.writeChar->writeValue(const_cast<uint8_t*>(packet.data()), packet.size(), false);
}

/**
 * @brief Protobuf ペイロードを BLE トランスポートフレームにのせ、指定レンズに送信する。
 *
 * buildBlePackets() で複数パケットに分割し、順に書く。
 * マルチパケットの場合は kBleBurstGapMs の遅延を挿入する。
 *
 * @param serviceId   宛先サービス ID
 * @param payload     送信する Protobuf ペイロード
 * @param reserveFlag BLE パケットヘッダのリザーブフラグ
 * @param sendLeft    true の場合左レンズにも送信する
 * @param sendRight   true の場合右レンズにも送信する
 */
void sendTransport(uint8_t serviceId,
                   const std::vector<uint8_t>& payload,
                   bool reserveFlag,
                   bool sendLeft,
                   bool sendRight) {
  const std::vector<std::vector<uint8_t>> packets =
      g2mentra::buildBlePackets(nextSyncId(), serviceId, payload, reserveFlag);

  for (size_t index = 0; index < packets.size(); ++index) {
    if (sendRight) {
      writePacket(gRight, packets[index]);
    }
    if (sendLeft) {
      writePacket(gLeft, packets[index]);
    }
    if (packets.size() > 1 && index + 1 < packets.size()) {
      delay(config::kBleBurstGapMs);
    }
  }
}

// 以下の send* 関数はサービス ID と送信先レンズを固定したラッパー。
// reserveFlag が true のものはグラス内部のアプリ層コマンド、false は DevSettings 層コマンド。

/// DEVICE_SETTINGS コマンドを送信する (左右第1引数で選択)。
void sendDevSettings(const std::vector<uint8_t>& payload, bool sendLeft, bool sendRight) {
  sendTransport(g2mentra::DEVICE_SETTINGS, payload, false, sendLeft, sendRight);
}

/// G2_SETTING コマンドを右レンズのみに送信する。
void sendG2Setting(const std::vector<uint8_t>& payload) {
  sendTransport(g2mentra::G2_SETTING, payload, true, false, true);
}

/// ONBOARDING コマンドを右レンズのみに送信する。
void sendOnboarding(const std::vector<uint8_t>& payload) {
  sendTransport(g2mentra::ONBOARDING, payload, true, false, true);
}

/// GESTURE_CTRL コマンドを右レンズのみに送信する。
void sendGestureCtrl(const std::vector<uint8_t>& payload) {
  sendTransport(g2mentra::GESTURE_CTRL, payload, true, false, true);
}

/// UI_SETTING コマンドを右レンズのみに送信する。
void sendUiSetting(const std::vector<uint8_t>& payload) {
  sendTransport(g2mentra::UI_SETTING, payload, true, false, true);
}

/// EVEN_HUB_CTRL コマンドを右レンズのみに送信する。
void sendEvenHubCtrl(const std::vector<uint8_t>& payload) {
  sendTransport(g2mentra::EVEN_HUB_CTRL, payload, true, false, true);
}

/// DASHBOARD コマンドを右レンズのみに送信する。
void sendDashboard(const std::vector<uint8_t>& payload) {
  sendTransport(g2mentra::DASHBOARD, payload, true, false, true);
}

/// EVEN_HUB コマンドを右レンズのみに送信する。
void sendEvenHub(const std::vector<uint8_t>& payload) {
  sendTransport(g2mentra::EVEN_HUB, payload, true, false, true);
}

/**
 * @brief ブートステップを切り替え、また次回実行の最小待機時間を設定する。
 *
 * @param step    遷移先の BootStep
 * @param delayMs 遷移までの最小待機時間 (ミリ秒)
 */
void scheduleBootStep(BootStep step, uint32_t delayMs) {
  gBootStep = step;
  gNextBootActionAtMs = millis() + delayMs;
}

/**
 * @brief TransportAssembler が完成したサービスパケットを処理する。
 *
 * EVEN_HUB タップイベントはカウンターインクリメントに変換する。
 * DEVICE_SETTINGS 認証レスポンスは認証フラグを更新する。
 *
 * @param lens      受信レンズの LensConnection
 * @param serviceId 完成メッセージのサービス ID
 * @param payload   完成メッセージのペイロード
 */
void handleServicePacket(LensConnection& lens,
                         uint8_t serviceId,
                         const std::vector<uint8_t>& payload) {
  if (serviceId == g2mentra::EVEN_HUB && g2mentra::isEvenHubTapEvent(payload)) {
    Serial.println("Queued counter increment from G2 tap");
    queueRemoteTap();
    return;
  }

  if (serviceId != g2mentra::DEVICE_SETTINGS) {
    return;
  }

  bool secAuth = false;
  if (g2mentra::parseDevSettingsAuthResponse(payload, secAuth) && secAuth) {
    lens.authenticated = true;
    Serial.printf("Auth OK from %s lens\n", lens.sideLabel);
  }
}

/**
 * @brief BLE NOTIFY コールバックから呼び出され、生パケットを TransportAssembler に渡す。
 *
 * パケットが完成したら handleServicePacket() に委譲する。
 */
void handleNotifyPacket(LensConnection& lens, uint8_t* data, size_t length) {
  std::vector<uint8_t> payload;
  uint8_t serviceId = 0;
  if (!lens.assembler.handlePacket(data, length, serviceId, payload)) {
    return;
  }
  handleServicePacket(lens, serviceId, payload);
}

/**
 * @brief BLE 切断時のコールバックハンドラ。
 *
 * 左レンズ切断時、右レンズがまだ接続中なら右単体モードで継続する。
 * それ以外の場合はセッション失敗フラグを立て、再接続をトリガーする。
 */
void onLensDisconnected(LensConnection& lens) {
  Serial.printf("%s lens disconnected\n", lens.sideLabel);
  lens.connected = false;
  lens.ready = false;
  lens.authenticated = false;

  if (lens.isLeft && gRight.connected) {
    Serial.println("LEFT lens is optional for right-only mode; continuing session");
    return;
  }

  gBootReady = false;
  gSetupComplete = false;
  gSessionStartFailed = true;
  gLastStartAttemptAtMs = millis();
}

/**
 * @brief ブートシーケンスの現在ステップを実行する。
 *
 * loop() 毎回呼び出す。タイマー以前に帰ってきた場合は何もしない。
 * 各 case は該当のコマンドを送信し、次のステップをスケジュールする。
 */
void processBootSequence() {
  if (gBootStep == BootStep::Idle || millis() < gNextBootActionAtMs) {
    return;
  }

  switch (gBootStep) {
    case BootStep::SendAuthLeft:
      if (!gLeft.connected) {
        scheduleBootStep(BootStep::SendAuthRight, 0);
        break;
      }
      Serial.println("Sending auth to left lens");
      sendDevSettings(g2mentra::buildAuthCommand(nextMagicRandom()), true, false);
      scheduleBootStep(BootStep::SendAuthRight, config::kStepGapMs);
      break;

    case BootStep::SendAuthRight:
      Serial.println("Sending auth to right lens");
      sendDevSettings(g2mentra::buildAuthCommand(nextMagicRandom()), false, true);
      scheduleBootStep(BootStep::SendPipeRoleChange, config::kStepGapMs);
      break;

    case BootStep::SendPipeRoleChange:
      Serial.println("Sending pipe role change");
      sendDevSettings(g2mentra::buildPipeRoleChangeCommand(nextMagicRandom()), false, true);
      scheduleBootStep(BootStep::SendTimeSync, config::kStepGapMs);
      break;

    case BootStep::SendTimeSync:
      Serial.println("Sending time sync");
      sendDevSettings(g2mentra::buildTimeSyncCommand(nextMagicRandom(),
                                                     currentUnixSeconds(),
                                                     config::kTimezoneHours),
                      false,
                      true);
      gAuthSequenceStartedAtMs = millis();
      scheduleBootStep(BootStep::WaitAuth, 300);
      break;

    case BootStep::WaitAuth:
      if (gRight.authenticated && (!isLeftLensRequiredForBoot() || gLeft.authenticated)) {
        if (gLeft.connected) {
          Serial.println("Both lenses authenticated");
        } else {
          Serial.println("Right lens authenticated; continuing in right-only mode");
        }
        scheduleBootStep(BootStep::SendOnboardingSkip, 0);
      } else if ((millis() - gAuthSequenceStartedAtMs) >= config::kAuthWaitTimeoutMs) {
        Serial.println("Auth wait timeout; continuing with right-lens init");
        scheduleBootStep(BootStep::SendOnboardingSkip, 0);
      } else {
        scheduleBootStep(BootStep::WaitAuth, 100);
      }
      break;

    case BootStep::SendOnboardingSkip:
      Serial.println("Skipping onboarding");
      sendOnboarding(g2mentra::buildOnboardingSkipCommand(nextMagicRandom()));
      scheduleBootStep(BootStep::SendUniverseSettings, config::kStepGapMs);
      break;

    case BootStep::SendUniverseSettings:
      Serial.println("Applying universe settings");
      sendG2Setting(g2mentra::buildUniverseSettingsCommand(nextMagicRandom()));
      scheduleBootStep(BootStep::SendGestureInit, config::kStepGapMs);
      break;

    case BootStep::SendGestureInit:
      Serial.println("Initializing gesture control");
      sendGestureCtrl(g2mentra::buildGestureInitCommand(nextMagicRandom()));
      scheduleBootStep(BootStep::SendUiSettingQuery, config::kStepGapMs);
      break;

    case BootStep::SendUiSettingQuery:
      Serial.println("Querying UI settings");
      sendUiSetting(g2mentra::buildUiSettingQueryCommand(nextMagicRandom()));
      scheduleBootStep(BootStep::SendEvenHubCtrlInit, config::kStepGapMs);
      break;

    case BootStep::SendEvenHubCtrlInit:
      Serial.println("Initializing EvenHub control channel");
      sendEvenHubCtrl(g2mentra::buildEvenHubCtrlInitCommand(nextMagicRandom()));
      scheduleBootStep(BootStep::SendDashboardInit, config::kStepGapMs);
      break;

    case BootStep::SendDashboardInit:
      Serial.println("Initializing dashboard service");
      sendDashboard(g2mentra::buildDashboardInitCommand(nextMagicRandom()));
      scheduleBootStep(BootStep::SendGestureControlList, config::kStepGapMs);
      break;

    case BootStep::SendGestureControlList:
      Serial.println("Sending gesture control list");
      sendG2Setting(g2mentra::buildGestureControlListCommand(nextMagicRandom()));
      scheduleBootStep(BootStep::SendDashboardNewsRequest, config::kStepGapMs);
      break;

    case BootStep::SendDashboardNewsRequest:
      Serial.println("Requesting dashboard news info");
      sendDashboard(g2mentra::buildDashboardNewsRequestCommand(nextMagicRandom()));
      scheduleBootStep(BootStep::SendDashboardAppNewsRequest, config::kStepGapMs);
      break;

    case BootStep::SendDashboardAppNewsRequest:
      Serial.println("Requesting dashboard app news info");
      sendDashboard(g2mentra::buildDashboardAppNewsRequestCommand(nextMagicRandom()));
      scheduleBootStep(BootStep::ShowHelloPage, 500);
      break;

    case BootStep::ShowHelloPage:
      Serial.println("Sending counter page");
      sendEvenHub(g2mentra::buildCreateTextPageCommand(nextMagicRandom(),
                                                       buildCounterText(),
                                                       config::kCounterTextContainerId,
                                                       true));
      gBootReady = true;
      gCounterDirty = false;
      gLastEvenHubHeartbeatAtMs = millis();
      gLastDevSettingsHeartbeatAtMs = millis();
      scheduleBootStep(BootStep::Ready, 0);
      break;

    case BootStep::Ready:
      break;

    case BootStep::Idle:
      break;
  }
}

/**
 * @brief 期限が来た EvenHub / DevSettings ハートビートを送信する。
 *
 * kHeartbeatIntervalMs ごとに 2 種類のハートビートを送信する。
 * グラスが接続中でない場合は何もしない。
 */
void sendHeartbeatsIfDue() {
  if (!gBootReady || !gRight.connected) {
    return;
  }

  const uint32_t nowMs = millis();
  if ((nowMs - gLastEvenHubHeartbeatAtMs) >= config::kHeartbeatIntervalMs) {
    sendEvenHub(g2mentra::buildEvenHubHeartbeatCommand(0));
    gLastEvenHubHeartbeatAtMs = nowMs;
    Serial.println("Sent EvenHub heartbeat");
  }

  if ((nowMs - gLastDevSettingsHeartbeatAtMs) >= config::kHeartbeatIntervalMs) {
    sendDevSettings(g2mentra::buildDevSettingsHeartbeatCommand(nextMagicRandom()), false, true);
    gLastDevSettingsHeartbeatAtMs = nowMs;
    Serial.println("Sent DevSettings heartbeat");
  }
}

/**
 * @brief Even G2 セッションを開始する。
 *
 * 実行順序:
 *   1. 状態をリセットする
 *   2. BLE スキャン / キャッシュでレンズを発見する
 *   3. 右レンズに接続する (必須)
 *   4. 左レンズに接続する (任意; 失敗時は右単体モードで継続)
 *   5. ブートシーケンスをキューに登録する
 *
 * @return true  少なくとも右レンズに接続できた
 * @return false 右レンズ接続または発見に失敗した
 */
bool startSession() {
  Serial.println();
  Serial.println("=== Even G2 bring-up start ===");
  resetSessionState();

  if (!discoverTargets()) {
    Serial.println("Could not discover the right G2 lens");
    return false;
  }

  if (!connectLens(gRight, rightNotifyCallback)) {
    Serial.println("Right lens connection failed");
    return false;
  }

  bool leftConnected = false;
  if (!gLeft.address.isEmpty()) {
    leftConnected = connectLens(gLeft, leftNotifyCallback);
    if (!leftConnected) {
      Serial.println("Left lens connection failed; continuing in right-only mode");
    }
  } else {
    Serial.println("Left lens not discovered; continuing in right-only mode");
  }

  delay(500);
  scheduleBootStep(leftConnected ? BootStep::SendAuthLeft : BootStep::SendAuthRight, 200);
  gSetupComplete = true;
  gSessionStartFailed = false;
  if (leftConnected) {
    Serial.println("Both lenses connected; boot sequence queued");
  } else {
    Serial.println("Right lens connected; right-only boot sequence queued");
  }
  return true;
}

/**
 * @brief Arduino 初期化関数。
 *
 * M5AtomS3 のディスプレイ・シリアルを初期化し、BLE デバイスを起動した後、
 * G2 セッションを開始する。
 */
void setup() {
  Serial.begin(115200);
  auto cfg = M5.config();
  cfg.serial_baudrate = 0;
  cfg.clear_display = true;
  cfg.led_brightness = 0;
  M5.begin(cfg);
  M5.Display.setTextWrap(false);
  renderLocalCounter();
  delay(1200);
  if (config::kEnableEspVerboseLogs) {
    Serial.setDebugOutput(true);
    esp_log_level_set("*", ESP_LOG_VERBOSE);
  }
  Serial.println();
  Serial.println("M5AtomS3 Even G2 Hello sketch");
  BLEDevice::init(config::kLocalClientName);

  gLastStartAttemptAtMs = millis();
  gSessionStartFailed = !startSession();
}

/**
 * @brief Arduino メインループ。
 *
 * 実行順序:
 *   1. pollInputs()                    — ボタン / G2 タップ入力を取得しカウンターを更新
 *   2. processBootSequence()           — 起動シーケンスの未完了ステップを進める
 *   3. sendHeartbeatsIfDue()           — 期限が来たハートビートを送信する
 *   4. syncCounterToGlassesIfNeeded()  — 変更されたカウンターをグラスに書く
 *   5. 切断時は kRetryDelayMs 待機後に再接続する
 */
void loop() {
  pollInputs();
  processBootSequence();
  sendHeartbeatsIfDue();
  syncCounterToGlassesIfNeeded();

  if (!gSetupComplete && gSessionStartFailed &&
      (millis() - gLastStartAttemptAtMs) >= config::kRetryDelayMs) {
    gLastStartAttemptAtMs = millis();
    gSessionStartFailed = !startSession();
  }

  delay(10);
}