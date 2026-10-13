# Smart Wi-Fi AP Optimizer

Windows Native Wi-Fi APIで周辺BSSを観測し、説明可能な0～100点のスコアで同一SSIDのAPを順位付けし、保存済みWi-Fiプロファイルを使って指定BSSIDへの接続を要求するC++20アプリケーションです。CLIに加えて、手動AP切替と常駐操作のためのネイティブWin32 GUIを提供します。802.1Xの資格情報、証明書設定、Wi-Fiプロファイルは作成・変更しません。

## 必要環境とビルド

- Windows 11
- Visual Studio 2022以降の「C++によるデスクトップ開発」とWindows SDK
- CMake 3.24以降

Developer PowerShellで実行します。

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

実行ファイルは `build\Release\wifiopt.exe`（CLI）と `build\Release\wifiopt_gui.exe`（GUI）です。本リポジトリで検証済みの成果物も同じ場所にあります。

Windows 11ではBSSID一覧の取得に位置情報アクセスが必要です。エラー5が出る場合は「設定 > プライバシーとセキュリティ > 位置情報」で位置情報サービスとデスクトップアプリのアクセスを許可してください。通常、管理者権限は不要です。

## 使用方法

### GUI

```powershell
.\build\Release\wifiopt_gui.exe
```

GUIでは、現在のSSID/BSSID/信号強度の更新、設定済みターゲットSSID（既定値`AP-TUT5`）のAPランキング、選択したBSSIDへの手動接続、自動最適化のON/OFF、接続履歴の表示ができます。ランキング行をダブルクリックするか、行を選択して「Details」を押すと、AP詳細、BSS Load、IE解析結果、品質/選択スコアの内訳を表示します。「Target SSID」の編集可能なドロップダウンから周辺SSIDを選ぶかSSIDを直接入力し、「Apply」を押すと設定を保存して、そのSSIDのランキングへ即時更新します。右上の閉じるボタンは終了ではなくウィンドウを隠し、通知領域のアイコンから再表示できます。通知領域の右クリックメニューには「Open」、現在状態に応じた自動最適化ON/OFF、「Exit」があります。

メイン画面の「グラフ」で現在APの時系列表示とCSV/JSON出力、「比較」でCtrl/Shift選択した2件以上のAP比較、「診断」でBlack Boxと直近24時間のWLANイベント表示・JSON出力を開けます。

GUIと`wifiopt.exe`は同じディレクトリに配置してください。GUIから自動最適化をONにすると、同じディレクトリの`wifiopt.exe`を`daemon`モードで起動します。GUIを完全に終了するには通知領域メニューの「Exit」を選択します。

「Connect to selected AP」は一時切断を起こし得ます。クリック前に選択行のBSSIDを確認してください。接続結果欄には成功/失敗、要求BSSID、実接続BSSIDが表示されます。

### CLI

```powershell
# 保存済みの対象SSIDを表示・変更
.\build\Release\wifiopt.exe target
.\build\Release\wifiopt.exe target TestNetwork123

# 動作モードを表示・変更（enabledとは独立）
.\build\Release\wifiopt.exe mode
.\build\Release\wifiopt.exe mode gaming

# 全SSIDのBSSを表示
.\build\Release\wifiopt.exe scan

# 対象SSIDだけ表示
.\build\Release\wifiopt.exe scan --ssid AP-TUT5

# 現在の接続状態と対象SSIDの順位
.\build\Release\wifiopt.exe status
.\build\Release\wifiopt.exe rank --ssid AP-TUT5

# 保存済みプロファイルでBSSIDを希望先として接続
.\build\Release\wifiopt.exe connect 98:8F:00:41:5A:E4 --ssid AP-TUT5

# 設定に関係なく評価・必要なら1回だけ切替
.\build\Release\wifiopt.exe optimize --ssid AP-TUT5

# 自動監視（初期値はOFF）
.\build\Release\wifiopt.exe auto on --ssid AP-TUT5
.\build\Release\wifiopt.exe auto status
.\build\Release\wifiopt.exe auto off

.\build\Release\wifiopt.exe history
.\build\Release\wifiopt.exe diagnose
```

