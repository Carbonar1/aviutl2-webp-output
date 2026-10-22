# AviUtl2 WebP出力ツール

AviUtl2から透過（アルファ）付きのロスレスアニメーションWebPを書き出すための自作ツール2つ。Claude Codeで開発。

> 開発中。動作確認は作者環境のみ。

## 構成

| ツール | 種類 | 対象 | 用途 |
|---|---|---|---|
| `webp_output2/` | AviUtl2出力プラグイン（.auo2 / x64） | AviUtl2 | AviUtl2から直接WebPを出力 |
| `avi2webp/` | 単独変換ツール（.exe / x64） | 無印AviUtlなど | 透過AVI・連番PNGをWebPに変換 |

## ダウンロード

ビルド済みのファイルは [Releases](../../releases) から入手できる。

- `webp_output2.au2pkg.zip`：AviUtl2用プラグインのパッケージ
- `avi2webp.exe`：変換ツール

※ 署名なしの自作exeのため、ダウンロード時や実行時にブラウザ・Windows Defenderが警告を出す場合がある。

## 1. WebP Output2（webp_output2.auo2）

AviUtl2の出力プラグイン。ファイル出力の一覧に **「WebP Output2 (Lossless+Alpha)」** として表示される。

- 透過（アルファチャンネル）付きのWebPを直接出力
- 常にロスレス（劣化なし）
- 設定ダイアログで指定可能：圧縮率（0〜9）・ループ回数（0=無限）
- 設定はプラグインと同じフォルダの `.ini` に保存

無印AviUtlの出力プラグインSDKにはアルファを受け取る仕組みがなく、透過出力には変換ツールを挟む2段階方式が必要だった。AviUtl2のSDKでは乗算済みアルファ付きの16bit RGBA（PA64形式）でフレームを受け取れるため、プラグイン1本で透過WebPを出せる。

### 導入
1. Releasesから `webp_output2.au2pkg.zip` をダウンロード
2. AviUtl2のパッケージとしてインストールする（またはzip内の `Plugin\webp_output2\webp_output2.auo2` をAviUtl2の `Plugin` フォルダに置く）
3. AviUtl2を起動し、ファイル出力から「WebP Output2」を選ぶ

## 2. avi2webp（透過WebP変換）

連番PNG（RGBA）または無圧縮RGBA(32bit) AVIから、ロスレスのアニメーションWebPを作るコマンドラインツール。

### 使い方
- **ドラッグ＆ドロップ**：AVIファイルかPNGが入ったフォルダを `avi2webp.exe` に落とすと、同じ場所に `元の名前_webp.webp` を作る
- **コマンドライン**：

```
avi2webp <入力フォルダ or AVIファイル> <出力ファイル.webp> [オプション]

  --fps N     フレームレート（既定：PNG=30 / AVI=ファイルのfps）
  --loop N    繰り返し回数（0=無限ループ、既定：0）
  --method N  WebP圧縮メソッド 0〜6（既定：6、大きいほど時間がかかる）
```

### 無印AviUtlでの透過AVIの作り方
1. 「ファイル」→「プラグイン出力」→「拡張編集 AVI/BMP出力(RGBA)」
2. ビデオ圧縮で **RGBA(32bit) 非圧縮** を選んで出力

※ UtVideo・Lagarith等の圧縮コーデックAVIは非対応

## ビルド

- Visual Studio 2022（C++によるデスクトップ開発）
- どちらも **x64**
- ソースはUTF-8。コンパイルオプションは `/source-charset:utf-8 /execution-charset:.932`

### 必要なもの（同梱していないもの）

ビルドには次の2つをリポジトリ内に用意する。

```
aviutl2-webp-output/
├ aviutl2_sdk/
│  └ include/          ← AviUtl2 SDK のヘッダ（output2.h など）
├ avi2webp/
│  ├ libwebp/          ← libwebp のソース一式
│  └ avi2webp.cpp など
└ webp_output2/
```

1. **AviUtl2 SDK**：AviUtl2公式配布のSDKから `include` フォルダを `aviutl2_sdk/include/` に置く（再配布の可否が不明なため同梱していない）
2. **libwebp**：公式リポジトリを `avi2webp/libwebp/` に取得し、静的ライブラリとしてビルドする

```
git clone https://chromium.googlesource.com/webm/libwebp avi2webp/libwebp
cd avi2webp/libwebp
nmake /f Makefile.vc CFG=release-static RTLIBCFG=static OBJDIR=output ARCH=x64
```

これで `avi2webp/libwebp/output/release-static/x64/lib/` に `libwebp.lib` `libwebpmux.lib` `libsharpyuv.lib` ができる。「x64 Native Tools Command Prompt for VS 2022」から実行すること。

### ビルドコマンド例
```
msbuild avi2webp\avi2webp.vcxproj /p:Configuration=Release /p:Platform=x64
msbuild webp_output2\webp_output2.vcxproj /p:Configuration=Release /p:Platform=x64
```

## 使用ライブラリ

- [libwebp](https://chromium.googlesource.com/webm/libwebp)（BSD License、全文は `LICENSE-libwebp.txt`）。配布バイナリに静的リンクしている
- [stb_image.h](https://github.com/nothings/stb)（Public Domain / MIT）

## 関連

- 無印AviUtl向けのAPNG版：[aviutl-apng](https://github.com/Carbonar1/aviutl-apng)

## 作者

松尾龍太（[@matsuo_kun2013](https://x.com/matsuo_kun2013)）
