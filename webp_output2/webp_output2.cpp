//----------------------------------------------------------------------------------
//	webp_output2 - AviUtl2(ExEdit2) 出力プラグイン
//	ロスレスWebP(アルファチャンネル対応)を直接出力する
//----------------------------------------------------------------------------------
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cwchar>
#include <vector>
#include <algorithm>

#include "aviutl2_sdk/output2.h"

#include "webp/encode.h"
#include "webp/mux.h"

#include "webp_output2.h"

//---------------------------------------------------------------------
//	PA64等のFOURCC値を組み立てるマクロ (バイトオーダーに注意、下位バイトから先頭文字)
//---------------------------------------------------------------------
#define MAKE_FOURCC(a, b, c, d) \
	((DWORD)(BYTE)(a) | ((DWORD)(BYTE)(b) << 8) | ((DWORD)(BYTE)(c) << 16) | ((DWORD)(BYTE)(d) << 24))

static const DWORD FORMAT_PA64 = MAKE_FOURCC('P', 'A', '6', '4');

//---------------------------------------------------------------------
//	関数プロトタイプ
//---------------------------------------------------------------------
bool func_output(OUTPUT_INFO* oip);
bool func_config(HWND hwnd, HINSTANCE dll_hinst);
LPCWSTR func_get_config_text();

//---------------------------------------------------------------------
//	設定
//---------------------------------------------------------------------
static int  g_level = 9;		//	圧縮率 0-9 (UI表示値。9=最高圧縮/最速ではない側)
static int  g_loop = 0;			//	ループ回数 (0=無限ループ)
static bool g_configLoaded = false;
static wchar_t g_iniPath[MAX_PATH] = L"";
static wchar_t g_configText[128] = L"";

//---------------------------------------------------------------------
//	圧縮率0-9 -> WebP method(0-6) へのスケール変換
//	境界値: 0->0, 9->6 になるように丸めマッピングする
//---------------------------------------------------------------------
static int level_to_method(int level)
{
	if (level < 0) level = 0;
	if (level > 9) level = 9;
	return (level * 6 + 4) / 9;
}

//---------------------------------------------------------------------
//	iniファイルのパスを確定する (dll本体と同じフォルダ、拡張子.ini)
//---------------------------------------------------------------------
static void ensure_ini_path()
{
	if (g_iniPath[0]) return;

	HMODULE hMod = NULL;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
		(LPCWSTR)&ensure_ini_path, &hMod);

	wchar_t path[MAX_PATH] = L"";
	if (hMod && GetModuleFileNameW(hMod, path, MAX_PATH) > 0) {
		wchar_t* dot = wcsrchr(path, L'.');
		if (dot) *dot = L'\0';
		wcscat_s(path, L".ini");
	}
	wcscpy_s(g_iniPath, path);
}

//---------------------------------------------------------------------
//	設定の読み込み(初回のみ)/書き込み
//---------------------------------------------------------------------
static void load_config_if_needed()
{
	if (g_configLoaded) return;
	g_configLoaded = true;

	ensure_ini_path();
	if (!g_iniPath[0]) return;

	int level = GetPrivateProfileIntW(L"WebPOutput2", L"Level", g_level, g_iniPath);
	if (level < 0) level = 0;
	if (level > 9) level = 9;
	g_level = level;

	int loop = GetPrivateProfileIntW(L"WebPOutput2", L"Loop", g_loop, g_iniPath);
	if (loop < 0) loop = 0;
	g_loop = loop;
}

static void save_config()
{
	ensure_ini_path();
	if (!g_iniPath[0]) return;

	wchar_t buf[32];
	swprintf(buf, 32, L"%d", g_level);
	WritePrivateProfileStringW(L"WebPOutput2", L"Level", buf, g_iniPath);

	swprintf(buf, 32, L"%d", g_loop);
	WritePrivateProfileStringW(L"WebPOutput2", L"Loop", buf, g_iniPath);
}

//---------------------------------------------------------------------
//	出力プラグイン構造体定義
//---------------------------------------------------------------------
OUTPUT_PLUGIN_TABLE output_plugin_table = {
	OUTPUT_PLUGIN_TABLE::FLAG_VIDEO,						//	フラグ (映像のみ、音声非対応)
	L"WebP Output2 (Lossless+Alpha)",						//	プラグインの名前
	L"WebP (*.webp)\0*.webp\0",								//	出力ファイルのフィルタ
	L"WebP Output2 version 1.00 (Lossless, Alpha対応)",	//	プラグインの情報
	func_output,											//	出力時に呼ばれる関数へのポインタ
	func_config,											//	出力設定のダイアログを要求された時に呼ばれる関数へのポインタ
	func_get_config_text,									//	出力設定のテキスト情報を取得する時に呼ばれる関数へのポインタ
};

//---------------------------------------------------------------------
//	プラグインDLL初期化関数 (未定義なら呼ばれません)
//---------------------------------------------------------------------
EXTERN_C __declspec(dllexport) bool InitializePlugin(DWORD version) { // versionは本体のバージョン番号
	return true;
}

