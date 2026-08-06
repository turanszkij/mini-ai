#include <Windows.h>
#include <commdlg.h> // Common Dialogs for Load/Save

#include <dwmapi.h> // DwmSetWindowAttribute
#pragma comment(lib, "dwmapi.lib")

#include <uxtheme.h>
#pragma comment(lib, "uxtheme.lib")
#pragma comment(linker,"\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")

#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

#include <wrl/client.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

#include <thread>
#include <string>
#include <fstream>
#include <atomic>
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <vector>
#include <cmath>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_WINDOWS_UTF8
#include "lib/stb/stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "lib/stb/stb_image_write.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "lib/stb/stb_image_resize2.h"

#include "lib/stable-diffusion/stable-diffusion.h"

#pragma warning(push)
#pragma warning(disable: 4267)
#pragma warning(disable: 4305)
#include "lib/llama/llama.h"
#include "lib/llama/mtmd.h"
#include "lib/llama/mtmd-image.h"
#include "lib/llama/mtmd-helper.h"
#pragma warning(pop)

#define LINK_DLL_FUNCTION(name, dll) using PFN_##name = decltype(&name); PFN_##name name = (PFN_##name)GetProcAddress(dll, #name); assert(name);

// Unique button IDs
#define IDC_LOAD_BUTTON 100
#define IDC_SAVE_BUTTON 101
#define IDC_GENERATE_BUTTON 102
#define IDC_COPY_BUTTON 103
#define IDC_CLEAR_BUTTON 104
#define IDC_UNDO_BUTTON 105
#define IDC_REDO_BUTTON 106

// Shortcut Command IDs
#define ID_ACCEL_LOAD     201
#define ID_ACCEL_SAVE     202
#define ID_ACCEL_GENERATE 204

#define IDI_APPICON 101
#define IDT_ANIM 1001          // timer for lightweight generation activity animation

static wchar_t originalWorkingDir[MAX_PATH] = {}; // at application start the working directory is remembered and all file operations will be done based on that
static wchar_t promptPath[MAX_PATH] = {}; // the absolute path of prompt.txt
static int w = 512, h = 512, c = 3; // properties of the current image
static unsigned char* rgba = nullptr; // byte data of current image
static unsigned char* rgba2 = nullptr; // byte data of current image's scaled version (aspect-preserved fit)
static int w2, h2; // size of the image display area (from window client)
static int disp_w = 0, disp_h = 0; // actual size of the scaled display image (aspect preserved, maximized)
static int batch_count = 1;// number of images to generate
static int video_fps = 24; // video generation frames per second
static int video_seconds = 2; // video generation total seconds
static int text_height = 180; // textbox input height
static const int button_height = 45; // height of all the buttons on the bottom row
static const int splitter_thickness = 8; // image/textbox separator thickness
static const int reference_image_area_height = 120; // fixed height area for reference images
static bool is_dragging = false; // separator dragging
static int progress = 0; // progress of current processing task
static int download_total_MB = 0; // current total model download size megabytes
static int download_completed_MB = 0; // current model download completed megabytes
static bool is_cpu = false; // cpu or gpu execution preference
static bool resize_fixed = false; // fix for WM_SIZE overriding a resize operation
static std::atomic_bool is_generating{ false }; // true when background thread is running generation task
static std::atomic_bool cancel_request{ false }; // true when user pushed STOP button and background generation task should be cancelled before it finishes
static std::wstring current_download; // if model is downloading, this is printed to feedback text
static std::string progress_errors; // errors while generation is running are collected here but not presented to user yet
static std::string final_errors; // if generation fails then the errors wil be copied here and shown to user
static CRITICAL_SECTION image_cs; // protects rgba / rgba2 / w,h,disp_* between UI and generation threads
static HWND window = nullptr;
static HWND hEdit = nullptr;
static HWND hBtnLoad = nullptr;
static HWND hBtnSave = nullptr;
static HWND hBtnCopy = nullptr;
static HWND hBtnClear = nullptr;
static HWND hBtnGenerate = nullptr;
static HWND hBtnUndo = nullptr;
static HWND hBtnRedo = nullptr;

enum class MODE
{
	IMAGE_GENERATE,
	IMAGE_EDIT,
	ASK,
	VIDEO,
};
static MODE mode = MODE::IMAGE_GENERATE;

enum class IMAGE_MODEL
{
	Z_IMAGE,
	FLUX2,
	STABLE_DIFFUSION_3_5,
	QWEN_IMAGE,
	ERNIE_IMAGE,
};
static IMAGE_MODEL image_model = IMAGE_MODEL::Z_IMAGE;

enum class EDIT_MODEL
{
	FLUX2,
	QWEN_IMAGE_EDIT,
};
static EDIT_MODEL edit_model = EDIT_MODEL::FLUX2;

enum class TEXT_MODEL
{
	QWEN_3_VL,
	GEMMA_4,
};
static TEXT_MODEL text_model = TEXT_MODEL::QWEN_3_VL;

enum class VIDEO_MODEL
{
	WAN_2_2,
	LTX_2_3,
	MINIMAX_H3,
};
static VIDEO_MODEL video_model = VIDEO_MODEL::WAN_2_2;

struct ReferenceImage 
{
	unsigned char* rgba = nullptr;
	int w = 0, h = 0;
	RECT render_rect = {};
	RECT close_rect = {};
};
static std::vector<ReferenceImage> reference_images;
void clear_ref_images()
{
	for (auto& img : reference_images)
	{
		if (img.rgba) free(img.rgba);
	}
	reference_images.clear();
}
int get_ref_container_height()
{
	if (mode == MODE::IMAGE_EDIT || mode == MODE::VIDEO)
	{
		return reference_image_area_height;
	}
	return 0;
}

static const int scale_presets[] = { 50,100,200,300 };

struct ResolutionPreset { int w, h; };
static const ResolutionPreset resolution_presets[] = {
	{512, 512},
	{1024, 1024},
	{640, 480},
	{800, 600},
	{1280, 720},
	{1920, 1080},
	{480, 640},
	{960, 1280}
};

struct VideoPreset { int fps, seconds; };
static const VideoPreset video_presets[] = {
	{8, 4},
	{8, 10},
	{16, 2},
	{16, 4},
	{16, 10},
	{24, 2},
	{24, 4},
	{24, 10},
};

struct HistoryEntry
{
	unsigned char* data;
	int size;
	int w, h;
	std::wstring prompt;
};
static std::vector<HistoryEntry> history;
static int history_index = -1;
static const int max_history_count = 100;

void resize_window_to_image()
{
	RECT rc = { 0, 0, w, h + get_ref_container_height() + splitter_thickness + button_height + text_height};
	AdjustWindowRect(&rc, (DWORD)GetWindowLongPtr(window, GWL_STYLE), GetMenu(window) != NULL);
	SetWindowPos(window, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED);
}
void redraw()
{
	wchar_t text[1024] = {};
	if (is_generating.load())
	{
		_snwprintf(text, sizeof(text), L"mini-ai %dx%dpx (%d%%)", w2, h2, progress);
	}
	else
	{
		_snwprintf(text, sizeof(text), L"mini-ai %dx%dpx", w2, h2);
	}
	SetWindowText(window, text);


	if (is_generating.load())
	{
		SetWindowText(hBtnGenerate, L"\x23F9 STOP");
	}
	else
	{
		switch (mode)
		{
		default:
		case MODE::IMAGE_GENERATE:
			SetWindowText(hBtnGenerate, L"\u2728 Image \u2728");
			break;
		case MODE::IMAGE_EDIT:
			SetWindowText(hBtnGenerate, L"\u2728 Edit \u2728");
			break;
		case MODE::ASK:
			SetWindowText(hBtnGenerate, L"\u2728 Ask \u2728");
			break;
		case MODE::VIDEO:
			SetWindowText(hBtnGenerate, L"\u2728 Video \u2728");
			break;
		}
	}

	EnterCriticalSection(&image_cs);
	if (rgba2)
	{
		free(rgba2);
		rgba2 = nullptr;
	}
	disp_w = 0;
	disp_h = 0;
	if (w2 > 0 && h2 > 0 && rgba && w > 0 && h > 0)
	{
		// Scale to maximize size inside the display area while preserving aspect ratio
		const float src_aspect = (float)w / (float)h;
		if ((float)w2 / src_aspect <= (float)h2)
		{
			// Limited by width
			disp_w = w2;
			disp_h = (int)((float)w2 / src_aspect + 0.5f);
			if (disp_h < 1) disp_h = 1;
		}
		else
		{
			// Limited by height
			disp_h = h2;
			disp_w = (int)((float)h2 * src_aspect + 0.5f);
			if (disp_w < 1) disp_w = 1;
		}
		rgba2 = stbir_resize_uint8_srgb(rgba, w, h, 0, (unsigned char*)malloc((size_t)disp_w * disp_h * 4), disp_w, disp_h, 0, STBIR_RGBA);
	}
	LeaveCriticalSection(&image_cs);

	int y = h2 + get_ref_container_height() + splitter_thickness;

	if (hEdit)
	{
		MoveWindow(hEdit, 0, y, w2, text_height, TRUE);
		y += text_height;
	}

	if (hBtnLoad)		MoveWindow(hBtnLoad, 0, y, button_height, button_height, TRUE);
	if (hBtnSave)		MoveWindow(hBtnSave, button_height, y, button_height, button_height, TRUE);
	if (hBtnCopy)		MoveWindow(hBtnCopy, button_height * 2, y, button_height, button_height, TRUE);
	if (hBtnClear)		MoveWindow(hBtnClear, button_height * 3, y, button_height, button_height, TRUE);
	if (hBtnUndo)		MoveWindow(hBtnUndo, button_height * 4, y, button_height, button_height, TRUE);
	if (hBtnRedo)		MoveWindow(hBtnRedo, button_height * 5, y, button_height, button_height, TRUE);
	if (hBtnGenerate)	MoveWindow(hBtnGenerate, button_height * 6, y, w2 - (button_height * 6), button_height, TRUE);

	RECT rc = { 0, 0, w2, h2 + get_ref_container_height() + splitter_thickness + text_height + button_height };
	AdjustWindowRect(&rc, (DWORD)GetWindowLongPtr(window, GWL_STYLE), GetMenu(window) != NULL);
	SetWindowPos(window, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED);
	InvalidateRect(window, NULL, TRUE);
	UpdateWindow(window);
}

