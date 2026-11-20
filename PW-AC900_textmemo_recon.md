# SHARP PW-AC900「テキストメモ」REA リバースエンジニアリング — 準備レポート

作成日: 2026-11-20
ステータス: **解析環境は構築完了 / 解析対象バイナリは未配置のため逆アセンブル未実施**

---

## 0. 現状サマリ

| 項目 | 状態 |
|---|---|
| REA CLI | ✅ v6.10.1 (`rea-agents`, Node.js 22.23.5) |
| Ghidra | ✅ 12.2 (`C:\tools\ghidra_12.2_PUBLIC`, SHA-256 検証済み) |
| Java | ✅ Temurin JDK 25.0.5.1 (`C:\Program Files\Eclipse Adoptium\jdk-25.0.5.10002-hotspot`) |
| `rea doctor --provider ghidra` | ✅ healthy |
| OpenCode への MCP 登録 | ✅ 完了 (`C:\Users\user\.config\opencode\opencode.json`, バックアップあり) |
| ローカル解析ツール | ✅ `src/nanddump.c` 他（Brain NAND/ROM バックアップツール、ソースあり） |
| **ダンプ済みイメージ / AppMain.exe** | ❌ **未配置**（`Z:` ドライブもアクセス不可） |

> 逆アセンブル／疑似コード解析は、対象の `AppMain.exe`（または NAND ダンプ）が配置されるまで実行できません。
> 以下は公開情報から確定できた事実と、今後の解析計画です。

### 0.1 作業ディレクトリに配置されていたファイル（解析対象ではない）

| ファイル | 内容 |
|---|---|
| `src/nanddump.c` | Windows CE 上で動く NAND ブロックデバイスダンプ工具（C89/Win32） |
| `src/nanddump.rc`, `src/resource.h` | ダイアログリソース（デバイス選択・進捗・出力先） |
| `tools/verify_image.py` | ダンプ画像のヘッダ解析・CRC32 検証・生データ抽出 |
| `README.md` | ビルド／使い方の説明 |

これは**実機からファームウェアを取り出すための前段ツール**であり、REA の解析対象そのものではない。
`nanddump.c` を精査し、以下のバグを修正した:

- ジオメトリ不明時にヘッダの `dwDataSizeLow/High` が 0 のままで、
  `verify_image.py` が「0 バイト / CRC 不一致」と誤判定し `--extract` も空になる問題。
  → ダンプ完了後に実際の書き込みバイト数をヘッダへ書き戻すよう修正（2026-11-20）。

---

## 1. 環境構築の記録

1. `winget install OpenJS.NodeJS.22` → Node.js 22.23.5 / npm 10.9.9
2. `npm install --global rea-agents` → REA CLI 6.10.1
3. `winget install EclipseAdoptium.Temurin.25.JDK` → JDK 25.0.5.1
   - 当初 JDK 21 を入れたが、Ghidra 12.2 は **JDK 25 以上**を要求するため入れ替えた
4. Ghidra 12.2 を GitHub 公式リリースからダウンロード
   - SHA-256: `2c2cd776db3c1f6a097ba418922378ac3649cadd5e844a6f5f088ae628a7be67`（検証一致）
5. 環境変数（ユーザースコープ）:
   - `JAVA_HOME=C:\Program Files\Eclipse Adoptium\jdk-25.0.5.10002-hotspot`
   - `GHIDRA_INSTALL_DIR=C:\tools\ghidra_12.2_PUBLIC`
6. `rea setup --client opencode --yes` で MCP 登録＋REA スキル導入

---

## 2. 対象の同定（観測事実 / 出典付き）

### 2.1 本体

| 項目 | 内容 | 出典 |
|---|---|---|
| 発売 | 2009年7月 | Sharp 機種別情報 |
| 世代 | 第1世代 Brain | Brain Wiki |
| OS | Windows Embedded CE 6.0 | OUCC Blog, Brain Wiki |
| CPU | ARM926EJ-S (Armv5TEJ) | OUCC Blog |
| LCD | 5型 HVGA 480×320 カラー / タッチ対応 | Sharp |
| メモリ | 本体約100MB / microSD 2GB以下 | Sharp FAQ |
| SoC 世代 | 後継 PW-x1 (2021) は i.MX7ULP + μITRON 系 RTOS に刷新 | Zopfcode / Brain Wiki |

