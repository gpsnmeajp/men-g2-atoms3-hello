# Even G2 BLE 調査レポート

## 概要

M5AtomS3 から Even G2 に BLE 接続して認証し、Hello World を表示する実装を進める中で、当初の想定と実機挙動に複数のズレが見つかった。

このレポートは、ここまでに判明した事実、想定とズレていた点、実際に入れた修正、現時点で残っている未検証事項を整理したものである。

## 目的

- Even G2 の左右レンズへ BLE 接続する
- 認証シーケンスを通す
- Hello World を表示する
- 定期 Heartbeat を送る

## 最初の想定

以下を前提に実装を開始した。

1. 発見できればそのまま接続できる
2. 接続失敗の主因はペアリング不足の可能性が高い
3. `00002760-08C2-11E1-9073-0E8AC72E0000` を主 service UUID として取得できる
4. 接続失敗時に `BLEClient` を破棄しても問題ない
5. 左右両方を毎回再発見しないと起動できない

実機ログとライブラリ内部ログを追った結果、上記の多くはそのままでは成立しなかった。

## 確認できた事実

### 1. 広告自体は接続可能状態だった

実機ログでは左右ともに以下の条件で発見されている。

- `type=1`
- `connectable=true`
- `scannable=true`
- `advType=0`
- `legacy=true`

つまり、少なくとも広告時点では「非接続広告だからつながらない」という説明は成り立たなかった。

### 2. 接続は全く失敗していたわけではなかった

NimBLE の verbose ログで、RIGHT レンズに対して以下が確認できた。

- `Connected event. Handle: 1`
- `[RIGHT] Connected`

これは、リンクレイヤの接続自体は成立していたことを示す。

したがって、当初の「接続に失敗している」という理解は不正確で、実際には「接続後のライブラリ側同期と初期化の扱いが崩れていた」が正しい。

### 3. GATT service は取得できていたが、想定した UUID ではなかった

RIGHT レンズの service discovery では以下が見つかった。

- `00001800-0000-1000-8000-00805f9b34fb`
- `00001801-0000-1000-8000-00805f9b34fb`
- `0000180a-0000-1000-8000-00805f9b34fb`
- `00002760-08c2-11e1-9073-0e8ac72e1001`
- `00002760-08c2-11e1-9073-0e8ac72e5450`
- `00002760-08c2-11e1-9073-0e8ac72e6450`
- `00002760-08c2-11e1-9073-0e8ac72e7450`
- `6e400001-b5a3-f393-e0a9-e50e24dcca9e`

一方、最初の Arduino 実装は `00002760-08C2-11E1-9073-0E8AC72E0000` を取得できる前提で書いていた。

この前提は外れていた。

### 4. Android 実装は service UUID 固定で拾っていなかった

参照元の G2.kt では、service UUID を固定取得せず、`onServicesDiscovered` で全 service と全 characteristic を走査して以下の characteristic UUID を拾っていた。

- WRITE: `00002760-08C2-11E1-9073-0E8AC72E5401`
- NOTIFY: `00002760-08C2-11E1-9073-0E8AC72E5402`
- AUDIO: `00002760-08C2-11E1-9073-0E8AC72E6402`

つまり、Arduino 実装側の「特定 service に WRITE/NOTIFY がぶら下がっている」という仮定がずれていた。

### 5. M5Stack ESP32 3.3.7 の BLE ライブラリは NimBLE 経路だった

当初は Bluedroid 側を見ていたが、実際のログとソースの条件分岐から、この環境では NimBLE 経路が使われていた。

これにより、接続と切断の扱い、タイムアウト待ち、service discovery の完了条件を NimBLE 実装に合わせて考える必要があった。

## 想定とズレていて修正が必要だった箇所

### 1. Address type を捨てていた

#### 最初の想定

MAC アドレス文字列だけ保存して `BLEAddress(string)` で接続すればよい。

#### 実際

scan 結果には `addressType` があり、G2 は `type=1` で広告していた。文字列だけで復元すると address type を取り落とすため、接続失敗要因になり得た。

#### 修正

- レンズごとに `addressType` を保持するよう変更
- scan ログに `type` を出力するよう変更
- cached address 再接続時も `addressType` を使うよう変更

### 2. Fresh advertisement object でも cached address でも同じ失敗だった

#### 最初の想定

文字列化した address の再構成が失敗の主因かもしれない。

#### 実際

`BLEAdvertisedDevice` をそのまま使った接続でも、cached address だけを使った接続でも同じように失敗した。

#### 結論

address 再構成だけが根因ではなかった。

### 3. 接続失敗の主因はペアリング不足ではなかった

#### 最初の想定

ペアリング不足で接続できていない可能性が高い。

#### 実際

`Connected event. Handle: 1` が出ているため、少なくとも接続フェーズのかなり後ろまでは進んでいた。

#### 結論

