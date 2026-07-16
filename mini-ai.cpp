#include <Windows.h>
#include <thread>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_WINDOWS_UTF8
#include "stb_image.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"

#include "stable-diffusion/stable-diffusion.h"

#define LINK_DLL_FUNCTION(name, dll) using PFN_##name = decltype(&name); PFN_##name name = (PFN_##name)GetProcAddress(dll, #name)

static int w = 512, h = 512, c = 3;
static unsigned char* rgba = nullptr;
static unsigned char* rgba2 = nullptr;
static int w2, h2;
static int text_height = 40;
static int progress = 0; 
static HWND window =  nullptr;

void set_title()
{
	char text[1024] = {};
	snprintf(text, sizeof(text), "mini-ai %d*%dpx (%d%%)", w2, h2, progress);
	SetWindowTextA(window, text);
}
void resize()
{
	if (rgba2)
	{
		free(rgba2);
		rgba2 = nullptr;
	}
	rgba2 = stbir_resize_uint8_srgb(rgba, w, h, 0, (unsigned char*)malloc(w2 * h2 * 4), w2, h2, 0, STBIR_RGBA);
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

LRESULT CALLBACK EditSubclassProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	WNDPROC originalEditProc = (WNDPROC)GetWindowLongPtr(hWnd, GWLP_USERDATA);
	switch (message) {
	case WM_KEYDOWN:
		if (wParam == VK_RETURN) {
			std::thread([=] {
				wchar_t wtext[1024] = {};
				GetWindowText(hWnd, wtext, ARRAYSIZE(wtext));
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
				sd_params.diffusion_model_path = "stable-diffusion/models/z_image_turbo-Q4_K.gguf";
				sd_params.vae_path = "stable-diffusion/models/ae.safetensors";
				sd_params.llm_path = "stable-diffusion/models/Qwen3-4B-Instruct-2507-Q4_K_M.gguf";
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
					sd_img_gen_params_init(&img_params);
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
		break; 
	case WM_CHAR:
		if (wParam == '\r') {
			return 0;
		}
		break;
	}
	return CallWindowProc(originalEditProc, hWnd, message, wParam, lParam);
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine, _In_ int nCmdShow)
{
	if (lpCmdLine && lpCmdLine[0])
	{
		char filename[4096] = {};
		stbi_convert_wchar_to_utf8(filename, sizeof(filename), lpCmdLine);
		rgba = stbi_load(filename, &w, &h, &c, 4);
	}

	static HWND hEdit = NULL;

	static bool exiting = false;
	static auto WndProc = [](HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) -> LRESULT
	{
		switch (message)
		{
		case WM_SIZE:
		{
			w2 = LOWORD(lParam);
			h2 = HIWORD(lParam) - text_height;
			set_title();
			if (rgba)
			{
				resize();
			}
			if (hEdit)
			{
				RECT rc;
				GetClientRect(hWnd, &rc);
				MoveWindow(hEdit, 0, rc.bottom - text_height, rc.right, text_height, TRUE);
			}
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
			bi.hdr.biHeight = -h2;
			SetDIBitsToDevice(hdc, 0, 0, w2, h2, 0, 0, 0, h2, rgba2, (BITMAPINFO*)&bi, DIB_RGB_COLORS);
			EndPaint(hWnd, &ps);
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
			RECT rc = { 0, 0, w, h };
			AdjustWindowRect(&rc, (DWORD)GetWindowLongPtr(hWnd, GWL_STYLE), GetMenu(hWnd) != NULL);
			SetWindowPos(hWnd, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED);
			SetForegroundWindow(hWnd);
		}
		break;
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
	wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wcex.lpszMenuName = NULL;
	wcex.lpszClassName = L"mini-ai";
	wcex.hIconSm = NULL;
	RegisterClassExW(&wcex);

	RECT wr = { 0, 0, w, h + text_height };
	DWORD window_style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
	AdjustWindowRect(&wr, window_style, FALSE);
	int window_width = wr.right - wr.left;
	int window_height = wr.bottom - wr.top;

	window = CreateWindowW(L"mini-ai", L"mini-ai", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, 0, window_width, window_height, nullptr, nullptr, NULL, nullptr);

	hEdit = CreateWindow(L"EDIT", NULL, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_LEFT | ES_AUTOHSCROLL, 0, 0, 0, 0, window, NULL, hInstance, NULL);
	SetWindowLongPtr(hEdit, GWLP_USERDATA, (LONG_PTR)GetWindowLongPtr(hEdit, GWLP_WNDPROC));
	SetWindowLongPtr(hEdit, GWLP_WNDPROC, (LONG_PTR)EditSubclassProc);
	
	HFONT hFont = CreateFont(34, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Arial");
	SendMessage(hEdit, WM_SETFONT, (WPARAM)hFont, TRUE);

	ShowWindow(window, SW_SHOWDEFAULT);
	DragAcceptFiles(window, TRUE);

	while (!exiting)
	{
		MSG msg = { 0 };
		if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessage(&msg);
			continue;
		}
	}

	if (rgba)
	{
		free(rgba);
	}
	if (rgba2)
	{
		free(rgba2);
	}

	return 0;
}
