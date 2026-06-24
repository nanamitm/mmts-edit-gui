# MMTS Edit GUI

MMTS（MMT/TLV）を非破壊編集するための Qt6/C++ プロトタイプ。

## 主な機能

- テキスト形式 `MMTSMAP 1` / バイナリ形式 `MMTSMAP2` のサイドカーを読み込み。
- トラック / MPT / RAP / シーク点の数を表示。
- シーク・RAP・MPT マーカー付きの軽量タイムラインを描画。
- **フレーム精度の映像プレビュー**（復号済み `.mmts`）: dantto4k の MMT/TLV
  デマルチプレクサ + libavcodec(HEVC) でデコード。シークバーと ±1 フレーム送りに
  対応。シークは `.mmtsmap` の直前 RAP へ飛んでから目的フレームまで前方デコードする。
  デコードはワーカースレッドで実行（UI は固まらない）。スクラブ要求は最新のみ採用
  （coalescing）、直近フレームはキャッシュするため、前方送りは高速・往復は即時。
- 現在のプレビュー位置から In/Out を設定。
- **対話的タイムライン**: 再生ヘッドがプレビューに追従。レーンのクリック/ドラッグで
  シーク＆区間選択。選択区間の In/Out グリップをドラッグしてトリム（「Snap to RAP」
  で RAP に吸着可）。
- **複数区間 EDL**: Segments パネルでカット一覧を管理。Add / Split @ playhead /
  Delete で非破壊のカットリストを作成。編集後のプログラムは区間を順に連結したもの。
- `.mmtsedit` サイドカー（version 1、`timeline` = 順序付き区間）の保存/読込。
- 編集結果を **MPEG-2 TS(.ts) として書き出し**（`File > Export Edited TS...`）:
  各区間を demux→dantto4k の RemuxerHandler で再多重化（HEVC+AAC を TS 化、再エンコード
  なし）。区間ごとに PTS/DTS を再ベースし、**継ぎ目のタイムスタンプを連続化**するため、
  汎用プレイヤーでもなめらかに再生できる（出力 TS の総尺＝各区間尺の合計）。tsduck を
  静的リンクして GUI 内で実行。字幕（ARIB字幕）も区間オフセットを適用して出力に含める。
  **IN/OUT ともフレーム精度**（スマートレンダリング）：IN が RAP 上でなければ lead-in
  `[IN, 次のRAP)` を、OUT が RAP 上でなければ tail `[最後のRAP, OUT)` を hevc_nvenc で
  再エンコード（IDR 始まりの GOP）し、その間をストリームコピーで接合する。コピー終端は
  **デコード順（DTS）で切る**ため、open-GOP の境界参照（RASL の先行ピクチャ）が宙に
  浮かず、繋ぎ目もデコード破綻しない。
- 編集結果を **MMTS(.mmts) として書き出し**（`File > Export Edited MMTS...`）:
  コピー区間はメディアの TLV パケットをバイト一致で透過コピー（無劣化）し、境界の部分
  GOP だけを再エンコードして MMT/TLV パケットとして合成・差し替える（**IN/OUT とも
  フレーム精度**）。音声/字幕 MPU は `[IN, OUT)` でゲーティングする。
  **出力タイムラインは連続**: MMT のプレゼンテーション時刻は MPT の `mpu_timestamp`
  記述子（とPCR源の NTP パケット）にしか存在しないため、2 区間目以降は全 MPT の
  タイムスタンプエントリと NTP をシフトして継ぎ目を詰める（メディアペイロードは
  無改変のまま）。**シーケンス番号も連続**: MMTP パケットシーケンスは全 pid、MPU
  シーケンスは映像/音声/字幕で出力全体を通して +1 連続に振り直す（MPT エントリの
  参照番号も同時に翻訳。データカルーセルの pid は番号自体が ID なので元のまま）。

## 使い方

本エディタは非破壊で、元の `.mmts` を変更しません。編集内容は「ソースの時間範囲の
リスト（EDL）」として小さな `.mmtsedit` ファイルに保存され、`Export` でそのリストから
新しい `.mmts` を生成します。

### 1. メディアを開く

