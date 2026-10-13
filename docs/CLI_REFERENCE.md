# CLIリファレンス

`wifiopt.exe` はデータを標準出力、エラーと入力診断を標準エラーへ出力します。既存コマンドは、`--format` を指定しない場合、従来と同じテキストを出力します。

## 共通出力オプション

`scan`、`status`、`rank`、`history`、`diagnose` は次を受け付けます。

- `--format text|json|csv|ndjson`: 既定は `text`。JSON文書、ヘッダー付きCSV、1行1 JSON オブジェクトも選択できる。
- `--quiet`: 正常時の標準出力を抑止する。終了コードと標準エラーは維持する。
- `--no-color`: 将来のカラー出力抑止用。現在の出力は無色なので no-op。

`inspect`、`optimize`、`simulate`、`blackbox` は `text|json`、`watch` は `text|ndjson` に対応します。`diagnose --report`も`text|json`だけに対応します。未対応形式は終了コード2です。`diagnose --json` は `--format json` の別名です。

## コマンド一覧

### `scan [--ssid SSID]`

周辺BSSをスキャンします。`--ssid` を指定するとそのSSIDだけをNative Wi-Fi APIへ要求します。接続操作は行いません。

```powershell
wifiopt scan
wifiopt scan --ssid AP-TUT5 --format json > scan.json
wifiopt scan --format csv
```

### `status`

現在の接続状態、SSID、BSSID、プロファイル、信号、インターフェイス、取得できる場合はリンク速度を表示します。

### `rank [--ssid SSID]`

全SSIDをスキャンして同一チャネル近隣数を計算し、対象SSIDのBSSを選択スコア順に表示します。テキスト出力は従来どおり各スコア理由を含みます。

### `inspect <BSSID> [--ssid SSID] [--verbose] [--raw-ie] [--format text|json]`

現在のスキャン結果からBSSIDを検索し、band、PHY、ビーコン間隔、BSS Load、最終観測時刻、現在APだけのリンク速度、品質スコア、選択スコアを表示します。

- `--ssid SSID`: SSIDでも絞り込む。
- `--verbose`: HT/VHT/Extended Capabilities/Vendor IE検出状況とスコア理由を追加する。
- `--raw-ie`: Native Wi-Fi APIのBSSリスト内で境界確認できたIE列だけを16進表示する。

対象がスキャンに存在しない場合は標準エラーへ診断を出し終了コード2です。スキャン自体がOS権限等で失敗した場合は終了コード1です。

### `connect <BSSID> [--ssid SSID]`

保存済みWindowsプロファイルを使い、指定BSSIDを希望先として接続します。これは接続を変更するコマンドです。

### `optimize [--ssid SSID] [--dry-run] [--explain] [--format text|json]`

通常実行は従来どおり1回評価し、必要なら接続を試みます。`--dry-run` は同じランキング・改善幅・ヒステリシス判定を行いますが、`connect_bssid` を呼ばず履歴も追加しません。

通常実行で接続試行が発生した場合だけ、従来のメッセージと推奨BSSIDに加えて、IP確認結果、切替前品質、切替後品質を表示します。品質はRSSI（dBm）とWindows Signal Quality（%）です。JSONでは`ip_verification`、`before_quality`、`after_quality`を追加し、未実施/取得不能は`null`です。IP確認は有効な非APIPA IPv4アドレスの取得有無であり、外部ホストへの到達性を意味しません。

- `--explain`: 改善幅、閾値、クールダウン状態、推奨APのスコア理由を表示する。
- one-shot CLIのクールダウン時刻は永続化されていないため、説明では非アクティブと表示する。常駐監視のクールダウンはdaemonプロセス内で管理される。
- `--dry-run`では接続、IP確認、履歴記録を行わず、上記の検証結果フィールドも追加表示しない。

### `history [--stats]`

オプションなしでは従来どおり最大200件の接続試行履歴を表示します。`--stats` は全保存履歴から次を集計します。

- 観測期間とサンプル数
- BSSID別の成功数、試行数、成功率
- 失敗したエラーコード別件数
- 有効な `actual_bssid` が直前記録から変わった回数
- ローカル時刻の4時間帯（0～5時、6～11時、12～17時、18～23時）別の成功数、試行数、成功率
- `duration_ms`がある新規履歴の平均接続所要時間

旧7列・8列履歴には接続所要時間がないため、所要時間付きサンプルが1件もない場合は従来どおりテキストで`unavailable (not recorded)`、JSONで`average_connection_time_ms: null`です。

### `blackbox [--since 30m|24h|7d] [--format text|json]`

永続化済みのWi-Fi Black Box障害レコードを表示します。各レコードには障害種別、時刻、エラーコード、明示的に取得できた場合だけの関連BSSID、障害前の観測、原因候補、推奨対処が含まれます。`--since`は指定期間より新しいレコードだけへ絞り込みます。省略時は保存上限内の全レコード（既定最大500件）です。

```powershell
wifiopt blackbox
wifiopt blackbox --since 24h --format json
```

### `diagnose [--since 30m|24h|7d] [--json]`

現在状態と接続試行の件数、失敗数、エラー845件数、保存ディレクトリを表示します。`--since` は正の整数と `s`、`m`、`h`、`d` のいずれかからなる相対期間です。省略時は従来どおり直近20件です。不正値は終了コード2です。

`--report [--format text|json]`を付けると、現在の接続状態、`history --stats`と同じ集計、直近20件のBlack Boxレコード、Windows WLAN AutoConfig Operationalログの関連イベントを1つのレポートにします。`--since`省略時、イベントログは直近24時間、履歴統計は保存済み全履歴が対象です。イベントログが権限不足、チャネル不在、非対応環境などで読めない場合も、理由をレポート内に記して現在状態・履歴・Black Boxの出力を続けます。

