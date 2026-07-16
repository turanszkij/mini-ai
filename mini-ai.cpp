#include <Windows.h>
#include <commdlg.h> // Common Dialogs for Load/Save
#include <dwmapi.h> // DwmSetWindowAttribute
#pragma comment(lib, "dwmapi.lib")

#include <uxtheme.h>
#pragma comment(lib, "uxtheme.lib")
#pragma comment(linker,"\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")

#include <thread>
#include <string>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_WINDOWS_UTF8
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"

#include "stable-diffusion.h"

#define LINK_DLL_FUNCTION(name, dll) using PFN_##name = decltype(&name); PFN_##name name = (PFN_##name)GetProcAddress(dll, #name)

// Unique IDs for our buttons
#define IDC_LOAD_BUTTON 100
#define IDC_SAVE_BUTTON 101
#define IDC_GENERATE_BUTTON 102
#define IDC_COPY_BUTTON 103

// Shortcut Command IDs
#define ID_ACCEL_LOAD     201
#define ID_ACCEL_SAVE     202
#define ID_ACCEL_COPY     203
#define ID_ACCEL_GENERATE 204

static int w = 512, h = 512, c = 3;
static unsigned char* rgba = nullptr;
static unsigned char* rgba2 = nullptr;
static int w2, h2;
static int text_height = 100;
static const int button_height = 45; // Height of the bottom row
static bool is_dragging = false; // Tracks if we are currently resizing the textbox
const int splitter_thickness = 6; // Hit-test thickness for dragging
static int progress = 0;
static HWND window = nullptr;
static HWND hEdit = nullptr;
static HWND hBtnLoad = nullptr;
static HWND hBtnSave = nullptr;
static HWND hBtnCopy = nullptr;
static HWND hBtnGenerate = nullptr;

struct Res { int w, h; const wchar_t* name; };
Res presets[] = { {512, 512, L"512x512"}, {768, 512, L"768x512"}, {512, 768, L"512x768"}, {1280, 720, L"1280x720"}, {960, 1280, L"960x1280"} };

void set_title()
{
	char text[1024] = {};
	snprintf(text, sizeof(text), "mini-ai %d*%dpx (%d%%)", w2, h2, progress);
	SetWindowTextA(window, text);
}

