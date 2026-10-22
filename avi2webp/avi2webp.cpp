//----------------------------------------------------------------------------------
//	avi2webp - 連番PNG(RGBA)からアニメーションWebP(ロスレス、アルファ対応)を作る単独コマンドラインツール
//----------------------------------------------------------------------------------
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <string>
#include <algorithm>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "webp/encode.h"
#include "webp/mux.h"

//---------------------------------------------------------------------
//		連番PNGファイルの列挙 (フォルダ内 *.png を名前順に)
//---------------------------------------------------------------------
static bool list_png_files(const std::string &dir, std::vector<std::string> &out)
{
	WIN32_FIND_DATAA fd;
	std::string pattern = dir + "\\*.png";
	HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return false;
	do {
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
			out.push_back(dir + "\\" + fd.cFileName);
		}
	} while (FindNextFileA(h, &fd));
	FindClose(h);
	std::sort(out.begin(), out.end());
	return !out.empty();
}

//---------------------------------------------------------------------
//		AVI読み込み (無圧縮 BI_RGB 32bit のみ対応)
//---------------------------------------------------------------------
typedef struct {
	std::vector<BYTE> data;					//	ファイル全体
	std::vector<std::pair<size_t, DWORD>> frames;	//	各フレームの(データ開始オフセット,サイズ)
	int w, h;
	double fps;
} AVI_INFO;

//	4CCの先頭2文字がストリーム番号、後2文字が "db"(未圧縮画像) or "dc"(圧縮画像)
static bool is_video_chunk_id(const char *id)
{
	return isdigit((unsigned char)id[0]) && isdigit((unsigned char)id[1])
		&& (id[2] == 'd') && (id[3] == 'b' || id[3] == 'c');
}

static bool load_avi(const std::string &path, AVI_INFO &out)
{
	HANDLE fh = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (fh == INVALID_HANDLE_VALUE) return false;
	DWORD fsize = GetFileSize(fh, NULL);
	out.data.resize(fsize);
	DWORD readBytes = 0;
	BOOL ok = ReadFile(fh, out.data.data(), fsize, &readBytes, NULL);
	CloseHandle(fh);
	if (!ok || readBytes != fsize) return false;

	const BYTE *d = out.data.data();
	if (fsize < 12 || memcmp(d, "RIFF", 4) != 0 || memcmp(d + 8, "AVI ", 4) != 0) return false;

	out.w = out.h = 0;
	BOOL foundStrf = FALSE;
	double microSecPerFrame = 0;

	//	RIFFの直下(hdrl/movi等のLIST)を順に辿る
	size_t pos = 12;
	while (pos + 8 <= fsize) {
		char id[5] = { 0 }; memcpy(id, d + pos, 4);
		DWORD size = *(const DWORD *)(d + pos + 4);
		size_t bodyStart = pos + 8;

		if (!strcmp(id, "LIST") && bodyStart + 4 <= fsize) {
			char listType[5] = { 0 }; memcpy(listType, d + bodyStart, 4);
			size_t inner = bodyStart + 4, innerEnd = bodyStart + size;
			if (innerEnd > fsize) innerEnd = fsize;

			if (!strcmp(listType, "hdrl")) {
				//	avih / strl(strh,strf) を探索
				size_t p2 = inner;
				while (p2 + 8 <= innerEnd) {
					char id2[5] = { 0 }; memcpy(id2, d + p2, 4);
					DWORD size2 = *(const DWORD *)(d + p2 + 4);
					size_t body2 = p2 + 8;
					if (!strcmp(id2, "avih") && body2 + 4 <= innerEnd) {
						DWORD usPerFrame = *(const DWORD *)(d + body2);
						microSecPerFrame = (double)usPerFrame;
					} else if (!strcmp(id2, "LIST") && body2 + 4 <= innerEnd) {
						char lt2[5] = { 0 }; memcpy(lt2, d + body2, 4);
						if (!strcmp(lt2, "strl")) {
							size_t p3 = body2 + 4, end3 = body2 + size2;
							if (end3 > innerEnd) end3 = innerEnd;
							while (p3 + 8 <= end3) {
								char id3[5] = { 0 }; memcpy(id3, d + p3, 4);
								DWORD size3 = *(const DWORD *)(d + p3 + 4);
								size_t body3 = p3 + 8;
								if (!strcmp(id3, "strf") && body3 + 20 <= end3) {
									//	BITMAPINFOHEADER
									int biWidth = *(const int *)(d + body3 + 4);
									int biHeight = *(const int *)(d + body3 + 8);
									WORD biBitCount = *(const WORD *)(d + body3 + 14);
									DWORD biCompression = *(const DWORD *)(d + body3 + 16);
									if (biBitCount == 32 && biCompression == 0) {
										out.w = biWidth;
										out.h = (biHeight < 0) ? -biHeight : biHeight;
										foundStrf = TRUE;
									}
								}
								p3 += 8 + size3 + (size3 & 1);
							}
						}
					}
					p2 += 8 + size2 + (size2 & 1);
				}
			} else if (!strcmp(listType, "movi")) {
				size_t p2 = inner;
				while (p2 + 8 <= innerEnd) {
					char id2[5] = { 0 }; memcpy(id2, d + p2, 4);
					DWORD size2 = *(const DWORD *)(d + p2 + 4);
					size_t body2 = p2 + 8;
					if (is_video_chunk_id(id2)) {
						if (id2[3] == 'c') { return false; }	//	圧縮フレームは非対応
						out.frames.push_back(std::make_pair(body2, size2));
					}
					p2 += 8 + size2 + (size2 & 1);
				}
			}
		}
		pos += 8 + size + (size & 1);
	}

	if (!foundStrf || out.w <= 0 || out.h <= 0 || out.frames.empty()) return false;
	out.fps = (microSecPerFrame > 0.0) ? (1000000.0 / microSecPerFrame) : 30.0;
	return true;
}