> **重要**: 第5世代以降 (PW-x1 等) のアプリはヘッダーレス `AppMain.bin` (ARM 機械語そのもの) だが、
> **PW-AC900 は第1世代なので `AppMain.exe` = Windows CE の PE 実行ファイル**。
> 逆アセンブル時は ARM (Armv5TE / ARM926EJ-S) としてロードする必要がある。

### 2.2 テキストメモ for Brain

| 項目 | 内容 | 出典 |
|---|---|---|
| 開発 | 富士ソフト (株) | Brain Library |
| バージョン | 1.1.0 (レビュー時点) | PC Watch |
| 配布 | PW-AC900 には**内蔵**。他機種は有償 ¥2,934 / 4.0MB | Brain Library |
| IME | FSKAREN for Windows Mobile (予測変換 / AI予測変換) | PC Watch / Brain Library |
| 入力 | キーボードのみ（手書き入力非対応） | Sharp FAQ / PC Watch |
| 最大文字数 | 32,768文字（全角半角問わず） | Sharp FAQ / PC Watch |
| 保存形式 | 拡張子 `.txt` の**プレーンテキスト / Shift-JIS のみ** | Sharp FAQ / PC Watch |
| 保存先 | 本体メモリ＝ルート / microSD＝`TextMemo` フォルダ | PC Watch |
| 文字化け | EUC / Unicode は文字化け。上限超過分は切り捨て表示 | PC Watch |
| ファイル名 | 数字・英字・全角ひらがな/カタカナ・漢字。パス全体で248文字以下 | Sharp 取扱説明書 |
| フォルダ | 1フォルダ200ファイル超は表示されない | Sharp 取扱説明書 |
| 追加アプリ配置 | `アプリ\<任意>\AppMain.exe` + 空の `index.din`（+ `AppMain.cfg`） | OUCC Blog / brainlilo README |

### 2.3 推論（未検証）

- `AppMain.exe` は Windows CE 用の PE で、Windows CE API（`CreateFileW` / `ReadFile` / `WriteFile` 等）を
  `coredll.dll` 経由で呼ぶと推定される（**推論**。インポート解析で要確認）。