//---------------------------------------------------------------------
//	プラグインDLL終了関数 (未定義なら呼ばれません)
//---------------------------------------------------------------------
EXTERN_C __declspec(dllexport) void UninitializePlugin() {
}

//---------------------------------------------------------------------
//	出力プラグイン構造体のポインタを渡す関数
//---------------------------------------------------------------------
EXTERN_C __declspec(dllexport) OUTPUT_PLUGIN_TABLE* GetOutputPluginTable(void)
{
	return &output_plugin_table;
}

//---------------------------------------------------------------------
//	PA64(乗算済みアルファ、16bit RGBA)の1フレーム分のバッファを
//	straight alphaの8bit RGBAバッファへ変換する
//
//	src	: func_get_videoが返すポインタ (R16,G16,B16,A16の順に並んだ乗算済みアルファ、タイトパッキング想定)
//	dst	: 変換後のRGBA8bitバッファ (w*h*4バイト、呼び出し側で確保)
//	w,h	: フレームサイズ
//---------------------------------------------------------------------
static void convert_pa64_to_rgba8(const uint16_t* src, uint8_t* dst, int w, int h)
{
	const size_t pixelCount = (size_t)w * h;
	for (size_t i = 0; i < pixelCount; i++) {
		uint16_t r16 = src[i * 4 + 0];
		uint16_t g16 = src[i * 4 + 1];
		uint16_t b16 = src[i * 4 + 2];
		uint16_t a16 = src[i * 4 + 3];

		uint8_t r8, g8, b8, a8;
		if (a16 == 0) {
			r8 = g8 = b8 = 0;
		} else {
			//	乗算済み(premultiplied)16bit値をstraightの8bit値へ変換
			int rr = (int)((uint32_t)r16 * 255u / a16);
			int gg = (int)((uint32_t)g16 * 255u / a16);
			int bb = (int)((uint32_t)b16 * 255u / a16);
			r8 = (uint8_t)std::min(255, rr);
			g8 = (uint8_t)std::min(255, gg);
			b8 = (uint8_t)std::min(255, bb);
		}
		//	アルファは16bit(0-65535) -> 8bit(0-255)に単純スケールダウン(四捨五入)
		a8 = (uint8_t)(((uint32_t)a16 * 255u + 32767u) / 65535u);

		dst[i * 4 + 0] = r8;
		dst[i * 4 + 1] = g8;
		dst[i * 4 + 2] = b8;
		dst[i * 4 + 3] = a8;
	}
}

