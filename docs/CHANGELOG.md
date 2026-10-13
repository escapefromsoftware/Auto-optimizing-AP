# 変更履歴

## v1.6 Application-aware Optimization / Personalized AP Learning

- `normal`、`gaming`、`call`、`manual`の動作モードと`wifiopt mode`を追加。
- オプトインのプロセス名検出で、通話アプリ実行中だけcall相当の切替抑制を適用。列挙失敗時はmanual相当へフォールバック。
- 接続要求時間`duration_ms`を9列目へ記録し、旧7列・8列履歴を継続読込。
- 時間帯別成功率と、明示指定時だけ有効な履歴の指数時間減衰を追加。

## v1.5 GUI拡張

- 直近1時間のRSSI、Signal Quality、選択スコアを表示・出力するグラフを追加。
- 複数AP比較、推薦文、同一物理APの可能性を示す保守的なヒューリスティックを追加。
- Black BoxとWLANイベントを表示・出力する診断ビューを追加し、Per-Monitor DPI v2対応を拡張。

## v1.4 Closed-loop Optimization

- 自動切替後に希望/実BSSID一致と非APIPA IPv4を確認し、切替前後の品質を記録。
- BSSID不一致またはIPv4確認失敗時、元BSSIDへの1回だけのロールバックを追加。
- 履歴を7列から、オプショナルなIPv4確認結果を持つ8列へ拡張し、旧形式との互換読込を維持。

## v1.3 Black Box / イベントログ

- 障害前観測をリング保持し、接続失敗や予期しない切断を永続化するWi-Fi Black Boxを追加。
- Windows WLAN AutoConfig Operationalログの読み取り専用照会と診断レポートを追加。
- イベント取得不能時も状態、履歴、Black Boxの診断を継続するフォールバックを追加。

## v1.2 CLI高度化

- JSON、CSV、NDJSON、quiet出力、`inspect`、`simulate --weights --compare`、`watch`を追加。
- `target`による対象SSIDの独立永続化、`history --stats`、期間指定診断を追加。
- schema version 1のscan JSONを使うオフライン再ランキングを追加。

## v1.1 AP詳細 / スコア分離

- HT/VHT/Extended Capabilities、BSS Transition Management、PHY、ビーコン間隔等のAP詳細を追加。
- 無線品質だけの品質スコアと、履歴・失敗・現在APを含む選択スコアを分離。
- 最終観測時刻と現在APのリンク速度を追加し、BSS Load欠損を推測せず表示。

## v1.0 初期実装

- Windows Native Wi-Fi APIによるBSSスキャン、BSSID指定接続、状態表示を実装。
- RSSI、Signal Quality、BSS Load、同一チャネル近隣、履歴を用いる説明可能なランキングを実装。
- 保存済みプロファイルだけを使うCLI、自動監視、ヒステリシス、クールダウン、接続履歴の基盤を実装。