void AddToolTip(HWND hwndParent, HWND hwndTarget, const wchar_t* text)
{
	static HWND hwndTT = NULL;
	if (hwndTT == NULL)
	{
		hwndTT = CreateWindowExW(
			WS_EX_TOPMOST,
			TOOLTIPS_CLASSW,
			NULL,
			WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
			CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
			hwndParent,
			NULL,
			GetModuleHandle(NULL),
			NULL
		);
		SetWindowPos(hwndTT, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	}

	TOOLINFOW ti = {};
	ti.cbSize = sizeof(ti);
	ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND; // TTF_SUBCLASS makes Windows automatically handle mouse tracking!
	ti.hwnd = hwndParent;
	ti.uId = (UINT_PTR)hwndTarget;
	ti.lpszText = const_cast<wchar_t*>(text);

	SendMessageW(hwndTT, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

void resize()
{
	if (rgba2)
	{
		free(rgba2);
		rgba2 = nullptr;
	}
	if (w2 > 0 && h2 > 0 && rgba)
	{
		rgba2 = stbir_resize_uint8_srgb(rgba, w, h, 0, (unsigned char*)malloc(w2 * h2 * 4), w2, h2, 0, STBIR_RGBA);
	}
}

void redraw()
{
	resize();
	InvalidateRect(window, NULL, TRUE);
	UpdateWindow(window);
}

void sd_log(enum sd_log_level_t level, const char* text, void* data)
{
	if (level == SD_LOG_DEBUG)
		return;
	OutputDebugStringA(text);
}

void sd_callback(int step, int steps, float time, void* data)
{
	progress = int(float(step) / float(steps) * 100);
	set_title();
}

void sd_preview(int step, int frame_count, sd_image_t* frames, bool is_noisy, void* data)
{
	if (frame_count == 0)
		return;
	sd_image_t* image = &frames[0];
	w = image->width;
	h = image->height;
	if (rgba)
	{
		free(rgba);
		rgba = nullptr;
	}
	rgba = (unsigned char*)malloc(w * h * 4);
	struct Color3 { unsigned char r, g, b; };
	struct Color4 { unsigned char r, g, b, a; };
	for (int i = 0; i < w * h; ++i)
	{
		const Color3& src = ((Color3*)image->data)[i];
		Color4& dst = ((Color4*)rgba)[i];
		dst.r = src.r;
		dst.g = src.g;
		dst.b = src.b;
		dst.a = 255;
	}
	redraw();
}

void trigger_generation()
{
	std::thread([] {
		wchar_t wtext[1024] = {};
		GetWindowText(hEdit, wtext, ARRAYSIZE(wtext));
		char text[4096] = {};
		stbi_convert_wchar_to_utf8(text, sizeof(text), wtext);

		HMODULE stable_diffusion = LoadLibrary(L"stable-diffusion.dll");
		assert(stable_diffusion);
		LINK_DLL_FUNCTION(sd_ctx_params_init, stable_diffusion);
		LINK_DLL_FUNCTION(new_sd_ctx, stable_diffusion);
		LINK_DLL_FUNCTION(sd_img_gen_params_init, stable_diffusion);
		LINK_DLL_FUNCTION(generate_image, stable_diffusion);
		LINK_DLL_FUNCTION(sd_sample_params_init, stable_diffusion);
		LINK_DLL_FUNCTION(free_sd_ctx, stable_diffusion);
		LINK_DLL_FUNCTION(sd_set_log_callback, stable_diffusion);
		LINK_DLL_FUNCTION(sd_set_progress_callback, stable_diffusion);
		LINK_DLL_FUNCTION(sd_set_preview_callback, stable_diffusion);

		sd_ctx_params_t sd_params;
		sd_ctx_params_init(&sd_params);
		sd_params.diffusion_model_path = "models/z_image_turbo-Q4_K_M.gguf";
		sd_params.vae_path = "models/ae.safetensors";
		sd_params.llm_path = "models/Qwen3-4B-Instruct-2507-Q4_K_M.gguf";
		sd_params.wtype = SD_TYPE_COUNT;
		sd_params.n_threads = -1;
		sd_params.rng_type = STD_DEFAULT_RNG;
		sd_params.vae_conv_direct = true;

		sd_set_log_callback(sd_log, nullptr);
		sd_set_progress_callback(sd_callback, nullptr);
		sd_set_preview_callback(sd_preview, PREVIEW_PROJ, 2, true, false, nullptr);

		sd_ctx_t* sd_ctx = new_sd_ctx(&sd_params);
		if (sd_ctx != nullptr)
		{
			sd_img_gen_params_t img_params;
			img_params.width = w2;
			img_params.height = h2;
			img_params.prompt = text;
			img_params.strength = 0.0f;
			img_params.batch_count = 1;

			sd_sample_params_init(&img_params.sample_params);
			img_params.sample_params.sample_method = EULER_SAMPLE_METHOD;
			img_params.sample_params.sample_steps = 8;
			img_params.sample_params.scheduler = SIMPLE_SCHEDULER;
			img_params.sample_params.eta = 1.0f;

			img_params.sample_params.guidance.txt_cfg = 1.0f;
			img_params.sample_params.guidance.img_cfg = 1.0f;
			img_params.sample_params.guidance.distilled_guidance = 3.5f;

			sd_image_t* image = nullptr;
			int num_images = 1;
			if (generate_image(sd_ctx, &img_params, &image, &num_images))
			{
				w = image->width;
				h = image->height;
				if (rgba)
				{
					free(rgba);
					rgba = nullptr;
				}
				rgba = (unsigned char*)malloc(w * h * 4);
				struct Color3 { unsigned char r, g, b; };
				struct Color4 { unsigned char r, g, b, a; };
				for (int i = 0; i < w * h; ++i)
				{
					const Color3& src = ((Color3*)image->data)[i];
					Color4& dst = ((Color4*)rgba)[i];
					dst.r = src.r;
					dst.g = src.g;
					dst.b = src.b;
					dst.a = 255;
				}
				redraw();
			}
			free_sd_ctx(sd_ctx);
		}
		FreeLibrary(stable_diffusion);
		}).detach();
}

void handle_load_image(HWND hWnd)
{
	wchar_t szFile[MAX_PATH] = { 0 };
	OPENFILENAMEW ofn = { 0 };
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = hWnd;
	ofn.lpstrFile = szFile;
	ofn.nMaxFile = sizeof(szFile) / sizeof(szFile[0]);
	ofn.lpstrFilter = L"Images\0*.png;*.jpg;*.jpeg;*.bmp;*.tga\0All Files\0*.*\0";
	ofn.nFilterIndex = 1;
	ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

	if (GetOpenFileNameW(&ofn))
	{
		char filename[4096] = {};
		stbi_convert_wchar_to_utf8(filename, sizeof(filename), szFile);

		if (rgba)
		{
			free(rgba);
			rgba = nullptr;
		}
		if (rgba2)
		{
			free(rgba2);
			rgba2 = nullptr;
		}
		rgba = stbi_load(filename, &w, &h, &c, 4);

		if (rgba)
		{
			RECT rc = { 0, 0, w, h + button_height + text_height };
			AdjustWindowRect(&rc, (DWORD)GetWindowLongPtr(hWnd, GWL_STYLE), GetMenu(hWnd) != NULL);
			SetWindowPos(hWnd, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED);
			SetForegroundWindow(hWnd);
			redraw();
		}
	}
}

void handle_save_image(HWND hWnd)
{
	if (!rgba2)
	{
		MessageBoxW(hWnd, L"No generated image to save!", L"Error", MB_ICONERROR | MB_OK);
		return;
	}

	wchar_t szFile[MAX_PATH] = { 0 };
	OPENFILENAMEW ofn = { 0 };
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = hWnd;
	ofn.lpstrFile = szFile;
	ofn.nMaxFile = sizeof(szFile) / sizeof(szFile[0]);
	ofn.lpstrFilter = L"PNG Image (*.png)\0*.png\0";
	ofn.nFilterIndex = 1;
	ofn.lpstrDefExt = L"png";
	ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;

	if (GetSaveFileNameW(&ofn))
	{
		char filename[4096] = {};
		stbi_convert_wchar_to_utf8(filename, sizeof(filename), szFile);

		int success = stbi_write_png(filename, w2, h2 - splitter_thickness, 4, rgba2, w2 * 4);
		if (!success)
		{
			MessageBoxW(hWnd, L"Failed to save image.", L"Error", MB_ICONERROR | MB_OK);
		}
	}
}

void handle_copy_image(HWND hWnd)
{
	int draw_height = h2 - splitter_thickness;
	if (!rgba2 || w2 <= 0 || draw_height <= 0)
	{
		MessageBoxW(hWnd, L"No generated image to copy!", L"Error", MB_ICONERROR | MB_OK);
		return;
	}

	// Calculate sizing for true Clipboard DIB representation
	size_t row_stride = w2 * 4;
	size_t image_size = row_stride * draw_height;
	size_t total_size = sizeof(BITMAPINFOHEADER) + image_size;

	HGLOBAL hClipboardData = GlobalAlloc(GMEM_MOVEABLE, total_size);
	if (!hClipboardData) return;

	void* pData = GlobalLock(hClipboardData);
	if (pData)
	{
		// Write standard DIB header chunk
		BITMAPINFOHEADER* pHeader = (BITMAPINFOHEADER*)pData;
		pHeader->biSize = sizeof(BITMAPINFOHEADER);
		pHeader->biWidth = w2;
		pHeader->biHeight = draw_height; // Positive = bottom-up DIB layout requirements
		pHeader->biPlanes = 1;
		pHeader->biBitCount = 32;
		pHeader->biCompression = BI_RGB;
		pHeader->biSizeImage = (DWORD)image_size;

		// Clipboard DIB stores pixels bottom-to-top, flip vertical rows directly into memory block
		unsigned char* pDestPixels = (unsigned char*)pData + sizeof(BITMAPINFOHEADER);
		for (int y = 0; y < draw_height; ++y)
		{
			unsigned char* src_row = rgba2 + ((draw_height - 1 - y) * row_stride);
			unsigned char* dst_row = pDestPixels + (y * row_stride);

			// Reorder raw storage layer RGBA -> Clipboard BGRA channel alignment configurations
			for (int x = 0; x < w2; ++x)
			{
				dst_row[x * 4 + 0] = src_row[x * 4 + 2]; // B
				dst_row[x * 4 + 1] = src_row[x * 4 + 1]; // G
				dst_row[x * 4 + 2] = src_row[x * 4 + 0]; // R
				dst_row[x * 4 + 3] = src_row[x * 4 + 3]; // A
			}
		}
		GlobalUnlock(hClipboardData);

		// Commit directly to OS environment pipeline loop
		if (OpenClipboard(hWnd))
		{
			EmptyClipboard();
			SetClipboardData(CF_DIB, hClipboardData);
			CloseClipboard();
		}
		else
		{
			GlobalFree(hClipboardData);
		}
	}
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine, _In_ int nCmdShow)
{
	if (lpCmdLine && lpCmdLine[0])
	{
		char filename[4096] = {};
		stbi_convert_wchar_to_utf8(filename, sizeof(filename), lpCmdLine);
		rgba = stbi_load(filename, &w, &h, &c, 4);
	}

	static bool exiting = false;
	static auto WndProc = [](HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) -> LRESULT
		{
			switch (message)
			{
			case WM_SIZE:
			{
				w2 = LOWORD(lParam);
				h2 = HIWORD(lParam) - text_height - button_height;
				set_title();
				if (rgba)
				{
					resize();
				}

				RECT rc;
				GetClientRect(hWnd, &rc);

				if (hEdit)
				{
					MoveWindow(hEdit, 0, rc.bottom - button_height - text_height, rc.right, text_height, TRUE);
				}

				// Position Buttons: Load (Square), Save (Square), Copy (Square), Generate (Fills remaining space)
				int square_width = button_height; // 45px width
				if (hBtnLoad)      MoveWindow(hBtnLoad, 0, rc.bottom - button_height, square_width, button_height, TRUE);
				if (hBtnSave)      MoveWindow(hBtnSave, square_width, rc.bottom - button_height, square_width, button_height, TRUE);
				if (hBtnCopy)      MoveWindow(hBtnCopy, square_width * 2, rc.bottom - button_height, square_width, button_height, TRUE);
				if (hBtnGenerate)  MoveWindow(hBtnGenerate, square_width * 3, rc.bottom - button_height, rc.right - (square_width * 3), button_height, TRUE);
			}
			break;
			case WM_DESTROY:
				exiting = true;
				PostQuitMessage(0);
				break;
			case WM_ERASEBKGND:
				return 1;
			case WM_PAINT:
			{
				PAINTSTRUCT ps;
				HDC hdc = BeginPaint(hWnd, &ps);

				int draw_height = h2 - splitter_thickness;

				if (rgba2 != nullptr)
				{
					struct BitmapInfoEx {
						BITMAPINFOHEADER hdr = {};
						DWORD rgbmask[3] = { 0x000000FF, 0x0000FF00, 0x00FF0000 };
					};
					BitmapInfoEx bi = {};
					bi.hdr.biSize = sizeof(BITMAPINFOHEADER);
					bi.hdr.biPlanes = 1;
					bi.hdr.biBitCount = 32;
					bi.hdr.biCompression = BI_BITFIELDS;
					bi.hdr.biWidth = w2;
					bi.hdr.biHeight = -draw_height;

					SetDIBitsToDevice(hdc, 0, 0, w2, draw_height, 0, 0, 0, draw_height, rgba2, (BITMAPINFO*)&bi, DIB_RGB_COLORS);
				}
				else
				{
					// Checkerboard
					int cellSize = 20;
					COLORREF color1 = RGB(35, 35, 35);
					COLORREF color2 = RGB(25, 25, 25);

					HBRUSH hBrush1 = CreateSolidBrush(color1);
					HBRUSH hBrush2 = CreateSolidBrush(color2);

					for (int y = 0; y < draw_height; y += cellSize) {
						for (int x = 0; x < w2; x += cellSize) {
							RECT cell = { x, y, x + cellSize, y + cellSize };
							// Alternate colors based on cell position
							if (((x / cellSize) + (y / cellSize)) % 2 == 0)
								FillRect(hdc, &cell, hBrush1);
							else
								FillRect(hdc, &cell, hBrush2);
						}
					}

					DeleteObject(hBrush1);
					DeleteObject(hBrush2);
				}

				HBRUSH hSplitterBrush = CreateSolidBrush(RGB(62, 62, 62));
				RECT splitter_rect = { 0, draw_height, w2, draw_height + splitter_thickness };
				FillRect(hdc, &splitter_rect, hSplitterBrush);
				DeleteObject(hSplitterBrush);

				EndPaint(hWnd, &ps);
			}
			break;

			case WM_COMMAND:
				if (HIWORD(wParam) == BN_CLICKED || HIWORD(wParam) == 1)
				{
					switch (LOWORD(wParam))
					{
					case IDC_LOAD_BUTTON:
					case ID_ACCEL_LOAD:
						handle_load_image(hWnd);
						break;
					case IDC_SAVE_BUTTON:
					case ID_ACCEL_SAVE:
						handle_save_image(hWnd);
						break;
					case IDC_COPY_BUTTON:
					case ID_ACCEL_COPY:
						handle_copy_image(hWnd);
						break;
					case IDC_GENERATE_BUTTON:
					case ID_ACCEL_GENERATE:
						trigger_generation();
						break;
					}
				}
				break;

			case WM_CONTEXTMENU:
			{
				if ((HWND)wParam == window)
				{
					HMENU hMenu = CreatePopupMenu();
					for (int i = 0; i < ARRAYSIZE(presets); ++i)
						AppendMenuW(hMenu, MF_STRING, 1000 + i, presets[i].name);

					POINT pt;
					GetCursorPos(&pt);
					int selection = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hWnd, NULL);

					selection -= 1000;
					if (selection >= 0 && selection < ARRAYSIZE(presets))
					{
						w2 = presets[selection].w;
						h2 = presets[selection].h;
						set_title();

						// Calculate the new window size based on current text/button height
						RECT rc = { 0, 0, w2, h2 + button_height + text_height };
						AdjustWindowRect(&rc, (DWORD)GetWindowLongPtr(hWnd, GWL_STYLE), GetMenu(hWnd) != NULL);

						// Resize the window
						SetWindowPos(hWnd, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED);

						// Refresh the drawing
						if (rgba) redraw();
					}

					DestroyMenu(hMenu);
					return 0;
				}
			}

			case WM_CTLCOLOREDIT:
			{
				HDC hdcEdit = (HDC)wParam;
				SetTextColor(hdcEdit, RGB(220, 220, 220));
				SetBkColor(hdcEdit, RGB(30, 30, 30));
				static HBRUSH hEditBg = CreateSolidBrush(RGB(30, 30, 30));
				return (INT_PTR)hEditBg;
			}
			break;

			case WM_CTLCOLORBTN:
			{
				HDC hdcEdit = (HDC)wParam;
				SetTextColor(hdcEdit, RGB(220, 220, 220));
				SetBkColor(hdcEdit, RGB(30, 30, 30));
				static HBRUSH hEditBg = CreateSolidBrush(RGB(30, 30, 30));
				return (INT_PTR)hEditBg;
			}
			break;

			case WM_DROPFILES:
			{
				HDROP hdrop = (HDROP)wParam;
				UINT filecount = DragQueryFile(hdrop, 0xFFFFFFFF, nullptr, 0);
				assert(filecount != 0);
				for (UINT i = 0; i < filecount; ++i)
				{
					wchar_t wfilename[1024] = {};
					UINT res = DragQueryFile(hdrop, i, wfilename, ARRAYSIZE(wfilename));
					if (res == 0)
					{
						assert(0);
						continue;
					}
					char filename[4096] = {};
					stbi_convert_wchar_to_utf8(filename, sizeof(filename), wfilename);
					if (rgba)
					{
						free(rgba);
						rgba = nullptr;
					}
					if (rgba2)
					{
						free(rgba2);
						rgba2 = nullptr;
					}
					rgba = stbi_load(filename, &w, &h, &c, 4);
				}
				RECT rc = { 0, 0, w, h + button_height + text_height };
				AdjustWindowRect(&rc, (DWORD)GetWindowLongPtr(hWnd, GWL_STYLE), GetMenu(hWnd) != NULL);
				SetWindowPos(hWnd, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED);
				SetForegroundWindow(hWnd);
			}
			break;

			case WM_LBUTTONDOWN:
			{
				int mouse_y = HIWORD(lParam);
				RECT rc;
				GetClientRect(hWnd, &rc);
				int splitter_y = rc.bottom - button_height - text_height - splitter_thickness;
				if (mouse_y >= splitter_y && mouse_y <= splitter_y + splitter_thickness)
				{
					is_dragging = true;
					SetCapture(hWnd);
				}
			}
			break;

			case WM_LBUTTONUP:
				if (is_dragging)
				{
					is_dragging = false;
					ReleaseCapture();

					// Only resize heavy layout representations when mouse interaction closes out
					if (rgba)
					{
						redraw();
					}
				}
				break;

			case WM_MOUSEMOVE:
			{
				RECT rc;
				GetClientRect(hWnd, &rc);
				int mouse_y = HIWORD(lParam);
				int splitter_y = rc.bottom - button_height - text_height - splitter_thickness;

				if (is_dragging)
				{
					int proposed_height = rc.bottom - button_height - mouse_y - (splitter_thickness / 2);
					if (proposed_height > 40 && proposed_height < (rc.bottom - 100 - button_height))
					{
						text_height = proposed_height;
						w2 = rc.right;
						h2 = rc.bottom - text_height - button_height;
						set_title();

						if (hEdit)
						{
							MoveWindow(hEdit, 0, rc.bottom - button_height - text_height, rc.right, text_height, TRUE);
						}

						int square_width = button_height;
						if (hBtnLoad)      MoveWindow(hBtnLoad, 0, rc.bottom - button_height, square_width, button_height, TRUE);
						if (hBtnSave)      MoveWindow(hBtnSave, square_width, rc.bottom - button_height, square_width, button_height, TRUE);
						if (hBtnCopy)      MoveWindow(hBtnCopy, square_width * 2, rc.bottom - button_height, square_width, button_height, TRUE);
						if (hBtnGenerate)  MoveWindow(hBtnGenerate, square_width * 3, rc.bottom - button_height, rc.right - (square_width * 3), button_height, TRUE);

						InvalidateRect(hWnd, NULL, TRUE);
					}
				}
			}
			break;

			case WM_SETCURSOR:
			{
				POINT pt;
				GetCursorPos(&pt);
				ScreenToClient(hWnd, &pt);
				RECT rc;
				GetClientRect(hWnd, &rc);
				int splitter_y = rc.bottom - button_height - text_height - splitter_thickness;
				if (is_dragging || (pt.y >= splitter_y && pt.y <= splitter_y + splitter_thickness))
				{
					SetCursor(LoadCursor(NULL, IDC_SIZENS));
					return TRUE;
				}
			}

			default:
				return DefWindowProc(hWnd, message, wParam, lParam);
			}
			return 0;
		};

	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	WNDCLASSEXW wcex = {};
	wcex.cbSize = sizeof(WNDCLASSEX);
	wcex.style = CS_HREDRAW | CS_VREDRAW;
	wcex.lpfnWndProc = WndProc;
	wcex.cbClsExtra = 0;
	wcex.cbWndExtra = 0;
	wcex.hInstance = hInstance;
	wcex.hIcon = NULL;
	wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wcex.hbrBackground = CreateSolidBrush(RGB(30, 30, 30));
	wcex.lpszMenuName = NULL;
	wcex.lpszClassName = L"mini-ai";
	wcex.hIconSm = NULL;
	RegisterClassExW(&wcex);

	RECT wr = { 0, 0, w, h + text_height + button_height };
	DWORD window_style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
	AdjustWindowRect(&wr, window_style, FALSE);
	int window_width = wr.right - wr.left;
	int window_height = wr.bottom - wr.top;

	window = CreateWindowW(L"mini-ai", L"mini-ai", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, 0, window_width, window_height, nullptr, nullptr, NULL, nullptr);
	hEdit = CreateWindowW(L"EDIT", NULL, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL, 0, 0, 0, 0, window, NULL, hInstance, NULL);
	//Edit_SetCueBannerTextFocused(hEdit, L"Type image description...", TRUE);

	// Create UI Button Controls matching native modern font layouts
	hBtnLoad = CreateWindowW(L"BUTTON", L"\xE8B7", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, window, (HMENU)IDC_LOAD_BUTTON, hInstance, NULL);
	hBtnSave = CreateWindowW(L"BUTTON", L"\xE74E", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, window, (HMENU)IDC_SAVE_BUTTON, hInstance, NULL);
	hBtnCopy = CreateWindowW(L"BUTTON", L"\xE8C8", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, window, (HMENU)IDC_COPY_BUTTON, hInstance, NULL);
	hBtnGenerate = CreateWindowW(L"BUTTON", L"\u2728 Generate \u2728", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, window, (HMENU)IDC_GENERATE_BUTTON, hInstance, NULL);

	HFONT hFont = CreateFont(34, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Arial");
	SendMessage(hEdit, WM_SETFONT, (WPARAM)hFont, TRUE);

	// Create structural asset mapping definitions for MDL2 system icons
	HFONT hIconFont = CreateFontW(24, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe MDL2 Assets");
	SendMessageW(hBtnLoad, WM_SETFONT, (WPARAM)hIconFont, TRUE);
	SendMessageW(hBtnSave, WM_SETFONT, (WPARAM)hIconFont, TRUE);
	SendMessageW(hBtnCopy, WM_SETFONT, (WPARAM)hIconFont, TRUE);
	HFONT hGenFont = CreateFontW(32, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Symbol");
	SendMessageW(hBtnGenerate, WM_SETFONT, (WPARAM)hGenFont, TRUE);

	AddToolTip(window, hBtnLoad, L"Load Image (Ctrl+O)");
	AddToolTip(window, hBtnSave, L"Save Image (Ctrl+S)");
	AddToolTip(window, hBtnCopy, L"Copy Image to Clipboard (Ctrl+C)");
	AddToolTip(window, hBtnGenerate, L"Generate Image from Prompt (Ctrl+Enter)");

	ShowWindow(window, SW_SHOWDEFAULT);
	DragAcceptFiles(window, TRUE);

	enum PreferredAppMode {
		Default,
		AllowDark,
		ForceDark,
		ForceLight,
		Max
	};

	using fnSetPreferredAppMode = PreferredAppMode(WINAPI*)(PreferredAppMode appMode);
	using fnAllowDarkModeForWindow = bool(WINAPI*)(HWND hWnd, bool allow);

	// Force Dark Mode context rules to frame structures
	BOOL darkmode = TRUE;
	DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkmode, sizeof(darkmode));

	SetWindowTheme(hBtnLoad, L"DarkMode_Explorer", NULL);
	SetWindowTheme(hBtnSave, L"DarkMode_Explorer", NULL);
	SetWindowTheme(hBtnCopy, L"DarkMode_Explorer", NULL);
	SetWindowTheme(hBtnGenerate, L"DarkMode_Explorer", NULL);
	SetWindowTheme(hEdit, L"DarkMode_Explorer", NULL);

	// keyboard shortcuts
	ACCEL accels[] = {
		{ FCONTROL | FVIRTKEY, 'O', ID_ACCEL_LOAD },     // Ctrl + O
		{ FCONTROL | FVIRTKEY, 'S', ID_ACCEL_SAVE },     // Ctrl + S
		{ FCONTROL | FVIRTKEY, 'C', ID_ACCEL_COPY },     // Ctrl + C
		{ FCONTROL | FVIRTKEY, VK_RETURN, ID_ACCEL_GENERATE } // Ctrl + Enter (intuitive for generating!)
	};
	HACCEL hAccel = CreateAcceleratorTableW(accels, ARRAYSIZE(accels));

	while (!exiting)
	{
		MSG msg = { 0 };
		if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
			if (!TranslateAcceleratorW(window, hAccel, &msg))
			{
				TranslateMessage(&msg);
				DispatchMessage(&msg);
			}
			continue;
		}
	}

	DestroyAcceleratorTable(hAccel);

	if (rgba)  free(rgba);
	if (rgba2) free(rgba2);

	return 0;
}