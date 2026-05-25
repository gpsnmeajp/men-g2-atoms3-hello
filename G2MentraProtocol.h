/**
 * @file G2MentraProtocol.h
 * @brief Even G2 スマートグラス向け BLE トランスポート／プロトコルヘルパー。
 *
 * Mentra OS プロジェクト (MIT) の G2.kt を参考に C++ へ移植したプロトコル実装。
 * BLE パケットの組み立て・分解、軽量 Protobuf エンコーダ、各種コマンドビルダーを提供する。
 * DIRECTION.md の方針に従い、スケッチ本体 (men-g2-atoms3-hello.ino) とは完全に分離されている。
 */
#pragma once

#include <Arduino.h>

#include <vector>

// Mentra OS プロジェクト (MIT) の G2.kt を参考に C++ へ移植したプロトコルヘルパー。
// DIRECTION.md の方針に従い、スケッチ本体とは完全に分離されている。
namespace g2mentra {

// ─── BLE サービス / キャラクタリスティック UUID ────────────────────────────────
/// Even G2 デバイスが公開する独自 BLE サービスの UUID。
constexpr char kServiceUuid[] = "00002760-08C2-11E1-9073-0E8AC72E0000";
/// G2 へコマンドを書き込む WRITE キャラクタリスティック UUID。
constexpr char kWriteCharUuid[] = "00002760-08C2-11E1-9073-0E8AC72E5401";
/// G2 からの通知を受け取る NOTIFY キャラクタリスティック UUID。
constexpr char kNotifyCharUuid[] = "00002760-08C2-11E1-9073-0E8AC72E5402";

// ─── トランスポート層の固定値 ──────────────────────────────────────────────────
/// BLE パケットの先頭を示す同期バイト (0xAA)。
constexpr uint8_t kHeaderByte = 0xAA;
/// 送信元識別子: スマートフォン (= このデバイス)。
constexpr uint8_t kSourcePhone = 0x01;
/// 送信先識別子: グラス本体。
constexpr uint8_t kDestGlasses = 0x02;
/// 1 BLE パケットに収められるペイロードの最大バイト数。
/// BLE ATT MTU の制約に合わせて 236 バイトとしている。
constexpr size_t kMaxPacketPayload = 236;

/**
 * @brief G2 BLE トランスポートで使用するサービス ID 一覧。
 *
 * 各 ID はトランスポートヘッダの serviceId フィールドに設定し、
 * グラス側でどのサブシステムへルーティングするかを示す。
 */
enum ServiceId : uint8_t {
  DASHBOARD       = 0x01,  ///< ダッシュボード (時計・天気などのウィジェット表示)
  MENU            = 0x03,  ///< メニュー操作
  EVEN_AI         = 0x07,  ///< Even AI アシスタント
  G2_SETTING      = 0x09,  ///< G2 本体設定 (ジェスチャーリスト・ユニバース設定など)
  UI_SETTING      = 0x0C,  ///< UI 表示設定 (輝度・フォントなど)
  GESTURE_CTRL    = 0x0D,  ///< ジェスチャーコントロール初期化・設定
  ONBOARDING      = 0x10,  ///< 初回セットアップ (オンボーディング) フロー制御
  DEVICE_SETTINGS = 0x80,  ///< デバイス設定: 認証・時刻同期・ハートビートなど
  EVEN_HUB_CTRL   = 0x81,  ///< EvenHub コントロールチャンネル (セッション管理)
  EVEN_HUB        = 0xE0,  ///< EvenHub: テキストページ表示・タップイベント受信など
};

/**
 * @brief DEVICE_SETTINGS サービスで使用するコマンド ID。
 *
 * 各コマンドは DevSettings Protobuf メッセージの field 1 (command_id) に設定する。
 */
enum DevCfgCommandId : uint8_t {
  AUTHENTICATION       = 4,   ///< ペアリング認証コマンド
  PIPE_ROLE_CHANGE     = 5,   ///< パイプ役割変更 (このデバイスをプライマリクライアントとして登録)
  RING_CONNECT_INFO    = 6,   ///< リング接続情報通知
  BASE_CONN_HEART_BEAT = 14,  ///< 基本接続ハートビート (疎通確認用の定期送信)
  TIME_SYNC            = 128, ///< 現在時刻・タイムゾーン同期コマンド
};

/**
 * @brief EVEN_HUB サービスで使用するコマンド ID。
 *
 * 各コマンドは EvenHub Protobuf メッセージの field 1 (command_id) に設定する。
 */
enum EvenHubCommandId : uint8_t {
  CREATE_STARTUP_PAGE   = 0,  ///< テキストページ新規作成 (起動時のページ初期化)
  UPDATE_IMAGE_RAW_DATA = 3,  ///< 画像データ更新 (未使用)
  UPDATE_TEXT_DATA      = 5,  ///< 既存テキストページのテキスト内容を更新
  REBUILD_PAGE          = 7,  ///< ページレイアウトの再構築
  SHUTDOWN_PAGE         = 9,  ///< ページのシャットダウン (非表示)
  HEARTBEAT             = 12, ///< EvenHub ハートビート (セッション維持用の定期送信)
  AUDIO_CONTROL         = 15, ///< 音声制御 (未使用)
};

/**
 * @brief G2 から届く EvenHub レスポンスのコマンド ID。
 */
enum EvenHubResponseCommandId : uint8_t {
  OS_NOTIFY_EVENT_TO_APP = 2, ///< OS からアプリへのイベント通知 (タップ・スクロールなど)
};

/**
 * @brief OS からアプリへ通知されるイベント種別。
 *
 * isEvenHubTapEvent() がシングルタップ (クリック) の検出に使用する。
 */
enum OsEventType : uint8_t {
  OS_EVENT_CLICK            = 0, ///< シングルタップ (クリック)
  OS_EVENT_SCROLL_TOP       = 1, ///< 上方向スクロール
  OS_EVENT_SCROLL_BOTTOM    = 2, ///< 下方向スクロール
  OS_EVENT_DOUBLE_CLICK     = 3, ///< ダブルタップ
  OS_EVENT_FOREGROUND_ENTER = 4, ///< アプリがフォアグラウンドに移行
  OS_EVENT_FOREGROUND_EXIT  = 5, ///< アプリがバックグラウンドに移行
  OS_EVENT_ABNORMAL_EXIT    = 6, ///< 異常終了によるアプリ強制終了
  OS_EVENT_SYSTEM_EXIT      = 7, ///< システムによるアプリ終了
};

/**
 * @brief 軽量 Protobuf エンコーダ。
 *
 * Google Protobuf の wire format サブセットをバイト列として組み立てる。
 * デコード機能は持たず、G2 へ送信するコマンドの構築にのみ使用する。
 * 使い捨てインスタンスとして生成し、take() でバイト列を取り出す。
 */
class ProtoWriter {
 public:
  /// Base-128 varint をバッファへ書き込む。
  void writeVarint(uint64_t value);