void update_undo_redo_states()
{
	EnableWindow(hBtnUndo, history_index > 0);
	EnableWindow(hBtnRedo, history_index < (int)history.size() - 1);
}
void push_history(unsigned char* raw_rgba, int width, int height, bool save_output = false, int batch_index = 0)
{
	int out_size = 0;
	unsigned char* png_data = nullptr;

	if (raw_rgba != nullptr)
	{
		png_data = stbi_write_png_to_mem(raw_rgba, width * 4, width, height, 4, &out_size);
	}

	while (history.size() > (size_t)(history_index + 1))
	{
		if (history.back().data != nullptr)
			free(history.back().data);
		history.pop_back();
	}

	if (history.size() >= max_history_count)
	{
		if (history[0].data != nullptr)
			free(history[0].data);
		history.erase(history.begin());
		history_index--;
	}

	if (history.size() > 0 && history.back().data == nullptr)
	{
		history.pop_back();
		history_index--;
	}

	int length = GetWindowTextLength(hEdit);
	std::wstring buffer(length, L'\0');
	GetWindowText(hEdit, &buffer[0], length + 1);

	history.push_back({ png_data, out_size, width, height, buffer });
	history_index++;

	if (save_output && png_data != nullptr)
	{
		// In this case it's also saved to output/ folder as real png file
		wchar_t output_path[MAX_PATH] = {};
		_snwprintf(output_path, MAX_PATH, L"%s%s", originalWorkingDir, L"/output");
		CreateDirectory(output_path, 0);

		time_t t = std::time(nullptr);
		struct tm* tmptr;
		struct tm time_info;
		tmptr = &time_info;
		localtime_s(&time_info, &t);
		std::wstringstream ss(L"");
		ss << output_path << L"/";
		ss << std::put_time(tmptr, L"%Y-%m-%d %H-%M-%S");
		ss << " " << batch_index;
		ss << ".png";
		std::ofstream file(ss.str().c_str(), std::ios::binary | std::ios::trunc);
		if (file.is_open())
		{
			file.write((const char*)png_data, (std::streamsize)out_size);
			file.close();
		}
	}

	update_undo_redo_states();
}
void load_history_entry()
{
	if (history_index < 0 || history_index >= (int)history.size()) return;

	if (rgba) { free(rgba); rgba = nullptr; }
	if (rgba2) { free(rgba2); rgba2 = nullptr; }

	if (history[history_index].data != nullptr)
	{
		int w_temp, h_temp, c_temp;
		unsigned char* decoded = stbi_load_from_memory(history[history_index].data, history[history_index].size, &w_temp, &h_temp, &c_temp, 4);

		if (decoded)
		{
			rgba = decoded;
			w = w_temp;
			h = h_temp;
		}

		SetWindowText(hEdit, history[history_index].prompt.c_str());
	}

	redraw();
}
void undo()
{
	if (history_index > 0) {
		history_index--;
		load_history_entry();
	}
	update_undo_redo_states();
}
void redo()
{
	if (history_index < (int)history.size() - 1) {
		history_index++;
		load_history_entry();
	}
	update_undo_redo_states();
}
void load_image()
{
	wchar_t szFile[MAX_PATH] = {};
	OPENFILENAMEW ofn = {};
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = window;
	ofn.lpstrFile = szFile;
	ofn.nMaxFile = sizeof(szFile) / sizeof(szFile[0]);
	ofn.lpstrFilter = L"Images\0*.png;*.jpg;*.jpeg;*.bmp;*.tga\0All Files\0*.*\0";
	ofn.nFilterIndex = 1;
	ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

	if (GetOpenFileName(&ofn))
	{
		char filename[MAX_PATH] = {};
		WideCharToMultiByte(CP_UTF8, 0, szFile, -1, filename, MAX_PATH, nullptr, nullptr);

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
			push_history(rgba, w, h);
			SetForegroundWindow(window);
			resize_window_to_image();
			redraw();
		}
	}
}
void save_image()
{
	if (!rgba2 || disp_w <= 0 || disp_h <= 0)
	{
		MessageBox(window, L"No generated image to save!", L"Error", MB_ICONERROR | MB_OK);
		return;
	}

	wchar_t szFile[MAX_PATH] = {};
	OPENFILENAMEW ofn = {};
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = window;
	ofn.lpstrFile = szFile;
	ofn.nMaxFile = sizeof(szFile) / sizeof(szFile[0]);

	// Default: PNG (Index 1), Supported: ICO, JPG, BMP, TGA
	ofn.lpstrFilter = L"PNG Image (*.png)\0*.png\0"
		L"ICO Icon (*.ico)\0*.ico\0"
		L"JPEG Image (*.jpg;*.jpeg)\0*.jpg;*.jpeg\0"
		L"BMP Image (*.bmp)\0*.bmp\0"
		L"TGA Image (*.tga)\0*.tga\0";
	ofn.nFilterIndex = 1;
	ofn.lpstrDefExt = L"png";
	ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;

	if (GetSaveFileName(&ofn))
	{
		char filename[MAX_PATH] = {};
		WideCharToMultiByte(CP_UTF8, 0, szFile, -1, filename, MAX_PATH, nullptr, nullptr);

		int width = disp_w;
		int height = disp_h;
		bool success = false;

		std::string str_filename(filename);

		static auto check_extension = [](const std::string& str, const std::string& ext) {
			return str.size() >= ext.size() && str.compare(str.size() - ext.size(), ext.size(), ext) == 0;
		};

		// 1. Multi-Resolution ICO
		if (ofn.nFilterIndex == 2 || check_extension(str_filename, ".ico"))
		{
			const int targetSizes[] = { 256, 128, 64, 48, 32, 16 };

			struct IconFrame {
				int size;
				int png_len;
				unsigned char* png_bytes;
			};

			std::vector<IconFrame> frames;

			for (int size : targetSizes)
			{
				if (size > width && size > height && size != 256) continue;

				const unsigned char* srcPtr = rgba2;
				std::vector<unsigned char> resized_buffer;

				if (width != size || height != size)
				{
					resized_buffer.resize(size * size * 4);
					stbir_resize_uint8_srgb(rgba2, width, height, width * 4, resized_buffer.data(), size, size, size * 4, STBIR_RGBA);
					srcPtr = resized_buffer.data();
				}

				int png_len = 0;
				unsigned char* png_bytes = stbi_write_png_to_mem(srcPtr, size * 4, size, size, 4, &png_len);
				if (png_bytes)
				{
					frames.push_back({ size, png_len, png_bytes });
				}
			}

			if (!frames.empty())
			{
#pragma pack(push, 1)
				struct ICOHeader {
					uint16_t idReserved = 0;
					uint16_t idType = 1; // 1 = ICO
					uint16_t idCount;
				};

				struct ICONDIRENTRY {
					uint8_t  bWidth;
					uint8_t  bHeight;
					uint8_t  bColorCount = 0;
					uint8_t  bReserved = 0;
					uint16_t wPlanes = 1;
					uint16_t wBitCount = 32;
					uint32_t dwBytesInRes;
					uint32_t dwImageOffset;
				};
#pragma pack(pop)

				ICOHeader header;
				header.idCount = static_cast<uint16_t>(frames.size());

				uint32_t currentOffset = sizeof(ICOHeader) + static_cast<uint32_t>(frames.size() * sizeof(ICONDIRENTRY));

				std::vector<ICONDIRENTRY> entries;
				for (const auto& frame : frames)
				{
					ICONDIRENTRY entry;
					entry.bWidth = (frame.size >= 256) ? 0 : static_cast<uint8_t>(frame.size);
					entry.bHeight = (frame.size >= 256) ? 0 : static_cast<uint8_t>(frame.size);
					entry.dwBytesInRes = static_cast<uint32_t>(frame.png_len);
					entry.dwImageOffset = currentOffset;

					entries.push_back(entry);
					currentOffset += frame.png_len;
				}

				std::ofstream file(filename, std::ios::binary);
				if (file.is_open())
				{
					file.write(reinterpret_cast<const char*>(&header), sizeof(header));
					file.write(reinterpret_cast<const char*>(entries.data()), entries.size() * sizeof(ICONDIRENTRY));

					for (auto& frame : frames)
					{
						file.write(reinterpret_cast<const char*>(frame.png_bytes), frame.png_len);
					}

					success = file.good();
					file.close();
				}

				for (auto& frame : frames)
				{
					STBIW_FREE(frame.png_bytes);
				}
			}
		}
		else if (ofn.nFilterIndex == 3 || check_extension(str_filename, ".jpg") || check_extension(str_filename, ".jpeg"))
		{
			success = stbi_write_jpg(filename, width, height, 4, rgba2, 90) != 0;
		}
		else if (ofn.nFilterIndex == 4 || check_extension(str_filename, ".bmp"))
		{
			success = stbi_write_bmp(filename, width, height, 4, rgba2) != 0;
		}
		else if (ofn.nFilterIndex == 5 || check_extension(str_filename, ".tga"))
		{
			success = stbi_write_tga(filename, width, height, 4, rgba2) != 0;
		}
		else
		{
			success = stbi_write_png(filename, width, height, 4, rgba2, width * 4) != 0;
		}

		if (!success)
		{
			MessageBox(window, L"Failed to save image.", L"Error", MB_ICONERROR | MB_OK);
		}
	}
}
void copy_image()
{
	int draw_width = disp_w;
	int draw_height = disp_h;
	if (!rgba2 || draw_width <= 0 || draw_height <= 0)
	{
		MessageBox(window, L"No generated image to copy!", L"Error", MB_ICONERROR | MB_OK);
		return;
	}

	size_t row_stride = (size_t)draw_width * 4;
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
		pHeader->biWidth = draw_width;
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
			for (int x = 0; x < draw_width; ++x)
			{
				dst_row[x * 4 + 0] = src_row[x * 4 + 2]; // B
				dst_row[x * 4 + 1] = src_row[x * 4 + 1]; // G
				dst_row[x * 4 + 2] = src_row[x * 4 + 0]; // R
				dst_row[x * 4 + 3] = src_row[x * 4 + 3]; // A
			}
		}
		GlobalUnlock(hClipboardData);

		if (OpenClipboard(window))
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
void paste_image(bool is_reference = false)
{
	if (!OpenClipboard(window)) return;

	if (IsClipboardFormatAvailable(CF_HDROP))
	{
		HANDLE hDrop = GetClipboardData(CF_HDROP);
		if (hDrop)
		{
			wchar_t wfilename[MAX_PATH] = {};
			if (DragQueryFile((HDROP)hDrop, 0, wfilename, ARRAYSIZE(wfilename)) > 0)
			{
				CloseClipboard();

				char filename[MAX_PATH] = {};
				WideCharToMultiByte(CP_UTF8, 0, wfilename, -1, filename, MAX_PATH, nullptr, nullptr);

				int new_w, new_h, new_c;
				unsigned char* new_rgba = stbi_load(filename, &new_w, &new_h, &new_c, 4);

				if (new_rgba)
				{
					if (is_reference)
					{
						ReferenceImage reference_image;
						reference_image.rgba = new_rgba;
						reference_image.w = new_w;
						reference_image.h = new_h;
						reference_images.push_back(reference_image);
					}
					else
					{
						if (rgba) free(rgba);
						if (rgba2) { free(rgba2); rgba2 = nullptr; }

						rgba = new_rgba;
						w = new_w;
						h = new_h;

						push_history(rgba, w, h);
						resize_window_to_image();
					}
					redraw();
				}
				return;
			}
		}
	}

	HANDLE hData = GetClipboardData(CF_DIB);
	if (!hData)
	{
		CloseClipboard();
		return;
	}

	BITMAPINFO* pbi = (BITMAPINFO*)GlobalLock(hData);
	if (pbi && pbi->bmiHeader.biBitCount == 32)
	{
		int width = pbi->bmiHeader.biWidth;
		int height = abs(pbi->bmiHeader.biHeight);

		// Ensure RGBA format
		unsigned char* pixels = (unsigned char*)malloc(width * height * 4);

		// Simplified extraction (assumes 32-bit DIB)
		unsigned char* src_bits = (unsigned char*)pbi + pbi->bmiHeader.biSize;
		for (int y = 0; y < height; y++)
		{
			for (int x = 0; x < width; x++)
			{
				// Clipboard DIB is BGRA
				pixels[(y * width + x) * 4 + 0] = src_bits[((height - 1 - y) * width + x) * 4 + 2]; // R
				pixels[(y * width + x) * 4 + 1] = src_bits[((height - 1 - y) * width + x) * 4 + 1]; // G
				pixels[(y * width + x) * 4 + 2] = src_bits[((height - 1 - y) * width + x) * 4 + 0]; // B
				pixels[(y * width + x) * 4 + 3] = src_bits[((height - 1 - y) * width + x) * 4 + 3]; // A
			}
		}
		GlobalUnlock(hData);

		if (is_reference)
		{
			ReferenceImage reference_image;
			reference_image.rgba = pixels;
			reference_image.w = width;
			reference_image.h = height;
			reference_images.push_back(reference_image);
		}
		else
		{
			if (rgba) free(rgba);
			if (rgba2) { free(rgba2); rgba2 = nullptr; }

			rgba = pixels;
			w = width;
			h = height;

			push_history(rgba, w, h);
		}
		resize_window_to_image();
		redraw();
	}
	CloseClipboard();
}

