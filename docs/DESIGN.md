# 設計

## 構成

- `core`: IE TLV解析、周波数→チャネル変換、スコアリング、決定的な順位、再試行制限
- `wlan_client`: Native Wi-Fi APIと全WLANメモリ/ハンドル/通知のRAII境界
- `storage`: JSON設定、デーモン状態、追記型接続履歴
- `optimizer`: 評価、ヒステリシス、接続、1回だけのロールバック、監視ループ
- `main`: CLI入力・表示とバックグラウンドプロセス制御
- `gui_main`: Win32 GUI、ListView、履歴ウィンドウ、システムトレイ、非同期操作の調停

`target_ssid`と`mode`は自動最適化の`enabled`状態とは独立した永続設定である。CLIの`target`、`mode`、GUIの「Apply」は、操作時に`Storage::load_config()`で最新設定を読み直し、対象フィールドだけを変更して`Storage::save_config()`へ渡す。評価間隔、改善幅、クールダウン、ON/OFF状態など他の値は保持する。各コマンドの`--ssid`は一時上書きであり、`target`またはGUIの適用操作を除いて対象SSIDを永続変更しない。

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

Station Countは表示だけに用い、負荷へ変換しない。同点は現在AP、RSSI、BSSID辞書順の順で決定し、毎回同じ結果になる。

履歴点は既定では従来の整数試行数に対するLaplace平滑化をそのまま使う。`ScorePolicy::connection_history_half_life_days`を明示した場合だけ、各履歴試行へ`2^(-経過日数/半減期日数)`を掛けた重み付き成功数・試行数を同じ平滑化式へ入れる。未指定時は時間参照も行わず、従来の得点と理由文字列を維持する。オフライン再現用には重みJSONの`history_reference_unix`で評価時刻を固定でき、省略時は現在時刻を使う。入力に個別の`history_samples`がない場合は、半減期が指定されても安全に従来の集計値へフォールバックする。これは統計的平滑化であり、機械学習モデルではない。将来モデルを導入する場合も、特徴量生成と推論を`ScorePolicy`境界の外へ分離し、既定の決定的スコアを互換経路として残す。

## 接続と復旧

保存済みプロファイルを名前または読取専用XML内SSIDで検索し、`WLAN_CONNECTION_PARAMETERS::pDesiredBssidList`に1件を設定する。通知は対象インターフェイスの接続完了/失敗だけを待ち、既定25秒でタイムアウトする。完了後は`wlan_intf_opcode_current_connection`を再取得し、希望BSSIDとの一致を判定する。

自動最適化のClosed-loop Optimizationは、切替要求、実BSSID一致確認、対象WLANインターフェイスのIPv4確認、切替前後のRSSI/Signal Quality比較、履歴/Black Box記録の順で進む。IPv4確認はインターフェイスGUIDをLUIDへ変換し、`GetAdaptersAddresses(AF_INET)`のユニキャスト一覧だけを500 ms間隔、最大約8秒読み取る。未割当、0/8、loopback、APIPA（169.254/16）、multicast以上は有効なIPv4とみなさない。外部パケットは送信しない。自動切替の成功扱いは、WLAN通知成功、実BSSID一致、有効IPv4確認の3条件をすべて満たした場合だけである。

BSSID不一致またはIPv4確認失敗時、開始前が同じSSIDへの接続なら元BSSIDへの復帰を1回だけ試す。復帰側も実BSSIDとIPv4を確認するが、接続要求の再試行は増やさず、無限再試行はない。各試行の希望/実BSSID、時刻、APIまたはWLAN reason code、説明、実施した場合のIPv4確認結果を履歴とBlack Boxへ保存する。`connect_bssid`呼出しの直前・直後を`steady_clock`で計測し、要求からWLAN通知完了またはタイムアウトまでを`duration_ms`として保存する。IP確認待ち時間は含めない。履歴TSVの8列目はオプショナルなIPv4確認結果、9列目はオプショナルな接続所要時間で、旧7列・8列行も継続して読み込む。

`optimize --dry-run`はランキングとヒステリシス判定後、接続要求より前に終了するため、IPv4確認も行わない。手動`connect`とGUIの手動接続は従来どおりWLAN通知と実BSSIDだけを判定し、この検証ループの対象外である。

## 常駐監視

`auto on` は同じexeを`daemon`モードで非表示起動する。名前付きMutexで多重起動を防ぎ、PID・heartbeat・SSID・BSSID・直近結果をJSONに保存する。OFF設定は500 ms単位の待機より優先して確認され、OFFは切断APIを呼ばない。切替中のNative Wi-Fi呼び出しはOS APIの完了/タイムアウトまでは同期的であり、その後OFFを反映する。