//---------------------------------------------------------------------
//		使い方
//---------------------------------------------------------------------
static void print_usage()
{
	printf(
		"avi2webp - 連番PNG(RGBA) または 無圧縮RGBA(32bit) AVI からロスレスアニメーションWebPを作成\n"
		"\n"
		"使い方:\n"
		"  avi2webp <入力フォルダ or AVIファイル> <出力ファイル.webp> [オプション]\n"
		"\n"
		"オプション:\n"
		"  --fps N     フレームレート (既定値: PNG=30 / AVI=ファイルのfps)\n"
		"  --loop N    繰り返し回数 (0=無限ループ, 既定値: 0)\n"
		"  --method N  WebP圧縮メソッド 0-6 (既定値: 6, 大きいほど圧縮に時間がかかる)\n"
		"\n"
		"※出力は常にロスレス(lossless=1, quality=100)。\n"
		"※AVIは AviUtl の「拡張編集 AVI/BMP出力(RGBA)」で\n"
		"  ビデオ圧縮=RGBA(32bit)非圧縮 で出力したものに対応。\n"
		"  UtVideo/Lagarith等の圧縮コーデックには非対応。\n"
	);
}

//---------------------------------------------------------------------
//		メイン
//---------------------------------------------------------------------
//	入力パスから出力ファイル名を自動生成する (ドラッグ&ドロップ用)
//	例: "C:\a\test1.avi" -> "C:\a\test1_webp.webp" / "C:\a\frames" -> "C:\a\frames_webp.webp"
static std::string make_default_output(const std::string &inputPath)
{
	std::string base = inputPath;
	while (!base.empty() && (base.back() == '\\' || base.back() == '/')) base.pop_back();
	size_t slash = base.find_last_of("\\/");
	std::string name = (slash == std::string::npos) ? base : base.substr(slash + 1);
	size_t dot = name.find_last_of('.');
	if (dot != std::string::npos) name = name.substr(0, dot);
	std::string dir = (slash == std::string::npos) ? "" : base.substr(0, slash + 1);
	return dir + name + "_webp.webp";
}