- 保存処理では `TextMemo\` と本体ルートの分岐、Shift-JIS ↔ 内部文字列の変換、
  32,768文字の切り捨て、改行コードの正規化が行われると推定される（**推論**）。
- IME (FSKAREN) は独立 DLL として同梱／リンクされている可能性がある（**推論**。PE インポートと
  文字列で要確認）。

---

## 3. なぜバイナリが必要か

- テキストメモの**データ形式**（Shift-JIS の `.txt`）自体は公開情報で既に解明済み。
- ユーザーが要求した「ファーム／アプリのバイナリのリバースエンジニアリング」には
  `AppMain.exe` またはファームウェアダンプ（`nk.bin` 等）が必須。
- これらは有償コンテンツ／実機内蔵のため、**公開のダウンロード先は見つからなかった**。
  - Brain Library のテキストメモは購入品（¥2,934）
  - ファームウェアは実機からのダンプが必要（Brain Hackers の `nkbin_maker` / `BrainDump` 等を使用）
  - 第三者アップローダーからの入手はライセンス上も不確実なため取得していない

---

## 4. 解析計画（バイナリ配置後に実行）

### 4.1 フェーズA: エンジン不要の静的解析

```powershell
$env:JAVA_HOME='C:\Program Files\Eclipse Adoptium\jdk-25.0.5.10002-hotspot'
$env:GHIDRA_INSTALL_DIR='C:\tools\ghidra_12.2_PUBLIC'
rea inspect-pe-imports     AppMain.exe --json   # coredll.dll 等の API
rea inspect-pe-resources   AppMain.exe --json   # 定型文・アイコン・UI リソース
rea inspect-pe-certificates AppMain.exe --json
rea inspect-native-exports AppMain.exe --json
rea inspect-binary-hardening AppMain.exe --json
rea inspect-artifact        AppMain.exe --json
```

### 4.2 フェーズB: Ghidra による逆アセンブル／疑似コード

```powershell
rea analyze  AppMain.exe --provider ghidra --json
rea search   AppMain.exe "TextMemo" --provider ghidra --json
rea search   AppMain.exe "CreateFile" --provider ghidra --json
rea search   AppMain.exe "FSKAREN" --provider ghidra --json
rea function AppMain.exe <address_or_symbol> --provider ghidra --json
rea decompile AppMain.exe <address> --provider ghidra --json
rea xrefs    AppMain.exe <address> --provider ghidra --json
rea decompile-all AppMain.exe --provider ghidra --json
```

MCP ツールを使う場合（このセッションで利用可能）:

```
open_binary { path: "<abs>\AppMain.exe", provider_id: "ghidra" }
binary_overview {}
search_strings { query: "TextMemo" }
search_procedures { query: "CreateFile" }
list_strings {}
list_procedures {}
trace_feature { query: "CreateFileW" }
analyze_function { procedure: "<addr>" }
procedure_pseudo_code { procedure: "<addr>" }
procedure_assembly  { procedure: "<addr>" }
xrefs { address: "<addr>" }
close_binary {}
```

### 4.3 検証したい仮説

| ID | 仮説 | 必要な証拠 |
|---|---|---|
| H1 | 保存先分岐（本体ルート / `TextMemo`） | 文字列 `TextMemo` への xref → パス結合関数 |
| H2 | Shift-JIS 変換と 32,768 文字制限 | `MultiByteToWideChar` / `WideCharToMultiByte` の呼び出し、定数 `0x8000` |
| H3 | 改行コードの扱い | 書き込み前に `\r\n` ↔ `\n` 変換する処理 |
| H4 | IME (FSKAREN) の組み込み | インポート DLL、文字列、リソース |
| H5 | ファイル I/O に Windows CE API を使用 | `coredll.dll` のインポート一覧 |
| H6 | 上限超過時の切り捨て | 長さチェックと切り捨て分岐 |

---

## 5. 次のステップ

### ルート A: 実機から AppMain.exe を直接取得できる場合

1. `C:\Users\user\Documents\TXDic` に `AppMain.exe`（テキストメモ for Brain）を配置
2. `.\analyze.ps1 -Binary .\AppMain.exe` を実行（フェーズA/B を自動実行し JSON を保存）
3. 結果を読み、4.3 の仮説を検証

### ルート B: NAND ダンプから取得する場合（このリポジトリのツールを使用）

1. `src/nanddump.c` を Windows CE 用にビルド（`README.md` 参照:
   eMbedded Visual C++ 4.0 / VS2005+CE SDK / CeGCC）
2. 実機で起動し、**`DSK9:` (NandUser) と `DSK6:` (NandDic)** をダンプ
   - テキストメモの `.txt` は `DSK9:` 側のユーザー領域に存在すると推定
   - アプリ本体 `AppMain.exe` は `DSK8:` (NandSystem) または `DSK6:` に存在すると推定
3. PC 侧で検証・生データ抽出:

   ```bat
   python tools\verify_image.py NAND_DUMP\DSK9.bin
   python tools\verify_image.py NAND_DUMP\DSK9.bin --extract dsk9_raw.bin
   ```

4. 生データから `.txt` と `AppMain.exe` を取り出す
   - FAT イメージなら 7-Zip / `mtools` / `fsstat` 等でマウント・抽出
   - 文字列検索で `BRNDMP01` ヘッダや `AppMain.exe` の PE シグネチャ `MZ` を探す
5. 抽出した `AppMain.exe` をルート A の手順で REA 解析

> いずれのルートも、**実機またはダンプ済みデータが手元にない限り開始できません**。

補助スクリプト: `C:\Users\user\Documents\TXDic\analyze.ps1`

---

## 6. 参照情報源

- SHARP 機種別情報 PW-AC900: https://jp.sharp/support/dictionary/product/pw-ac900.html
- SHARP FAQ 116551 / 116553（テキストメモ保存形式・機能）
- PC Watch「シャープ PW-AC900」（テキストメモ詳細レビュー）
- Brain Library「テキストメモ for Brain」
- Brain Hackers README / Scrapbox（U-Boot 機種判別、EBOOT ファイル名）
- OUCC Blog「SHARP Brain 用アプリケーションの作成方法」（AppMain.exe 配置形式、CeGCC）
- Zopfcode「リセットとフリーズで解析する電子辞書リバエン記」（世代差・アーキテクチャ）
- REA: https://github.com/morluto/rea