クールダウンは監視プロセス内で管理し、改善幅12点未満なら切り替えない。プロセス停止をPIDで実測するため、保存されたON値だけで稼働中とは表示しない。GUIも同じJSON状態とCLI相当の制御手順を利用する。

### 動作モードとプロセス検出

`normal`は従来の`min_improvement`をそのまま使う。`gaming`と`call`は`min_improvement`自体を書き換えず、それぞれ`gaming_additional_margin`（既定8点）、`call_additional_margin`（既定15点）を評価時の閾値へ加える。`manual`はdaemonの観測、Black Box、状態保存を継続しながら`optimize_once`をdry-runとして実行するため、`connect_bssid`を呼ばない。明示的な`wifiopt optimize`はユーザー操作としてmanual制約を解除してnormal閾値を使い、`connect`も従来どおりである。`enabled`はdaemonを動かすかという従来の意味を維持し、モードとは独立する。

`process_detection_enabled`は既定falseである。false時はプロセスAPIを一切呼ばない。true時だけ、daemonの既存評価周期内で`CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS)`、`Process32FirstW`、`Process32NextW`を1回実行し、`call_process_names`に設定された実行ファイル名と大文字小文字を無視して照合する。一致中は保存モードを書き換えず、その評価だけ`call`閾値を使う。取得するのは`PROCESSENTRY32W::szExeFile`だけで、ウィンドウ、通信、コマンドライン、キーストロークは取得しない。列挙失敗時は、その評価をmanual相当のdry-runへ落とし、Windowsエラーコードをdaemon stateの`detail`へ保存する。

履歴統計はUnix時刻をローカル時刻へ変換し、0～5時、6～11時、12～17時、18～23時の4帯で成功数・試行数・成功率を集計する。これは記録済みの接続試行だけを母数とする。

## Win32 GUI

`src/gui_main.cpp`がウィンドウ、GDI描画、非同期調停を担当し、`src/gui_logic.cpp`が比較推薦文と同一物理AP推定の副作用を持たないGUI用判定を担当する。`wifiopt_gui`は既存の`wifiopt_core`をリンクし、WLAN、スコアリング、Black Box、イベントログ、ストレージの実装はCLIと共通である。表示には標準Win32コントロールとCommon ControlsのListView/ListBoxを使用し、`Shell_NotifyIcon`で通知領域に常駐する。Explorer再起動時は登録済み`TaskbarCreated`メッセージでアイコンを再登録する。メインウィンドウのクラス名は`SmartWifiApOptimizerGuiWindow-v1`、タイトルは`Smart Wi-Fi AP Optimizer`である。

UIスレッドは通常の`GetMessage`/`DispatchMessage`ループを所有する。スキャン、順位付け、BSSID指定接続、自動監視停止待ちは同期呼び出し時間が長いため、一度に1本だけのワーカースレッドで実行する。完了結果は保護された共有領域へ移し、`WM_APP`メッセージでUIスレッドへ通知する。コントロール更新とトレイ更新はUIスレッドだけで行う。終了時は進行中ワーカーをjoinしてからコアオブジェクトとウィンドウを破棄する。

更新操作は`WlanClient::status()`と`WlanClient::scan(std::nullopt)`を呼ぶ。1回の全SSIDスキャン結果から、空SSIDを除いて重複排除した候補リストと、対象SSIDだけを抽出して履歴情報を付与したランキングを生成する。順位計算には共通の`rank_access_points`を使用する。手動接続はCLIの`connect`と同じく、選択BSSIDに対する`WlanClient::connect_bssid`、`result.success && result.desired_bssid_matched`による最終成功判定、`Storage::append_history`の順に実行する。GUIは手動接続へ自動ロールバックを追加しないため、CLIの`connect`と意味が一致する。

自動最適化表示は`AutoConfig::enabled`だけでなく、`Storage::load_daemon_state()`のPIDを`process_is_running()`で検査する。ON操作は設定を先に保存し、停止中なら同一ディレクトリの`wifiopt.exe`を`start_daemon_process`で`daemon`起動する。起動失敗時はCLIと同じく設定をOFFへ戻す。OFF操作は設定を保存して最大約3秒の停止待ちを行い、現在のWi-Fi接続には触れない。

WLAN接続通知はGUIのウィンドウメッセージへ直接統合しない。`WlanClient::connect_bssid`がワーカースレッド上でNative Wi-Fi通知を条件変数へ集約し、完了した結果だけがGUIメッセージループへ渡る。この境界により、既存の通知処理を変更せず再利用する。