- `File > Open Media...` で（復号済みの）`.mmts` を選択。
- 同じ場所のサイドカーインデックス `<名前>.mmtsmap` を読み込みます。**マップが無い
  場合はアプリが作成を提案**し、ファイル全体をスキャンして生成します（大きな 8K
  録画では数分かかることがあります。キャンセル付きの進捗ダイアログを表示）。結果は
  `dantto4k --write-mmtsmap-only` と同一です。
- 読み込みが終わると先頭フレームがプレビューに表示され、タイムラインに
  RAP（青）/ シーク（緑）/ MPT（橙）のマーカーが出ます。

### 2. プレビューを操作する

- **シークバー**のドラッグ、または**タイムラインのレーン**をクリック/ドラッグして
  スクラブします。
- **`-1f` / `+1f`** で 1 フレーム単位の後退/前進（フレーム精度）。位置ラベルに
  `HH:MM:SS.mmm (#フレーム番号)` を表示します。
- 前方送りは高速で、ある地点付近の往復は即時です（直近のデコード済みフレームを
  キャッシュ）。

### 3. カットリストを作る（Segments タブ）

編集後のプログラムは、**区間を順に連結**したものです。

- **Add** — 現在の再生ヘッドから末尾までの区間を追加。
- **Split @ playhead** — 再生ヘッド位置で区間を 2 つに分割。
- **Delete** — 選択中の区間を削除。
- 区間を選択（テーブル、またはタイムライン上のブロックをクリック）し、**`Set In` /
  `Set Out`**（現在のプレビュー位置を使用）、**In/Out スピンボックス**、または
  タイムライン上の赤/橙の**グリップ**のドラッグで境界を設定します。
- **Snap to RAP**（ツールバー、既定オン）は In/Out ドラッグを最寄りのランダム
  アクセス点に吸着させ、カットをクリーンな GOP 境界に合わせます。

### 4. 編集の保存 / 読込

- `File > Save Edit...` で `.mmtsedit`（EDL）を保存。
- `File > Open Edit...` で復元。参照先のマップ**とソースメディア**を読み込むため、
  すぐにプレビューが使えます。

### 5. TS を書き出す

- `File > Export Edited TS...` で、区間だけを連結した **MPEG-2 TS(.ts)** を生成します。
  各区間を demux→TS 再多重化し（HEVC+AAC、再エンコードなし）、PTS/DTS を連続化します。
  キャンセル付きの進捗ダイアログを表示します。
- 継ぎ目のタイムスタンプが連続化されるため、汎用プレイヤーでもなめらかに再生できます。
  **IN / OUT ともフレーム精度**（RAP 上でなければ lead-in / tail を再エンコードして接合）。
  字幕（ARIB字幕）も含まれます。Snap to RAP を切れば任意フレームで IN/OUT を切れます
  （その区間は再エンコードが入るぶん書き出しに時間がかかります）。

### 6. MMTS を書き出す

- `File > Export Edited MMTS...` で、区間だけを連結した **MMT/TLV(.mmts)** を生成します。
  コピー区間は元のメディアパケットをそのまま透過するため**無劣化**、境界の部分 GOP のみ
  hevc_nvenc で再エンコードして MMT パケットに再パックします（**IN/OUT ともフレーム精度**）。
- **タイムラインもシーケンス番号も連続化されます**: タイムスタンプは MPT シグナリング
  （と NTP パケット）の書き換えで、MMTP パケット/MPU シーケンス番号はヘッダフィールドの
  振り直しで連続化します（メディアペイロードは無改変）。シークも区間をまたいで通常
  どおり動作し、シーケンス連続性チェックを行う再生側でもそのまま扱えます。
  音声/字幕は MPU 粒度（〜0.1 秒）で `[IN, OUT)` にゲーティングされます。

### 7. コマンドライン版（`mmts-edit-cli`）

GUI を開かずに書き出すための、GUI の Export メニューと同じエクスポータ
（`MmtsSmartExporter` / `MmtsTsExporter`）を使うヘッドレス CLI。GUI で対話的に
カットして `.mmtsedit` を保存し、それを CLI でバッチ書き出しする使い方を想定
（大きな 8K 録画を毎回 GUI で開き直さずに済む）。