主因はペアリング以前の段階ではなく、NimBLE ライブラリの接続完了判定とその後の初期化フローだった。

### 4. NimBLE `connect()` が G2 に対して誤タイムアウトしていた

#### 最初の想定

接続タイムアウトは本当に接続不成立を意味する。

#### 実際

NimBLE 側では `BLE_GAP_EVENT_CONNECT` が来ているのに、その後の進み方が G2 と噛み合わず、`connect()` が失敗扱いで戻っていた。

#### 修正

インストール済みの M5Stack ESP32 BLE ライブラリ側で、NimBLE の `BLEClient::connect()` 周辺を調整し、GAP connect 成功で待機解除できるように修正した。

#### 注意

この修正は workspace ではなく、Arduino15 配下のインストール済みライブラリへ入っている。パッケージ更新で消える可能性がある。

### 5. `BLEClient` を接続失敗時に delete していた

#### 最初の想定

接続失敗した `BLEClient` はその場で破棄して問題ない。

#### 実際

接続失敗として戻ったあとも、NimBLE host task 側で disconnect イベント処理が継続していた。そこで既に delete 済みの `BLEClient` を参照し、Guru Meditation が発生した。

#### 修正

- `BLEClient` をレンズごとに保持して再利用する形へ変更
- callback も動的生成ではなく静的に保持する形へ変更
- 接続失敗時は状態だけ落とし、即 delete しないよう変更

### 6. 毎回左右両方を再発見する前提が厳しすぎた

#### 最初の想定

毎回 scan で左右両方を発見してから接続すべき。

#### 実際

一度接続試行した後、片側の広告が一時的に見えなくなることがあった。

#### 修正

- 左右レンズの last-seen address と type をキャッシュするよう変更
- 再試行時は cached address を優先利用するよう変更

### 7. RIGHT 側だけで先に進めるようにした

#### 背景

G2.kt を見る限り、表示系・Heartbeat・大半の command は右レンズ中心に送られていた。

#### 修正

- LEFT の失敗で全体を止めない right-only モードを追加
- 調査を止めずに先へ進めるための戦術的変更であり、根因そのものではない

### 8. Service UUID 固定前提が誤りだった

#### 最初の想定

`00002760-08C2-11E1-9073-0E8AC72E0000` を取得して、そこから WRITE/NOTIFY characteristic を取れる。

#### 実際

その UUID の service は見つからず、代わりに `...1001`, `...5450`, `...6450`, `...7450` が見つかった。

さらに Android 実装は service UUID 固定ではなく、全 service を総当たりして characteristic UUID で判定していた。

#### 修正

- service UUID 固定探索を廃止
- 全 discovered service から WRITE/NOTIFY characteristic UUID を探索する形へ変更
- service count と discovered service UUID をログへ出力するよう変更

## 追加した診断

以下の診断を追加した。

1. advertisement の `connectable`, `scannable`, `advType`, `legacy` をログ化
2. fresh advertisement object と cached address の両経路を試すログを追加
3. ESP / Arduino core の verbose ログを有効化
4. service discovery の試行回数、件数、UUID 一覧をログ化
5. panic の backtrace を addr2line で解析し、クラッシュ地点を確定

## 現時点で分かっていること

1. 接続不能が根因ではない
2. 接続後の NimBLE ライブラリの扱いと service 探索条件にズレがある
3. service UUID 固定前提は G2 の実装実態と合っていない
4. `BLEClient` の寿命管理を誤ると host task 側で crash する

## 現時点で未検証の点

最新の修正で、全 service から WRITE/NOTIFY characteristic を走査する処理を入れているが、この変更後の実機ログはまだ未確認である。

確認したいポイントは以下である。

1. WRITE characteristic `...5401` がどの service から見つかるか
2. NOTIFY characteristic `...5402` がどの service から見つかるか
3. その後 `RIGHT lens ready=true` まで進むか
4. 認証応答が実際に返るか

## 変更対象

### Workspace 内

- `men-g2-atoms3-hello.ino`
- `G2MentraProtocol.h`
- `G2MentraProtocol.cpp`
- `build_opt.h`

### Workspace 外

- インストール済み M5Stack ESP32 BLE ライブラリの NimBLE `BLEClient.cpp`

## まとめ

今回の問題は、単純な「BLE 接続できない」ではなく、以下の複合要因だった。

1. G2 の実 service 構成が最初の想定と異なっていた
2. M5Stack ESP32 3.3.7 の NimBLE 実装が G2 に対してそのままでは誤タイムアウトした
3. 接続失敗時の `BLEClient` 寿命管理が unsafe だった
4. Android 実装は service UUID 固定ではなく characteristic UUID 走査で初期化していた

現時点では、根因の大半は切り分け済みであり、次の主要検証点は「WRITE/NOTIFY characteristic を正しく掴めるか」と「その先の認証フローが通るか」である。