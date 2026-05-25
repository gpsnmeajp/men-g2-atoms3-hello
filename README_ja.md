# Men G2 Counter for M5AtomS3

[English README](README.md)

M5Stack(M5AtomS3) から Even G2 に BLE 接続し、認証・起動シーケンスを実行して EvenHub テキストコンテナを作成したうえで、AtomS3 本体と G2 グラスの両方に同期したカウンタを表示する Arduino スケッチです。

M5AtomS3とEven G2を直接接続し、文字列を送信します。EvenアプリおよびMentra OSアプリ、スマートフォンに依存しません。

Mentra OS の Even G2 実装をもとにしたプロトコル処理は、本スケッチとのコード分離を明確にするため G2MentraProtocol.h と G2MentraProtocol.cpp に独立させています。

ほぼ全部GPT-5.4が作成。

動作風景: https://x.com/Seg_Faul/status/2058883443916980324

## 現在の実装内容

- Even G2 を BLE スキャンし、右レンズを必須として接続する
- 左レンズが見つかった場合はあわせて接続する
- G2.kt を参考にした認証と起動シーケンスを実行する
- UI イベント受領可能な EvenHub テキストコンテナを作成する
- AtomS3 の画面と G2 グラス内に同じカウンタ値を表示する
- AtomS3 の BtnA を押すとカウントアップする
- G2 のタップイベントを受け取るとカウントアップする
- EvenHub heartbeat と DevSettings heartbeat を定期送信する
- 切断や起動失敗時にセッション再試行を行う

## ファイル構成

- men-g2-atoms3-hello.ino: BLE 接続、起動シーケンス、カウンタ状態、AtomS3 表示、入力処理を担当するメインスケッチ
- G2MentraProtocol.h: Mentra OS 由来のパケット生成と解析の宣言を分離したヘッダ
- G2MentraProtocol.cpp: EvenHub テキスト更新やタップイベント解析を含むプロトコル実装
- G2_BLE_Investigation_Report.md: BLE bring-up 調査と実機で判明したズレの記録
- LICENSE: このリポジトリのライセンス
- MentraOS_LICENSE: 参照元 Mentra OS のライセンス情報

## 必要なもの

- M5AtomS3
- Even G2 本体。既存のスマートフォンアプリや他デバイスからは切断しておくこと
- Arduino CLI 1.5.0 以降、または同等のボードパッケージを使う Arduino IDE
- FQBN が m5stack:esp32:m5stack_atoms3 の M5Stack ESP32 ボードパッケージ
- M5Unified ライブラリ

## セットアップ

必要に応じてボードパッケージとライブラリをインストールしてください。

```powershell
arduino-cli core install m5stack:esp32
arduino-cli lib install M5Unified
```

## ビルド

ワークスペースのルートで実行します。

```powershell
arduino-cli compile --fqbn m5stack:esp32:m5stack_atoms3 .
```

## 書き込み

まずポートを確認します。

```powershell
arduino-cli board list
```

その後、実際のポート名に合わせて書き込みます。

```powershell
arduino-cli upload -p COM3 --fqbn m5stack:esp32:m5stack_atoms3 .
```

シリアルモニタは 115200 bps を使用してください。起動、探索、接続、入力イベントのログが確認できます。

## 設定項目

必要に応じて men-g2-atoms3-hello.ino の先頭付近の定数を調整してください。

- kTargetSerialFragment: 複数台の G2 が近くにある場合の対象絞り込み
- kKnownLeftAddress と kKnownRightAddress: スキャンを省略して固定アドレスへ接続したい場合
- kHeartbeatIntervalMs: heartbeat 送信間隔
- kRemoteTapDebounceMs: G2 側タップイベントのデバウンス時間
- kEnableEspVerboseLogs: ESP / BLE の詳細ログ出力を有効化するかどうか

## 注意点

- 現在のスケッチでは右レンズが必須です。左レンズは見つかれば利用しますが、必須ではありません。
- BLE の挙動は G2 側ファームウェアと、インストール済みの M5Stack BLE ライブラリ実装に依存します。
- NimBLE 側の具体的なローカル差分は [BLEClient_nimble_connect_fix.patch](BLEClient_nimble_connect_fix.patch) に保存しています。
- パッチ対象は Arduino15 配下の BLEClient.cpp で、主な変更点は BLE_GAP_EVENT_CONNECT 成功時点で connect() の待機を解除すること、m_isConnected をその場では立てず connect() 完了後にだけ true にすること、MTU 交換エラーで即失敗にしないことです。
- ボードパッケージ更新時は、このパッチがそのまま適用できるか再確認してください。背景や調査経緯は G2_BLE_Investigation_Report.md を参照してください。
- 詳細な経緯や既知のズレは G2_BLE_Investigation_Report.md を参照してください。

## ライセンス

このリポジトリのコードは MIT License です。LICENSE を参照してください。

Mentra OS 由来のプロトコル処理はMIT License です。

[Mentra-Community/MentraOS : MIT LICENSE](https://github.com/Mentra-Community/MentraOS/blob/dev/LICENSE)