```text
mmts-edit-cli --edit <project.mmtsedit> --output <out.ts|out.mmts> [options]
mmts-edit-cli --input <src.mmts> --output <out.ts|out.mmts> --cut <start>-<end> [--cut ...] [options]
```

- 出力フォーマットは `--output` の拡張子で決まる: `.ts`（連続 PTS の MPEG-2 TS、
  tsduck 有効ビルドが必要）/ `.mmts`（フレーム精度の MMT/TLV）。
- `.mmtsmap` サイドカーが無い場合は自動でスキャン・生成する（`--map` で明示指定可）。
- `--cut` の時刻はミリ秒、または `[HH:]MM:SS[.mmm]` 形式のタイムコードで指定。
- `--no-frame-accurate`: 境界の再エンコードが（GPU 非対応などで）できない場合に
  失敗させず RAP 境界へフォールバックする。既定はフレーム精度を要求して失敗させる。
- `--quiet`: 進捗出力を抑制。

## 依存関係

- Qt 6（Widgets）。
- dantto4k のソース。既定では `../dantto4k` を参照（CAS 非依存のサブセットを直接
  コンパイル）。
- ビルド済み ffmpeg 開発ライブラリ（LGPL shared）を `thirdparty/ffmpeg`
  （`include/lib/bin`）に配置。場所は `-DDANTTO4K_DIR=...` / `-DFFMPEG_DIR=...` で
  上書き可能。
- tsduck（TS 出力用）。dantto4k の `thirdparty/tsduck` にビルド済みの静的ライブラリ
  （`bin/<Config>-x64/tsducklib.lib` `tscorelib.lib` と `bin/include`）が必要。dantto4k を
  一度ビルドすれば生成される。

`.mmtsedit` の形式（`timeline` はソースのカット区間の順序付きリスト。編集後の
プログラムはその連結）:

```json
{
  "version": 1,
  "source": "sample.mmts",
  "sourceSize": 123456789,
  "map": "sample.mmtsmap",
  "timeline": [
    { "sourceStartMs": 25000,   "sourceEndMs": 60000 },
    { "sourceStartMs": 90000,   "sourceEndMs": 1800000 }
  ]
}
```

## ディレクトリ構成

```text
src/
  main.cpp
  model/    MmtsMap, MmtsEdit          (.mmtsmap / .mmtsedit のパース、EDL モデル)
            MmtsCutPlan                (EDL→RAP境界/コピー範囲の共有プランナ)
            MmtsExporter               (RAP境界バイトコピーの .mmts 出力)
  decode/   MmtsVideoDecoder           (フレーム精度 HEVC デコード)
            MmtsDecodeController        (ワーカースレッド + 先読み + フレームキャッシュ)
            MmtsMapBuilder             (dantto4k スキャンで .mmtsmap 生成)
            MmtsTsExporter             (EDL を連続 PTS の MPEG-2 TS に再多重化)
            MmtsSmartExporter          (フレーム精度 .mmts 出力: 透過コピー+境界スプライス)
  ui/       MainWindow, PreviewWidget, TimelineWidget
tests/      decode_test, controller_test, edit_test, mapbuild_test,
            export_test, ts_export_test, smart_export_test  (ヘッドレスのスモークテスト)
            mmts_edit_cli  (CLI 本体。`mmts-edit-cli` 実行ファイルとしてビルド)
```

## ビルド

```powershell
C:\Qt\Tools\CMake_64\bin\cmake.exe -S . -B build-msvc -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=C:\Qt\6.11.1\msvc2022_64
C:\Qt\Tools\CMake_64\bin\cmake.exe --build build-msvc --config Release
```

ffmpeg の開発ライブラリはリポジトリに含めていません。Windows 向けの LGPL **shared**
ビルド（例: BtbN/FFmpeg-Builds）をダウンロードし、`thirdparty/ffmpeg/{include,lib,bin}`
が存在するように `thirdparty/ffmpeg` に展開するか、`-DFFMPEG_DIR=...` で既存の
インストール先を指定してください。

## ライセンス

MIT（`LICENSE` を参照）。本プロジェクトは dantto4k のソースをリンクし、実行時に
ffmpeg（LGPL）を使用します。これらはそれぞれのライセンスに従います。
