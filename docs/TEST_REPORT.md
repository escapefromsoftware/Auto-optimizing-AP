# テスト報告

実施日: 2026-10-13（Asia/Tokyo）

## ビルド環境と結果

- Windows 11 10.0.26200
- MSVC 19.51.36246.0 / Visual Studio Build Tools 18.6.3
- Windows SDK 10.0.26100.0
- CMake 4.2.3-msvc3 / C++20 / x64
- Release: 成功（`build\Release\wifiopt.exe`、`wifiopt_gui.exe`、`wifiopt_tests.exe`生成）
- Debug: 成功（`build\Debug\wifiopt.exe`、`wifiopt_gui.exe`、`wifiopt_tests.exe`生成）

初回Releaseビルドではテストの`GetCurrentProcessId`宣言不足で1件失敗した。`windows.h`を追加し、Release/Debugを再ビルドして成功した。GUI追加後もRelease/Debugの全ターゲットを再ビルドした。最終ビルド出力にコンパイルエラーおよび警告はなかった。

Priority 1実装時は、インストール済みWindows SDK 10.0.26100.0の`um\wlanapi.h`を読み、`WLAN_BSS_ENTRY::dot11BssPhyType`と`usBeaconPeriod`、IEの`ulIeOffset`/`ulIeSize`を確認した。同SDKの`shared\netioapi.h`で`MIB_IF_ROW2::ReceiveLinkSpeed`/`TransmitLinkSpeed`、`GetIfEntry2`、`ConvertInterfaceGuidToLuid`を確認した。また、認証・暗号の既定アルゴリズムが`WLAN_AVAILABLE_NETWORK`にあり、`WLAN_BSS_ENTRY`には対応フィールドがないことを確認した。

## 自動テスト

実行: `ctest --test-dir build -C Release --output-on-failure`

結果: Release/Debugとも1/1テスト実行、13テストグループ、全成功、0失敗（Priority 1実装後の最終実行はRelease 1.14秒、Debug 3.44秒）。テスト実行ファイルの直接起動でも両構成とも`All 13 test groups passed`を確認した。

| 項目 | 結果 |
|---|---|
| BSS Load正常解析（little endian 3フィールド） | 成功 |
| BSS Load欠損 | 成功 |
| IEヘッダー不足、宣言長超過、BSS Load切詰め | 成功 |
| HT/VHT/Extended Capabilities、BSS Transition Management bit 19、Vendor Specific IE個数の正常解析 | 成功 |
| Extended Capabilitiesのオクテット不足を「不明」として保持 | 成功 |
| 追加IEパーサーのヘッダー不足、宣言長超過、途中切詰め、未知ID混在 | 成功 |
| スコア範囲、良好APが不良APより上位 | 成功 |
| 品質スコアが信号+BSS Load+チャネル干渉だけであること | 成功 |
| 選択スコアが従来式（履歴、失敗、現在APを含む）と一致すること | 成功 |
| 履歴・失敗・現在APを変えても品質スコアが不変であること | 成功 |
| 欠損BSS Loadを推定値にせず説明 | 成功 |
| 同点時の現在AP/RSSI/BSSID決定規則 | 成功 |
| 失敗履歴のスコア反映 | 成功 |
| 希望BSSIDと実BSSIDの正規化比較 | 成功 |
| 2.4/5/6 GHz周波数→チャネル | 成功 |
| 再接続回数上限と最小間隔 | 成功 |
| 初期設定OFF、JSON生成、履歴永続化/再読込 | 成功 |
| BSSID別スキャン観測時刻の`last_seen.tsv`保存 | 成功 |
| `target_ssid`だけを変更・保存・再読込しても`enabled`、評価間隔、改善幅、クールダウンが不変 | 成功 |

## CLI/実機確認

テスト用保存先をリポジトリ内`.runtime`へ設定して実施した。

対象SSIDコマンドは、OS一時領域の分離した`WIFIOPT_DATA_DIR`を使用し、Release版`wifiopt.exe`そのものを起動して次の順で確認した。

- `wifiopt target`: `AP-TUT5`、終了コード0。
- `wifiopt target TestNetwork123`: 保存成功、終了コード0。
- 再度`wifiopt target`: `TestNetwork123`、終了コード0。
- `wifiopt target AP-TUT5`: 復元成功、終了コード0。
- 復元後の`wifiopt target`: `AP-TUT5`、終了コード0。
- 空白だけのSSID: エラーメッセージを表示し、終了コード2。
- この一連のコマンドではWLANスキャンや接続切り替えを呼ばないコードパスを使用した。実Wi-Fi接続は変更していない。

実ユーザー設定（`%LOCALAPPDATA%\SmartWifiApOptimizer\config.json`）への変更確認も試みたが、検証環境のワークスペース制限が一時ファイル作成を拒否したため、実ユーザー設定は書き換わらなかった。検証前後に読み取った`target_ssid`はいずれも`AP-TUT5`で、他フィールドも不変だった。分離した検証用設定は最後に`AP-TUT5`へ復元済みである。OS一時領域の検証ディレクトリ削除は同じ制限に拒否されたため、資格情報を含まない復元済み`config.json`だけが残っている。