```powershell
wifiopt diagnose --report
wifiopt diagnose --report --since 7d --format json
```

`--report`なしの既定出力は変更されません。

### `simulate --input FILE [--weights FILE] [--compare] [--explain] [--format text|json]`

`scan --format json` の出力をオフラインで読み、既存の `rank_access_points` / `score_access_point` で再計算します。WLANハンドルを作成せず、スキャンも接続も行いません。

- `--weights FILE`: 下記の重みJSONを適用する。省略時は従来値と完全に同じ。
- `--compare`: 既定重みの順位と選択重みの順位を併記する。
- `--explain`: 各APの `reasons` を出力する。

重みファイル例:

```json
{
  "signal_rssi": 32,
  "signal_quality": 13,
  "bss_load": 20,
  "bss_load_neutral": 10,
  "channel_interference": 8,
  "channel_interference_decay": 0.35,
  "connection_history": 17,
  "connection_history_neutral": 8.5,
  "current_ap_bonus": 8,
  "recent_failure_penalty": 7,
  "connection_history_half_life_days": 7,
  "history_reference_unix": 1700000000
}
```

`connection_history_half_life_days`を省略すると、接続履歴は従来と完全に同じ整数集計とLaplace平滑化です。指定時だけ、入力APの`history_samples`（各要素は`unix_time`と`success`）へ指数時間減衰を適用します。`history_reference_unix`は再現可能な比較用の任意値で、省略時は現在時刻です。個別サンプルのない通常のscan JSONでは従来集計へフォールバックします。半減期は0より大きく36500日以下、その他の重みは0以上1000以下の有限数です。リポジトリ同梱の`tests/data/scan_sample.json`と`tests/data/weights_time_decay.json`は`--compare`でこの代替方式をオフライン比較できます。

### `watch --interval SECONDS|Ns|Nm [--duration Ns|Nm|Nh|Nd] [--format text|ndjson] [--ssid SSID]`

対象SSIDを継続的にスキャン・ランキングします。`--interval` は必須で最小5秒です。単位なしは秒として扱います。`--duration` 省略時はCtrl+Cまで継続します。各周期のランキングだけを保持するため、監視時間に比例するメモリ蓄積は行いません。読み取り専用で接続操作は行いません。

### `auto on|off|status [--ssid SSID]`

自動最適化daemonを開始、停止、照会します。`auto on` はmanual以外のモードで実際の接続変更を行い得ます。`auto status`は従来項目に加えて現在のモードを表示します。

### `target [SSID]`

引数なしで保存対象SSIDを表示し、引数ありで `target_ssid` だけを更新します。

### `mode [normal|gaming|call|manual]`

引数なしで保存済み動作モードを表示し、引数ありで`mode`だけを更新します。`enabled`やSSID、間隔、閾値、クールダウン等は変更しません。

- `normal`: 従来のヒステリシス閾値を使用。
- `gaming`: 設定されたgaming追加マージンを閾値へ加算。
- `call`: gamingより慎重なcall追加マージンを閾値へ加算。
- `manual`: daemonは観測・状態記録を続けるが接続を変更しない。明示的な`optimize`/`connect`は実行可能。

プロセス検出はconfig JSONの`process_detection_enabled`を`true`へ明示変更した場合だけ有効です。`call_process_names`は実行ファイル名のJSON配列で、既定は`Discord.exe`、`Teams.exe`、`ms-teams.exe`、`Zoom.exe`です。一致中は保存モードを変更せず、その評価周期だけcall相当になります。無効時はプロセス列挙を行いません。

## JSONスキーマ

JSON文書は必ずトップレベルに `schema_version: 1`、`command`、`data` を持ちます。フィールド追加は互換変更、意味変更や削除は将来のスキーマ番号更新対象です。

`scan` の概略例（`simulate --input` がそのまま受理）:

```json
{
  "schema_version": 1,
  "command": "scan",
  "data": [
    {
      "ssid": "AP-TUT5",
      "bssid": "00:11:22:33:44:55",
      "rssi_dbm": -55,
      "signal_quality": 80,
      "frequency_khz": 5180000,
      "channel": 36,
      "band": "5 GHz",
      "current": false,
      "cochannel_neighbors": 2,
      "bss_load": {
        "station_count": 10,
        "channel_utilization": 64,
        "available_admission_capacity": 0
      },
      "phy_standard": "802.11ac (VHT)",
      "beacon_interval_tu": 100,
      "ht_capabilities": true,
      "vht_capabilities": true,
      "extended_capabilities": true,
      "bss_transition_management": null,
      "vendor_specific_ie_count": 2,
      "last_seen_unix": 1700000000,
      "receive_link_speed_bps": null,
      "transmit_link_speed_bps": null,
      "history_attempts": 0,
      "history_successes": 0,
      "recent_failures": 0
    }
  ]
}
```

`rank` と `simulate` のAPには `quality_score`、`score`、必要に応じ `reasons` が加わります。欠損値は `null` であり、0や非対応を意味しません。NDJSONは各非空行が独立したJSONオブジェクトです。

## 終了コード

| コード | 意味 |
|---:|---|
| 0 | コマンドが正常完了した。dry-runで「切替不要」または「切替予定」の判定が完了した場合も0。 |
| 1 | Native Wi-Fi API、ファイルI/O、接続、その他の実行時エラー。 |
| 2 | コマンド、引数、形式、期間、BSSID、入力JSONなどが不正、または `inspect` の対象が見つからない。 |

