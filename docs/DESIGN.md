# 設計

## 構成

- `core`: IE TLV解析、周波数→チャネル変換、スコアリング、決定的な順位、再試行制限
- `wlan_client`: Native Wi-Fi APIと全WLANメモリ/ハンドル/通知のRAII境界
- `storage`: JSON設定、デーモン状態、追記型接続履歴
- `optimizer`: 評価、ヒステリシス、接続、1回だけのロールバック、監視ループ
- `main`: CLI入力・表示とバックグラウンドプロセス制御
- `gui_main`: Win32 GUI、ListView、履歴ウィンドウ、システムトレイ、非同期操作の調停

`target_ssid`は自動最適化の`enabled`状態とは独立した永続設定である。CLIの`target`コマンドとGUIの「Apply」は、操作時に`Storage::load_config()`で最新設定を読み直し、`target_ssid`だけを変更して`Storage::save_config()`へ渡す。評価間隔、改善幅、クールダウン、ON/OFF状態は同じ値を保持する。各コマンドの`--ssid`は一時上書きであり、`target`またはGUIの適用操作を除いて対象SSIDを永続変更しない。

`WlanOpenHandle`、`WlanEnumInterfaces`、`WlanScan`、`WlanGetNetworkBssList`、`WlanQueryInterface`、`WlanConnect`、`WlanRegisterNotification`、`WlanFreeMemory`を使用する。WLAN APIが割り当てた領域はカスタムdeleter付き`unique_ptr`、WLANハンドルとプロセス/Mutexハンドルは所有者のデストラクタまたは全終了経路で解放する。

## スキャンとIE解析

スキャン結果は `(SSID, BSSID)` で重複排除する。RSSI、link quality、中心周波数、計算チャネル、現在BSSIDを格納する。順位付け時は全SSIDをスキャンしてから対象SSIDを抽出するため、他SSIDの同一チャネルBSSも近隣数に含む。

IEは `[element-id, length, payload]` のTLVとして残長を毎回確認する。ヘッダー不足、宣言長超過、BSS Load（ID 11）の5バイト未満はいずれも「取得不能」とし、バッファ外を読まない。BSS LoadのStation Count、Channel Utilization、Available Admission Capacityは実測広告値として保持する。

追加IEパーサーも同じ境界検査を共有する考え方で、各要素のヘッダーと宣言長がバッファ内に収まることを確認してから読む。HT Capabilities（ID 45）、VHT Capabilities（ID 191）、Extended Capabilities（ID 127）の存在とVendor Specific（ID 221）の個数を保持する。Extended Capabilitiesは3オクテット以上ある場合だけbit 19を読み、BSS Transition Managementの対応/非対応と、情報不足による不明を区別する。未知IDは長さ分だけ安全にスキップし、途中で壊れたTLVがあれば解析結果全体を取得不能とする。

`WLAN_BSS_ENTRY`のIE領域はエントリ先頭から`ulIeOffset`、長さ`ulIeSize`で参照する。さらに`WLAN_BSS_LIST::dwTotalSize`に対してエントリ位置、オフセット、サイズを段階的に検証してから`span`を構築する。PHY規格はSDKで確認した`dot11BssPhyType`、ビーコン間隔は`usBeaconPeriod`から取得する。

## スコア

品質スコアは信号、BSS Load、同一チャネル近隣の3項だけを合計した0～73点である。選択スコアは従来の最終スコアで、品質スコアへ履歴、失敗ペナルティ、現在APボーナスを加えて0～100へ丸め込む。詳細な式と中立値・平滑化は[スコアリング](SCORING.md)に記載する。

| 要素 | 点数 | 方法 |
|---|---:|---|
| RSSI + link quality | 45 | RSSI -90～-50 dBmを線形正規化32点、品質13点 |
| Channel Utilization | 20 | `20 * (1 - raw/255)`。欠損時は中立10点で欠損表示 |
| 同一チャネル近隣 | 8 | `8 / (1 + 0.35 * neighbor_count)` |
| 接続履歴 | 17 | 試行ありはLaplace平滑化成功率、なしは中立8.5点 |
| 直近失敗 | 減点 | 1回7点、最大21点 |
| 現在AP | 加点 | 切替コスト/安定性を表す8点 |

Station Countは表示だけに用い、負荷へ変換しない。遅延は本版では測定しない。同点は現在AP、RSSI、BSSID辞書順の順で決定し、毎回同じ結果になる。

## 接続と復旧

保存済みプロファイルを名前または読取専用XML内SSIDで検索し、`WLAN_CONNECTION_PARAMETERS::pDesiredBssidList`に1件を設定する。通知は対象インターフェイスの接続完了/失敗だけを待ち、既定25秒でタイムアウトする。完了後は`wlan_intf_opcode_current_connection`を再取得し、希望BSSIDとの一致を判定する。成功扱いは通知成功かつ実BSSID一致の場合だけである。