- `--help`: 成功。全必須コマンドを表示。
- `auto status`初期状態: `Configured: OFF`, `Monitor process: not running`を確認。
- `status`: Native Wi-Fi APIで`Wi-Fi: disconnected`を確認。
- `auto on`: PID 9804でバックグラウンド監視起動を確認。
- 起動後`auto status`: 設定ON、プロセスrunning、状態monitoringを確認。
- `auto off`: 接続を変更しない旨を表示し、プロセス停止を確認。
- 停止後`auto status`: 設定OFF、プロセスnot running、状態stoppedを確認。
- 不正BSSID: 接続APIを呼ばず入力エラーとして拒否することを確認。
- 全BSSスキャンおよびAP-TUT5順位: `WlanScan`がWindowsエラー5（アクセス拒否）を返した。位置情報設定の具体的な案内が表示された。

したがって、実際に検出できたAP数は **未取得（0件を意味しない）**。AP-TUT5の推奨BSSIDも未決定。位置情報権限がない状態を0件成功として扱っていない。

## GUIスモークテスト

実Wi-Fi設定への影響を避けるため、Release/Debugそれぞれにリポジトリ内の分離した`WIFIOPT_DATA_DIR`を指定して`wifiopt_gui.exe`をバックグラウンド起動した。接続ボタンおよび自動最適化ボタンは操作していない。

| 確認項目 | Release | Debug |
|---|---|---|
| 起動後10秒以内にプロセスとクラス`SmartWifiApOptimizerGuiWindow-v1`のメインウィンドウを検出 | 成功 | 成功 |
| 検出ウィンドウのPIDが起動プロセスと一致 | 成功 | 成功 |
| `EnumChildWindows`でクラス`ComboBox`、ID 1008の対象SSIDコンボボックスを検出 | 成功 | 成功 |
| `EnumChildWindows`でクラス`Button`、ID 1009、テキスト`Apply`の適用ボタンを検出 | 成功 | 成功 |
| `EnumChildWindows`でクラス`Button`、ID 1011、テキスト`Details`の詳細ボタンを検出 | 成功 | 成功 |
| クラス`SmartWifiApOptimizerDetailsWindow-v1`の非表示詳細ウィンドウを検出 | 成功 | 成功 |
| 詳細ウィンドウ内のクラス`Edit`、ID 1012の内容コントロールを検出 | 成功 | 成功 |
| GUIの終了コマンドを`WM_COMMAND`で送信後、15秒以内に終了 | 成功 | 成功 |
| 終了コード | 0 | 0 |

このスモークテストで確認したのはプロセス生存、メイン/詳細ウィンドウ生成、新しい子コントロールの生成、メッセージループ経由の正常終了である。ランキング行のダブルクリック、「Details」ボタン押下、AP選択後の詳細文字列更新、コンボボックスのドロップダウン、候補のクリック、「Apply」のクリック、SSID変更後の画面描画、実Wi-Fi接続はシミュレーションしておらず、**未確認**である。詳細内容の項目と取得不可表示、初期テキストを保存済みSSIDへ設定する処理、適用時に最新設定を再読込して`target_ssid`だけを保存する処理、タイトルを更新して再スキャンする処理はコードレビューで確認した。別プロセスの編集コントロール文字列は`GetWindowText`の検証対象にしていない。トレイアイコン、ListViewの描画、詳細画面の見た目、マウス操作感、High-DPI表示もスクリーンショットまたは目視確認を行っておらず、未確認項目を成功扱いにはしていない。

## GUI接続コードパスのレビュー

`src/gui_main.cpp`の「Connect to selected AP」処理を`src/main.cpp`の`connect`コマンドと照合した。いずれも次の順序である。

1. 設定済みターゲットSSIDと要求BSSIDを`WlanClient::connect_bssid`へ渡す。
2. `ConnectResult::success && ConnectResult::desired_bssid_matched`だけを最終成功とする。
3. 要求BSSID、実BSSID、上記成功値、APIエラー（なければreason code）、詳細を`Storage::append_history`へ記録する。
4. GUIは成功/失敗、要求BSSID、実接続BSSIDを結果ラベルへ表示する。

この一致はコードレビューで確認した。検証中に接続ボタンは押しておらず、実AP切替、希望BSSIDと実BSSIDの実機照合は**未実施**である。

## 未検証

- 実BSSID/RSSI/周波数の実機列挙（位置情報アクセス拒否のため）
- 実APのBSS Load IE取得
- 実APのHT/VHT/Extended/Vendor Specific IE取得と詳細画面表示
- 実接続APの`GetIfEntry2`リンク速度表示
- 実AP選択後の詳細ウィンドウ内容、ダブルクリック/Detailsボタン操作
- AP-TUT5へのBSSID指定接続、希望先と実接続先の実機照合
- 認証失敗通知、EAP 845の実機記録
- 接続通知タイムアウト、Native APIエラー後のハンドル解放の動的リーク検査
- 切替失敗後の元BSSID復帰
- GUIのトレイメニューを含むマウス操作と外観の目視確認
- GUIの対象SSIDドロップダウン展開、候補選択、Applyクリックによる設定変更の操作シミュレーション
- GUIのPer-Monitor DPI v2表示、複数モニター/DPI間移動

接続切替は今回の読み取り専用解析タスクの範囲外であり、ユーザーから実施しないよう明示されているためスキップした。大学ネットワークへの負荷試験、プロファイル/証明書/資格情報の変更は実施していない。

## リソース管理の確認範囲

コードレビューと全終了経路の確認では、WLAN列挙/BSS/接続属性/プロファイルXML/RSSI領域が`WlanFreeMemory` deleter、WLANハンドルがデストラクタ、通知が待機後に解除、プロセス/Mutexハンドルが明示解放される。動的リーク検査ツールによる検証は未実施のため、リークなしを実測成功とはしていない。
