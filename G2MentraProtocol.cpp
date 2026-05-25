/**
 * @file G2MentraProtocol.cpp
 * @brief Even G2 BLE プロトコルヘルパーの実装。
 *
 * buildBlePackets() で BLE トランスポートフレームを構築し、
 * TransportAssembler でマルチパケットを再組み立てする。
 * 各 buildXxx() 関数は Protobuf エンコードされたコマンドペイロードを返す。
 */
#include "G2MentraProtocol.h"

#include <time.h>

namespace g2mentra {
namespace {

/**
 * @brief G2 トランスポートで使用する CRC-16 を計算する。
 *
 * 標準的な CRC-16/ARC 系アルゴリズムを使用。
 * 各 BLE パケット列の最終パケット末尾 2 バイトに付加され、
 * 受信側がペイロード全体の整合性を検証するために使う。
 *
 * @param data CRC 計算対象のバイト列 (完全なペイロード)
 * @return 16 ビット CRC 値
 */
uint16_t calcCrc16(const std::vector<uint8_t>& data) {
  uint32_t crc = 0xFFFF;
  for (uint8_t value : data) {
    crc = ((crc >> 8) | ((crc << 8) & 0xFF00U)) ^ value;
    crc ^= ((crc & 0xFFU) >> 4);
    crc ^= ((crc << 12) & 0xFFFFU);
    crc ^= (((crc & 0xFFU) << 5) & 0xFFFFU);
  }
  return static_cast<uint16_t>(crc & 0xFFFFU);
}

/**
 * @brief Protobuf Base-128 varint をバイト列から読み取る。
 *
 * データの offset バイト目を起点として varint を解釈し、
 * 読み取り後に offset を次のフィールド先頭へ進める。
 * 最大 63 ビット (9 バイト) までの varint をサポート。
 *
 * @param data   読み取り元バイト列
 * @param offset [in/out] 読み取り開始位置。成功時は次の読み取り位置へ更新
 * @param value  [out] デコードした varint 値
 * @return true  読み取り成功
 * @return false バッファ不足またはオーバーフロー
 */
bool readVarint(const std::vector<uint8_t>& data, size_t& offset, uint64_t& value) {
  value = 0;
  uint8_t shift = 0;
  while (offset < data.size()) {
    const uint8_t byte = data[offset++];
    value |= (static_cast<uint64_t>(byte & 0x7F) << shift);
    if ((byte & 0x80) == 0) {
      return true;
    }
    shift += 7;
    if (shift > 63) {
      return false;
    }
  }
  return false;
}

/**
 * @brief Protobuf フィールドを wire type に応じてスキップする。
 *
 * 目的のフィールドに到達するまで無陡係なフィールドを飛び越すために使用する。
 * 以下の wire type に対応:
 *   0 = varint, 1 = 64-bit fixed, 2 = length-delimited, 5 = 32-bit fixed
 *
 * @param data     パース対象のバイト列
 * @param offset   [in/out] フィールドデータの開始位置。成功時は次フィールド先頭へ更新
 * @param wireType スキップするフィールドの wire type (tag & 0x07)
 * @return true  スキップ成功
 * @return false 未対応の wire type またはバッファ不足
 */
bool skipField(const std::vector<uint8_t>& data, size_t& offset, uint8_t wireType) {
  uint64_t value = 0;
  switch (wireType) {
    case 0:
      return readVarint(data, offset, value);
    case 1:
      if (offset + 8 > data.size()) {
        return false;
      }
      offset += 8;
      return true;
    case 2: {
      if (!readVarint(data, offset, value)) {
        return false;
      }
      if (offset + value > data.size()) {
        return false;
      }
      offset += static_cast<size_t>(value);
      return true;
    }
    case 5:
      if (offset + 4 > data.size()) {
        return false;
      }
      offset += 4;
      return true;
    default:
      return false;
  }
}

/**
 * @brief Protobuf バイト列から指定フィールド番号の varint 値を探す。
 *
 * すべてのフィールドを先頭から順に走査し、wire type 0 が一致した時点で値を返す。
 * 最初に見つかったフィールドを返す (Protobuf で同一フィールド番号が複数ある場合は最初のみ)。
 *
 * @param data              パース対象の Protobuf バイト列
 * @param targetFieldNumber 探すフィールド番号
 * @param value             [out] 見つかった場合の varint 値
 * @return true  指定フィールドが見つかり値を取得した
 * @return false フィールドが見つからないまたはパースエラー
 */
bool findVarintField(const std::vector<uint8_t>& data,
                    uint32_t targetFieldNumber,
                    uint64_t& value) {
  size_t offset = 0;
  while (offset < data.size()) {
    uint64_t tag = 0;
    if (!readVarint(data, offset, tag)) {
      return false;
    }

    const uint32_t fieldNumber = static_cast<uint32_t>(tag >> 3);
    const uint8_t wireType = static_cast<uint8_t>(tag & 0x07);
    if (wireType == 0) {
      uint64_t fieldValue = 0;
      if (!readVarint(data, offset, fieldValue)) {
        return false;
      }
      if (fieldNumber == targetFieldNumber) {
        value = fieldValue;
        return true;
      }
      continue;
    }

    if (!skipField(data, offset, wireType)) {
      return false;
    }
  }

  return false;
}

/**
 * @brief Protobuf バイト列から指定フィールド番号の bytes/string 値を探す。
 *
 * wire type 2 (length-delimited) のフィールドを先頭から順に走査し、
 * 指定フィールド番号が一致した時点で内容を返す。
 *
 * @param data              パース対象の Protobuf バイト列
 * @param targetFieldNumber 探すフィールド番号
 * @param value             [out] 見つかった場合の bytes データ
 * @return true  指定フィールドが見つかり値を取得した
 * @return false フィールドが見つからないまたはパースエラー
 */
bool findBytesField(const std::vector<uint8_t>& data,
                   uint32_t targetFieldNumber,
                   std::vector<uint8_t>& value) {
  size_t offset = 0;
  while (offset < data.size()) {
    uint64_t tag = 0;
    if (!readVarint(data, offset, tag)) {
      return false;
    }

    const uint32_t fieldNumber = static_cast<uint32_t>(tag >> 3);
    const uint8_t wireType = static_cast<uint8_t>(tag & 0x07);
    if (wireType == 2) {
      uint64_t length = 0;
      if (!readVarint(data, offset, length)) {
        return false;
      }
      if (offset + length > data.size()) {
        return false;
      }
      if (fieldNumber == targetFieldNumber) {
        value.assign(data.begin() + offset, data.begin() + offset + length);
        return true;
      }
      offset += static_cast<size_t>(length);
      continue;
    }

    if (!skipField(data, offset, wireType)) {
      return false;
    }
  }

  return false;
}

/**
 * @brief EvenHub コマンドの決まりきったフレーム (command_id, magic_random, フィールド) を構築する。
 *
 * EvenHub プロトコルの多くのコマンドは同じトップレベルフレームを共有するため、
 * 共通処理をこのヘルパーに集約している。
 *
 * @param commandId    EvenHubCommandId 列挙値
 * @param subFieldNumber サブメッセージを格納するフィールド番号
 * @param subMessage   コマンド固有のサブメッセージ (Protobuf バイト列)
 * @param magicRandom  リクエストを一意に識別する 8 ビットカウンタ値
 * @return エンコードされた EvenHub Protobuf メッセージバイト列
 */
std::vector<uint8_t> buildEvenHubMessage(uint8_t commandId,
                                         uint32_t subFieldNumber,
                                         const std::vector<uint8_t>& subMessage,
                                         uint8_t magicRandom) {
  ProtoWriter writer;
  writer.writeInt32Field(1, commandId);
  writer.writeInt32Field(2, magicRandom);
  writer.writeMessageField(subFieldNumber, subMessage);
  return writer.take();
}

/**
 * @brief EvenHub テキストコンテナの Protobuf メッセージを構築する。
 *
 * テキスト表示エリアの位置・サイズ・整列・コンテナ ID などを指定する。
 * 座標は Even G2 の画面解像度 (576x288) を基準としている。
 *
 * @param containerId        将来の UPDATE_TEXT_DATA で照射するためのコンテナ ID
 * @param text               初期表示テキスト
 * @param enableEventCapture true の場合、タップなどの OS イベントを受此する
 * @return エンコードされた TextContainer Protobuf メッセージバイト列
 */
std::vector<uint8_t> buildTextContainerMessage(uint32_t containerId,
                                              const String& text,
                                              bool enableEventCapture) {
  ProtoWriter writer;
  writer.writeInt32Field(1, 0);
  writer.writeInt32Field(2, 0);
  writer.writeInt32Field(3, 576);
  writer.writeInt32Field(4, 288);
  writer.writeInt32Field(5, 0);
  writer.writeInt32Field(6, 0);
  writer.writeInt32Field(7, 0);
  writer.writeInt32Field(8, 4);
  writer.writeInt32Field(9, static_cast<int32_t>(containerId));
  writer.writeStringField(10, "text-main");
  writer.writeInt32Field(11, enableEventCapture ? 1 : 0);
  writer.writeStringField(12, text);
  return writer.take();
}

}  // namespace

// ─── ProtoWriter 実装 ───────────────────────────────────────────────────────────────

// Base-128 エンコード: 下位 7 ビットの組を繰り返し書き出し、
// 続きバイトがある限り MSB に 1 を立てる。
void ProtoWriter::writeVarint(uint64_t value) {
  while (value > 0x7F) {
    buffer_.push_back(static_cast<uint8_t>((value & 0x7F) | 0x80));
    value >>= 7;
  }
  buffer_.push_back(static_cast<uint8_t>(value & 0x7F));
}

// tag = (fieldNumber << 3) | 0 を varint で先に書き、次に値を符号拡張して varint で書く。
void ProtoWriter::writeInt32Field(uint32_t fieldNumber, int32_t value) {
  writeVarint(static_cast<uint64_t>(fieldNumber) << 3);
  writeVarint(static_cast<uint64_t>(static_cast<int64_t>(value)));
}

// false は 0、true は 1 として writeInt32Field で委譲する。
void ProtoWriter::writeBoolField(uint32_t fieldNumber, bool value) {
  writeInt32Field(fieldNumber, value ? 1 : 0);
}

// tag = (fieldNumber << 3) | 2 を書き、長さを varint で書き、その後生バイトをそのまま追加する。
void ProtoWriter::writeBytesField(uint32_t fieldNumber,
                                  const uint8_t* value,
                                  size_t length) {
  writeVarint((static_cast<uint64_t>(fieldNumber) << 3) | 2U);
  writeVarint(length);
  buffer_.insert(buffer_.end(), value, value + length);
}

// ポインタオーバーロード版。std::vector からデータポインタとサイズを取り出して委譲する。
void ProtoWriter::writeBytesField(uint32_t fieldNumber, const std::vector<uint8_t>& value) {
  writeBytesField(fieldNumber, value.data(), value.size());
}

// Arduino String の c_str() / length() で writeBytesField に委譲する。
void ProtoWriter::writeStringField(uint32_t fieldNumber, const String& value) {
  writeBytesField(fieldNumber,
                  reinterpret_cast<const uint8_t*>(value.c_str()),
                  value.length());
}

// Protobuf におけるネストメッセージは wire type 2 でエンコードされるので writeBytesField に委譲する。
void ProtoWriter::writeMessageField(uint32_t fieldNumber,
                                    const std::vector<uint8_t>& subMessage) {
  writeBytesField(fieldNumber, subMessage);
}

const std::vector<uint8_t>& ProtoWriter::bytes() const {
  return buffer_;
}

std::vector<uint8_t> ProtoWriter::take() {
  return buffer_;
}

// ─── TransportAssembler 実装 ─────────────────────────────────────────────────────

/**
 * BLE パケットファーマット:
 *   [0]  = kHeaderByte (0xAA)
 *   [1]  = (dest << 4) | src
 *   [2]  = syncId       — この送信シーケンス全体で共通の ID
 *   [3]  = payloadLen   — このパケットのペイロードバイト数内 (CRC 2 バイト含む)
 *   [4]  = totalPackets — シーケンス内のパケット総数
 *   [5]  = serialNum    — 1 始まりの連番番号
 *   [6]  = serviceId    — ServiceId 列挙値
 *   [7]  = status       — 0x20 = reserve flag, bits [1:4] = result code
 *   [8+] = ペイロードデータ
 *   [末尾 2B] = CRC-16 (最終パケットのみ)
 */
bool TransportAssembler::handlePacket(const uint8_t* rawData,
                                      size_t length,
                                      uint8_t& serviceId,
                                      std::vector<uint8_t>& payload) {
  payload.clear();
  if (length < 8 || rawData[0] != kHeaderByte) {
    return false;
  }

  const uint8_t payloadLen = rawData[3];
  const size_t expectedLength = static_cast<size_t>(payloadLen) + 8;
  if (length < expectedLength) {
    return false;
  }

  const uint8_t syncId = rawData[2];         // 同期 ID: マルチパケット照射用
  const uint8_t totalPackets = rawData[4];   // このシーケンス内の総パケット数
  const uint8_t serialNum = rawData[5];      // このパケットの連番番号 (1 始まり)
  const uint8_t packetServiceId = rawData[6]; // サービス ID
  const uint8_t status = rawData[7];         // ステータスバイト
  const uint8_t resultCode = (status >> 1) & 0x0F; // bits[1:4] = 結果コード。0 以外はエラー
  if (resultCode != 0) {
    return false;
  }

  const bool isLast = serialNum == totalPackets;
  if (isLast && payloadLen < 2) {
    return false;
  }
  const size_t payloadEnd = 8 + payloadLen - (isLast ? 2 : 0);
  if (payloadEnd < 8 || payloadEnd > length) {
    return false;
  }

  if (totalPackets == 1) {
    serviceId = packetServiceId;
    payload.assign(rawData + 8, rawData + payloadEnd);
    return true;
  }

  if (serialNum == 1) {
    active_ = true;
    expectedServiceId_ = packetServiceId;
    expectedSyncId_ = syncId;
    partialPayload_.assign(rawData + 8, rawData + payloadEnd);
    return false;
  }

  if (!active_ || expectedServiceId_ != packetServiceId || expectedSyncId_ != syncId) {
    partialPayload_.clear();
    active_ = false;
    return false;
  }

  partialPayload_.insert(partialPayload_.end(), rawData + 8, rawData + payloadEnd);
  if (!isLast) {
    return false;
  }

  serviceId = packetServiceId;
  payload = partialPayload_;
  partialPayload_.clear();
  active_ = false;
  return true;
}

// ─── BLE パケットビルダー ───────────────────────────────────────────────────────────────

std::vector<std::vector<uint8_t>> buildBlePackets(uint8_t syncId,
                                                  uint8_t serviceId,
                                                  const std::vector<uint8_t>& payload,
                                                  bool reserveFlag) {
  // ペイロードを kMaxPacketPayload バイトのチャンクに分割する。
  // ペイロードが空の場合でも空チャンクを 1 つ作成して送信する。
  std::vector<std::vector<uint8_t>> chunks;
  size_t offset = 0;
  while (offset < payload.size()) {
    const size_t end = min(offset + kMaxPacketPayload, payload.size());
    chunks.emplace_back(payload.begin() + offset, payload.begin() + end);
    offset = end;
  }

  // チャンク数が丁度 kMaxPacketPayload の倍数になると最終チャンクが
  // CRC のみになるケースを防ぐため、空チャンクを末尾に追加する。
  if (chunks.empty()) {
    chunks.emplace_back();
  }
  if (chunks.back().size() == kMaxPacketPayload) {
    chunks.emplace_back();
  }

  // ペイロード全体の CRC-16 を一度だけ計算し、最終パケット末尾に付加する。
  const uint8_t totalPackets = static_cast<uint8_t>(chunks.size());
  const uint16_t crc = calcCrc16(payload);
  std::vector<std::vector<uint8_t>> packets;
  packets.reserve(chunks.size());

  for (size_t index = 0; index < chunks.size(); ++index) {
    const bool isLast = (index + 1) == chunks.size();
    const uint8_t serialNum = static_cast<uint8_t>(index + 1);        // 1 始まりの連番
    const uint8_t status = reserveFlag ? 0x20 : 0x00;                  // リザーブフラグ
    const uint8_t payloadLen = static_cast<uint8_t>(chunks[index].size() + (isLast ? 2 : 0)); // CRC 2B 含む

    std::vector<uint8_t> packet;
    packet.reserve(8 + chunks[index].size() + (isLast ? 2 : 0));
    packet.push_back(kHeaderByte);
    packet.push_back(static_cast<uint8_t>((kDestGlasses << 4) | kSourcePhone));
    packet.push_back(syncId);
    packet.push_back(payloadLen);
    packet.push_back(totalPackets);
    packet.push_back(serialNum);
    packet.push_back(serviceId);
    packet.push_back(status);
    packet.insert(packet.end(), chunks[index].begin(), chunks[index].end());
    if (isLast) {
      packet.push_back(static_cast<uint8_t>(crc & 0xFF));
      packet.push_back(static_cast<uint8_t>((crc >> 8) & 0xFF));
    }
    packets.push_back(packet);
  }

  return packets;
}

// ─── DEVICE_SETTINGS コマンドビルダー 実装 ───────────────────────────────────────────

// 認証リクエストを符号化する。
// field 1 = AUTHENTICATION、field 2 = magicRandom、
// field 3 = AuthRequest { field1 = true (永続認証)、field2 = 4 (デバイス種別コード) }
std::vector<uint8_t> buildAuthCommand(uint8_t magicRandom) {
  ProtoWriter authWriter;
  authWriter.writeBoolField(1, true);
  authWriter.writeInt32Field(2, 4);

  ProtoWriter writer;
  writer.writeInt32Field(1, AUTHENTICATION);
  writer.writeInt32Field(2, magicRandom);
  writer.writeMessageField(3, authWriter.take());
  return writer.take();
}

// パイプ役割を「Phone」に変更する。
// field 4 = PipeRoleChange { field1 = 1 (Phone ロール) }
std::vector<uint8_t> buildPipeRoleChangeCommand(uint8_t magicRandom) {
  ProtoWriter roleWriter;
  roleWriter.writeInt32Field(1, 1);

  ProtoWriter writer;
  writer.writeInt32Field(1, PIPE_ROLE_CHANGE);
  writer.writeInt32Field(2, magicRandom);
  writer.writeMessageField(4, roleWriter.take());
  return writer.take();
}

// Unix エポック秒と UTC オフセット時間を送信する。
// フィールド番号 128 はサブメッセージの格納で TIME_SYNC に固有の大きな値。
std::vector<uint8_t> buildTimeSyncCommand(uint8_t magicRandom,
                                          int32_t timestampSeconds,
                                          int32_t timezoneHours) {
  ProtoWriter timeWriter;
  timeWriter.writeInt32Field(1, timestampSeconds);
  timeWriter.writeInt32Field(2, timezoneHours);

  ProtoWriter writer;
  writer.writeInt32Field(1, TIME_SYNC);
  writer.writeInt32Field(2, magicRandom);
  writer.writeMessageField(128, timeWriter.take());
  return writer.take();
}

// 空のサブメッセージを field 13 に格納するだけのシンプルなハートビート。
std::vector<uint8_t> buildDevSettingsHeartbeatCommand(uint8_t magicRandom) {
  ProtoWriter writer;
  writer.writeInt32Field(1, BASE_CONN_HEART_BEAT);
  writer.writeInt32Field(2, magicRandom);
  writer.writeMessageField(13, std::vector<uint8_t>{});
  return writer.take();
}

// ─── その他コマンドビルダー 実装 ─────────────────────────────────────────────────

// オンボーディング ID = 4 (スキップ)。command_id = 1, magic = magicRandom。
std::vector<uint8_t> buildOnboardingSkipCommand(uint8_t magicRandom) {
  ProtoWriter configWriter;
  configWriter.writeInt32Field(1, 4);

  ProtoWriter writer;
  writer.writeInt32Field(1, 1);
  writer.writeInt32Field(2, magicRandom);
  writer.writeMessageField(3, configWriter.take());
  return writer.take();
}

// 各フィールドは ユニバースレイアウト (Even OS 固有の画面構成) を定義する blob。
// バイナリ内容は G2.kt から移植したもので、内部フィールドの詳細は未開示。
std::vector<uint8_t> buildUniverseSettingsCommand(uint8_t magicRandom) {
  const uint8_t universeBlob[] = {
      0x4A, 0x0A, 0x08, 0x00, 0x10, 0x00, 0x18, 0x01, 0x20, 0x00, 0x28, 0x01,
  };

  ProtoWriter writer;
  writer.writeInt32Field(1, 1);
  writer.writeInt32Field(2, magicRandom);
  writer.writeBytesField(3, universeBlob, sizeof(universeBlob));
  return writer.take();
}

// command_id = 0 (GESTURE_CTRL 初期化)。その他のフィールドはなし。
std::vector<uint8_t> buildGestureInitCommand(uint8_t magicRandom) {
  ProtoWriter writer;
  writer.writeInt32Field(1, 0);
  writer.writeInt32Field(2, magicRandom);
  return writer.take();
}

// リクエスト blob には 設定照会フラグ (0x08 0x01) とパディングが含まれる。
std::vector<uint8_t> buildUiSettingQueryCommand(uint8_t magicRandom) {
  const uint8_t requestBlob[] = {0x08, 0x01, 0x10, 0x00};

  ProtoWriter writer;
  writer.writeInt32Field(1, 2);
  writer.writeInt32Field(2, magicRandom);
  writer.writeBytesField(4, requestBlob, sizeof(requestBlob));
  return writer.take();
}

// command_id = 1, 空のサブメッセージを field 3 に格納するだけのシンプルな初期化コマンド。
std::vector<uint8_t> buildEvenHubCtrlInitCommand(uint8_t magicRandom) {
  ProtoWriter writer;
  writer.writeInt32Field(1, 1);
  writer.writeInt32Field(2, magicRandom);
  writer.writeMessageField(3, std::vector<uint8_t>{});
  return writer.take();
}

// ダッシュボードビューモード、ウィジェット表示順序、ステータスバー表示順序を設定する。
// 内容は G2.kt から移植された固定値であり、グラスのデフォルトダッシュボード表示を再現する。
std::vector<uint8_t> buildDashboardInitCommand(uint8_t magicRandom) {
  const uint8_t statusDisplayOrder[] = {1, 2, 3};
  const uint8_t widgetDisplayOrder[] = {1, 3, 2, 2};

  ProtoWriter displayWriter;
  displayWriter.writeInt32Field(1, 4);
  displayWriter.writeInt32Field(2, 3);
  displayWriter.writeBytesField(3, statusDisplayOrder, sizeof(statusDisplayOrder));
  displayWriter.writeInt32Field(4, 4);
  displayWriter.writeBytesField(5, widgetDisplayOrder, sizeof(widgetDisplayOrder));
  displayWriter.writeInt32Field(6, 1);
  displayWriter.writeInt32Field(7, 1);

  ProtoWriter receiveWriter;
  receiveWriter.writeMessageField(2, displayWriter.take());

  ProtoWriter writer;
  writer.writeInt32Field(1, 2);
  writer.writeInt32Field(2, magicRandom);
  writer.writeMessageField(4, receiveWriter.take());
  return writer.take();
}

// 3 種類のジェスチャー (tap_0/tap_1/tap_2) を登録するパケット。
// gestureBlob は G2.kt から移植した固定バイナリシーケンス。
std::vector<uint8_t> buildGestureControlListCommand(uint8_t magicRandom) {
  const uint8_t gestureBlob[] = {
      0x52, 0x18, 0x0A, 0x06, 0x08, 0x00, 0x10, 0x00, 0x18, 0x00,
      0x0A, 0x06, 0x08, 0x00, 0x10, 0x01, 0x18, 0x00,
      0x0A, 0x06, 0x08, 0x00, 0x10, 0x02, 0x18, 0x00,
  };

  ProtoWriter writer;
  writer.writeInt32Field(1, 1);
  writer.writeInt32Field(2, magicRandom);
  writer.writeBytesField(3, gestureBlob, sizeof(gestureBlob));
  return writer.take();
}

// command_id = 5, field 7 にリクエスト blob。
std::vector<uint8_t> buildDashboardNewsRequestCommand(uint8_t magicRandom) {
  const uint8_t requestBlob[] = {0x08, 0x01};

  ProtoWriter writer;
  writer.writeInt32Field(1, 5);
  writer.writeInt32Field(2, magicRandom);
  writer.writeBytesField(7, requestBlob, sizeof(requestBlob));
  return writer.take();
}

// command_id = 7, field 9 にリクエスト blob。
std::vector<uint8_t> buildDashboardAppNewsRequestCommand(uint8_t magicRandom) {
  const uint8_t requestBlob[] = {0x08, 0x01};

  ProtoWriter writer;
  writer.writeInt32Field(1, 7);
  writer.writeInt32Field(2, magicRandom);
  writer.writeBytesField(9, requestBlob, sizeof(requestBlob));
  return writer.take();
}

// ─── EvenHub ページコマンドビルダー 実装 ─────────────────────────────────────────

// commandId = HEARTBEAT (12), サブメッセージは空。magicRandom はハートビートでは常に 0。
std::vector<uint8_t> buildEvenHubHeartbeatCommand(uint8_t magicRandom) {
  return buildEvenHubMessage(HEARTBEAT, 14, std::vector<uint8_t>{}, magicRandom);
}

// ページメッセージ内にテキストコンテナをネストし、CREATE_STARTUP_PAGE コマンドとして送信。
std::vector<uint8_t> buildCreateTextPageCommand(uint8_t magicRandom,
                                                const String& text,
                                                uint32_t containerId,
                                                bool enableEventCapture) {
  ProtoWriter pageWriter;
  const std::vector<uint8_t> textContainer =
      buildTextContainerMessage(containerId, text, enableEventCapture);
  pageWriter.writeInt32Field(1, 1);
  pageWriter.writeMessageField(3, textContainer);
  return buildEvenHubMessage(CREATE_STARTUP_PAGE, 3, pageWriter.take(), magicRandom);
}

// containerId で対象コンテナを指定し、テキスト内容と長さを上書きする。
// テキスト長は field 4 に文字数 (bytes) のみ、実際の内容は field 5 で送信。
std::vector<uint8_t> buildUpdateTextCommand(uint8_t magicRandom,
                                            const String& text,
                                            uint32_t containerId) {
  ProtoWriter textWriter;
  textWriter.writeInt32Field(1, static_cast<int32_t>(containerId));
  textWriter.writeInt32Field(3, 0);
  textWriter.writeInt32Field(4, static_cast<int32_t>(text.length()));
  textWriter.writeStringField(5, text);
  return buildEvenHubMessage(UPDATE_TEXT_DATA, 9, textWriter.take(), magicRandom);
}

// ─── 受信パーサー 実装 ──────────────────────────────────────────────────────────────────

// DEVICE_SETTINGS 認証レスポンスの解析実装。
// ペイロードの field 1 (command_id) をチェックし、AUTHENTICATION なら field 3 の authBlob を取得する。
// authBlob 内の field 1 (sec_auth) が 0 以外なら認証成功とみなす。
bool parseDevSettingsAuthResponse(const std::vector<uint8_t>& payload, bool& secAuth) {
  secAuth = false;
  int32_t commandId = -1;
  std::vector<uint8_t> authBlob;

  size_t offset = 0;
  while (offset < payload.size()) {
    uint64_t tag = 0;
    if (!readVarint(payload, offset, tag)) {
      return false;
    }

    const uint32_t fieldNumber = static_cast<uint32_t>(tag >> 3);
    const uint8_t wireType = static_cast<uint8_t>(tag & 0x07);

    if (wireType == 0) {
      uint64_t value = 0;
      if (!readVarint(payload, offset, value)) {
        return false;
      }
      if (fieldNumber == 1) {
        commandId = static_cast<int32_t>(value);
      }
      continue;
    }

    if (wireType == 2) {
      uint64_t length = 0;
      if (!readVarint(payload, offset, length)) {
        return false;
      }
      if (offset + length > payload.size()) {
        return false;
      }
      if (fieldNumber == 3) {
        authBlob.assign(payload.begin() + offset, payload.begin() + offset + length);
      }
      offset += static_cast<size_t>(length);
      continue;
    }

    if (!skipField(payload, offset, wireType)) {
      return false;
    }
  }

  if (commandId != AUTHENTICATION || authBlob.empty()) {
    return false;
  }

  offset = 0;
  while (offset < authBlob.size()) {
    uint64_t tag = 0;
    if (!readVarint(authBlob, offset, tag)) {
      return false;
    }

    const uint32_t fieldNumber = static_cast<uint32_t>(tag >> 3);
    const uint8_t wireType = static_cast<uint8_t>(tag & 0x07);
    if (fieldNumber == 1 && wireType == 0) {
      uint64_t value = 0;
      if (!readVarint(authBlob, offset, value)) {
        return false;
      }
      secAuth = (value != 0);
      return true;
    }

    if (!skipField(authBlob, offset, wireType)) {
      return false;
    }
  }

  return false;
}

// EvenHub タップイベント判定実装。
// field 1 が OS_NOTIFY_EVENT_TO_APP (2) であることを確認し、
// field 13 (deviceEvent) -> field 2 (textEvent) -> field 3 (eventType)
// または field 13 -> field 3 (systemEvent) -> field 1 (eventType)
// のパスで OS_EVENT_CLICK (0) であれば true を返す。
bool isEvenHubTapEvent(const std::vector<uint8_t>& payload) {
  uint64_t commandId = 0;
  if (!findVarintField(payload, 1, commandId) || commandId != OS_NOTIFY_EVENT_TO_APP) {
    return false;
  }

  std::vector<uint8_t> deviceEventData;
  if (!findBytesField(payload, 13, deviceEventData)) {
    return false;
  }

  std::vector<uint8_t> textEventData;
  uint64_t eventType = 0;
  if (findBytesField(deviceEventData, 2, textEventData) &&
      findVarintField(textEventData, 3, eventType)) {
    return eventType == OS_EVENT_CLICK;
  }

  std::vector<uint8_t> systemEventData;
  if (findBytesField(deviceEventData, 3, systemEventData)) {
    if (!findVarintField(systemEventData, 1, eventType)) {
      return true;
    }
    return eventType == OS_EVENT_CLICK;
  }

  return false;
}

}  // namespace g2mentra