ランキングには品質スコア列と従来の選択スコア列を並べる。選択行のダブルクリックまたは「Details」でモデルレス詳細ウィンドウを表示し、基本無線情報、PHY/ビーコン間隔、BSS Load、IE解析結果、両スコアと既存の理由文字列をまとめる。詳細ウィンドウは起動時に非表示で生成し、GUIスモークテストでボタンと内容コントロールの存在を検証できるようにする。

最終検出日時はAPのビーコンTimestampではない。スキャンでBSSIDを観測した時刻を`Storage::record_observations`が`last_seen.tsv`へ原子的に保存し、その観測時刻であることを画面に明記する。リンク速度は現在接続中のBSSIDだけを対象に、WLANインターフェイスGUIDを`ConvertInterfaceGuidToLuid`で変換し、`GetIfEntry2`の`ReceiveLinkSpeed`/`TransmitLinkSpeed`を読む。未接続APやAPI取得失敗は欠損として明示する。

### Priority 5 GUI拡張

グラフ、比較、診断はメインウィンドウ所有のモデルレスウィンドウで、起動時に非表示で生成する。これにより既存のトレイ常駐を維持し、スモークテストは実WLANデータがなくてもウィンドウクラスと子コントロールの生成を確認できる。各ビューの閉じる操作はビューを非表示にするだけで、メインウィンドウ、トレイ、自動最適化プロセスへ終了や設定変更を伝播しない。

リアルタイムグラフは表示時の`WM_SHOWWINDOW`で7秒周期の`SetTimer`を開始し、非表示時および`WM_NCDESTROY`で`KillTimer`する。`WM_TIMER`はスキャン自体を行わず、専用ワーカーを1本だけ起動する。ワーカーは既存`Optimizer::rank`を呼び、対象SSIDの`current` APからRSSI、Signal Quality、選択スコアを1サンプルにする。結果は`WM_APP`でUIスレッドへ戻し、直近1時間かつ最大600件のメモリバッファへ追加する。表示期間（1/5/30/60分）の変更はバッファの時刻フィルタと再描画だけであり、スキャン周期を変更しない。GDIの`Polyline`でRSSIを-100～-30 dBm、Signal Qualityと選択スコアを0～100へ写像する。CSV/JSON出力も現在の表示期間に含まれるサンプルだけを対象とする。

既存の操作ワーカーとグラフワーカーは独立してUIメッセージループを塞がない。一方、両者が同一プロセス内で`Storage`を介して観測・履歴を読む区間はmutexで直列化し、同時ファイル更新を避ける。終了時は停止フラグを設定して両ワーカーをjoinしてから各ウィンドウを破棄する。グラフを閉じた時点で新しい周期処理は停止し、すでに開始済みの読み取り専用スキャンだけは安全に完了させ、非表示なら結果を表示バッファへ追加しない。

ランキングListViewは複数選択を許可する。比較ビューは選択済み`RankedAccessPoint`をコピーし、信号、周波数帯/チャネル、BSS Load、`Storage::enrich_with_history`で付加済みの接続履歴、品質スコア、選択スコアを表示する。推薦文は選択スコア最大を基準に、RSSI、履歴成功率、BSS Load欠損を説明する。物理AP推定は「周波数一致」「チャネル一致」「正規化BSSIDの数値差16以内」の3条件が同時に成立した場合だけ可能性ありとし、全結果にヒューリスティックであり確実な証拠ではない旨を明記する。この純粋判定は`wifiopt_gui_logic`として単体テスト対象にする。

診断ビューの更新は共通操作ワーカーで`BlackBox::load_records(20)`と`query_wlan_events(86400, 100)`を呼ぶだけで、障害分類やイベント抽出を再実装しない。イベントログ取得不能時は既存`EventLogResult::message`とWindowsエラーコードをそのまま表示する。JSON出力は既存`blackbox_record_json`と`json_escape`を再利用し、CLI診断レポートと同じschema version 1のBlack Box/Event Log構造を持つ。

全ウィンドウはPer-Monitor DPI v2プロセスコンテキストを維持し、`WM_DPICHANGED`の推奨矩形、DPI換算した余白・列幅・フォントを適用する。日本語ラベルを含むGUIターゲットはMSVCの`/utf-8`でコンパイルする。

## セキュリティ境界

Wi-Fi XMLや資格情報を書かず、EAP/証明書検証を変更しない。プロファイルXMLはSSID照合のためだけに読み、保存しない。AP設備には通信しない。設定JSONの原子的置換には`MoveFileEx`を使う。