高度なCLIでは、機械可読出力、BSSID詳細確認、接続しない事前判定、履歴集計、オフライン再計算、ヘッドレス監視を利用できます。

```powershell
# スキャン結果を保存し、Wi-Fiを操作せず同じデータを再ランキング
.\build\Release\wifiopt.exe scan --format json > scan.json
.\build\Release\wifiopt.exe simulate --input scan.json --explain

# AP詳細とIE検出状況（読み取り専用）
.\build\Release\wifiopt.exe inspect 98:8F:00:41:5A:E4 --ssid AP-TUT5 --verbose

# 実際には切り替えず、既存optimizerの判定結果だけを確認
.\build\Release\wifiopt.exe optimize --dry-run --explain --format json

# 履歴統計と直近24時間の診断
.\build\Release\wifiopt.exe history --stats
.\build\Release\wifiopt.exe diagnose --since 24h --json

# 5秒間隔で15秒だけNDJSON監視（接続操作なし）
.\build\Release\wifiopt.exe watch --interval 5s --duration 15s --format ndjson
```

全コマンド、重みファイル、JSONスキーマ、終了コードは [CLIリファレンス](docs/CLI_REFERENCE.md) を参照してください。

`target [SSID]`はWLANへ接続せず、保存済み設定の`target_ssid`だけを表示または変更します。`enabled`、評価間隔、改善幅、クールダウンなどの他設定は保持されます。`--ssid`は従来どおり、そのコマンド実行中だけの一時指定です。

`connect` と `optimize` は一時切断を起こし得ます。希望BSSIDはドライバーへの要求であり保証ではないため、完了後に実BSSIDを照合して不一致を明示します。切替失敗時は元BSSIDへの復帰を1回だけ試みます。

## 自動最適化

`auto on` は非表示の独立プロセスを開始するため、CLI終了後も監視が続きます。初期値はOFFです。設定は `%LOCALAPPDATA%\SmartWifiApOptimizer\config.json`、状態と履歴も同じディレクトリに保存します。テストやポータブル運用では `WIFIOPT_DATA_DIR` で保存先を上書きできます。

- 既定の評価間隔: 60秒
- 最低改善幅（ヒステリシス）: 12点
- 切替後クールダウン: 600秒
- 動作モード: normal（gaming/callは追加マージン、manualは監視・記録のみ）
- 通話アプリのプロセス名検出: 既定OFF（設定で明示的に有効化）
- `auto off` は現在の接続を切らず、監視だけを止める
- 設定がONでもプロセスが停止していれば `auto status` が不整合を警告する
- Windows起動時の自動実行は本版では設定しない

閾値はJSONで編集できます。範囲外値は安全な範囲へ補正されます。監視中の編集は次周期で反映されます。

## スコアの読み方

品質スコアは信号45点、BSS Load 20点、同一チャネル近隣8点だけを合計します。選択スコア（従来の`rank`スコア）は、これに接続履歴17点、現在APへの8点の切替抑制ボーナス、直近失敗の減点を加えます。BSS Load非提供時は中立点を置き、「混雑度を取得した」とは表示しません。Station Countは端末数であり通信量ではない旨も表示します。詳細は [スコアリング](docs/SCORING.md) と [設計書](docs/DESIGN.md) を参照してください。

## 安全性

本ツールが操作するのはローカルPCのWLANクライアントだけです。大学側APへの管理操作、認証回避、負荷試験、資格情報の収集は行いません。実機切替前には `status` で現在SSID/BSSIDを記録してください。

検証結果は [テスト報告](docs/TEST_REPORT.md)、既知の制約は [制約事項](docs/LIMITATIONS.md) に記載しています。