static int run_main(int argc, char **argv, bool interactive)
{
	std::string inputPath = argv[1];
	std::string outputFile;
	int optStart;

	if (interactive) {
		//	ドラッグ&ドロップ: 出力ファイル名は自動生成
		outputFile = make_default_output(inputPath);
		optStart = 2;
	} else {
		outputFile = argv[2];
		optStart = 3;
	}

	double fps = 0.0;	//	0 = 未指定
	int loop = 0;
	int method = 6;

	for (int i = optStart; i < argc; i++) {
		if (!strcmp(argv[i], "--fps") && i + 1 < argc) fps = atof(argv[++i]);
		else if (!strcmp(argv[i], "--loop") && i + 1 < argc) loop = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--method") && i + 1 < argc) method = atoi(argv[++i]);
		else { fprintf(stderr, "不明なオプション: %s\n", argv[i]); print_usage(); return 1; }
	}
	if (method < 0) method = 0;
	if (method > 6) method = 6;

	//	入力がフォルダ(連番PNG) か AVIファイル かを判定
	DWORD attr = GetFileAttributesA(inputPath.c_str());
	bool isDir = (attr != INVALID_FILE_ATTRIBUTES) && (attr & FILE_ATTRIBUTE_DIRECTORY);
	bool useAvi = false;
	std::vector<std::string> files;
	AVI_INFO avi;

	if (isDir) {
		if (!list_png_files(inputPath, files)) {
			fprintf(stderr, "エラー: 入力フォルダにPNGファイルが見つからない: %s\n", inputPath.c_str());
			return 1;
		}
		printf("%zu枚のPNGを検出\n", files.size());
		if (fps <= 0.0) fps = 30.0;
	} else {
		if (!load_avi(inputPath, avi)) {
			fprintf(stderr,
				"エラー: AVIを読み込めない、または非対応形式(無圧縮RGBA(32bit)のみ対応): %s\n",
				inputPath.c_str());
			return 1;
		}
		useAvi = true;
		printf("AVIから%zuフレーム検出 (%dx%d, %.3ffps)\n", avi.frames.size(), avi.w, avi.h, avi.fps);
		if (fps <= 0.0) fps = avi.fps;
	}

	size_t frameCount = useAvi ? avi.frames.size() : files.size();

	//	1フレームあたりのミリ秒 (累積タイムスタンプ計算用)
	double msPerFrame = 1000.0 / fps;

	WebPAnimEncoderOptions encOptions;
	if (!WebPAnimEncoderOptionsInit(&encOptions)) {
		fprintf(stderr, "エラー: WebPAnimEncoderOptionsInitに失敗\n");
		return 1;
	}
	encOptions.anim_params.loop_count = (loop < 0) ? 0 : loop;
	encOptions.anim_params.bgcolor = 0x00000000;	//	透明背景

	WebPConfig config;
	if (!WebPConfigInit(&config)) {
		fprintf(stderr, "エラー: WebPConfigInitに失敗\n");
		return 1;
	}
	config.lossless = 1;
	config.quality = 100.0f;
	config.method = method;
	config.exact = 1;	//	透明部分のRGB値も保持(見た目には影響しないが劣化防止)
	if (!WebPValidateConfig(&config)) {
		fprintf(stderr, "エラー: WebP設定が不正\n");
		return 1;
	}

	int w = 0, h = 0;
	WebPAnimEncoder *enc = NULL;
	int timestamp_ms = 0;

	for (size_t i = 0; i < frameCount; i++) {
		unsigned char *img = NULL;		//	トップダウンRGBAのフレームデータへのポインタ
		std::vector<BYTE> aviFrameBuf;	//	AVIモード時の変換用バッファ
		int fw, fh;

		if (useAvi) {
			fw = avi.w; fh = avi.h;
			size_t off = avi.frames[i].first;
			DWORD len = avi.frames[i].second;
			if (len < (DWORD)(fw * fh * 4)) {
				fprintf(stderr, "エラー: AVIフレームサイズが不足 (frame %zu)\n", i);
				if (enc) WebPAnimEncoderDelete(enc);
				return 1;
			}
			const BYTE *src = avi.data.data() + off;	//	BGRA, 下から上向き, パディング無し
			aviFrameBuf.resize((size_t)fw * fh * 4);
			for (int y = 0; y < fh; y++) {
				const BYTE *srow = src + (size_t)(fh - 1 - y) * fw * 4;
				BYTE *drow = aviFrameBuf.data() + (size_t)y * fw * 4;
				for (int x = 0; x < fw; x++) {
					drow[x * 4 + 0] = srow[x * 4 + 2];	//	R
					drow[x * 4 + 1] = srow[x * 4 + 1];	//	G
					drow[x * 4 + 2] = srow[x * 4 + 0];	//	B
					drow[x * 4 + 3] = srow[x * 4 + 3];	//	A
				}
			}
			img = aviFrameBuf.data();
		} else {
			int fc;
			img = stbi_load(files[i].c_str(), &fw, &fh, &fc, 4);	//	強制RGBA
			if (!img) {
				fprintf(stderr, "エラー: 画像を読み込めない: %s\n", files[i].c_str());
				if (enc) WebPAnimEncoderDelete(enc);
				return 1;
			}
		}

		if (i == 0) {
			w = fw; h = fh;
			enc = WebPAnimEncoderNew(w, h, &encOptions);
			if (!enc) {
				fprintf(stderr, "エラー: WebPAnimEncoderNewに失敗\n");
				if (!useAvi) stbi_image_free(img);
				return 1;
			}
		} else if (fw != w || fh != h) {
			fprintf(stderr, "エラー: 画像サイズが一致しない (%dx%d)\n", fw, fh);
			if (!useAvi) stbi_image_free(img);
			WebPAnimEncoderDelete(enc);
			return 1;
		}

		WebPPicture pic;
		if (!WebPPictureInit(&pic)) {
			fprintf(stderr, "エラー: WebPPictureInitに失敗\n");
			if (!useAvi) stbi_image_free(img);
			WebPAnimEncoderDelete(enc);
			return 1;
		}
		pic.width = w;
		pic.height = h;
		pic.use_argb = 1;
		if (!WebPPictureImportRGBA(&pic, img, w * 4)) {
			fprintf(stderr, "エラー: WebPPictureImportRGBAに失敗 (frame %zu)\n", i);
			if (!useAvi) stbi_image_free(img);
			WebPPictureFree(&pic);
			WebPAnimEncoderDelete(enc);
			return 1;
		}
		if (!useAvi) stbi_image_free(img);

		if (!WebPAnimEncoderAdd(enc, &pic, timestamp_ms, &config)) {
			fprintf(stderr, "エラー: WebPAnimEncoderAddに失敗 (frame %zu): %s\n", i, WebPAnimEncoderGetError(enc));
			WebPPictureFree(&pic);
			WebPAnimEncoderDelete(enc);
			return 1;
		}
		WebPPictureFree(&pic);

		timestamp_ms += (int)(msPerFrame + 0.5);

		printf("\r%zu/%zu フレーム処理中...", i + 1, frameCount);
		fflush(stdout);
	}
	printf("\n");

	//	最終フレームの終端タイムスタンプを打つため、frame=NULLで締める
	if (!WebPAnimEncoderAdd(enc, NULL, timestamp_ms, NULL)) {
		fprintf(stderr, "エラー: WebPAnimEncoderAdd(終端)に失敗: %s\n", WebPAnimEncoderGetError(enc));
		WebPAnimEncoderDelete(enc);
		return 1;
	}

	WebPData webpData;
	WebPDataInit(&webpData);
	if (!WebPAnimEncoderAssemble(enc, &webpData)) {
		fprintf(stderr, "エラー: WebPAnimEncoderAssembleに失敗: %s\n", WebPAnimEncoderGetError(enc));
		WebPAnimEncoderDelete(enc);
		return 1;
	}
	WebPAnimEncoderDelete(enc);

	HANDLE fp = CreateFileA(outputFile.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (fp == INVALID_HANDLE_VALUE) {
		fprintf(stderr, "エラー: 出力ファイルを作成できない: %s\n", outputFile.c_str());
		WebPDataClear(&webpData);
		return 1;
	}
	DWORD written = 0;
	size_t expectedSize = webpData.size;
	BOOL wok = WriteFile(fp, webpData.bytes, (DWORD)expectedSize, &written, NULL);
	CloseHandle(fp);
	WebPDataClear(&webpData);
	if (!wok || (size_t)written != expectedSize) {
		fprintf(stderr, "エラー: ファイル書き込みに失敗: %s\n", outputFile.c_str());
		return 1;
	}

	printf("完成: %s\n", outputFile.c_str());
	return 0;
}

//---------------------------------------------------------------------
//		エントリポイント
//		exeにファイル/フォルダをドラッグ&ドロップした場合(引数1個)は
//		出力先を自動生成し、終了時にキー入力待ちでウィンドウを閉じない。
//---------------------------------------------------------------------
int main(int argc, char **argv)
{
	if (argc < 2) {
		print_usage();
		system("pause");
		return 1;
	}

	bool interactive = (argc == 2) || (argc >= 3 && argv[2][0] == '-');
	int ret = run_main(argc, argv, interactive);

	if (interactive) {
		printf("\n");
		system("pause");
	}
	return ret;
}