失敗時、開始前が同じSSIDへの接続なら元BSSIDへの復帰を1回だけ試す。無限再試行はない。各試行の希望/実BSSID、時刻、APIまたはWLAN reason code、説明を履歴へ保存する。

## 常駐監視

`auto on` は同じexeを`daemon`モードで非表示起動する。名前付きMutexで多重起動を防ぎ、PID・heartbeat・SSID・BSSID・直近結果をJSONに保存する。OFF設定は500 ms単位の待機より優先して確認され、OFFは切断APIを呼ばない。切替中のNative Wi-Fi呼び出しはOS APIの完了/タイムアウトまでは同期的であり、その後OFFを反映する。

クールダウンは監視プロセス内で管理し、改善幅12点未満なら切り替えない。プロセス停止をPIDで実測するため、保存されたON値だけで稼働中とは表示しない。GUIも同じJSON状態とCLI相当の制御手順を利用する。

## Win32 GUI

`src/gui_main.cpp`だけがGUI固有の実装で、`wifiopt_gui`は既存の`wifiopt_core`をリンクする。コア、WLAN、スコアリング、ストレージの実装はCLIと共通であり、GUI用の複製や別アルゴリズムは持たない。表示には標準Win32コントロールとCommon ControlsのListView/ListBoxを使用し、`Shell_NotifyIcon`で通知領域に常駐する。Explorer再起動時は登録済み`TaskbarCreated`メッセージでアイコンを再登録する。メインウィンドウのクラス名は`SmartWifiApOptimizerGuiWindow-v1`、タイトルは`Smart Wi-Fi AP Optimizer`である。

UIスレッドは通常の`GetMessage`/`DispatchMessage`ループを所有する。スキャン、順位付け、BSSID指定接続、自動監視停止待ちは同期呼び出し時間が長いため、一度に1本だけのワーカースレッドで実行する。完了結果は保護された共有領域へ移し、`WM_APP`メッセージでUIスレッドへ通知する。コントロール更新とトレイ更新はUIスレッドだけで行う。終了時は進行中ワーカーをjoinしてからコアオブジェクトとウィンドウを破棄する。

更新操作は`WlanClient::status()`と`WlanClient::scan(std::nullopt)`を呼ぶ。1回の全SSIDスキャン結果から、空SSIDを除いて重複排除した候補リストと、対象SSIDだけを抽出して履歴情報を付与したランキングを生成する。順位計算には共通の`rank_access_points`を使用する。手動接続はCLIの`connect`と同じく、選択BSSIDに対する`WlanClient::connect_bssid`、`result.success && result.desired_bssid_matched`による最終成功判定、`Storage::append_history`の順に実行する。GUIは手動接続へ自動ロールバックを追加しないため、CLIの`connect`と意味が一致する。

自動最適化表示は`AutoConfig::enabled`だけでなく、`Storage::load_daemon_state()`のPIDを`process_is_running()`で検査する。ON操作は設定を先に保存し、停止中なら同一ディレクトリの`wifiopt.exe`を`start_daemon_process`で`daemon`起動する。起動失敗時はCLIと同じく設定をOFFへ戻す。OFF操作は設定を保存して最大約3秒の停止待ちを行い、現在のWi-Fi接続には触れない。

WLAN接続通知はGUIのウィンドウメッセージへ直接統合しない。`WlanClient::connect_bssid`がワーカースレッド上でNative Wi-Fi通知を条件変数へ集約し、完了した結果だけがGUIメッセージループへ渡る。この境界により、既存の通知処理を変更せず再利用する。

ランキングには品質スコア列と従来の選択スコア列を並べる。選択行のダブルクリックまたは「Details」でモデルレス詳細ウィンドウを表示し、基本無線情報、PHY/ビーコン間隔、BSS Load、IE解析結果、両スコアと既存の理由文字列をまとめる。詳細ウィンドウは起動時に非表示で生成し、GUIスモークテストでボタンと内容コントロールの存在を検証できるようにする。

最終検出日時はAPのビーコンTimestampではない。スキャンでBSSIDを観測した時刻を`Storage::record_observations`が`last_seen.tsv`へ原子的に保存し、その観測時刻であることを画面に明記する。リンク速度は現在接続中のBSSIDだけを対象に、WLANインターフェイスGUIDを`ConvertInterfaceGuidToLuid`で変換し、`GetIfEntry2`の`ReceiveLinkSpeed`/`TransmitLinkSpeed`を読む。未接続APやAPI取得失敗は欠損として明示する。

## セキュリティ境界

Wi-Fi XMLや資格情報を書かず、EAP/証明書検証を変更しない。プロファイルXMLはSSID照合のためだけに読み、保存しない。AP設備には通信しない。設定JSONの原子的置換には`MoveFileEx`を使う。