//---------------------------------------------------------------------
//	出力プラグイン出力関数
//---------------------------------------------------------------------
bool func_output(OUTPUT_INFO* oip) {
	load_config_if_needed();

	if (!(oip->flag & OUTPUT_INFO::FLAG_VIDEO) || oip->n <= 0) {
		MessageBox(NULL, L"映像データがありません。", L"WebP Output2", MB_OK | MB_ICONERROR);
		return false;
	}

	const int w = oip->w;
	const int h = oip->h;

	//	フレームレート (avi2webp.cppと同様のタイムスタンプ計算)
	double fps = (oip->scale > 0) ? ((double)oip->rate / (double)oip->scale) : 30.0;
	if (fps <= 0.0) fps = 30.0;
	double msPerFrame = 1000.0 / fps;

	WebPAnimEncoderOptions encOptions;
	if (!WebPAnimEncoderOptionsInit(&encOptions)) {
		MessageBox(NULL, L"WebPAnimEncoderOptionsInitに失敗しました。", L"WebP Output2", MB_OK | MB_ICONERROR);
		return false;
	}
	encOptions.anim_params.loop_count = (g_loop < 0) ? 0 : g_loop;
	encOptions.anim_params.bgcolor = 0x00000000;	//	透明背景

	WebPConfig config;
	if (!WebPConfigInit(&config)) {
		MessageBox(NULL, L"WebPConfigInitに失敗しました。", L"WebP Output2", MB_OK | MB_ICONERROR);
		return false;
	}
	config.lossless = 1;
	config.quality = 100.0f;
	config.method = level_to_method(g_level);	//	UIの圧縮率0-9をWebP method0-6へ変換
	config.exact = 1;	//	透明部分のRGB値も保持
	config.thread_level = 1;	//	マルチスレッドエンコード有効化(画質・圧縮率に影響なし、速度のみ向上)
	if (!WebPValidateConfig(&config)) {
		MessageBox(NULL, L"WebP設定が不正です。", L"WebP Output2", MB_OK | MB_ICONERROR);
		return false;
	}

	WebPAnimEncoder* enc = WebPAnimEncoderNew(w, h, &encOptions);
	if (!enc) {
		MessageBox(NULL, L"WebPAnimEncoderNewに失敗しました。", L"WebP Output2", MB_OK | MB_ICONERROR);
		return false;
	}

	oip->func_set_buffer_size(4, 0); // データ取得バッファ数を変更 (映像のみ)

	std::vector<uint8_t> rgba8((size_t)w * h * 4);
	int timestamp_ms = 0;
	bool aborted = false;
	bool encodeError = false;

	for (int frame = 0; frame < oip->n; frame++) {
		oip->func_rest_time_disp(frame, oip->n); // 残り時間を表示
		if (oip->func_is_abort()) { aborted = true; break; } // 中断の確認

		void* data = oip->func_get_video(frame, FORMAT_PA64);
		if (!data) {
			encodeError = true;
			break;
		}

		convert_pa64_to_rgba8((const uint16_t*)data, rgba8.data(), w, h);

		WebPPicture pic;
		if (!WebPPictureInit(&pic)) {
			encodeError = true;
			break;
		}
		pic.width = w;
		pic.height = h;
		pic.use_argb = 1;
		if (!WebPPictureImportRGBA(&pic, rgba8.data(), w * 4)) {
			WebPPictureFree(&pic);
			encodeError = true;
			break;
		}

		bool addOk = WebPAnimEncoderAdd(enc, &pic, timestamp_ms, &config);
		WebPPictureFree(&pic);
		if (!addOk) {
			encodeError = true;
			break;
		}

		timestamp_ms += (int)(msPerFrame + 0.5);
	}

	bool ok = false;
	WebPData webpData;
	WebPDataInit(&webpData);

	if (!encodeError) {
		//	最終フレームの終端タイムスタンプを打つため、frame=NULLで締める
		if (!WebPAnimEncoderAdd(enc, NULL, timestamp_ms, NULL)) {
			encodeError = true;
		}
	}

	if (!encodeError && !aborted) {
		if (WebPAnimEncoderAssemble(enc, &webpData)) {
			HANDLE fp = CreateFileW(oip->savefile, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
			if (fp != INVALID_HANDLE_VALUE) {
				DWORD written = 0;
				size_t expectedSize = webpData.size;
				BOOL wok = WriteFile(fp, webpData.bytes, (DWORD)expectedSize, &written, NULL);
				CloseHandle(fp);
				ok = wok && ((size_t)written == expectedSize);
			}
		}
	}

	//	WebPDataClear()は成功判定より前に呼ぶとサイズが0クリアされて誤判定するので、必ず判定の後に呼ぶこと
	WebPDataClear(&webpData);
	WebPAnimEncoderDelete(enc);

	if (aborted) {
		return false;
	}
	if (!ok) {
		MessageBox(NULL, L"WebPファイルの書き出しに失敗しました。", L"WebP Output2", MB_OK | MB_ICONERROR);
		return false;
	}

	return true;
}

//---------------------------------------------------------------------
//	設定ダイアログ プロシージャ
//---------------------------------------------------------------------
static INT_PTR CALLBACK config_dlg_proc(HWND hdlg, UINT umsg, WPARAM wparam, LPARAM lparam)
{
	switch (umsg) {
		case WM_INITDIALOG:
		{
			HWND hCombo = GetDlgItem(hdlg, IDC_COMBO_LEVEL);
			for (int i = 0; i <= 9; i++) {
				wchar_t buf[4];
				swprintf(buf, 4, L"%d", i);
				SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)buf);
			}
			SendMessageW(hCombo, CB_SETCURSEL, (WPARAM)g_level, 0);
			SetDlgItemInt(hdlg, IDC_EDIT_LOOP, (UINT)g_loop, FALSE);
			return TRUE;
		}
		case WM_COMMAND:
			switch (LOWORD(wparam)) {
				case IDOK:
				{
					HWND hCombo = GetDlgItem(hdlg, IDC_COMBO_LEVEL);
					int sel = (int)SendMessageW(hCombo, CB_GETCURSEL, 0, 0);
					if (sel != CB_ERR) {
						if (sel < 0) sel = 0;
						if (sel > 9) sel = 9;
						g_level = sel;
					}

					BOOL ok = FALSE;
					int loop = (int)GetDlgItemInt(hdlg, IDC_EDIT_LOOP, &ok, FALSE);
					g_loop = (loop < 0) ? 0 : loop;

					save_config();
					EndDialog(hdlg, IDOK);
					return TRUE;
				}
				case IDCANCEL:
					EndDialog(hdlg, IDCANCEL);
					return TRUE;
			}
			break;
	}
	return FALSE;
}

//---------------------------------------------------------------------
//	設定ダイアログ表示
//---------------------------------------------------------------------
bool func_config(HWND hwnd, HINSTANCE dll_hinst) {
	load_config_if_needed();
	DialogBoxW(dll_hinst, L"CONFIG", hwnd, config_dlg_proc);
	return true;
}

//---------------------------------------------------------------------
//	出力設定のテキスト情報 (設定ボタンの隣に表示される)
//---------------------------------------------------------------------
LPCWSTR func_get_config_text() {
	load_config_if_needed();
	swprintf(g_configText, 128, L"Lossless / Alpha / 圧縮率%d(method%d) / ループ%d",
		g_level, level_to_method(g_level), g_loop);
	return g_configText;
}
