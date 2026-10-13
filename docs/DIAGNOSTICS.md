# 障害解析

## Wi-Fi Black Box

Black Boxは、`watch`、daemon、`connect`、`optimize`がすでに取得した接続状態を軽量な`WifiObservation`へ変換し、プロセス内のリングバッファへ保持する。既定容量は30件で、時刻、SSID、BSSID、RSSI、Signal Quality、接続状態だけを持つ。監視のための追加WLANスキャンは行わない。`WlanClient::scan`内部で取得済みの`ConnectionStatus`を呼び出し側へ返し、接続処理の前後では既存または軽量な`status()`結果を使う。

次を障害イベントとして扱う。

- 接続試行の`ConnectionRecord.success == false`
- 連続する観測が`connected=true`から`false`へ変化し、既知の操作に伴う切断として抑止されていない場合
- 有効な要求BSSIDと実BSSIDが両方得られ、両者が一致しない場合

接続試行中に生じた切断は、その接続失敗レコードで表し、同じ変化を意図しない切断として二重記録しない。`auto off`はWi-Fi切断を実行せずdaemonだけを停止するため、停止後の未接続観測を生成しない。APIには既知の操作を`expected_disconnect=true`として抑止する経路もある。

障害時には、リングに残る障害前の観測、障害時の観測、イベント種別、エラーコード、要求/実BSSID、詳細、原因候補、推奨対処を1レコードへまとめる。BSSIDはNative Wi-Fi APIまたは接続要求から明示的に得た有効値だけを保存し、不明な場合は空文字列のままとする。エラー845は802.1X/EAP認証失敗の証拠として扱うが、特定APの故障や混雑の証明とは扱わない。

レコードはデータディレクトリの`blackbox.jsonl`へschema version 1のJSON Linesとして保存する。既存Storageと同じ一時ファイル＋`MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`による置換を使用し、破損した個別行は読込時に無視する。既定上限は直近500件で、追加時に古いレコードから削る。パスワード、秘密鍵、証明書本体、ユーザー資格情報を取得または記録するフィールドはない。

## Windows WLANイベントログ

`Microsoft-Windows-WLAN-AutoConfig/Operational`を`EvtQuery`のチャネルパスクエリで新しい順に読み、`EvtNext`で列挙し、`EvtRenderEventXml`でXML化する。ログの消去、書込み、チャネル設定変更は行わない。

XMLパーサはイベントログAPIから分離され、文字列から次を抽出する。

- `System/EventID`
- `TimeCreated/@SystemTime`をUnix秒へ変換した時刻
- `EventData/Data`のSSID、BSSID、ReasonCode/ErrorCode、理由またはメッセージ

BSSIDは構文が有効な場合だけ正規化する。理由文字列がない既知イベントには、イベントIDに基づく中立的な短い説明を付ける。接続失敗、切断、セキュリティ/802.1X失敗として関連するイベントだけをレポートへ含める。イベントログを開けない、列挙できない、または環境が非対応の場合は`EventLogResult.available=false`と説明・Windowsエラーコードを返し、Black Boxや履歴統計を失敗させない。

## 診断レポート

`wifiopt diagnose --report`は現在の接続状態、既存`compute_history_stats`による履歴統計、直近20件のBlack Boxレコード、直近24時間（または`--since`指定期間）のWLANイベントを1つにまとめる。`text`と`json`に対応する。イベントログが読めない場合も、その状態をレポート内へ含めて終了を継続する。

従来の`wifiopt diagnose`（`--report`なし）は出力フィールド、既定の直近20件、対応形式、終了コードを変更していない。

## 既知の制約

- リングバッファはプロセス内だけにあり、プロセス開始前や観測間隔の途中の状態は復元できない。
- RSSIとSignal Qualityは観測時にOSから得られた場合だけ意味を持つ。未接続時は既定の`-100`/`0`である。
- DHCP失敗、DNS失敗、既定ゲートウェイ/インターネット到達性喪失は、現在のコアが対応情報や疎通計測を取得しないため検知対象外である。
- Operationalチャネルが無効、存在しない、保持期間外、または読み取り権限がない場合、過去イベントは取得できない。
- Windowsのローカライズやイベントスキーマ差により、XMLに存在しないSSID、BSSID、説明、エラーコードは空欄または`null`になる。値を推測で補完しない。
- `blackbox.jsonl`のread-modify-writeにはプロセス間ロックがなく、複数プロセスが同時に障害を書いた場合の厳密なマージは保証しない。