  /// wire type 0 (varint) のフィールドを書き込む。
  /// @param fieldNumber Protobuf フィールド番号
  /// @param value       符号付き 32 ビット整数値 (varint エンコード)
  void writeInt32Field(uint32_t fieldNumber, int32_t value);

  /// bool 値を wire type 0 フィールドとして書き込む。
  void writeBoolField(uint32_t fieldNumber, bool value);

  /// 生バイト配列を wire type 2 (length-delimited) フィールドとして書き込む。
  void writeBytesField(uint32_t fieldNumber, const uint8_t* value, size_t length);

  /// std::vector<uint8_t> を wire type 2 フィールドとして書き込む。
  void writeBytesField(uint32_t fieldNumber, const std::vector<uint8_t>& value);

  /// Arduino String を wire type 2 (UTF-8 string) フィールドとして書き込む。
  void writeStringField(uint32_t fieldNumber, const String& value);

  /// ネストされた Protobuf メッセージを wire type 2 フィールドとして書き込む。
  void writeMessageField(uint32_t fieldNumber, const std::vector<uint8_t>& subMessage);

  /// 内部バッファへの const 参照を返す (コピーなし)。
  const std::vector<uint8_t>& bytes() const;

  /// 内部バッファを move して返す。呼び出し後バッファは空になる。
  std::vector<uint8_t> take();