void generation()
{
	static sd_ctx_t* sd_ctx = nullptr;
	using PFN_sd_cancel_generation = decltype(&sd_cancel_generation);
	static PFN_sd_cancel_generation sd_cancel_generation = nullptr;

	if (is_generating.load())
	{
		// When entering this function while generation already running, it acts as a generation cancellation request:
		cancel_request.store(true);
		if (sd_ctx != nullptr && sd_cancel_generation != nullptr)
		{
			sd_cancel_generation(sd_ctx, SD_CANCEL_ALL);
		}
		return;
	}

	cancel_request.store(false);
	progress_errors.clear();
	final_errors.clear();

	std::thread worker([] {
		is_generating.store(true);
		redraw();

		std::string prompt;
		const int textbox_length = GetWindowTextLength(hEdit);
		if (textbox_length > 0)
		{
			std::wstring buffer(textbox_length, L'\0');
			GetWindowText(hEdit, &buffer[0], textbox_length + 1);
			int utf8len = WideCharToMultiByte(CP_UTF8, 0, buffer.c_str(), -1, nullptr, 0, nullptr, nullptr);
			prompt.resize(utf8len, '\0');
			WideCharToMultiByte(CP_UTF8, 0, buffer.c_str(), -1, prompt.data(), (int)prompt.length(), nullptr, nullptr);
		}

		const bool has_image = rgba != nullptr || rgba2 != nullptr;

		wchar_t models_path[MAX_PATH] = {};
		_snwprintf(models_path, MAX_PATH, L"%s%s", originalWorkingDir, L"/models");
		CreateDirectory(models_path, 0);

		int64_t seed = -1;
		wchar_t seed_path[MAX_PATH] = {};
		_snwprintf(seed_path, MAX_PATH, L"%s%s", originalWorkingDir, L"/seed.txt");
		std::ifstream seedfile(seed_path);
		if (seedfile.is_open())
		{
			seedfile >> seed;
			seedfile.close();
		}

		static auto EnsureModelExists = [](const wchar_t* url, const wchar_t* fileName) {
			if (std::filesystem::exists(fileName))
				return;
			current_download = fileName;
			size_t found = current_download.find_last_of(L"/\\");
			current_download = current_download.substr(found + 1);
			wchar_t tempFileName[MAX_PATH] = {};
			_snwprintf(tempFileName, MAX_PATH, L"%s.tmp", fileName);
			InvalidateRect(window, NULL, TRUE);

			URL_COMPONENTS uc = {};
			uc.dwStructSize = sizeof(uc);
			wchar_t host[256]{}, path[1024]{};
			uc.lpszHostName = host;
			uc.dwHostNameLength = _countof(host);
			uc.lpszUrlPath = path;
			uc.dwUrlPathLength = _countof(path);

			bool success = false;
			if (WinHttpCrackUrl(url, 0, 0, &uc))
			{
				HINTERNET hSession = WinHttpOpen(L"MiniAI", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
				if (hSession)
				{
					DWORD httpProto = WINHTTP_PROTOCOL_FLAG_HTTP2;
					WinHttpSetOption(hSession, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, &httpProto, sizeof(httpProto));
					HINTERNET hConnect = WinHttpConnect(hSession, host, uc.nPort, 0);
					if (hConnect)
					{
						DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
						HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
						if (hRequest)
						{
							if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(hRequest, NULL))
							{
								UINT64 dwSize = 0;
								{
									wchar_t sizeStr[64] = {};
									DWORD sizeStrLen = sizeof(sizeStr);
									if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, sizeStr, &sizeStrLen, WINHTTP_NO_HEADER_INDEX))
									{
										dwSize = _wcstoui64(sizeStr, nullptr, 10);
									}
								}
								std::ofstream outFile(tempFileName, std::ios::binary);
								std::vector<char> buffer(64 * 1024);
								DWORD bytesRead = 0;
								UINT64 totalRead = 0;
								int last_progress = -1;
								int last_completed_MB = -1;
								download_total_MB = int(dwSize / (1024ull * 1024ull));
								download_completed_MB = 0;
								while (WinHttpReadData(hRequest, buffer.data(), (DWORD)buffer.size(), &bytesRead) && bytesRead > 0)
								{
									outFile.write(buffer.data(), bytesRead);
									totalRead += bytesRead;

									download_completed_MB = int(totalRead / (1024ull * 1024ull));
									int current_progress = (dwSize > 0) ? int((double)totalRead / (double)dwSize * 100) : 0;
									if (current_progress != last_progress || download_completed_MB != last_completed_MB)
									{
										progress = current_progress;
										last_progress = current_progress;
										last_completed_MB = download_completed_MB;
										InvalidateRect(window, NULL, FALSE);
									}
								}
								outFile.close();
								success = (bytesRead == 0);
							}
							else
							{
								MessageBox(window, L"Could not connect to URL", L"Download Error", MB_OK | MB_ICONERROR);
							}
							WinHttpCloseHandle(hRequest);
						}
						WinHttpCloseHandle(hConnect);
					}
					WinHttpCloseHandle(hSession);
				}
			}
			if (success) {
				std::filesystem::rename(tempFileName, fileName);
			}
			else {
				std::filesystem::remove(tempFileName); // clean up partial file
			}
			current_download.clear();
			progress = 0;
			download_total_MB = 0;
			download_completed_MB = 0;
			InvalidateRect(window, NULL, TRUE);
		};

		static auto rgb2rgba = [](const uint8_t* srcRGB, uint8_t* dstRGBA, int width, int height) {
			for (int i = 0; i < width * height; ++i)
			{
				dstRGBA[i * 4 + 0] = srcRGB[i * 3 + 0];
				dstRGBA[i * 4 + 1] = srcRGB[i * 3 + 1];
				dstRGBA[i * 4 + 2] = srcRGB[i * 3 + 2];
				dstRGBA[i * 4 + 3] = 255;
			}
		};
		static auto rgba2rgb = [](const uint8_t* srcRGBA, uint8_t* dstRGB, int width, int height) {
			for (int i = 0; i < width * height; ++i)
			{
				dstRGB[i * 3 + 0] = srcRGBA[i * 4 + 0];
				dstRGB[i * 3 + 1] = srcRGBA[i * 4 + 1];
				dstRGB[i * 3 + 2] = srcRGBA[i * 4 + 2];
			}
		};

		if (mode == MODE::ASK)
		{
			// Use llama library for text generation:

			wchar_t dll_dir[MAX_PATH] = {};
			_snwprintf(dll_dir, MAX_PATH, L"%s/lib/llama", originalWorkingDir);
			SetDllDirectory(dll_dir);

			char u8_dll_dir[MAX_PATH];
			WideCharToMultiByte(CP_UTF8, 0, dll_dir, -1, u8_dll_dir, MAX_PATH, nullptr, nullptr);

			HMODULE llama = LoadLibrary(L"llama.dll");
			if (llama == nullptr)
			{
				MessageBox(window, L"llama.dll couldn't be loaded!", L"Error!", 0);
				return;
			}
			HMODULE ggml = LoadLibrary(L"ggml.dll");
			if (ggml == nullptr)
			{
				FreeLibrary(llama);
				MessageBox(window, L"ggml.dll couldn't be loaded!", L"Error!", 0);
				return;
			}

			LINK_DLL_FUNCTION(llama_model_default_params, llama);
			LINK_DLL_FUNCTION(llama_context_default_params, llama);
			LINK_DLL_FUNCTION(llama_model_load_from_file, llama);
			LINK_DLL_FUNCTION(llama_init_from_model, llama);
			LINK_DLL_FUNCTION(llama_sampler_chain_init, llama);
			LINK_DLL_FUNCTION(llama_sampler_chain_default_params, llama);
			LINK_DLL_FUNCTION(llama_sampler_chain_add, llama);
			LINK_DLL_FUNCTION(llama_sampler_init_penalties, llama);
			LINK_DLL_FUNCTION(llama_sampler_init_greedy, llama);
			LINK_DLL_FUNCTION(llama_sampler_init_mirostat_v2, llama);
			LINK_DLL_FUNCTION(llama_sampler_init_temp, llama);
			LINK_DLL_FUNCTION(llama_sampler_init_min_p, llama);
			LINK_DLL_FUNCTION(llama_sampler_init_dist, llama);
			LINK_DLL_FUNCTION(llama_sampler_init_top_k, llama);
			LINK_DLL_FUNCTION(llama_sampler_init_top_p, llama);
			LINK_DLL_FUNCTION(llama_model_get_vocab, llama);
			LINK_DLL_FUNCTION(llama_sampler_sample, llama);
			LINK_DLL_FUNCTION(llama_vocab_is_eog, llama);
			LINK_DLL_FUNCTION(llama_vocab_eot, llama);
			LINK_DLL_FUNCTION(llama_token_to_piece, llama);
			LINK_DLL_FUNCTION(llama_batch_get_one, llama);
			LINK_DLL_FUNCTION(llama_decode, llama);
			LINK_DLL_FUNCTION(llama_sampler_free, llama);
			LINK_DLL_FUNCTION(llama_model_free, llama);
			LINK_DLL_FUNCTION(llama_free, llama);
			LINK_DLL_FUNCTION(llama_backend_free, llama);
			LINK_DLL_FUNCTION(llama_log_set, llama);
			LINK_DLL_FUNCTION(llama_batch_init, llama);
			LINK_DLL_FUNCTION(llama_batch_free, llama);
			LINK_DLL_FUNCTION(llama_get_logits_ith, llama);
			LINK_DLL_FUNCTION(llama_tokenize, llama);
			LINK_DLL_FUNCTION(llama_model_chat_template, llama);
			LINK_DLL_FUNCTION(llama_chat_apply_template, llama);

			LINK_DLL_FUNCTION(ggml_backend_load_all, ggml);
			LINK_DLL_FUNCTION(ggml_backend_load_all_from_path, ggml);
			LINK_DLL_FUNCTION(ggml_backend_load, ggml);
			LINK_DLL_FUNCTION(ggml_backend_unload, ggml);
			LINK_DLL_FUNCTION(ggml_backend_reg_count, ggml);
			LINK_DLL_FUNCTION(ggml_backend_reg_get, ggml);

			static auto llama_callback = [](enum ggml_log_level level, const char* text, void* user_data) {
				if (level >= GGML_LOG_LEVEL_ERROR)
				{
					progress_errors += text;
				}
				int cnt = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
				std::wstring wstr(cnt, 0);
				MultiByteToWideChar(CP_UTF8, 0, text, -1, wstr.data(), cnt);
				OutputDebugString(wstr.c_str());
			};
			static auto my_llama_progress_callback = [](float in_progress, void* user_data) {
				progress = int(in_progress * 100);
				redraw();
				return true;
			};
			static auto post_description = [](std::string result_text) {
				// text can contain leading spaces and other stuff for some reason:
				while (!result_text.empty() &&
					(
						result_text.front() == ' ' ||
						result_text.front() == '\n' ||
						result_text.front() == '-'
						)
					)
				{
					result_text.erase(result_text.begin());
				}
				// Text line endings should be Windows-like for textbox:
				size_t pos = 0;
				while ((pos = result_text.find('\n', pos)) != std::string::npos)
				{
					if (pos == 0 || result_text[pos - 1] != '\r')
					{  // Avoid turning existing \r\n into \r\r\n
						result_text.insert(pos, 1, '\r');
						pos += 2;  // Skip past the \r\n we just inserted
					}
					else
					{
						++pos;
					}
				}
				int cnt = MultiByteToWideChar(CP_UTF8, 0, result_text.c_str(), -1, nullptr, 0);
				std::wstring wstr(cnt, 0);
				MultiByteToWideChar(CP_UTF8, 0, result_text.c_str(), -1, wstr.data(), cnt);
				SetWindowText(hEdit, wstr.c_str());
			};

			ggml_backend_load_all_from_path(u8_dll_dir);
			llama_log_set(llama_callback, nullptr);

			wchar_t model_path[MAX_PATH] = {};
			char u8_model_path[MAX_PATH] = {};

			if (text_model == TEXT_MODEL::QWEN_3_VL)
			{
				_snwprintf(model_path, MAX_PATH, L"%s%s", originalWorkingDir, L"/models/Qwen3VL-4B-Instruct-Q4_K_M.gguf");
				EnsureModelExists(L"https://huggingface.co/Qwen/Qwen3-VL-4B-Instruct-GGUF/resolve/main/Qwen3VL-4B-Instruct-Q4_K_M.gguf?download=true", model_path);
			}
			else if (text_model == TEXT_MODEL::GEMMA_4)
			{
				_snwprintf(model_path, MAX_PATH, L"%s%s", originalWorkingDir, L"/models/gemma-4-E4B-it-Q4_K_M.gguf");
				EnsureModelExists(L"https://huggingface.co/unsloth/gemma-4-E4B-it-GGUF/resolve/main/gemma-4-E4B-it-Q4_K_M.gguf?download=true", model_path);
			}

			WideCharToMultiByte(CP_UTF8, 0, model_path, -1, u8_model_path, MAX_PATH, nullptr, nullptr);

			llama_model_params model_params = llama_model_default_params();
			model_params.n_gpu_layers = is_cpu ? 0 : -1;
			model_params.progress_callback = my_llama_progress_callback;

			llama_context_params ctx_params = llama_context_default_params();
			ctx_params.n_ctx = 8192;
			ctx_params.n_batch = 512;

			llama_model* model = llama_model_load_from_file(u8_model_path, model_params);
			if (model != nullptr && !cancel_request.load())
			{
				llama_context* ctx = llama_init_from_model(model, ctx_params);
				if (ctx != nullptr && !cancel_request.load())
				{
					if (has_image)
					{
						// Image -> text generation:
						wchar_t mmproj_path[MAX_PATH] = {};
						char u8_mmproj_path[MAX_PATH] = {};

						if (text_model == TEXT_MODEL::QWEN_3_VL)
						{
							_snwprintf(mmproj_path, MAX_PATH, L"%s%s", originalWorkingDir, L"/models/mmproj-Qwen3VL-4B-Instruct-Q8_0.gguf");
							EnsureModelExists(L"https://huggingface.co/Qwen/Qwen3-VL-4B-Instruct-GGUF/resolve/main/mmproj-Qwen3VL-4B-Instruct-Q8_0.gguf?download=true", mmproj_path);
						}
						else if (text_model == TEXT_MODEL::GEMMA_4)
						{
							_snwprintf(mmproj_path, MAX_PATH, L"%s%s", originalWorkingDir, L"/models/mmproj-BF16.gguf");
							EnsureModelExists(L"https://huggingface.co/unsloth/gemma-4-E4B-it-GGUF/resolve/main/mmproj-BF16.gguf?download=true", mmproj_path);
						}
						WideCharToMultiByte(CP_UTF8, 0, mmproj_path, -1, u8_mmproj_path, MAX_PATH, nullptr, nullptr);

						HMODULE mtmd = LoadLibrary(L"mtmd.dll");
						if (mtmd == nullptr)
						{
							FreeLibrary(llama);
							FreeLibrary(ggml);
							MessageBox(window, L"mtmd.dll couldn't be loaded!", L"Error!", 0);
							return;
						}

						LINK_DLL_FUNCTION(mtmd_context_params_default, mtmd);
						LINK_DLL_FUNCTION(mtmd_init_from_file, mtmd);
						LINK_DLL_FUNCTION(mtmd_bitmap_init, mtmd);
						LINK_DLL_FUNCTION(mtmd_default_marker, mtmd);
						LINK_DLL_FUNCTION(mtmd_input_chunks_init, mtmd);
						LINK_DLL_FUNCTION(mtmd_tokenize, mtmd);
						LINK_DLL_FUNCTION(mtmd_helper_eval_chunks, mtmd);
						LINK_DLL_FUNCTION(mtmd_input_chunks_free, mtmd);
						LINK_DLL_FUNCTION(mtmd_bitmap_free, mtmd);
						LINK_DLL_FUNCTION(mtmd_free, mtmd);
						LINK_DLL_FUNCTION(mtmd_log_set, mtmd);

						mtmd_log_set(llama_callback, nullptr);

						mtmd_context_params mtmd_params = mtmd_context_params_default();
						mtmd_params.n_threads = 4;
						mtmd_params.use_gpu = is_cpu ? false : true;
						mtmd_params.progress_callback = my_llama_progress_callback;
						mtmd_params.image_min_tokens = -1;

						mtmd_context* ctx_mtmd = mtmd_init_from_file(u8_mmproj_path, model, mtmd_params);
						if (ctx_mtmd != nullptr && !cancel_request.load())
						{
							// Convert RGBA -> RGB
							std::vector<uint8_t> rgb_data(w * h * 3);
							if (rgba)
							{
								rgba2rgb(rgba, rgb_data.data(), w, h);
							}
							mtmd_bitmap* bitmap = mtmd_bitmap_init(w, h, rgb_data.data());

							std::string llama_prompt = std::string(mtmd_default_marker());

							if (text_model == TEXT_MODEL::QWEN_3_VL)
							{
								llama_prompt +=
									"<|im_start|>system\n"
									"You are a helpful assistant, answer the user's questions or follow the orders, based on the attached image.\n"
									"Do not repeat the description. Do not write internal notes. Output plain text only.<|im_end|>\n";
								llama_prompt += "<|im_start|>user\n";
								if (prompt.empty())
								{
									llama_prompt += "Describe the image in detail";
								}
								else
								{
									llama_prompt += prompt.c_str();
								}
								llama_prompt += "\n";
								llama_prompt += "<|im_end|>\n";
								llama_prompt += "<|im_start|>assistant\n";
							}
							else if (text_model == TEXT_MODEL::GEMMA_4)
							{
								llama_prompt += "<|turn>system\n";
								llama_prompt += "You are a helpful assistant, answer the user's questions or follow the orders, based on the attached image.\n";
								llama_prompt += "Do not repeat the description. Do not write internal notes. Output plain text only.<turn|>\n";
								llama_prompt += "<|turn>user\n";
								if (prompt.empty())
									llama_prompt += "Describe the image in detail";
								else
									llama_prompt += prompt;
								llama_prompt += "<turn|>\n";
								llama_prompt += "<|turn>model\n";
							}

							mtmd_input_text input_text = {};
							input_text.text = llama_prompt.c_str();
							input_text.text_len = llama_prompt.length();
							input_text.add_special = true;
							input_text.parse_special = true;

							mtmd_input_chunks* chunks = mtmd_input_chunks_init();

							const mtmd_bitmap* bitmaps[1] = { bitmap };
							if (mtmd_tokenize(ctx_mtmd, chunks, &input_text, bitmaps, 1) == 0)
							{
								llama_pos n_past = 0;
								if (mtmd_helper_eval_chunks(ctx_mtmd, ctx, chunks, n_past, 0, 512, true, &n_past) == 0)
								{
									llama_sampler* smpl = llama_sampler_chain_init(llama_sampler_chain_default_params());
									llama_sampler_chain_add(smpl, llama_sampler_init_penalties(128, 1.1f, 0.0f, 0.0f));
									llama_sampler_chain_add(smpl, llama_sampler_init_top_k(40));
									llama_sampler_chain_add(smpl, llama_sampler_init_top_p(0.92f, 1));
									llama_sampler_chain_add(smpl, llama_sampler_init_temp(0.85f));
									llama_sampler_chain_add(smpl, llama_sampler_init_dist(seed < 0 ? LLAMA_DEFAULT_SEED : uint32_t(seed)));

									llama_token new_token_id;
									const struct llama_vocab* vocab = llama_model_get_vocab(model);
									bool started_generating = false;
									std::string result_text;
									while (n_past < (llama_pos)ctx_params.n_ctx && !cancel_request.load())
									{
										new_token_id = llama_sampler_sample(smpl, ctx, -1);

										if (started_generating && (llama_vocab_is_eog(vocab, new_token_id) || new_token_id == llama_vocab_eot(vocab)))
											break;

										char buf[256];
										int n = llama_token_to_piece(vocab, new_token_id, buf, sizeof(buf), 0, false);

										if (n > 0) {
											std::string piece(buf, n);

											// Skip leading whitespace/newlines if we haven't started yet
											if (!started_generating && (piece == "\n" || piece == " "))
											{
												// continue; // Optional: keep skipping until you hit real text
											}
											else
											{
												started_generating = true;
												result_text += piece;
											}
										}

										llama_batch batch = llama_batch_get_one(&new_token_id, 1);
										llama_decode(ctx, batch);
										n_past += 1;

										post_description(result_text);
									}
									post_description(result_text);

									llama_sampler_free(smpl);
								}
							}
							mtmd_input_chunks_free(chunks);
							mtmd_bitmap_free(bitmap);
						}
						mtmd_free(ctx_mtmd);
						FreeLibrary(mtmd);
					}
					else
					{
						// Text -> text generation
						std::string llama_prompt;
						if (text_model == TEXT_MODEL::QWEN_3_VL)
						{
							llama_prompt +=
								"<|im_start|>system\n"
								"You are a helpful assistant, answer the user's questions or follow the orders.\n"
								"Do not repeat the description. Do not write internal notes. Output plain text only.<|im_end|>\n";
							llama_prompt += "<|im_start|>user\n";
							llama_prompt += prompt.c_str();
							llama_prompt += "\n";
							llama_prompt += "<|im_end|>\n";
							llama_prompt += "<|im_start|>assistant\n";
						}
						else if (text_model == TEXT_MODEL::GEMMA_4)
						{
							llama_prompt += "<|turn>system\n";
							llama_prompt += "You are a helpful assistant, answer the user's questions or follow the orders.\n";
							llama_prompt += "Do not repeat the description. Do not write internal notes. Output plain text only.\n";
							llama_prompt += "<|turn>user\n";
							llama_prompt += prompt;
							llama_prompt += "<turn|>\n";
							llama_prompt += "<|turn>model\n";
						}

						const llama_vocab* vocab = llama_model_get_vocab(model);

						int n_prompt_tokens = -llama_tokenize(vocab, llama_prompt.c_str(), (int32_t)llama_prompt.length(), NULL, 0, true, true);
						std::vector<llama_token> prompt_tokens(n_prompt_tokens);
						if (llama_tokenize(vocab, llama_prompt.c_str(), (int32_t)llama_prompt.length(), prompt_tokens.data(), (int)prompt_tokens.size(), true, true) >= 0)
						{
							llama_sampler* smpl = llama_sampler_chain_init(llama_sampler_chain_default_params());
							llama_sampler_chain_add(smpl, llama_sampler_init_greedy());
							llama_sampler_chain_add(smpl, llama_sampler_init_dist(seed < 0 ? LLAMA_DEFAULT_SEED : uint32_t(seed)));

							llama_batch batch = llama_batch_init(ctx_params.n_batch, 0, 1);
							for (size_t i = 0; i < prompt_tokens.size(); i++) {
								batch.token[i] = prompt_tokens[i];
								batch.pos[i] = (llama_pos)i;
								batch.n_seq_id[i] = 1;
								batch.seq_id[i][0] = 0;
								batch.logits[i] = (i == prompt_tokens.size() - 1);
							}
							batch.n_tokens = (int32_t)prompt_tokens.size();

							if (llama_decode(ctx, batch) == 0)
							{
								std::string result_text = "";
								int n_cur = batch.n_tokens;
								int n_decode_max = 1024;

								while (n_cur < n_decode_max && !cancel_request.load())
								{
									llama_token new_token_id = llama_sampler_sample(smpl, ctx, -1);
									if (llama_vocab_is_eog(vocab, new_token_id) || new_token_id == llama_vocab_eot(vocab)) {
										break;
									}
									char piece_buf[128] = { 0 };
									int n = llama_token_to_piece(vocab, new_token_id, piece_buf, sizeof(piece_buf), 0, true);
									if (n > 0) {
										result_text.append(piece_buf, n);
									}

									batch.n_tokens = 0;
									batch.token[0] = new_token_id;
									batch.pos[0] = n_cur;
									batch.n_seq_id[0] = 1;
									batch.seq_id[0][0] = 0;
									batch.logits[0] = true;
									batch.n_tokens = 1;

									n_cur++;

									if (llama_decode(ctx, batch) != 0)
										break;

									post_description(result_text);
								}
								post_description(result_text);
							}

							llama_batch_free(batch);
							llama_sampler_free(smpl);
						}
					}
				}
				llama_free(ctx);
			}
			final_errors = progress_errors;

			llama_model_free(model);
			llama_backend_free();

			while (true)
			{
				size_t n = ggml_backend_reg_count();
				if (n == 0) break;
				ggml_backend_reg_t reg = ggml_backend_reg_get(n - 1);
				if (reg)
				{
					ggml_backend_unload(reg);
				}
			}

			FreeLibrary(llama);
			FreeLibrary(ggml);
		}
		else
		{
			// Use stable diffusion library for image/video generation:

			// Save prompt.txt:
			std::ofstream prompt_file(promptPath, std::ios::binary); // binary because text mode inserts newlines
			if (prompt_file.is_open())
			{
				prompt_file << prompt;
				prompt_file.close();
			}

			wchar_t dll_dir[MAX_PATH] = {};
			_snwprintf(dll_dir, MAX_PATH, L"%s/lib/stable-diffusion", originalWorkingDir);
			SetDllDirectory(dll_dir);

			HMODULE stable_diffusion = LoadLibrary(L"stable-diffusion.dll");
			if (stable_diffusion == nullptr)
			{
				MessageBox(window, L"stable_diffusion.dll couldn't be loaded!", L"Error!", 0);
				return;
			}
			HMODULE ggml = LoadLibrary(L"ggml.dll");
			if (ggml == nullptr)
			{
				FreeLibrary(stable_diffusion);
				MessageBox(window, L"ggml.dll couldn't be loaded!", L"Error!", 0);
				return;
			}

			LINK_DLL_FUNCTION(sd_ctx_params_init, stable_diffusion);
			LINK_DLL_FUNCTION(new_sd_ctx, stable_diffusion);
			LINK_DLL_FUNCTION(sd_img_gen_params_init, stable_diffusion);
			LINK_DLL_FUNCTION(sd_vid_gen_params_init, stable_diffusion);
			LINK_DLL_FUNCTION(generate_image, stable_diffusion);
			LINK_DLL_FUNCTION(generate_video, stable_diffusion);
			LINK_DLL_FUNCTION(sd_sample_params_init, stable_diffusion);
			LINK_DLL_FUNCTION(sd_hires_params_init, stable_diffusion);
			LINK_DLL_FUNCTION(free_sd_ctx, stable_diffusion);
			LINK_DLL_FUNCTION(sd_set_log_callback, stable_diffusion);
			LINK_DLL_FUNCTION(sd_set_progress_callback, stable_diffusion);
			LINK_DLL_FUNCTION(sd_set_preview_callback, stable_diffusion);
			sd_cancel_generation = (PFN_sd_cancel_generation)GetProcAddress(stable_diffusion, "sd_cancel_generation");

			LINK_DLL_FUNCTION(ggml_backend_load_all, ggml);
			LINK_DLL_FUNCTION(ggml_backend_load_all_from_path, ggml);
			LINK_DLL_FUNCTION(ggml_backend_load, ggml);
			LINK_DLL_FUNCTION(ggml_backend_unload, ggml);
			LINK_DLL_FUNCTION(ggml_backend_reg_count, ggml);
			LINK_DLL_FUNCTION(ggml_backend_reg_get, ggml);

			sd_ctx_params_t sd_params;
			sd_ctx_params_init(&sd_params);

			char u8_dll_dir[MAX_PATH];
			WideCharToMultiByte(CP_UTF8, 0, dll_dir, -1, u8_dll_dir, MAX_PATH, nullptr, nullptr);
			ggml_backend_load_all_from_path(u8_dll_dir);

			static auto sd_log = [](enum sd_log_level_t level, const char* text, void* data) {
				if (level == SD_LOG_DEBUG)
					return;
				if (level == SD_LOG_ERROR)
				{
					progress_errors += text;
					InvalidateRect(window, NULL, TRUE);
				}
				int cnt = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
				std::wstring wstr(cnt, 0);
				MultiByteToWideChar(CP_UTF8, 0, text, -1, wstr.data(), cnt);
				OutputDebugString(wstr.c_str());
			};
			static auto sd_callback = [](int step, int steps, float time, void* data) {
				progress = int(float(step) / float(steps) * 100);
				redraw();
			};
			static auto sd_preview = [](int step, int frame_count, sd_image_t* frames, bool is_noisy, void* data) {
				if (frame_count == 0)
					return;
				sd_image_t* image = &frames[frame_count - 1];
				EnterCriticalSection(&image_cs);
				w = image->width;
				h = image->height;
				if (rgba)
				{
					free(rgba);
					rgba = nullptr;
				}
				rgba = (unsigned char*)malloc(w * h * 4);
				rgb2rgba(image->data, rgba, w, h);
				LeaveCriticalSection(&image_cs);
				redraw();
			};

			sd_set_log_callback(sd_log, nullptr);
			sd_set_progress_callback(sd_callback, nullptr);
			sd_set_preview_callback(sd_preview, PREVIEW_PROJ, 2, true, false, nullptr);

			wchar_t vae_path[MAX_PATH] = {};
			wchar_t audio_vae_path[MAX_PATH] = {};
			wchar_t t5xxl_path[MAX_PATH] = {};
			wchar_t text_encoder_path[MAX_PATH] = {};
			wchar_t diffusion_model_path[MAX_PATH] = {};
			wchar_t llm_vision_model_path[MAX_PATH] = {};
			wchar_t clip_vision_model_path[MAX_PATH] = {};
			wchar_t clip_l_model_path[MAX_PATH] = {};
			wchar_t clip_g_model_path[MAX_PATH] = {};
			wchar_t embeddings_connectors_path[MAX_PATH] = {};

			if (mode == MODE::VIDEO)
			{
				if (video_model == VIDEO_MODEL::WAN_2_2)
				{
					// Wan 2.2
					_snwprintf(vae_path, MAX_PATH, L"%s%s", originalWorkingDir, L"/models/wan2.2_vae.safetensors");
					_snwprintf(t5xxl_path, MAX_PATH, L"%s%s", originalWorkingDir, L"/models/umt5-xxl-encoder-Q3_K_M.gguf");
					_snwprintf(diffusion_model_path, MAX_PATH, L"%s%s", originalWorkingDir, L"/models/Wan2.2-TI2V-5B-Q3_K_M.gguf");
					_snwprintf(clip_vision_model_path, MAX_PATH, L"%s%s", originalWorkingDir, L"/models/clip_vision_h.safetensors");

					EnsureModelExists(L"https://huggingface.co/Comfy-Org/Wan_2.2_ComfyUI_Repackaged/resolve/main/split_files/vae/wan2.2_vae.safetensors?download=true", vae_path);
					EnsureModelExists(L"https://huggingface.co/city96/umt5-xxl-encoder-gguf/resolve/main/umt5-xxl-encoder-Q3_K_M.gguf?download=true", t5xxl_path);
					EnsureModelExists(L"https://huggingface.co/QuantStack/Wan2.2-TI2V-5B-GGUF/resolve/main/Wan2.2-TI2V-5B-Q3_K_M.gguf?download=true", diffusion_model_path);
					EnsureModelExists(L"https://huggingface.co/Comfy-Org/Wan_2.1_ComfyUI_repackaged/resolve/main/split_files/clip_vision/clip_vision_h.safetensors?download=true", clip_vision_model_path);

					sd_params.backend = "te=cpu"; // fix for out of memory on 8GB GPU
				}
				else if (video_model == VIDEO_MODEL::LTX_2_3)
				{
					// LTX 2.3
					_snwprintf(vae_path, MAX_PATH, L"%s%s", originalWorkingDir, L"/models/ltx-2.3-22b-distilled_video_vae.safetensors");
					_snwprintf(audio_vae_path, MAX_PATH, L"%s%s", originalWorkingDir, L"/models/ltx-2.3-22b-distilled_audio_vae.safetensors");
					_snwprintf(text_encoder_path, MAX_PATH, L"%s%s", originalWorkingDir, L"/models/gemma-3-12b-it-qat-UD-Q4_K_XL.gguf");
					_snwprintf(diffusion_model_path, MAX_PATH, L"%s%s", originalWorkingDir, L"/models/ltx-2.3-22b-distilled-1.1-Q3_K_M.gguf");
					_snwprintf(embeddings_connectors_path, MAX_PATH, L"%s%s", originalWorkingDir, L"/models/ltx-2.3-22b-distilled_embeddings_connectors.safetensors");

					EnsureModelExists(L"https://huggingface.co/unsloth/LTX-2.3-GGUF/resolve/main/vae/ltx-2.3-22b-distilled_video_vae.safetensors?download=true", vae_path);
					EnsureModelExists(L"https://huggingface.co/unsloth/LTX-2.3-GGUF/resolve/main/vae/ltx-2.3-22b-distilled_audio_vae.safetensors?download=true", audio_vae_path);
					EnsureModelExists(L"https://huggingface.co/unsloth/gemma-3-12b-it-qat-GGUF/resolve/main/gemma-3-12b-it-qat-UD-Q4_K_XL.gguf?download=true", text_encoder_path);
					EnsureModelExists(L"https://huggingface.co/unsloth/LTX-2.3-GGUF/resolve/main/text_encoders/ltx-2.3-22b-distilled_embeddings_connectors.safetensors?download=true", embeddings_connectors_path);
					EnsureModelExists(L"https://huggingface.co/unsloth/LTX-2.3-GGUF/resolve/main/distilled-1.1/ltx-2.3-22b-distilled-1.1-Q3_K_M.gguf?download=true", diffusion_model_path);

					sd_params.backend = "te=cpu"; // fix for out of memory on 8GB GPU
				}
				else if (video_model == VIDEO_MODEL::MINIMAX_H3)
				{
					// Minimax H3
					_snwprintf(vae_path, MAX_PATH, L"%s%s", originalWorkingDir, L"/models/minimax_h3_video_vae_fp16.safetensors");
					_snwprintf(audio_vae_path, MAX_PATH, L"%s%s", originalWorkingDir, L"/models/minimax_h3_audio_vae_fp32.safetensors");
					_snwprintf(text_encoder_path, MAX_PATH, L"%s%s", originalWorkingDir, L"/models/qwen3vl_32b_minimax_h3-Q4_K_M.gguf");

					EnsureModelExists(L"https://huggingface.co/Comfy-Org/MiniMax-H3/resolve/main/vae/minimax_h3_video_vae_fp16.safetensors?download=true", vae_path);
					EnsureModelExists(L"https://huggingface.co/Comfy-Org/MiniMax-H3/resolve/main/vae/minimax_h3_audio_vae_fp32.safetensors?download=true", audio_vae_path);
					EnsureModelExists(L"https://huggingface.co/leejet/MiniMax-H3-GGUF/resolve/main/qwen3vl_32b_minimax_h3-Q4_K_M.gguf?download=true", text_encoder_path);
					if (reference_images.empty())
					{
						_snwprintf(diffusion_model_path, MAX_PATH, L"%s%s", originalWorkingDir, L"/models/minimax_h3_fl2va_pruned-Q4_K_M.gguf");
						EnsureModelExists(L"https://huggingface.co/leejet/MiniMax-H3-GGUF/resolve/main/minimax_h3_fl2va_pruned-Q4_K_M.gguf?download=true", diffusion_model_path);
					}
					else
					{
						_snwprintf(diffusion_model_path, MAX_PATH, L"%s%s", originalWorkingDir, L"/models/minimax_h3_ref2va_pruned-Q4_K_M.gguf");
						EnsureModelExists(L"https://huggingface.co/leejet/MiniMax-H3-GGUF/resolve/main/minimax_h3_ref2va_pruned-Q4_K_M.gguf?download=true", diffusion_model_path);
					}

					sd_params.backend = "te=cpu"; // fix for out of memory on 8GB GPU
				}
			}
			else if (image_model == IMAGE_MODEL::FLUX2 || (mode == MODE::IMAGE_EDIT && edit_model == EDIT_MODEL::FLUX2))
			{
				// Flux 2
				_snwprintf(vae_path, MAX_PATH, L"%s%s", originalWorkingDir, L"/models/flux2-vae.safetensors");
				_snwprintf(text_encoder_path, MAX_PATH, L"%s%s", originalWorkingDir, L"/models/Qwen3-8B-Q4_K_M.gguf");
				_snwprintf(diffusion_model_path, MAX_PATH, L"%s%s", originalWorkingDir, L"/models/flux-2-klein-9b-Q4_K_M.gguf");

				EnsureModelExists(L"https://huggingface.co/Comfy-Org/flux2-dev/resolve/main/split_files/vae/flux2-vae.safetensors?download=true", vae_path);
				EnsureModelExists(L"https://huggingface.co/Qwen/Qwen3-8B-GGUF/resolve/main/Qwen3-8B-Q4_K_M.gguf?download=true", text_encoder_path);
				EnsureModelExists(L"https://huggingface.co/unsloth/FLUX.2-klein-9B-GGUF/resolve/main/flux-2-klein-9b-Q4_K_M.gguf?download=true", diffusion_model_path);

				sd_params.backend = "te=cpu"; // fix for out of memory on 8GB GPU
			}
			else if (image_model == IMAGE_MODEL::QWEN_IMAGE || (mode == MODE::IMAGE_EDIT && edit_model == EDIT_MODEL::QWEN_IMAGE_EDIT))
			{
				// Qwen Image
				_snwprintf(vae_path, MAX_PATH, L"%s%s", originalWorkingDir, L"/models/Qwen_Image-VAE.safetensors");
				_snwprintf(text_encoder_path, MAX_PATH, L"%s%s", originalWorkingDir, L"/models/Qwen2.5-VL-7B-Instruct-Q4_K_M.gguf");

				EnsureModelExists(L"https://huggingface.co/QuantStack/Qwen-Image-GGUF/resolve/main/VAE/Qwen_Image-VAE.safetensors?download=true", vae_path);
				EnsureModelExists(L"https://huggingface.co/unsloth/Qwen2.5-VL-7B-Instruct-GGUF/resolve/main/Qwen2.5-VL-7B-Instruct-Q4_K_M.gguf?download=true", text_encoder_path);

				if (mode == MODE::IMAGE_EDIT)
				{
					_snwprintf(llm_vision_model_path, MAX_PATH, L"%s%s", originalWorkingDir, L"/models/Qwen2.5-VL-7B-Instruct-mmproj-BF16.gguf");
					_snwprintf(diffusion_model_path, MAX_PATH, L"%s%s", originalWorkingDir, L"/models/Qwen_Image_Edit-Q4_K_M.gguf");
					EnsureModelExists(L"https://huggingface.co/QuantStack/Qwen-Image-Edit-GGUF/resolve/main/mmproj/Qwen2.5-VL-7B-Instruct-mmproj-BF16.gguf?download=true", llm_vision_model_path);
					EnsureModelExists(L"https://huggingface.co/QuantStack/Qwen-Image-Edit-GGUF/resolve/main/Qwen_Image_Edit-Q4_K_M.gguf?download=true", diffusion_model_path);
				}
				else
				{
					_snwprintf(diffusion_model_path, MAX_PATH, L"%s%s", originalWorkingDir, L"/models/Qwen_Image-Q4_K_M.gguf");
					EnsureModelExists(L"https://huggingface.co/QuantStack/Qwen-Image-GGUF/resolve/main/Qwen_Image-Q4_K_M.gguf?download=true", diffusion_model_path);
				}

				sd_params.backend = "te=cpu"; // fix for out of memory on 8GB GPU
			}
			else if (image_model == IMAGE_MODEL::Z_IMAGE)
			{
				// Z-image
				_snwprintf(vae_path, MAX_PATH, L"%s%s", originalWorkingDir, L"/models/ae.safetensors");
				_snwprintf(text_encoder_path, MAX_PATH, L"%s%s", originalWorkingDir, L"/models/Qwen3-4B-Instruct-2507-Q4_K_M.gguf");
				_snwprintf(diffusion_model_path, MAX_PATH, L"%s%s", originalWorkingDir, L"/models/z_image_turbo-Q4_K.gguf");

				EnsureModelExists(L"https://huggingface.co/Comfy-Org/z_image_turbo/resolve/main/split_files/vae/ae.safetensors?download=true", vae_path);
				EnsureModelExists(L"https://huggingface.co/unsloth/Qwen3-4B-Instruct-2507-GGUF/resolve/main/Qwen3-4B-Instruct-2507-Q4_K_M.gguf?download=true", text_encoder_path);
				EnsureModelExists(L"https://huggingface.co/leejet/Z-Image-Turbo-GGUF/resolve/main/z_image_turbo-Q4_K.gguf?download=true", diffusion_model_path);
			}
			else if (image_model == IMAGE_MODEL::ERNIE_IMAGE)
			{
				// Ernie-image
				_snwprintf(vae_path, MAX_PATH, L"%s%s", originalWorkingDir, L"/models/flux2-vae.safetensors");
				_snwprintf(text_encoder_path, MAX_PATH, L"%s%s", originalWorkingDir, L"/models/Ministral-3-3B-Instruct-2512-Q4_K_M.gguf");
				_snwprintf(llm_vision_model_path, MAX_PATH, L"%s%s", originalWorkingDir, L"/models/mmproj-BF16-ministral.gguf");
				_snwprintf(diffusion_model_path, MAX_PATH, L"%s%s", originalWorkingDir, L"/models/ernie-image-turbo-Q4_K_M.gguf");

				EnsureModelExists(L"https://huggingface.co/Comfy-Org/flux2-dev/resolve/main/split_files/vae/flux2-vae.safetensors?download=true", vae_path);
				EnsureModelExists(L"https://huggingface.co/unsloth/Ministral-3-3B-Instruct-2512-GGUF/resolve/main/Ministral-3-3B-Instruct-2512-Q4_K_M.gguf?download=true", text_encoder_path);
				EnsureModelExists(L"https://huggingface.co/unsloth/Ministral-3-3B-Instruct-2512-GGUF/resolve/main/mmproj-BF16.gguf?download=true", llm_vision_model_path);
				EnsureModelExists(L"https://huggingface.co/unsloth/ERNIE-Image-Turbo-GGUF/resolve/main/ernie-image-turbo-Q4_K_M.gguf?download=true", diffusion_model_path);
			}
			else if (image_model == IMAGE_MODEL::STABLE_DIFFUSION_3_5)
			{
				// Stable Diffusion 3.5 large
				_snwprintf(vae_path, MAX_PATH, L"%s%s", originalWorkingDir, L"/models/sd3.5_large-vae.safetensors");
				_snwprintf(clip_l_model_path, MAX_PATH, L"%s%s", originalWorkingDir, L"/models/clip_l.safetensors");
				_snwprintf(clip_g_model_path, MAX_PATH, L"%s%s", originalWorkingDir, L"/models/clip_g.safetensors");
				_snwprintf(t5xxl_path, MAX_PATH, L"%s%s", originalWorkingDir, L"/models/t5xxl_fp8_e4m3fn.safetensors");
				_snwprintf(diffusion_model_path, MAX_PATH, L"%s%s", originalWorkingDir, L"/models/sd3.5_large-q4_1.gguf");

				EnsureModelExists(L"https://huggingface.co/calcuis/sd3.5-large-gguf/resolve/main/diffusion_pytorch_model.safetensors?download=true", vae_path);
				EnsureModelExists(L"https://huggingface.co/calcuis/sd3.5-large-gguf/resolve/main/clip_l.safetensors?download=true", clip_l_model_path);
				EnsureModelExists(L"https://huggingface.co/calcuis/sd3.5-large-gguf/resolve/main/clip_g.safetensors?download=true", clip_g_model_path);
				EnsureModelExists(L"https://huggingface.co/calcuis/sd3.5-large-gguf/resolve/main/t5xxl_fp8_e4m3fn.safetensors?download=true", t5xxl_path);
				EnsureModelExists(L"https://huggingface.co/calcuis/sd3.5-large-gguf/resolve/main/sd3.5_large-q4_1.gguf?download=true", diffusion_model_path);
				
				sd_params.backend = "te=cpu"; // fix for out of memory on 8GB GPU
			}

			char u8_vae_path[MAX_PATH] = {};
			char u8_audio_vae_path[MAX_PATH] = {};
			char u8_t5xxl_path[MAX_PATH] = {};
			char u8_text_encoder_path[MAX_PATH] = {};
			char u8_diffusion_model_path[MAX_PATH] = {};
			char u8_llm_vision_model_path[MAX_PATH] = {};
			char u8_clip_vision_model_path[MAX_PATH] = {};
			char u8_clip_l_model_path[MAX_PATH] = {};
			char u8_clip_g_model_path[MAX_PATH] = {};
			char u8_embeddings_connectors_path[MAX_PATH] = {};

			WideCharToMultiByte(CP_UTF8, 0, vae_path, -1, u8_vae_path, MAX_PATH, nullptr, nullptr);
			WideCharToMultiByte(CP_UTF8, 0, audio_vae_path, -1, u8_audio_vae_path, MAX_PATH, nullptr, nullptr);
			WideCharToMultiByte(CP_UTF8, 0, t5xxl_path, -1, u8_t5xxl_path, MAX_PATH, nullptr, nullptr);
			WideCharToMultiByte(CP_UTF8, 0, text_encoder_path, -1, u8_text_encoder_path, MAX_PATH, nullptr, nullptr);
			WideCharToMultiByte(CP_UTF8, 0, diffusion_model_path, -1, u8_diffusion_model_path, MAX_PATH, nullptr, nullptr);
			WideCharToMultiByte(CP_UTF8, 0, llm_vision_model_path, -1, u8_llm_vision_model_path, MAX_PATH, nullptr, nullptr);
			WideCharToMultiByte(CP_UTF8, 0, clip_vision_model_path, -1, u8_clip_vision_model_path, MAX_PATH, nullptr, nullptr);
			WideCharToMultiByte(CP_UTF8, 0, clip_l_model_path, -1, u8_clip_l_model_path, MAX_PATH, nullptr, nullptr);
			WideCharToMultiByte(CP_UTF8, 0, clip_g_model_path, -1, u8_clip_g_model_path, MAX_PATH, nullptr, nullptr);
			WideCharToMultiByte(CP_UTF8, 0, embeddings_connectors_path, -1, u8_embeddings_connectors_path, MAX_PATH, nullptr, nullptr);

			sd_params.vae_path = u8_vae_path;
			sd_params.audio_vae_path = u8_audio_vae_path;
			sd_params.llm_path = u8_text_encoder_path;
			sd_params.t5xxl_path = u8_t5xxl_path;
			sd_params.diffusion_model_path = u8_diffusion_model_path;
			sd_params.llm_vision_path = u8_llm_vision_model_path;
			sd_params.clip_vision_path = u8_clip_vision_model_path;
			sd_params.clip_l_path = u8_clip_l_model_path;
			sd_params.clip_g_path = u8_clip_g_model_path;
			sd_params.embeddings_connectors_path = u8_embeddings_connectors_path;
			sd_params.wtype = SD_TYPE_COUNT;
			sd_params.n_threads = -1;
			sd_params.rng_type = STD_DEFAULT_RNG;
			sd_params.vae_conv_direct = true;
			//sd_params.flash_attn = true;

			if (mode == MODE::VIDEO)
			{
				sd_params.prediction = FLOW_PRED;
			}

			//sd_params.backend = "te=cpu"; // text encode on CPU
			//sd_params.backend = "vae=cpu"; // VAE decode on CPU
			//sd_params.backend = "controlnet=cpu"; // control net processing on CPU
			if (is_cpu)
			{
				sd_params.backend = "cpu"; // fully runs on CPU (slow)
			}
			sd_params.params_backend = "*=cpu"; // --offload-to-cpu param in the command line tool, allows larger models in small vram by offloading model to CPU RAM, but can still use the GPU for generation

			sd_ctx = new_sd_ctx(&sd_params);
			if (sd_ctx != nullptr)
			{
				if (mode == MODE::VIDEO)
				{
					sd_vid_gen_params_t vid_params;
					sd_vid_gen_params_init(&vid_params);
					vid_params.width = w2;
					vid_params.height = h2;
					vid_params.seed = seed;
					vid_params.prompt = prompt.c_str();

					vid_params.fps = video_fps;
					vid_params.video_frames = vid_params.fps * video_seconds + 1; // +1 start frame

					vid_params.strength = 1.0f;
					vid_params.vace_strength = 1.0f;

					vid_params.vae_tiling_params.enabled = true;
					vid_params.vae_tiling_params.temporal_tiling = true;

					sd_sample_params_init(&vid_params.sample_params);

					if (video_model == VIDEO_MODEL::WAN_2_2)
					{
						vid_params.sample_params.sample_method = EULER_SAMPLE_METHOD;
						vid_params.sample_params.sample_steps = 30;
						vid_params.sample_params.scheduler = SIMPLE_SCHEDULER;
						vid_params.sample_params.eta = 0.0f;
						vid_params.sample_params.flow_shift = 8.0f;

						vid_params.sample_params.guidance.txt_cfg = 4.0f;
						vid_params.sample_params.guidance.img_cfg = 1.0f;
						vid_params.sample_params.guidance.distilled_guidance = 1.0f;
					}
					else if (video_model == VIDEO_MODEL::LTX_2_3)
					{
						vid_params.sample_params.sample_method = EULER_SAMPLE_METHOD;
						vid_params.sample_params.sample_steps = 8;
						vid_params.sample_params.scheduler = SIMPLE_SCHEDULER;
						vid_params.sample_params.eta = 0.0f;
						vid_params.sample_params.flow_shift = 1.0f;

						vid_params.sample_params.guidance.txt_cfg = 1.0f;
						vid_params.sample_params.guidance.img_cfg = 1.0f;
						vid_params.sample_params.guidance.distilled_guidance = 1.0f;
					}
					else if (video_model == VIDEO_MODEL::MINIMAX_H3)
					{
						vid_params.sample_params.sample_method = EULER_SAMPLE_METHOD;
						vid_params.sample_params.sample_steps = 16;
						vid_params.sample_params.scheduler = SIMPLE_SCHEDULER;
						vid_params.sample_params.eta = 0.0f;
						vid_params.sample_params.flow_shift = 12.0f;

						vid_params.sample_params.guidance.txt_cfg = 1.0f;
						vid_params.sample_params.guidance.img_cfg = 1.0f;
						vid_params.sample_params.guidance.distilled_guidance = 1.0f;
					}

					std::vector<sd_image_t> sd_reference_images;

					sd_image_t ref_img = {};
					if (rgba != nullptr)
					{
						ref_img.width = w;
						ref_img.height = h;
						ref_img.channel = 4;
						ref_img.data = rgba;
					}
					if (video_model == VIDEO_MODEL::MINIMAX_H3 && !reference_images.empty())
					{
						// Minimax has separate model if reference images are used
						sd_reference_images.push_back(ref_img);
					}
					else
					{
						vid_params.init_image = ref_img;
					}

					// Additional reference images
					for (const auto& rimg : reference_images)
					{
						if (rimg.rgba && rimg.w > 0 && rimg.h > 0)
						{
							sd_image_t sdimg = {};
							sdimg.width = rimg.w;
							sdimg.height = rimg.h;
							sdimg.channel = 4;
							sdimg.data = rimg.rgba;
							sd_reference_images.push_back(sdimg);
						}
					}
					if (!sd_reference_images.empty())
					{
						if (video_model == VIDEO_MODEL::MINIMAX_H3)
						{
							vid_params.ref_images = sd_reference_images.data();
							vid_params.ref_images_count = (int)sd_reference_images.size();
						}
						else
						{
							vid_params.control_frames = sd_reference_images.data();
							vid_params.control_frames_size = (int)sd_reference_images.size();
						}
					}

					sd_audio_t* audio = nullptr;
					sd_image_t* frames = nullptr;
					int num_frames = 0;
					if (generate_video(sd_ctx, &vid_params, &frames, &num_frames, &audio))
					{
						time_t t = std::time(nullptr);
						struct tm* tmptr;
						struct tm time_info;
						tmptr = &time_info;
						localtime_s(&time_info, &t);

						// Present last frame to window
						if (num_frames > 0)
						{
							sd_image_t* image = &frames[num_frames - 1];
							EnterCriticalSection(&image_cs);
							w = image->width;
							h = image->height;
							if (rgba)
							{
								free(rgba);
								rgba = nullptr;
							}
							rgba = (unsigned char*)malloc(w * h * 4);
							rgb2rgba(image->data, rgba, w, h);
							LeaveCriticalSection(&image_cs);
							push_history(rgba, w, h);
						}

						wchar_t output_dir[MAX_PATH] = {};
						_snwprintf(output_dir, MAX_PATH, L"%s%s", originalWorkingDir, L"/output/video");
						CreateDirectory(output_dir, 0);

						// Save to MP4:
						if (num_frames > 0)
						{
							// 1. Setup Output Directory and File Path
							std::wstringstream ss;
							ss << output_dir << L"/";
							ss << std::put_time(tmptr, L"%Y-%m-%d %H-%M-%S");
							ss << L".mp4";
							std::wstring output_file = ss.str();

							int width = frames[0].width;
							int height = frames[0].height;
							UINT64 frameDuration = UINT64(10000000 / vid_params.fps); // 100-nanosecond units

							// 2. Initialize Media Foundation
							CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
							MFStartup(MF_VERSION);

							Microsoft::WRL::ComPtr<IMFSinkWriter> pSinkWriter;
							Microsoft::WRL::ComPtr<IMFMediaType>  pMediaTypeOut;
							Microsoft::WRL::ComPtr<IMFMediaType>  pMediaTypeIn;
							DWORD videoStreamIndex = 0;
							DWORD audioStreamIndex = 0;
							const bool has_audio = (audio != nullptr && audio->data != nullptr && audio->sample_count > 0);

							// 3. Create Sink Writer
							if (SUCCEEDED(MFCreateSinkWriterFromURL(output_file.c_str(), NULL, NULL, &pSinkWriter)))
							{
								// 4. Set Video Output Media Type (H.264 Video in MP4)
								MFCreateMediaType(&pMediaTypeOut);
								pMediaTypeOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
								pMediaTypeOut->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
								pMediaTypeOut->SetUINT32(MF_MT_AVG_BITRATE, 5000000);
								pMediaTypeOut->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
								MFSetAttributeSize(pMediaTypeOut.Get(), MF_MT_FRAME_SIZE, width, height);
								MFSetAttributeRatio(pMediaTypeOut.Get(), MF_MT_FRAME_RATE, (UINT32)vid_params.fps, 1);
								MFSetAttributeRatio(pMediaTypeOut.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
								pSinkWriter->AddStream(pMediaTypeOut.Get(), &videoStreamIndex);

								// 5. Set Video Input Media Type (Raw RGB24)
								MFCreateMediaType(&pMediaTypeIn);
								pMediaTypeIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
								pMediaTypeIn->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB24);
								pMediaTypeIn->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
								MFSetAttributeSize(pMediaTypeIn.Get(), MF_MT_FRAME_SIZE, width, height);
								MFSetAttributeRatio(pMediaTypeIn.Get(), MF_MT_FRAME_RATE, (UINT32)vid_params.fps, 1);
								MFSetAttributeRatio(pMediaTypeIn.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
								pSinkWriter->SetInputMediaType(videoStreamIndex, pMediaTypeIn.Get(), NULL);

								// 6. Set Audio Stream (AAC Output, 16-bit PCM Input)
								if (has_audio)
								{
									Microsoft::WRL::ComPtr<IMFMediaType> pAudioMediaTypeOut;
									Microsoft::WRL::ComPtr<IMFMediaType> pAudioMediaTypeIn;

									// Output AAC
									MFCreateMediaType(&pAudioMediaTypeOut);
									pAudioMediaTypeOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
									pAudioMediaTypeOut->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
									pAudioMediaTypeOut->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, audio->channels);
									pAudioMediaTypeOut->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, audio->sample_rate);
									pAudioMediaTypeOut->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
									pAudioMediaTypeOut->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, audio->sample_rate * audio->channels * 2);
									pAudioMediaTypeOut->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, audio->channels * 2);
									pSinkWriter->AddStream(pAudioMediaTypeOut.Get(), &audioStreamIndex);

									// Input PCM 16-bit
									MFCreateMediaType(&pAudioMediaTypeIn);
									pAudioMediaTypeIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
									pAudioMediaTypeIn->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
									pAudioMediaTypeIn->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, audio->channels);
									pAudioMediaTypeIn->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, audio->sample_rate);
									pAudioMediaTypeIn->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
									pAudioMediaTypeIn->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, audio->channels * 2);
									pAudioMediaTypeIn->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, audio->sample_rate * audio->channels * 2);
									pSinkWriter->SetInputMediaType(audioStreamIndex, pAudioMediaTypeIn.Get(), NULL);
								}

								// 7. Start Writing
								pSinkWriter->BeginWriting();

								// Write Video Samples
								DWORD cbBuffer = width * height * 3;
								int stride = width * 3;
								for (int i = 0; i < num_frames; ++i)
								{
									Microsoft::WRL::ComPtr<IMFSample>     pSample;
									Microsoft::WRL::ComPtr<IMFMediaBuffer> pBuffer;
									BYTE* pData = NULL;

									MFCreateMemoryBuffer(cbBuffer, &pBuffer);
									pBuffer->Lock(&pData, NULL, NULL);

									// Copy buffer: Flip bottom-up and swap RGB -> BGR
									for (int y = 0; y < height; ++y)
									{
										const BYTE* srcRow = frames[i].data + (height - 1 - y) * stride;
										BYTE* dstRow = pData + y * stride;

										for (int x = 0; x < width; ++x)
										{
											int idx = x * 3;
											dstRow[idx + 0] = srcRow[idx + 2];
											dstRow[idx + 1] = srcRow[idx + 1];
											dstRow[idx + 2] = srcRow[idx + 0];
										}
									}

									pBuffer->Unlock();
									pBuffer->SetCurrentLength(cbBuffer);

									MFCreateSample(&pSample);
									pSample->AddBuffer(pBuffer.Get());

									pSample->SetSampleTime(i * frameDuration);
									pSample->SetSampleDuration(frameDuration);

									pSinkWriter->WriteSample(videoStreamIndex, pSample.Get());
								}

								// Write Audio Sample
								if (has_audio)
								{
									size_t total_samples = audio->sample_count * audio->channels;
									DWORD cbAudioBuffer = (DWORD)(total_samples * sizeof(int16_t));

									Microsoft::WRL::ComPtr<IMFSample>     pAudioSample;
									Microsoft::WRL::ComPtr<IMFMediaBuffer> pAudioBuffer;
									BYTE* pAudioData = NULL;

									if (SUCCEEDED(MFCreateMemoryBuffer(cbAudioBuffer, &pAudioBuffer)))
									{
										pAudioBuffer->Lock(&pAudioData, NULL, NULL);

										// Convert float PCM [-1.0f, 1.0f] to int16 PCM
										int16_t* pcm16 = (int16_t*)pAudioData;
										for (size_t s = 0; s < total_samples; ++s)
										{
											float sample = audio->data[s];
											if (sample > 1.0f) sample = 1.0f;
											if (sample < -1.0f) sample = -1.0f;
											pcm16[s] = (int16_t)(sample * 32767.0f);
										}

										pAudioBuffer->Unlock();
										pAudioBuffer->SetCurrentLength(cbAudioBuffer);

										MFCreateSample(&pAudioSample);
										pAudioSample->AddBuffer(pAudioBuffer.Get());

										pAudioSample->SetSampleTime(0);
										UINT64 audioDuration = (UINT64)audio->sample_count * 10000000 / audio->sample_rate;
										pAudioSample->SetSampleDuration(audioDuration);

										pSinkWriter->WriteSample(audioStreamIndex, pAudioSample.Get());
									}
								}

								// 8. Finalize Writer
								pSinkWriter->Finalize();
							}
							MFShutdown();
							CoUninitialize();
							ShellExecute(NULL, L"open", output_file.c_str(), NULL, NULL, SW_SHOWNORMAL); // open video
						}
					}
					else
					{
						final_errors = progress_errors;
					}
				}
				else
				{
					sd_img_gen_params_t img_params;
					sd_img_gen_params_init(&img_params);
					img_params.width = w2;
					img_params.height = h2;
					img_params.seed = seed;
					img_params.prompt = prompt.c_str();
					img_params.strength = 1.0f;
					img_params.batch_count = batch_count;
					img_params.vae_tiling_params.enabled = true; // reduces memory usage in VAE decode pass, but slower processing

					sd_sample_params_init(&img_params.sample_params);
					img_params.sample_params.eta = 0.0f;
					img_params.sample_params.sample_method = EULER_SAMPLE_METHOD;

					img_params.sample_params.guidance.txt_cfg = 1.0f;
					img_params.sample_params.guidance.img_cfg = 1.0f;
					img_params.sample_params.guidance.distilled_guidance = 1.0f;

					if (image_model == IMAGE_MODEL::FLUX2 || (mode == MODE::IMAGE_EDIT && edit_model == EDIT_MODEL::FLUX2))
					{
						img_params.sample_params.scheduler = FLUX2_SCHEDULER;
						img_params.sample_params.sample_steps = 4;
					}
					else if (image_model == IMAGE_MODEL::QWEN_IMAGE || (mode == MODE::IMAGE_EDIT && edit_model == EDIT_MODEL::QWEN_IMAGE_EDIT))
					{
						img_params.sample_params.scheduler = SIMPLE_SCHEDULER;
						img_params.sample_params.sample_steps = 30;
						img_params.sample_params.guidance.txt_cfg = 2.5f;
						img_params.sample_params.flow_shift = 3.0f;
					}
					else if (image_model == IMAGE_MODEL::Z_IMAGE)
					{
						img_params.sample_params.scheduler = SIMPLE_SCHEDULER;
						img_params.sample_params.sample_steps = 8;
					}
					else if (image_model == IMAGE_MODEL::ERNIE_IMAGE)
					{
						img_params.sample_params.scheduler = SIMPLE_SCHEDULER;
						img_params.sample_params.sample_steps = 8;
					}
					else if (image_model == IMAGE_MODEL::STABLE_DIFFUSION_3_5)
					{
						img_params.sample_params.scheduler = SGM_UNIFORM_SCHEDULER;
						img_params.sample_params.sample_steps = 28;
						img_params.sample_params.guidance.txt_cfg = 3.5f;
					}

					std::vector<sd_image_t> sd_reference_images;
					if (mode == MODE::IMAGE_EDIT)
					{
						sd_image_t ref_img = {};
						if (rgba != nullptr)
						{
							ref_img.width = w;
							ref_img.height = h;
							ref_img.channel = 4;
							ref_img.data = rgba;
						}
						if (ref_img.data)
						{
							sd_reference_images.push_back(ref_img);
						}

						// Additional reference images
						for (const auto& rimg : reference_images)
						{
							if (rimg.rgba && rimg.w > 0 && rimg.h > 0)
							{
								sd_image_t sdimg = {};
								sdimg.width = rimg.w;
								sdimg.height = rimg.h;
								sdimg.channel = 4;
								sdimg.data = rimg.rgba;
								sd_reference_images.push_back(sdimg);
							}
						}
					}

					if (!sd_reference_images.empty())
					{
						img_params.ref_images = sd_reference_images.data();
						img_params.ref_images_count = (int)sd_reference_images.size();
					}

					sd_image_t* images = nullptr;
					int num_images = 0;
					if (generate_image(sd_ctx, &img_params, &images, &num_images))
					{
						for (int batch_index = 0; batch_index < num_images; ++batch_index)
						{
							const sd_image_t& image = images[batch_index];
							EnterCriticalSection(&image_cs);
							w = image.width;
							h = image.height;
							if (rgba)
							{
								free(rgba);
								rgba = nullptr;
							}
							rgba = (unsigned char*)malloc(w * h * 4);
							rgb2rgba(image.data, rgba, w, h);
							LeaveCriticalSection(&image_cs);
							push_history(rgba, w, h, true, batch_index); // save output!
						}
					}
					else
					{
						final_errors = progress_errors;
					}
				}
				free_sd_ctx(sd_ctx);
			}

			while (true)
			{
				size_t n = ggml_backend_reg_count();
				if (0 == n) break;
				ggml_backend_reg_t reg = ggml_backend_reg_get(n - 1);
				if (reg)
				{
					ggml_backend_unload(reg);
				}
			}

			FreeLibrary(stable_diffusion);
			FreeLibrary(ggml);
		}

		is_generating.store(false);
		cancel_request.store(false);
		sd_ctx = nullptr;
		progress = 0;
		redraw();
	});

	SetThreadDescription((HANDLE)worker.native_handle(), L"AI");

	worker.detach();

	// Start lightweight animation timer (indeterminate activity indicator)
	SetTimer(window, IDT_ANIM, 40, NULL); // ~25 fps
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine, _In_ int nCmdShow)
{
	_wgetcwd(originalWorkingDir, MAX_PATH); // save original working dir at startup
	InitializeCriticalSection(&image_cs);

	static bool exiting = false;
	static auto WndProc = [](HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) -> LRESULT {
		switch (message)
		{
		case WM_SIZE:
		{
			if (!resize_fixed)
			{
				w2 = LOWORD(lParam);
				h2 = HIWORD(lParam) - text_height - button_height - splitter_thickness - get_ref_container_height();
			}
			redraw();
		}
		break;
		case WM_DESTROY:
			exiting = true;
			KillTimer(hWnd, IDT_ANIM);
			clear_ref_images();
			PostQuitMessage(0);
			break;
		case WM_TIMER:
			if (wParam == IDT_ANIM)
			{
				if (!is_generating.load() && !cancel_request.load())
				{
					KillTimer(hWnd, IDT_ANIM);
				}
				else
				{
					// Only invalidate the image area for cheap animation updates
					RECT r = { 0, 0, w2, h2 };
					InvalidateRect(hWnd, &r, FALSE);
				}
			}
			break;
		case WM_ERASEBKGND:
			return 1;
		case WM_PAINT:
		{
			PAINTSTRUCT ps;
			HDC hdc = BeginPaint(hWnd, &ps);

			// Single offscreen DIB for everything we paint (image + refs + splitter)
			const int ref_h = get_ref_container_height();
			const int paint_h = h2 + ref_h + splitter_thickness;
			if (w2 <= 0 || paint_h <= 0)
			{
				EndPaint(hWnd, &ps);
				break;
			}

			BITMAPINFO bmi = {};
			bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
			bmi.bmiHeader.biWidth = w2;
			bmi.bmiHeader.biHeight = -paint_h;
			bmi.bmiHeader.biPlanes = 1;
			bmi.bmiHeader.biBitCount = 32;
			bmi.bmiHeader.biCompression = BI_RGB;
			void* bits = nullptr;
			HBITMAP hBmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
			if (hBmp && bits)
			{
				// Checkerboard + image into the top h2 rows
				EnterCriticalSection(&image_cs);
				unsigned char* dst = (unsigned char*)bits;
				const int cell = 32;
				const int ox = (w2 - disp_w) / 2;
				const int oy = (h2 - disp_h) / 2;
				for (int y = 0; y < h2; ++y)
				{
					for (int x = 0; x < w2; ++x)
					{
						const unsigned char cb = (((x / cell) + (y / cell)) & 1) ? 25 : 35;
						unsigned char a = 0;
						unsigned char sr = 0, sg = 0, sb = 0;
						if (rgba2 && disp_w > 0 && disp_h > 0 && x >= ox && x < ox + disp_w && y >= oy && y < oy + disp_h)
						{
							const unsigned char* p = rgba2 + ((size_t)(y - oy) * disp_w + (x - ox)) * 4;
							sr = p[0];
							sg = p[1];
							sb = p[2];
							a = p[3];
						}
						const unsigned char inv = 255 - a;
						dst[0] = (sb * a + cb * inv) / 255; // B
						dst[1] = (sg * a + cb * inv) / 255; // G
						dst[2] = (sr * a + cb * inv) / 255; // R
						dst[3] = 255;
						dst += 4;
					}
				}
				LeaveCriticalSection(&image_cs);

				// Soft transparent Gaussian glow traveling around the perimeter (blended into the DIB)
				if ((is_generating.load() || cancel_request.load()) && current_download.empty() && h2 > 20 && w2 > 20)
				{
					const int thickness = 16;
					const float sigma = 200.0f;
					const int cycle_ms = 6000;
					const int peri = 2 * (w2 + h2);
					const ULONGLONG t = GetTickCount64();
					float head = (float)((t % cycle_ms) * peri) / (float)cycle_ms;

					auto blend_pixel = [&](int x, int y, float g)
					{
						if (x < 0 || y < 0 || x >= w2 || y >= h2 || g < 0.03f)
							return;
						unsigned char* p = (unsigned char*)bits + ((size_t)y * w2 + x) * 4;
						const int gr = (int)(g * 80);
						const int gg = (int)(g * 180);
						const int gb = (int)(g * 250);
						const int alpha = (int)(g * 255); // how strongly the glow covers
						const int inv = 255 - alpha;
						p[0] = (unsigned char)((gb * 255 + p[0] * inv) / 255); // B
						p[1] = (unsigned char)((gg * 255 + p[1] * inv) / 255); // G
						p[2] = (unsigned char)((gr * 255 + p[2] * inv) / 255); // R
					};

					auto process_side = [&](int x0, int y0, int x1, int y1, float peri_start, bool inward_horizontal)
					{
						bool horizontal = (y0 == y1);
						int len = horizontal ? abs(x1 - x0) : abs(y1 - y0);
						int dir = horizontal ? (x1 >= x0 ? 1 : -1) : (y1 >= y0 ? 1 : -1);

						for (int i = 0; i <= len; ++i)
						{
							float pcoord = peri_start + (float)i;
							float dist = fabsf(pcoord - head);
							if (dist > peri * 0.5f) 
								dist = (float)peri - dist;

							float g = expf(-(dist * dist) / (2.0f * sigma * sigma));
							if (g < 0.03f)
								continue;

							int x = horizontal ? (x0 + i * dir) : x0;
							int y = horizontal ? y0 : (y0 + i * dir);

							// Blend a few pixels inward so the glow has thickness but stays soft
							for (int d = 0; d < thickness; ++d)
							{
								float fall = 1.0f - (float)d / (float)thickness;
								float gg = g * fall * fall; // quadratic falloff for softer look
								if (horizontal)
								{
									int yy = (y0 == 0) ? d : (h2 - 1 - d);
									blend_pixel(x, yy, gg);
								}
								else
								{
									int xx = (x0 == 0) ? d : (w2 - 1 - d);
									blend_pixel(xx, y, gg);
								}
							}
						}
					};

					process_side(0, 0, w2 - 1, 0, 0.0f, true);                            // top
					process_side(w2 - 1, 0, w2 - 1, h2 - 1, (float)w2, false);           // right
					process_side(w2 - 1, h2 - 1, 0, h2 - 1, (float)(w2 + h2), true);     // bottom
					process_side(0, h2 - 1, 0, 0, (float)(w2 + h2 + w2), false);         // left
				}

				// Select the single DIB into a DC for GDI text / refs / splitter
				HDC memDC = CreateCompatibleDC(hdc);
				HBITMAP hOldBmp = (HBITMAP)SelectObject(memDC, hBmp);
				SetBkMode(memDC, TRANSPARENT);

				auto draw_centered_text = [&](const wchar_t* status, int fontSize, COLORREF color, bool singleLine)
				{
					HFONT hFont = CreateFont(fontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
						DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
					HFONT hOld = (HFONT)SelectObject(memDC, hFont);
					RECT textRect = { 0, 0, w2, h2 };

					if (singleLine)
					{
						RECT shadowRect = textRect;
						OffsetRect(&shadowRect, 2, 2);
						SetTextColor(memDC, RGB(10, 10, 10));
						DrawText(memDC, status, -1, &shadowRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
						SetTextColor(memDC, color);
						DrawText(memDC, status, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
					}
					else
					{
						RECT calcRect = textRect;
						DrawText(memDC, status, -1, &calcRect, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
						int textHeight = calcRect.bottom - calcRect.top;
						int offset = (h2 - textHeight) / 2;
						textRect.top += offset;
						textRect.bottom = textRect.top + textHeight;
						RECT shadowRect = textRect;
						OffsetRect(&shadowRect, 2, 2);
						SetTextColor(memDC, RGB(10, 10, 10));
						DrawText(memDC, status, -1, &shadowRect, DT_CENTER | DT_WORDBREAK);
						SetTextColor(memDC, color);
						DrawText(memDC, status, -1, &textRect, DT_CENTER | DT_WORDBREAK);
					}

					SelectObject(memDC, hOld);
					DeleteObject(hFont);
				};

				if (!current_download.empty())
				{
					wchar_t status[4096] = {};
					_snwprintf(status, ARRAYSIZE(status), L"Downloading model: %d%% (%d MB / %d MB)\n%s", progress, download_completed_MB, download_total_MB, current_download.c_str());
					draw_centered_text(status, 22, RGB(255, 255, 255), false);
				}
				else if (progress > 0 && progress < 100)
				{
					wchar_t status[32] = {};
					_snwprintf(status, ARRAYSIZE(status), L"%d%%", progress);
					draw_centered_text(status, 64, RGB(255, 255, 255), true);
				}
				else if (!final_errors.empty())
				{
					int cnt = MultiByteToWideChar(CP_UTF8, 0, final_errors.c_str(), -1, nullptr, 0);
					std::wstring wstr(cnt, 0);
					MultiByteToWideChar(CP_UTF8, 0, final_errors.c_str(), -1, wstr.data(), cnt);
					wchar_t status[4096] = {};
					_snwprintf(status, ARRAYSIZE(status), L"Errors:\n%s", wstr.c_str());
					draw_centered_text(status, 22, RGB(255, 255, 255), false);
				}
				else if (is_generating.load() || cancel_request.load())
				{
					const wchar_t* status = cancel_request.load() ? L"Stopping..." : L"Working...";
					draw_centered_text(status, 64, RGB(255, 255, 255), true);
				}
				else if (rgba == nullptr && !is_generating.load())
				{
					const wchar_t* status = L"The image will be generated here.\nOr drag and drop an image here.";
					draw_centered_text(status, 26, RGB(155, 155, 155), false);
				}

				if (mode == MODE::IMAGE_EDIT || mode == MODE::VIDEO)
				{
					RECT ref_rect = { 0, h2, w2, h2 + ref_h };

					// Subtle diagonal-hatch background (distinct from the main checkerboard)
					for (int y = h2; y < h2 + ref_h; ++y)
					{
						unsigned char* row = (unsigned char*)bits + ((size_t)y * w2) * 4;
						for (int x = 0; x < w2; ++x)
						{
							const bool stripe = (((x + y) % 10) == 0);
							const unsigned char v = stripe ? 32 : 16;
							row[0] = v;
							row[1] = v;
							row[2] = v;
							row[3] = 255;
							row += 4;
						}
					}

					if (reference_images.empty())
					{
						HFONT hRefHintFont = CreateFont(24, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
						HFONT hOldFont = (HFONT)SelectObject(memDC, hRefHintFont);
						SetTextColor(memDC, RGB(120, 120, 120));
						DrawText(memDC, L"You can drop additional reference images here", -1, &ref_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
						SelectObject(memDC, hOldFont);
						DeleteObject(hRefHintFont);
					}
					else
					{
						int current_x = 8;
						const int margin = 8;
						const int target_h = ref_h - (margin * 2);

						for (size_t i = 0; i < reference_images.size(); ++i)
						{
							ReferenceImage& reference_image = reference_images[i];
							int target_w = (int)((float)reference_image.w * ((float)target_h / (float)reference_image.h));
							if (target_w <= 0) target_w = target_h;

							reference_image.render_rect = { current_x, h2 + margin, current_x + target_w, h2 + margin + target_h };
							reference_image.close_rect = { reference_image.render_rect.right - 18, reference_image.render_rect.top + 2, reference_image.render_rect.right - 2, reference_image.render_rect.top + 18 };

							unsigned char* scaled_ref = stbir_resize_uint8_srgb(reference_image.rgba, reference_image.w, reference_image.h, 0, (unsigned char*)malloc(target_w * target_h * 4), target_w, target_h, 0, STBIR_RGBA);
							if (scaled_ref)
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
								bi.hdr.biWidth = target_w;
								bi.hdr.biHeight = -target_h;
								SetDIBitsToDevice(memDC, current_x, h2 + margin, target_w, target_h, 0, 0, 0, target_h, scaled_ref, (BITMAPINFO*)&bi, DIB_RGB_COLORS);
								free(scaled_ref);
							}

							HPEN hPen = CreatePen(PS_SOLID, 3, RGB(120, 120, 120));
							HPEN hOldPen = (HPEN)SelectObject(memDC, hPen);
							HBRUSH hOldBrush = (HBRUSH)SelectObject(memDC, GetStockObject(HOLLOW_BRUSH));
							Rectangle(memDC, reference_image.render_rect.left, reference_image.render_rect.top, reference_image.render_rect.right, reference_image.render_rect.bottom);
							SelectObject(memDC, hOldBrush);
							SelectObject(memDC, hOldPen);
							DeleteObject(hPen);

							HBRUSH hBadgeBrush = CreateSolidBrush(RGB(180, 40, 40));
							FillRect(memDC, &reference_image.close_rect, hBadgeBrush);
							DeleteObject(hBadgeBrush);

							SetTextColor(memDC, RGB(255, 255, 255));
							HFONT hXFont = CreateFont(14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
							HFONT hOldF = (HFONT)SelectObject(memDC, hXFont);
							DrawText(memDC, L"\u2715", -1, &reference_image.close_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
							SelectObject(memDC, hOldF);
							DeleteObject(hXFont);

							current_x += target_w + margin;
						}
					}
				}

				HBRUSH hSplitterBrush = CreateSolidBrush(RGB(62, 62, 62));
				RECT splitter_rect = { 0, h2 + ref_h, w2, h2 + ref_h + splitter_thickness };
				FillRect(memDC, &splitter_rect, hSplitterBrush);
				DeleteObject(hSplitterBrush);

				SelectObject(memDC, hOldBmp);
				DeleteDC(memDC);

				SetDIBitsToDevice(hdc, 0, 0, w2, paint_h, 0, 0, 0, paint_h, bits, &bmi, DIB_RGB_COLORS);
				DeleteObject(hBmp);
			}

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
					load_image();
					break;
				case IDC_SAVE_BUTTON:
				case ID_ACCEL_SAVE:
					save_image();
					break;
				case IDC_COPY_BUTTON:
					copy_image();
					break;
				case IDC_CLEAR_BUTTON:
					if (rgba) { free(rgba); rgba = nullptr; }
					if (rgba2) { free(rgba2); rgba2 = nullptr; }
					push_history(nullptr, w, h);
					redraw();
					break;
				case IDC_GENERATE_BUTTON:
				case ID_ACCEL_GENERATE:
					generation();
					break;
				case IDC_UNDO_BUTTON:
					undo();
					break;
				case IDC_REDO_BUTTON:
					redo();
					break;
				}
			}
			break;

		case WM_KEYDOWN:
			if (GetKeyState(VK_CONTROL) & 0x8000)
			{
				if (wParam == 'V')
				{
					if (GetFocus() != hEdit) paste_image();
				}
				else if (wParam == 'Z')
				{
					if (GetFocus() != hEdit) undo();
				}
				else if (wParam == 'Y')
				{
					if (GetFocus() != hEdit) redo();
				}
			}
			break;

		case WM_CONTEXTMENU:
		{
			if ((HWND)wParam == window)
			{
				HMENU hMenu = CreatePopupMenu();
				AppendMenu(hMenu, MF_STRING, 1099, L"New image");
				AppendMenu(hMenu, MF_STRING, 1100, L"Copy (Ctrl + C)");
				AppendMenu(hMenu, MF_STRING, 1101, L"Paste (Ctrl + V)");
				if (get_ref_container_height() > 0)
				{
					AppendMenu(hMenu, MF_STRING, 1102, L"Paste as reference image");
				}
				AppendMenu(hMenu, MF_STRING | (is_cpu ? MF_CHECKED : 0), 1103, L"Use CPU (slow)");
				AppendMenu(hMenu, MF_STRING, 1104, L"Open output folder");

				HMENU hBatchCountMenu = CreatePopupMenu();
				AppendMenu(hBatchCountMenu, MF_STRING | (batch_count == 1 ? MF_CHECKED : 0), 1111, L"1");
				AppendMenu(hBatchCountMenu, MF_STRING | (batch_count == 4 ? MF_CHECKED : 0), 1112, L"4");
				AppendMenu(hBatchCountMenu, MF_STRING | (batch_count == 8 ? MF_CHECKED : 0), 1113, L"8");
				AppendMenu(hBatchCountMenu, MF_STRING | (batch_count == 16 ? MF_CHECKED : 0), 1114, L"16");
				AppendMenu(hMenu, MF_POPUP | MF_STRING, (UINT_PTR)hBatchCountMenu, L"Batch  count...");

				AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);

				HMENU hImageModelMenu = CreatePopupMenu();
				AppendMenu(hImageModelMenu, MF_STRING | (image_model == IMAGE_MODEL::Z_IMAGE ? MF_CHECKED : 0), 1200, L"Z-Image");
				AppendMenu(hImageModelMenu, MF_STRING | (image_model == IMAGE_MODEL::FLUX2 ? MF_CHECKED : 0), 1201, L"Flux 2");
				AppendMenu(hImageModelMenu, MF_STRING | (image_model == IMAGE_MODEL::STABLE_DIFFUSION_3_5 ? MF_CHECKED : 0), 1202, L"Stable Diffusion 3.5");
				AppendMenu(hImageModelMenu, MF_STRING | (image_model == IMAGE_MODEL::QWEN_IMAGE ? MF_CHECKED : 0), 1203, L"Qwen image");
				AppendMenu(hImageModelMenu, MF_STRING | (image_model == IMAGE_MODEL::ERNIE_IMAGE ? MF_CHECKED : 0), 1204, L"Ernie image");
				AppendMenu(hMenu, MF_POPUP | MF_STRING, (UINT_PTR)hImageModelMenu, L"Image generation model...");

				HMENU hEditModelMenu = CreatePopupMenu();
				AppendMenu(hEditModelMenu, MF_STRING | (edit_model == EDIT_MODEL::FLUX2 ? MF_CHECKED : 0), 1500, L"Flux 2");
				AppendMenu(hEditModelMenu, MF_STRING | (edit_model == EDIT_MODEL::QWEN_IMAGE_EDIT ? MF_CHECKED : 0), 1501, L"Qwen image edit");
				AppendMenu(hMenu, MF_POPUP | MF_STRING, (UINT_PTR)hEditModelMenu, L"Image edit model...");

				HMENU hTextModelMenu = CreatePopupMenu();
				AppendMenu(hTextModelMenu, MF_STRING | (text_model == TEXT_MODEL::QWEN_3_VL ? MF_CHECKED : 0), 1300, L"Qwen 3 VL");
				AppendMenu(hTextModelMenu, MF_STRING | (text_model == TEXT_MODEL::GEMMA_4 ? MF_CHECKED : 0), 1301, L"Gemma 4");
				AppendMenu(hMenu, MF_POPUP | MF_STRING, (UINT_PTR)hTextModelMenu, L"Text generation model...");

				HMENU hVideoModelMenu = CreatePopupMenu();
				AppendMenu(hVideoModelMenu, MF_STRING | (video_model == VIDEO_MODEL::WAN_2_2 ? MF_CHECKED : 0), 1400, L"Wan 2.2");
				AppendMenu(hVideoModelMenu, MF_STRING | (video_model == VIDEO_MODEL::LTX_2_3 ? MF_CHECKED : 0), 1401, L"LTX 2.3");
				AppendMenu(hVideoModelMenu, MF_STRING | (video_model == VIDEO_MODEL::MINIMAX_H3 ? MF_CHECKED : 0), 1402, L"Minimax H3");
				AppendMenu(hMenu, MF_POPUP | MF_STRING, (UINT_PTR)hVideoModelMenu, L"Video generation model...");

				AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);

				for (int i = 0; i < ARRAYSIZE(scale_presets); ++i)
				{
					wchar_t restext[32] = {};
					_snwprintf(restext, ARRAYSIZE(restext), L"%d%%", scale_presets[i]);
					UINT flags = MF_STRING;
					const float scale = float(scale_presets[i]) / 100.0f;
					if (int(w * scale) == w2 && int(h * scale) == h2)
					{
						flags |= MF_CHECKED;
					}
					AppendMenu(hMenu, flags, 2000 + i, restext);
				}

				AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);

				for (int i = 0; i < ARRAYSIZE(resolution_presets); ++i)
				{
					wchar_t restext[32] = {};
					_snwprintf(restext, ARRAYSIZE(restext), L"%dx%d px", resolution_presets[i].w, resolution_presets[i].h);
					UINT flags = MF_STRING;
					if (w2 == resolution_presets[i].w && h2 == resolution_presets[i].h)
					{
						flags |= MF_CHECKED;
					}
					AppendMenu(hMenu, flags, 3000 + i, restext);
				}

				AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);

				HMENU hVideoMenu = CreatePopupMenu();
				for (int i = 0; i < ARRAYSIZE(video_presets); ++i)
				{
					wchar_t restext[32] = {};
					_snwprintf(restext, ARRAYSIZE(restext), L"%d fps, %d seconds", video_presets[i].fps, video_presets[i].seconds);
					UINT flags = MF_STRING;
					if (video_fps == video_presets[i].fps && video_seconds == video_presets[i].seconds)
					{
						flags |= MF_CHECKED;
					}
					AppendMenu(hVideoMenu, flags, 4000 + i, restext);
				}
				AppendMenu(hMenu, MF_POPUP | MF_STRING, (UINT_PTR)hVideoMenu, L"Video settings...");

				AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);

				AppendMenu(hMenu, MF_STRING, 8000, L"About...");

				POINT pt;
				GetCursorPos(&pt);
				int selection = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hWnd, NULL);

				if (selection == 1099) { // New image
					if (rgba) { free(rgba); rgba = nullptr; }
					if (rgba2) { free(rgba2); rgba2 = nullptr; }
					clear_ref_images();
					push_history(nullptr, w, h);
					redraw();
				}
				else if (selection == 1100) { // Copy
					copy_image();
				}
				else if (selection == 1101) { // Paste
					paste_image();
				}
				else if (selection == 1102) { // Paste as reference
					paste_image(true);
				}
				else if (selection == 1103) { // CPU
					is_cpu = !is_cpu;
				}
				else if (selection == 1104) { // output folder
					wchar_t output_path[MAX_PATH] = {};
					_snwprintf(output_path, MAX_PATH, L"%s/output/", originalWorkingDir);
					ShellExecute(NULL, L"open", output_path, NULL, NULL, SW_SHOWNORMAL);
				}
				else if (selection == 1111) {
					batch_count = 1;
				}
				else if (selection == 1112) {
					batch_count = 4;
				}
				else if (selection == 1113) {
					batch_count = 8;
				}
				else if (selection == 1114) {
					batch_count = 16;
				}
				else if (selection == 1200) {
					image_model = IMAGE_MODEL::Z_IMAGE;
				}
				else if (selection == 1201) {
					image_model = IMAGE_MODEL::FLUX2;
				}
				else if (selection == 1202) {
					image_model = IMAGE_MODEL::STABLE_DIFFUSION_3_5;
				}
				else if (selection == 1203) {
					image_model = IMAGE_MODEL::QWEN_IMAGE;
				}
				else if (selection == 1204) {
					image_model = IMAGE_MODEL::ERNIE_IMAGE;
				}
				else if (selection == 1300) {
					text_model = TEXT_MODEL::QWEN_3_VL;
				}
				else if (selection == 1301) {
					text_model = TEXT_MODEL::GEMMA_4;
				}
				else if (selection == 1400) {
					video_model = VIDEO_MODEL::WAN_2_2;
				}
				else if (selection == 1401) {
					video_model = VIDEO_MODEL::LTX_2_3;
				}
				else if (selection == 1402) {
					video_model = VIDEO_MODEL::MINIMAX_H3;
				}
				else if (selection == 1500) {
					edit_model = EDIT_MODEL::FLUX2;
				}
				else if (selection == 1501) {
					edit_model = EDIT_MODEL::QWEN_IMAGE_EDIT;
				}
				else if (selection == 8000) // about
				{
					MessageBox(hWnd, L"Created by: Turánszki János\nhttps://github.com/turanszkij/mini-ai\n\nOpen source libraries used:\n- llama\n- stable-diffusion.cpp\n- stb_image.h\n- stb_image_write.h\n- stb_image_resize2.h\n", L"Mini-AI", MB_OK | MB_ICONINFORMATION);
				}
				else if (selection >= 4000) // video preset selection
				{
					selection -= 4000;
					if (selection >= 0 && selection < ARRAYSIZE(video_presets))
					{
						video_fps = video_presets[selection].fps;
						video_seconds = video_presets[selection].seconds;
					}
				}
				else if (selection >= 3000) // resolution selection
				{
					resize_fixed = true;
					selection -= 3000;
					if (selection >= 0 && selection < ARRAYSIZE(resolution_presets))
					{
						w2 = resolution_presets[selection].w;
						h2 = resolution_presets[selection].h;
						redraw();
					}
					resize_fixed = false;
				}
				else if (selection >= 2000) // resolution scale selection
				{
					resize_fixed = true;
					selection -= 2000;
					if (selection >= 0 && selection < ARRAYSIZE(scale_presets))
					{
						const float scale = float(scale_presets[selection]) / 100.0f;
						w2 = int(w * scale);
						h2 = int(h * scale);
						redraw();
					}
					resize_fixed = false;
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
			POINT pt;
			DragQueryPoint(hdrop, &pt);

			UINT filecount = DragQueryFile(hdrop, 0xFFFFFFFF, nullptr, 0);
			assert(filecount != 0);

			const bool is_ref_area = (pt.y > h2) && (pt.y < h2 + get_ref_container_height());

			for (UINT i = 0; i < filecount; ++i)
			{
				wchar_t wfilename[1024] = {};
				UINT res = DragQueryFile(hdrop, i, wfilename, ARRAYSIZE(wfilename));
				if (res == 0)
				{
					assert(0);
					continue;
				}
				char filename[MAX_PATH] = {};
				WideCharToMultiByte(CP_UTF8, 0, wfilename, -1, filename, MAX_PATH, nullptr, nullptr);

				if (is_ref_area)
				{
					int rw = 0, rh = 0, rc = 0;
					unsigned char* rrgba = stbi_load(filename, &rw, &rh, &rc, 4);
					if (rrgba)
					{
						ReferenceImage reference_image;
						reference_image.rgba = rrgba;
						reference_image.w = rw;
						reference_image.h = rh;
						reference_images.push_back(reference_image);
					}
				}
				else
				{
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
					if (rgba) push_history(rgba, w, h);
					resize_window_to_image();
				}
			}

			SetForegroundWindow(hWnd);
			redraw();
		}
		break;

		case WM_NOTIFY:
		{
			NMHDR* pNmhdr = (NMHDR*)lParam;
			if (pNmhdr->code == BCN_DROPDOWN && pNmhdr->idFrom == IDC_GENERATE_BUTTON)
			{
				HMENU hMenu = CreatePopupMenu();
				AppendMenu(hMenu, MF_STRING | (mode == MODE::IMAGE_GENERATE ? MF_CHECKED : 0), 101, L"Generate image");
				AppendMenu(hMenu, MF_STRING | (mode == MODE::IMAGE_EDIT ? MF_CHECKED : 0), 102, L"Edit image");
				AppendMenu(hMenu, MF_STRING | (mode == MODE::ASK ? MF_CHECKED : 0), 103, L"Ask anything");
				AppendMenu(hMenu, MF_STRING | (mode == MODE::VIDEO ? MF_CHECKED : 0), 104, L"Generate video");

				RECT rc;
				GetWindowRect(hBtnGenerate, &rc);
				int selection = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, rc.left, rc.bottom, 0, hWnd, NULL);

				switch (selection)
				{
				default:
				case 101:
					mode = MODE::IMAGE_GENERATE;
					break;
				case 102:
					mode = MODE::IMAGE_EDIT;
					break;
				case 103:
					mode = MODE::ASK;
					break;
				case 104:
					mode = MODE::VIDEO;
					break;
				}

				DestroyMenu(hMenu);

				redraw();
				return 0;
			}
		}
		break;

		case WM_LBUTTONDOWN:
		{
			int mouse_x = LOWORD(lParam);
			int mouse_y = HIWORD(lParam);

			// Check clicks inside reference image close rect to delete items
			if (mouse_y >= h2 && mouse_y < h2 + get_ref_container_height())
			{
				for (size_t i = 0; i < reference_images.size(); ++i)
				{
					if (PtInRect(&reference_images[i].close_rect, POINT{ mouse_x, mouse_y }))
					{
						if (reference_images[i].rgba) free(reference_images[i].rgba);
						reference_images.erase(reference_images.begin() + i);
						redraw();
						break;
					}
				}
			}

			if (mouse_y >= h2 + get_ref_container_height() && mouse_y <= h2 + get_ref_container_height() + splitter_thickness)
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
				redraw();
			}
			break;

		case WM_MOUSEMOVE:
		{
			int mouse_y = HIWORD(lParam);

			if (is_dragging)
			{
				RECT rc;
				GetClientRect(hWnd, &rc);
				int proposed_height = rc.bottom - button_height - mouse_y - (splitter_thickness / 2);
				if (proposed_height > 40 && proposed_height < (rc.bottom - 100 - button_height - get_ref_container_height()))
				{
					text_height = proposed_height;
					w2 = rc.right;
					h2 = rc.bottom - text_height - button_height - splitter_thickness - get_ref_container_height();
					redraw();
				}
			}
		}
		break;

		case WM_SETCURSOR:
		{
			POINT pt;
			GetCursorPos(&pt);
			ScreenToClient(hWnd, &pt);
			if (is_dragging || (pt.y >= h2 + get_ref_container_height() && pt.y <= h2 + get_ref_container_height() + splitter_thickness))
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
	wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wcex.hbrBackground = NULL;
	wcex.lpszMenuName = NULL;
	wcex.lpszClassName = L"mini-ai";
	wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APPICON));
	wcex.hIconSm = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APPICON));
	RegisterClassEx(&wcex);

	RECT wr = { 0, 0, w, h + get_ref_container_height() + splitter_thickness + text_height + button_height };
	DWORD window_style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
	AdjustWindowRect(&wr, window_style, FALSE);
	int window_width = wr.right - wr.left;
	int window_height = wr.bottom - wr.top;

	window = CreateWindow(L"mini-ai", L"mini-ai", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, 0, window_width, window_height, nullptr, nullptr, NULL, nullptr);
	hEdit = CreateWindow(L"EDIT", NULL, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL, 0, 0, 0, 0, window, NULL, hInstance, NULL);

	hBtnLoad = CreateWindow(L"BUTTON", L"\xE8B7", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, window, (HMENU)IDC_LOAD_BUTTON, hInstance, NULL);
	hBtnSave = CreateWindow(L"BUTTON", L"\xE74E", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, window, (HMENU)IDC_SAVE_BUTTON, hInstance, NULL);
	hBtnCopy = CreateWindow(L"BUTTON", L"\xE8C8", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, window, (HMENU)IDC_COPY_BUTTON, hInstance, NULL);
	hBtnClear = CreateWindow(L"BUTTON", L"\xE74D", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, window, (HMENU)IDC_CLEAR_BUTTON, hInstance, NULL);
	hBtnGenerate = CreateWindow(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_SPLITBUTTON, 0, 0, 0, 0, window, (HMENU)IDC_GENERATE_BUTTON, hInstance, NULL);
	hBtnUndo = CreateWindow(L"BUTTON", L"\xE7A7", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, window, (HMENU)IDC_UNDO_BUTTON, hInstance, NULL);
	hBtnRedo = CreateWindow(L"BUTTON", L"\xE7A6", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, window, (HMENU)IDC_REDO_BUTTON, hInstance, NULL);

	HFONT hFont = CreateFont(34, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Arial");
	SendMessage(hEdit, WM_SETFONT, (WPARAM)hFont, TRUE);

	HFONT hIconFont = CreateFont(24, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe MDL2 Assets");
	SendMessage(hBtnLoad, WM_SETFONT, (WPARAM)hIconFont, TRUE);
	SendMessage(hBtnSave, WM_SETFONT, (WPARAM)hIconFont, TRUE);
	SendMessage(hBtnCopy, WM_SETFONT, (WPARAM)hIconFont, TRUE);
	SendMessage(hBtnClear, WM_SETFONT, (WPARAM)hIconFont, TRUE);
	SendMessage(hBtnUndo, WM_SETFONT, (WPARAM)hIconFont, TRUE);
	SendMessage(hBtnRedo, WM_SETFONT, (WPARAM)hIconFont, TRUE);
	HFONT hGenFont = CreateFont(32, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Symbol");
	SendMessage(hBtnGenerate, WM_SETFONT, (WPARAM)hGenFont, TRUE);

	auto AddToolTip = [](HWND hwndParent, HWND hwndTarget, const wchar_t* text) {
		static HWND hwndTT = NULL;
		if (hwndTT == NULL)
		{
			hwndTT = CreateWindowEx(
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

		SendMessage(hwndTT, TTM_ADDTOOLW, 0, (LPARAM)&ti);
	};
	AddToolTip(window, hBtnLoad, L"Load Image (Ctrl+O)");
	AddToolTip(window, hBtnSave, L"Save Image (Ctrl+S)");
	AddToolTip(window, hBtnCopy, L"Copy Image to Clipboard (Ctrl+C)");
	AddToolTip(window, hBtnClear, L"Clear image and start over");
	AddToolTip(window, hBtnUndo, L"Previous image");
	AddToolTip(window, hBtnRedo, L"Next image");
	AddToolTip(window, hBtnGenerate, L"Generate Image from Prompt (Ctrl+Enter). If there is already an image, it will be used as input to generation");

	update_undo_redo_states();

	ShowWindow(window, SW_SHOWDEFAULT);
	DragAcceptFiles(window, TRUE);

	BOOL darkmode = TRUE;
	DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkmode, sizeof(darkmode));
	SetWindowTheme(hBtnLoad, L"DarkMode_Explorer", NULL);
	SetWindowTheme(hBtnSave, L"DarkMode_Explorer", NULL);
	SetWindowTheme(hBtnCopy, L"DarkMode_Explorer", NULL);
	SetWindowTheme(hBtnClear, L"DarkMode_Explorer", NULL);
	SetWindowTheme(hBtnUndo, L"DarkMode_Explorer", NULL);
	SetWindowTheme(hBtnRedo, L"DarkMode_Explorer", NULL);
	SetWindowTheme(hBtnGenerate, L"DarkMode_Explorer", NULL);
	SetWindowTheme(hEdit, L"DarkMode_Explorer", NULL);

	// keyboard shortcuts
	ACCEL accels[] = {
		{ FCONTROL | FVIRTKEY, 'O', ID_ACCEL_LOAD },
		{ FCONTROL | FVIRTKEY, 'S', ID_ACCEL_SAVE },
		{ FCONTROL | FVIRTKEY, VK_RETURN, ID_ACCEL_GENERATE }
	};
	HACCEL hAccel = CreateAcceleratorTable(accels, ARRAYSIZE(accels));

	// Load prompt from prompt.txt if available:
	_snwprintf(promptPath, MAX_PATH, L"%s/prompt.txt", originalWorkingDir);
	std::string prompt;
	std::ifstream file(promptPath, std::ios::binary | std::ios::ate);
	if (file.is_open())
	{
		size_t dataSize = (size_t)file.tellg();
		file.seekg((std::streampos)0);
		prompt.resize(dataSize + 1, 0);
		file.read((char*)prompt.data(), dataSize);
		file.close();

		int cnt = MultiByteToWideChar(CP_UTF8, 0, prompt.c_str(), -1, nullptr, 0);
		std::wstring wstr(cnt, 0);
		MultiByteToWideChar(CP_UTF8, 0, prompt.c_str(), -1, wstr.data(), cnt);
		SetWindowText(hEdit, wstr.c_str());
	}
	else
	{
		SetWindowText(hEdit, L"A beautiful mountain landscape...");
	}

	while (!exiting)
	{
		MSG msg = {};
		if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
			if (!TranslateAccelerator(window, hAccel, &msg))
			{
				TranslateMessage(&msg);
				DispatchMessage(&msg);
			}
			continue;
		}
	}

	DestroyAcceleratorTable(hAccel);

	EnterCriticalSection(&image_cs);
	if (rgba)  free(rgba);
	if (rgba2) free(rgba2);
	rgba = nullptr;
	rgba2 = nullptr;
	LeaveCriticalSection(&image_cs);
	DeleteCriticalSection(&image_cs);

	return 0;
}