 private:
  std::vector<uint8_t> buffer_; ///< エンコード済みバイト列の蓄積バッファ
};

/**
 * @brief マルチパケット BLE フレームを再組み立てするステートマシン。
 *
 * G2 は大きなペイロードを複数の BLE パケットに分割して送信する。
 * このクラスは NOTIFY コールバックで受け取った生パケットを順次 handlePacket() に
 * 渡すことで、完全なペイロードが揃った時点で serviceId と payload を返す。
 *
 * 各 LensConnection に 1 インスタンスを持ち、状態は非スレッドセーフ。
 */
class TransportAssembler {
 public:
  /**
   * @brief BLE 生パケットを処理し、完全なペイロードが揃ったら true を返す。
   *
   * @param rawData   BLE NOTIFY で受信した生バイト列
   * @param length    rawData の長さ
   * @param serviceId [out] 完成したメッセージのサービス ID
   * @param payload   [out] 完成したメッセージのペイロード (Protobuf バイト列)
   * @return true  1 つの完全なメッセージが揃った (serviceId, payload が有効)
   * @return false まだ途中パケット、またはエラー
   */
  bool handlePacket(const uint8_t* rawData,
                    size_t length,
                    uint8_t& serviceId,
                    std::vector<uint8_t>& payload);

 private:
  bool active_ = false;               ///< マルチパケット受信が進行中かどうか
  uint8_t expectedServiceId_ = 0;    ///< 進行中シーケンスのサービス ID
  uint8_t expectedSyncId_ = 0;       ///< 進行中シーケンスの同期 ID
  std::vector<uint8_t> partialPayload_; ///< 受信済みチャンクの蓄積バッファ
};

// ─── トランスポート層 ─────────────────────────────────────────────────────────
/**
 * @brief ペイロードを MTU サイズに分割し、BLE パケット列を構築する。
 *
 * 各パケットは 8 バイトのヘッダ + チャンクペイロードで構成され、
 * 最終パケットの末尾にはペイロード全体の CRC-16 (2 バイト) が付加される。
 *
 * @param syncId      送受信を紐付ける同期 ID (0–255 でラップアラウンド)
 * @param serviceId   宛先サービス ID (ServiceId 列挙値)
 * @param payload     送信したい Protobuf ペイロード全体
 * @param reserveFlag true の場合はヘッダのステータスバイトにリザーブフラグ (0x20) をセット
 * @return 送信順に並んだ BLE パケット列
 */
std::vector<std::vector<uint8_t>> buildBlePackets(uint8_t syncId,
                                                  uint8_t serviceId,
                                                  const std::vector<uint8_t>& payload,
                                                  bool reserveFlag);

// ─── DEVICE_SETTINGS コマンドビルダー ─────────────────────────────────────────

/**
 * @brief 認証コマンドを構築する (AUTHENTICATION)。
 *
 * 接続直後に左右両レンズへ送信し、BLE セッションの認証を行う。
 * @param magicRandom リクエストを一意に識別する 8 ビットカウンタ値
 */
std::vector<uint8_t> buildAuthCommand(uint8_t magicRandom);

/**
 * @brief パイプ役割変更コマンドを構築する (PIPE_ROLE_CHANGE)。
 *
 * 右レンズへ送信し、このデバイスをプライマリ接続クライアントとして登録する。
 * @param magicRandom リクエストを一意に識別する 8 ビットカウンタ値
 */
std::vector<uint8_t> buildPipeRoleChangeCommand(uint8_t magicRandom);

/**
 * @brief 時刻同期コマンドを構築する (TIME_SYNC)。
 *
 * グラスの内部時計を現在の Unix 時刻とタイムゾーンで更新する。
 * @param magicRandom      リクエストを一意に識別する 8 ビットカウンタ値
 * @param timestampSeconds 現在時刻 (Unix エポックからの秒数)
 * @param timezoneHours    UTC からのオフセット時間 (例: JST = +9)
 */
std::vector<uint8_t> buildTimeSyncCommand(uint8_t magicRandom,
                                          int32_t timestampSeconds,
                                          int32_t timezoneHours);

/**
 * @brief DEVICE_SETTINGS ハートビートコマンドを構築する (BASE_CONN_HEART_BEAT)。
 *
 * 定期送信することで BLE 接続の疎通を維持する。
 * @param magicRandom リクエストを一意に識別する 8 ビットカウンタ値
 */
std::vector<uint8_t> buildDevSettingsHeartbeatCommand(uint8_t magicRandom);

// ─── サービス別コマンドビルダー ────────────────────────────────────────────────

/// オンボーディング (初回セットアップ) をスキップするコマンドを構築する。
std::vector<uint8_t> buildOnboardingSkipCommand(uint8_t magicRandom);

/// ユニバース設定 (表示レイアウト全体設定) コマンドを構築する。
std::vector<uint8_t> buildUniverseSettingsCommand(uint8_t magicRandom);

/// ジェスチャーコントロールを初期化するコマンドを構築する。
std::vector<uint8_t> buildGestureInitCommand(uint8_t magicRandom);

/// 現在の UI 設定を照会するコマンドを構築する。
std::vector<uint8_t> buildUiSettingQueryCommand(uint8_t magicRandom);

/// EvenHub コントロールチャンネルを初期化するコマンドを構築する。
std::vector<uint8_t> buildEvenHubCtrlInitCommand(uint8_t magicRandom);

/// ダッシュボードサービスを初期化するコマンドを構築する。
std::vector<uint8_t> buildDashboardInitCommand(uint8_t magicRandom);

/// ジェスチャーコントロールリストを送信するコマンドを構築する。
std::vector<uint8_t> buildGestureControlListCommand(uint8_t magicRandom);

/// ダッシュボードのニュース情報をリクエストするコマンドを構築する。
std::vector<uint8_t> buildDashboardNewsRequestCommand(uint8_t magicRandom);

/// ダッシュボードのアプリニュース情報をリクエストするコマンドを構築する。
std::vector<uint8_t> buildDashboardAppNewsRequestCommand(uint8_t magicRandom);

/// EvenHub ハートビートコマンドを構築する。EvenHub セッションの維持に使用する。
std::vector<uint8_t> buildEvenHubHeartbeatCommand(uint8_t magicRandom);

/**
 * @brief グラスにテキストページを新規作成するコマンドを構築する (CREATE_STARTUP_PAGE)。
 *
 * 起動後に一度だけ呼び出し、表示エリアとテキストコンテナを登録する。
 * @param magicRandom        リクエストを一意に識別する 8 ビットカウンタ値
 * @param text               表示するテキスト文字列
 * @param containerId        テキストコンテナを識別する ID (後から updateText で更新可能)
 * @param enableEventCapture true の場合、タップなどのイベントを通知として受け取る
 */
std::vector<uint8_t> buildCreateTextPageCommand(uint8_t magicRandom,
                                                const String& text,
                                                uint32_t containerId = 1,
                                                bool enableEventCapture = true);

/**
 * @brief 既存テキストコンテナのテキストを更新するコマンドを構築する (UPDATE_TEXT_DATA)。
 *
 * buildCreateTextPageCommand() で作成済みのコンテナに対して使用する。
 * @param magicRandom リクエストを一意に識別する 8 ビットカウンタ値
 * @param text        新しいテキスト文字列
 * @param containerId 更新対象コンテナの ID
 */
std::vector<uint8_t> buildUpdateTextCommand(uint8_t magicRandom,
                                            const String& text,
                                            uint32_t containerId = 1);

// ─── 受信パーサー ─────────────────────────────────────────────────────────────

/**
 * @brief DEVICE_SETTINGS 認証レスポンスを解析し、認証成功フラグを取り出す。
 *
 * @param payload TransportAssembler が返した Protobuf ペイロード
 * @param secAuth [out] true = 認証成功 (sec_auth フラグが true)
 * @return true  解析成功 (commandId == AUTHENTICATION かつペイロードが正常)
 * @return false 解析失敗 (フォーマット不正、または対象フィールドなし)
 */
bool parseDevSettingsAuthResponse(const std::vector<uint8_t>& payload, bool& secAuth);

/**
 * @brief EvenHub ペイロードがタップ (クリック) イベントかどうかを判定する。
 *
 * テキストイベントとシステムイベントの両パスを確認し、
 * どちらかで OS_EVENT_CLICK であれば true を返す。
 *
 * @param payload TransportAssembler が返した EvenHub サービスのペイロード
 * @return true  シングルタップイベント
 * @return false その他のイベント、またはパース失敗
 */
bool isEvenHubTapEvent(const std::vector<uint8_t>& payload);

}  // namespace g2mentra