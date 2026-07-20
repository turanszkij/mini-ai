#include <Windows.h>
#include <commdlg.h> // Common Dialogs for Load/Save

#include <dwmapi.h> // DwmSetWindowAttribute
#pragma comment(lib, "dwmapi.lib")

#include <uxtheme.h>
#pragma comment(lib, "uxtheme.lib")
#pragma comment(linker,"\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")

#include <wininet.h>
#pragma comment(lib, "wininet.lib")

#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")

#include <thread>
#include <string>
#include <fstream>
#include <atomic>
#include <filesystem>
#include <mutex>

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

// Unique IDs for our buttons
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
#define ID_ACCEL_COPY     203
#define ID_ACCEL_GENERATE 204

#define IDI_APPICON 101

enum MODE
{
	MODE_IMAGE_GENERATE,
	MODE_IMAGE_EDIT,
	MODE_IMAGE_DESCRIBE,
	MODE_IMAGE_STORY,
} static mode = MODE_IMAGE_GENERATE;
static wchar_t originalWorkingDir[MAX_PATH];
static int w = 512, h = 512, c = 3;
static unsigned char* rgba = nullptr;
static unsigned char* rgba2 = nullptr;
static int w2, h2;
static int text_height = 100;
static const int button_height = 45; // Height of the bottom row
static bool is_dragging = false; // Tracks if we are currently resizing the textbox
const int splitter_thickness = 6; // Hit-test thickness for dragging
static int progress = 0;
static std::atomic_bool is_generating{ false };
static std::wstring current_download;
static std::string current_errors;
static HWND window = nullptr;
static HWND hEdit = nullptr;
static HWND hBtnLoad = nullptr;
static HWND hBtnSave = nullptr;
static HWND hBtnCopy = nullptr;
static HWND hBtnClear = nullptr;
static HWND hBtnGenerate = nullptr;
static HWND hBtnUndo = nullptr;
static HWND hBtnRedo = nullptr;

static float image_to_image_strength = 0.32f;
static float image_to_image_txt_cfg = 2.0f;
static int image_to_image_steps = 20;

struct Res { int w, h; const wchar_t* name; };
Res presets[] = { {512, 512, L"512x512"}, {1024, 1024, L"1024x1024"}, {640, 480, L"640x480"}, {800, 600, L"800x600"}, {1280, 720, L"1280x720"}, {1920, 1080, L"1920x1080"}, {480, 640, L"480x640"}, {960, 1280, L"960x1280"} };


void SavePrompt(HWND hEdit) {
	int length = GetWindowTextLengthW(hEdit);
	std::wstring buffer(length, L'\0');
	GetWindowTextW(hEdit, &buffer[0], length + 1);
	wchar_t path[MAX_PATH] = {};
	_snwprintf(path, MAX_PATH, L"%s/prompt.txt", originalWorkingDir);
	std::wofstream file(path);
	if (file.is_open()) 
	{
		file << buffer;
		file.close();
	}
}
void LoadPrompt(HWND hEdit) {
	wchar_t path[MAX_PATH] = {};
	_snwprintf(path, MAX_PATH, L"%s/prompt.txt", originalWorkingDir);
	std::wifstream file(path);
	if (file.is_open()) {
		std::wstring line;
		std::getline(file, line);
		SetWindowTextW(hEdit, line.c_str());
		file.close();
	}
	else {
		// default if file doesn't exist
		SetWindowTextW(hEdit, L"A beautiful mountain landscape...");
	}
}

void set_title()
{
	wchar_t text[1024] = {};
	if (progress > 0)
	{
		_snwprintf(text, sizeof(text), L"mini-ai %dx%dpx (%d%%)", w2, h2, progress);
	}
	else
	{
		_snwprintf(text, sizeof(text), L"mini-ai %dx%dpx", w2, h2);
	}
	SetWindowText(window, text);
}

void EnsureModelExists(const wchar_t* url, const wchar_t* fileName)
{
	if (std::filesystem::exists(fileName))
		return;

	std::wstring tempFileName = std::wstring(fileName) + L".tmp";

	current_download = fileName;

	size_t found;
	found = current_download.find_last_of(L"/\\");
	current_download = current_download.substr(found + 1);

	InvalidateRect(window, NULL, TRUE);

	HINTERNET hInternet = InternetOpen(L"MiniAI", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
	HINTERNET hUrl = InternetOpenUrl(hInternet, url, NULL, 0, INTERNET_FLAG_RELOAD | INTERNET_FLAG_SECURE, 0);

	bool success = false;
	if (hUrl)
	{
		DWORD dwSize = 0;
		DWORD dwHeaderSize = sizeof(dwSize);
		HttpQueryInfo(hUrl, HTTP_QUERY_CONTENT_LENGTH | HTTP_QUERY_FLAG_NUMBER, &dwSize, &dwHeaderSize, NULL);

		std::ofstream outFile(tempFileName, std::ios::binary);
		std::vector<char> buffer(1024 * 1024);
		DWORD bytesRead = 0;
		DWORD totalRead = 0;
		int last_progress = -1;

		while (InternetReadFile(hUrl, buffer.data(), (DWORD)buffer.size(), &bytesRead) && bytesRead > 0)
		{
			outFile.write(buffer.data(), bytesRead);
			totalRead += bytesRead;

			if (dwSize > 0)
			{
				int current_progress = (int)((double)totalRead / (double)dwSize * 100);
				if (current_progress != last_progress)
				{
					progress = current_progress;
					last_progress = current_progress;
					InvalidateRect(window, NULL, FALSE);
				}
			}
		}
		outFile.close();
		InternetCloseHandle(hUrl);

		success = (bytesRead == 0);
	}
	else
	{
		MessageBox(window, L"Could not connect to URL", L"Download Error", MB_OK | MB_ICONERROR);
	}

	InternetCloseHandle(hInternet);

	if (success) {
		std::filesystem::rename(tempFileName, fileName);
	}
	else {
		std::filesystem::remove(tempFileName); // clean up partial file
	}

	current_download.clear();
	progress = 0;
	InvalidateRect(window, NULL, TRUE);
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


struct HistoryEntry {
	unsigned char* data;
	int size;
	int w, h;
};
static std::vector<HistoryEntry> history;
static int history_index = -1;
static std::mutex history_mutex;
void update_undo_redo_states()
{
	EnableWindow(hBtnUndo, history_index > 0);
	EnableWindow(hBtnRedo, history_index < (int)history.size() - 1);
}
void push_history(unsigned char* raw_rgba, int width, int height, bool save_output = false)
{
	int out_size = 0;
	// Compress to PNG in memory
	unsigned char* png_data = nullptr;
	
	if (raw_rgba != nullptr)
	{
		png_data = stbi_write_png_to_mem(raw_rgba, width * 4, width, height, 4, &out_size);
	}

	std::scoped_lock lock(history_mutex);

	// If we undo'd and then generate/load a new image, clear the "redo" future
	while (history.size() > (size_t)(history_index + 1))
	{
		if (history.back().data != nullptr)
			free(history.back().data);
		history.pop_back();
	}

	// Cap history size (e.g., 10 images)
	if (history.size() >= 10)
	{
		if (history[0].data != nullptr)
			free(history[0].data);
		history.erase(history.begin());
		history_index--;
	}

	// Last empty history is completely replaced
	if (history.size() > 0 && history.back().data == nullptr)
	{
		history.pop_back();
		history_index--;
	}

	history.push_back({ png_data, out_size, width, height });
	history_index++;

	if (save_output && png_data != nullptr)
	{
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
	std::lock_guard<std::mutex> lock(history_mutex);
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
	}

	redraw();
}
void handle_undo()
{
	if (history_index > 0) {
		history_index--;
		load_history_entry();
	}
	update_undo_redo_states();
}
void handle_redo()
{
	if (history_index < (int)history.size() - 1) {
		history_index++;
		load_history_entry();
	}
	update_undo_redo_states();
}

void sd_log(enum sd_log_level_t level, const char* text, void* data)
{
	if (level == SD_LOG_DEBUG)
		return;
	if (level == SD_LOG_ERROR)
	{
		current_errors += text;
		InvalidateRect(window, NULL, TRUE);
	}
	OutputDebugStringA(text);
}

void sd_callback(int step, int steps, float time, void* data)
{
	progress = int(float(step) / float(steps) * 100);
	set_title();
	InvalidateRect(window, NULL, FALSE);
	UpdateWindow(window); // Force immediate refresh
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

void llama_callback(enum ggml_log_level level, const char* text, void* user_data) 
{
	if (level >= GGML_LOG_LEVEL_ERROR)
	{
		current_errors += text;
	}
	OutputDebugStringA(text);
}

bool my_llama_progress_callback(float in_progress, void* user_data)
{
	progress = int(in_progress * 100);
	set_title();
	InvalidateRect(window, NULL, FALSE);
	UpdateWindow(window); // Force immediate refresh
	return true;
}

void post_description(std::string result_text)
{
	// text can contain leading spaces for some reason:
	while (!result_text.empty() && result_text.front() == ' ')
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
}

void trigger_generation()
{
	if (is_generating.load())
		return;
	current_errors.clear();

	std::thread([] {
		if (mode == MODE_IMAGE_DESCRIBE || mode == MODE_IMAGE_STORY)
		{
			if (rgba == nullptr)
			{
				current_errors = "There is no image to describe!";
				redraw();
				return;
			}

			// Use llama library for text generation:

			std::string prompt;
			static const char prompt_describe[] =
				"Describe the image in a natural, descriptive style. "
				"Do not repeat the description. "
				"Do not use markdown, bullet points, headers, or code blocks.";
			static const char prompt_story[] =
				"Create a long-format story based on this image in a natural, descriptive style. "
				"Focus on events leading up to the moment, describe thoughts and interactions of the characters involved. "
				"Do not repeat the description. "
				"Do not use markdown, bullet points, headers, or code blocks.";
			if (mode == MODE_IMAGE_DESCRIBE)
			{
				prompt = prompt_describe;
			}
			else
			{
				prompt = prompt_story;
			}

			wchar_t model_path[MAX_PATH] = {};
			wchar_t mmproj_path[MAX_PATH] = {};
			_snwprintf(model_path, MAX_PATH, L"%s%s", originalWorkingDir, L"/models/Qwen3.5-2B-Q5_K_M.gguf");
			_snwprintf(mmproj_path, MAX_PATH, L"%s%s", originalWorkingDir, L"/models/mmproj-F16.gguf");

			char u8_model_path[MAX_PATH] = {};
			char u8_mmproj_path[MAX_PATH] = {};

			stbi_convert_wchar_to_utf8(u8_model_path, MAX_PATH, model_path);
			stbi_convert_wchar_to_utf8(u8_mmproj_path, MAX_PATH, mmproj_path);

			wchar_t models_path[MAX_PATH] = {};
			_snwprintf(models_path, MAX_PATH, L"%s%s", originalWorkingDir, L"/models");
			CreateDirectory(models_path, 0);
			EnsureModelExists(L"https://huggingface.co/unsloth/Qwen3.5-2B-GGUF/resolve/main/Qwen3.5-2B-Q5_K_M.gguf?download=true", model_path);
			EnsureModelExists(L"https://huggingface.co/unsloth/Qwen3.5-2B-GGUF/resolve/main/mmproj-F16.gguf?download=true", mmproj_path);

			_wchdir(L"lib/llama");
			HMODULE llama = LoadLibrary(L"llama.dll");
			if (llama == nullptr)
			{
				MessageBoxA(window, "llama.dll couldn't be loaded!", "Error!", 0);
				return;
			}
			HMODULE ggml = LoadLibrary(L"ggml.dll");
			if (ggml == nullptr)
			{
				FreeLibrary(llama);
				MessageBoxA(window, "ggml.dll couldn't be loaded!", "Error!", 0);
				return;
			}
			HMODULE mtmd = LoadLibrary(L"mtmd.dll");
			if (mtmd == nullptr)
			{
				FreeLibrary(llama);
				FreeLibrary(ggml);
				MessageBoxA(window, "mtmd.dll couldn't be loaded!", "Error!", 0);
				return;
			}

			LINK_DLL_FUNCTION(llama_model_default_params, llama);
			LINK_DLL_FUNCTION(llama_context_default_params, llama);
			LINK_DLL_FUNCTION(llama_model_load_from_file, llama);
			LINK_DLL_FUNCTION(llama_init_from_model, llama);
			LINK_DLL_FUNCTION(llama_sampler_chain_init, llama);
			LINK_DLL_FUNCTION(llama_sampler_chain_add, llama);
			LINK_DLL_FUNCTION(llama_sampler_init_penalties, llama);
			LINK_DLL_FUNCTION(llama_sampler_init_greedy, llama);
			LINK_DLL_FUNCTION(llama_sampler_init_mirostat_v2, llama);
			LINK_DLL_FUNCTION(llama_sampler_init_temp, llama);
			LINK_DLL_FUNCTION(llama_sampler_init_min_p, llama);
			LINK_DLL_FUNCTION(llama_sampler_init_dist, llama);
			LINK_DLL_FUNCTION(llama_model_get_vocab, llama);
			LINK_DLL_FUNCTION(llama_sampler_sample, llama);
			LINK_DLL_FUNCTION(llama_vocab_is_eog, llama);
			LINK_DLL_FUNCTION(llama_token_to_piece, llama);
			LINK_DLL_FUNCTION(llama_batch_get_one, llama);
			LINK_DLL_FUNCTION(llama_decode, llama);
			LINK_DLL_FUNCTION(llama_sampler_free, llama);
			LINK_DLL_FUNCTION(llama_model_free, llama);
			LINK_DLL_FUNCTION(llama_free, llama);
			LINK_DLL_FUNCTION(llama_log_set, llama);

			LINK_DLL_FUNCTION(ggml_backend_load_all, ggml);

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

			ggml_backend_load_all();

			llama_log_set(llama_callback, nullptr);
			mtmd_log_set(llama_callback, nullptr);

			llama_model_params model_params = llama_model_default_params();
			model_params.n_gpu_layers = -1;
			model_params.progress_callback = my_llama_progress_callback;

			llama_context_params ctx_params = llama_context_default_params();
			ctx_params.n_ctx = 4096;
			ctx_params.n_batch = 512;

			llama_model* model = llama_model_load_from_file(u8_model_path, model_params);
			if (model == nullptr)
				return;

			llama_context* ctx = llama_init_from_model(model, ctx_params);
			if (ctx == nullptr)
				return;

			mtmd_context_params mtmd_params = mtmd_context_params_default();
			mtmd_params.n_threads = 4;
			mtmd_params.use_gpu = true;
			mtmd_params.progress_callback = my_llama_progress_callback;

			mtmd_context* ctx_mtmd = mtmd_init_from_file(u8_mmproj_path, model, mtmd_params);
			if(ctx_mtmd == nullptr)
				return;

			// Convert RGBA -> RGB (mtmd expects RGB24)
			std::vector<uint8_t> rgb_data(w * h * 3);
			for (int i = 0; i < w * h; ++i) {
				rgb_data[i * 3 + 0] = rgba[i * 4 + 0];  // R
				rgb_data[i * 3 + 1] = rgba[i * 4 + 1];  // G
				rgb_data[i * 3 + 2] = rgba[i * 4 + 2];  // B
			}
			mtmd_bitmap* bitmap = mtmd_bitmap_init(w, h, rgb_data.data());

			const char* image_marker = mtmd_default_marker();

			std::string full_text = std::string(image_marker) + prompt;

			mtmd_input_text input_text = {};
			input_text.text = full_text.c_str();
			input_text.text_len = full_text.length();
			input_text.add_special = true;
			input_text.parse_special = true;

			mtmd_input_chunks* chunks = mtmd_input_chunks_init();

			const mtmd_bitmap* bitmaps[1] = { bitmap };

			if (mtmd_tokenize(ctx_mtmd, chunks, &input_text, bitmaps, 1) != 0)
				return;

			llama_pos n_past = 0;
			if (mtmd_helper_eval_chunks(ctx_mtmd, ctx, chunks, n_past, 0, 512, true, &n_past) != 0)
				return;

			llama_sampler_chain_params chain_params = {};
			llama_sampler* smpl = llama_sampler_chain_init(chain_params);
			llama_sampler_chain_add(smpl, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));
			llama_sampler_chain_add(smpl, llama_sampler_init_penalties(128, 1.1f, 0.0f, 0.0f));
			//llama_sampler_chain_add(smpl, llama_sampler_init_mirostat_v2(0, 0.1f, 5.0f));
			//llama_sampler_chain_add(smpl, llama_sampler_init_temp(0.78f));
			//llama_sampler_chain_add(smpl, llama_sampler_init_min_p(0.05f, 1));
			llama_sampler_chain_add(smpl, llama_sampler_init_greedy());

			llama_token new_token_id;
			std::string result_text = "";
			const struct llama_vocab* vocab = llama_model_get_vocab(model);
			bool started_generating = false;
			while (n_past < 2048) 
			{
				new_token_id = llama_sampler_sample(smpl, ctx, -1);

				if (started_generating && llama_vocab_is_eog(vocab, new_token_id))
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

			mtmd_input_chunks_free(chunks);
			mtmd_bitmap_free(bitmap);
			mtmd_free(ctx_mtmd);
			llama_sampler_free(smpl);
			llama_free(ctx);
			llama_model_free(model);

			FreeLibrary(llama);
			FreeLibrary(ggml);
			FreeLibrary(mtmd);

			post_description(result_text);
		}
		else
		{
			// Use stable diffusion library for image generation:

			int length = GetWindowTextLengthW(hEdit);
			std::wstring buffer(length, L'\0');
			GetWindowTextW(hEdit, &buffer[0], length + 1);
			int utf8len = stbi_convert_wchar_to_utf8(nullptr, 0, buffer.c_str());
			std::string text(utf8len, '\0');
			stbi_convert_wchar_to_utf8(text.data(), text.length(), buffer.c_str());
			SavePrompt(hEdit);

			_wchdir(L"lib/stable-diffusion");
			HMODULE stable_diffusion = LoadLibrary(L"stable-diffusion.dll");
			if (stable_diffusion == nullptr)
			{
				MessageBoxA(window, "stable_diffusion.dll couldn't be loaded!", "Error!", 0);
				return;
			}

			LINK_DLL_FUNCTION(sd_ctx_params_init, stable_diffusion);
			LINK_DLL_FUNCTION(new_sd_ctx, stable_diffusion);
			LINK_DLL_FUNCTION(sd_img_gen_params_init, stable_diffusion);
			LINK_DLL_FUNCTION(generate_image, stable_diffusion);
			LINK_DLL_FUNCTION(sd_sample_params_init, stable_diffusion);
			LINK_DLL_FUNCTION(free_sd_ctx, stable_diffusion);
			LINK_DLL_FUNCTION(sd_set_log_callback, stable_diffusion);
			LINK_DLL_FUNCTION(sd_set_progress_callback, stable_diffusion);
			LINK_DLL_FUNCTION(sd_set_preview_callback, stable_diffusion);

			wchar_t vae_path[MAX_PATH] = {};
			wchar_t text_encoder_path[MAX_PATH] = {};
			wchar_t diffusion_model_path[MAX_PATH] = {};

			_snwprintf(vae_path, MAX_PATH, L"%s%s", originalWorkingDir, L"/models/ae.safetensors");
			_snwprintf(text_encoder_path, MAX_PATH, L"%s%s", originalWorkingDir, L"/models/Qwen3-4B-Instruct-2507-Q4_K_M.gguf");
			_snwprintf(diffusion_model_path, MAX_PATH, L"%s%s", originalWorkingDir, L"/models/z_image_turbo-Q4_K.gguf");

			char u8_vae_path[MAX_PATH] = {};
			char u8_text_encoder_path[MAX_PATH] = {};
			char u8_diffusion_model_path[MAX_PATH] = {};

			stbi_convert_wchar_to_utf8(u8_vae_path, MAX_PATH, vae_path);
			stbi_convert_wchar_to_utf8(u8_text_encoder_path, MAX_PATH, text_encoder_path);
			stbi_convert_wchar_to_utf8(u8_diffusion_model_path, MAX_PATH, diffusion_model_path);

			wchar_t models_path[MAX_PATH] = {};
			_snwprintf(models_path, MAX_PATH, L"%s%s", originalWorkingDir, L"/models");
			CreateDirectory(models_path, 0);
			EnsureModelExists(L"https://huggingface.co/Comfy-Org/z_image_turbo/resolve/main/split_files/vae/ae.safetensors?download=true", vae_path);
			EnsureModelExists(L"https://huggingface.co/unsloth/Qwen3-4B-Instruct-2507-GGUF/resolve/main/Qwen3-4B-Instruct-2507-Q4_K_M.gguf?download=true", text_encoder_path);
			EnsureModelExists(L"https://huggingface.co/leejet/Z-Image-Turbo-GGUF/resolve/main/z_image_turbo-Q4_K.gguf?download=true", diffusion_model_path);

			sd_ctx_params_t sd_params;
			sd_ctx_params_init(&sd_params);
			sd_params.vae_path = u8_vae_path;
			sd_params.llm_path = u8_text_encoder_path;
			sd_params.diffusion_model_path = u8_diffusion_model_path;
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
				img_params.prompt = text.c_str();
				img_params.strength = 0.0f;
				img_params.batch_count = 1;

				img_params.vae_tiling_params.enabled = true;

				sd_sample_params_init(&img_params.sample_params);
				img_params.sample_params.sample_method = EULER_SAMPLE_METHOD;
				img_params.sample_params.sample_steps = 8;
				img_params.sample_params.scheduler = SIMPLE_SCHEDULER;
				img_params.sample_params.eta = 1.0f;

				img_params.sample_params.guidance.txt_cfg = 1.0f;
				img_params.sample_params.guidance.img_cfg = 1.0f;
				img_params.sample_params.guidance.distilled_guidance = 3.5f;

				sd_image_t init_img = {};
				if (mode == MODE_IMAGE_EDIT)
				{
					if (rgba2 != nullptr)
					{
						init_img.width = w2;
						init_img.height = h2;
						init_img.channel = 3;
						init_img.data = (uint8_t*)malloc(w2 * h2 * 3);
						for (int i = 0; i < w2 * h2; ++i)
						{
							init_img.data[i * 3 + 0] = rgba2[i * 4 + 0];
							init_img.data[i * 3 + 1] = rgba2[i * 4 + 1];
							init_img.data[i * 3 + 2] = rgba2[i * 4 + 2];
						}
						img_params.init_image = init_img;
						img_params.strength = image_to_image_strength;
						img_params.sample_params.guidance.txt_cfg = image_to_image_txt_cfg;
						img_params.sample_params.sample_steps = image_to_image_steps;
					}
					else if (rgba != nullptr)
					{
						init_img.width = w;
						init_img.height = h;
						init_img.channel = 3;
						init_img.data = (uint8_t*)malloc(w * h * 3);
						for (int i = 0; i < w * h; ++i)
						{
							init_img.data[i * 3 + 0] = rgba[i * 4 + 0];
							init_img.data[i * 3 + 1] = rgba[i * 4 + 1];
							init_img.data[i * 3 + 2] = rgba[i * 4 + 2];
						}
						img_params.init_image = init_img;
						img_params.strength = image_to_image_strength;
						img_params.sample_params.guidance.txt_cfg = image_to_image_txt_cfg;
						img_params.sample_params.sample_steps = image_to_image_steps;
					}
				}

				sd_image_t* image = nullptr;
				int num_images = 1;
				is_generating.store(true);
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
					push_history(rgba, w, h, true); // save output!
					redraw();
				}
				free_sd_ctx(sd_ctx);
				if (init_img.data) free(init_img.data);
			}
			FreeLibrary(stable_diffusion);
		}

		// RESTORE WORKING DIRECTORY:
		_wchdir(originalWorkingDir);

		is_generating.store(false);
	}).detach();
}

void handle_load_image(HWND hWnd)
{
	wchar_t szFile[MAX_PATH] = {};
	OPENFILENAMEW ofn = {};
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = hWnd;
	ofn.lpstrFile = szFile;
	ofn.nMaxFile = sizeof(szFile) / sizeof(szFile[0]);
	ofn.lpstrFilter = L"Images\0*.png;*.jpg;*.jpeg;*.bmp;*.tga\0All Files\0*.*\0";
	ofn.nFilterIndex = 1;
	ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

	if (GetOpenFileNameW(&ofn))
	{
		char filename[MAX_PATH] = {};
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
			push_history(rgba, w, h);
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

	wchar_t szFile[MAX_PATH] = {};
	OPENFILENAMEW ofn = {};
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = hWnd;
	ofn.lpstrFile = szFile;
	ofn.nMaxFile = sizeof(szFile) / sizeof(szFile[0]);
	ofn.lpstrFilter = L"PNG Image (*.png)\0*.png\0";
	ofn.nFilterIndex = 1;
	ofn.lpstrDefExt = L"png";
	ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;

	if (GetSaveFileNameW(&ofn))
	{
		char filename[MAX_PATH] = {};
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

void handle_paste_image(HWND hWnd)
{
	if (!OpenClipboard(hWnd)) return;

	// 1. Check if the clipboard contains copied files (e.g., from Desktop/Explorer)
	if (IsClipboardFormatAvailable(CF_HDROP))
	{
		HANDLE hDrop = GetClipboardData(CF_HDROP);
		if (hDrop)
		{
			wchar_t wfilename[1024] = {};
			// Grab the first file path copied to the clipboard
			if (DragQueryFileW((HDROP)hDrop, 0, wfilename, ARRAYSIZE(wfilename)) > 0)
			{
				CloseClipboard(); // We have the path, we can close the clipboard now

				char filename[MAX_PATH] = {};
				stbi_convert_wchar_to_utf8(filename, sizeof(filename), wfilename);

				int new_w, new_h, new_c;
				unsigned char* new_rgba = stbi_load(filename, &new_w, &new_h, &new_c, 4);

				if (new_rgba)
				{
					if (rgba) free(rgba);
					if (rgba2) { free(rgba2); rgba2 = nullptr; }

					rgba = new_rgba;
					w = new_w;
					h = new_h;

					push_history(rgba, w, h);

					// Resize window to fit the new image
					RECT rc = { 0, 0, w, h + button_height + text_height };
					AdjustWindowRect(&rc, (DWORD)GetWindowLongPtr(hWnd, GWL_STYLE), GetMenu(hWnd) != NULL);
					SetWindowPos(hWnd, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED);
					redraw();
				}
				return; // Exit here so we don't proceed to the DIB check
			}
		}
	}

	// 2. Fallback to checking for raw image data (e.g., from MS Paint or Photo Viewer)
	HANDLE hData = GetClipboardData(CF_DIB);
	if (!hData)
	{
		CloseClipboard();
		return;
	}

	BITMAPINFO* pbi = (BITMAPINFO*)GlobalLock(hData);
	if (pbi)
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
				pixels[(y * width + x) * 4 + 3] = 255;
			}
		}
		GlobalUnlock(hData);

		if (rgba) free(rgba);
		if (rgba2) { free(rgba2); rgba2 = nullptr; }

		rgba = pixels;
		w = width;
		h = height;

		push_history(rgba, w, h);

		// Resize window to fit
		RECT rc = { 0, 0, w, h + button_height + text_height };
		AdjustWindowRect(&rc, (DWORD)GetWindowLongPtr(hWnd, GWL_STYLE), GetMenu(hWnd) != NULL);
		SetWindowPos(hWnd, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED);
		redraw();
	}
	CloseClipboard();
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine, _In_ int nCmdShow)
{
	if (lpCmdLine && lpCmdLine[0])
	{
		char filename[MAX_PATH] = {};
		stbi_convert_wchar_to_utf8(filename, sizeof(filename), lpCmdLine);
		rgba = stbi_load(filename, &w, &h, &c, 4);
	}

	_wgetcwd(originalWorkingDir, MAX_PATH); // save original working dir at startup

	CoInitialize(NULL);

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

				int square_width = button_height;
				if (hBtnLoad)    MoveWindow(hBtnLoad, 0, rc.bottom - button_height, square_width, button_height, TRUE);
				if (hBtnSave)    MoveWindow(hBtnSave, square_width, rc.bottom - button_height, square_width, button_height, TRUE);
				if (hBtnCopy)    MoveWindow(hBtnCopy, square_width * 2, rc.bottom - button_height, square_width, button_height, TRUE);
				if (hBtnClear)   MoveWindow(hBtnClear, square_width * 3, rc.bottom - button_height, square_width, button_height, TRUE);
				if (hBtnUndo)    MoveWindow(hBtnUndo, square_width * 4, rc.bottom - button_height, square_width, button_height, TRUE);
				if (hBtnRedo)    MoveWindow(hBtnRedo, square_width * 5, rc.bottom - button_height, square_width, button_height, TRUE);
				if (hBtnGenerate) MoveWindow(hBtnGenerate, square_width * 6, rc.bottom - button_height, rc.right - (square_width * 6), button_height, TRUE);
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

				if (!current_download.empty())
				{
					// Print download progress text to image area:
					wchar_t status[4096] = {};
					wsprintfW(status, L"Downloading model: %d%%\n%s", progress, current_download.c_str());
					SetBkMode(hdc, TRANSPARENT);
					HFONT hProgressFont = CreateFontW(22, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
					HFONT hOldFont = (HFONT)SelectObject(hdc, hProgressFont);
					RECT textRect = { 0, 0, w2, h2 - splitter_thickness };
					RECT calcRect = textRect;
					DrawTextW(hdc, status, -1, &calcRect, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
					int textHeight = calcRect.bottom - calcRect.top;
					int containerHeight = textRect.bottom - textRect.top;
					int offset = (containerHeight - textHeight) / 2;
					textRect.top += offset;
					textRect.bottom = textRect.top + textHeight;
					RECT shadowRect = textRect;
					OffsetRect(&shadowRect, 2, 2);
					SetTextColor(hdc, RGB(10, 10, 10));
					DrawTextW(hdc, status, -1, &shadowRect, DT_CENTER | DT_WORDBREAK);
					SetTextColor(hdc, RGB(255, 255, 255));
					DrawTextW(hdc, status, -1, &textRect, DT_CENTER | DT_WORDBREAK);
					SelectObject(hdc, hOldFont);
					DeleteObject(hProgressFont);
				}
				else if (progress > 0 && progress < 100)
				{
					// Print progress text to image area:
					wchar_t status[32] = {};
					wsprintfW(status, L"%d%%", progress);
					SetBkMode(hdc, TRANSPARENT);
					HFONT hProgressFont = CreateFontW(64, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
					HFONT hOldFont = (HFONT)SelectObject(hdc, hProgressFont);
					RECT textRect = { 0, 0, w2, h2 - splitter_thickness };
					RECT shadowRect = textRect;
					OffsetRect(&shadowRect, 2, 2);
					SetTextColor(hdc, RGB(10, 10, 10));
					DrawTextW(hdc, status, -1, &shadowRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
					SetTextColor(hdc, RGB(255, 255, 255));
					DrawTextW(hdc, status, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
					SelectObject(hdc, hOldFont);
					DeleteObject(hProgressFont);
				}
				else if (!current_errors.empty())
				{
					int cnt = MultiByteToWideChar(CP_UTF8, 0, current_errors.c_str(), -1, nullptr, 0);
					std::wstring wstr(cnt, 0);
					MultiByteToWideChar(CP_UTF8, 0, current_errors.c_str(), -1, wstr.data(), cnt);
					wchar_t status[4096] = {};
					wsprintfW(status, L"Errors:\n%s", wstr.c_str());
					SetBkMode(hdc, TRANSPARENT);
					HFONT hProgressFont = CreateFontW(22, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
					HFONT hOldFont = (HFONT)SelectObject(hdc, hProgressFont);
					RECT textRect = { 0, 0, w2, h2 - splitter_thickness };
					RECT calcRect = textRect;
					DrawTextW(hdc, status, -1, &calcRect, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
					int textHeight = calcRect.bottom - calcRect.top;
					int containerHeight = textRect.bottom - textRect.top;
					int offset = (containerHeight - textHeight) / 2;
					textRect.top += offset;
					textRect.bottom = textRect.top + textHeight;
					RECT shadowRect = textRect;
					OffsetRect(&shadowRect, 2, 2);
					SetTextColor(hdc, RGB(10, 10, 10));
					DrawTextW(hdc, status, -1, &shadowRect, DT_CENTER | DT_WORDBREAK);
					SetTextColor(hdc, RGB(255, 255, 255));
					DrawTextW(hdc, status, -1, &textRect, DT_CENTER | DT_WORDBREAK);
					SelectObject(hdc, hOldFont);
					DeleteObject(hProgressFont);
				}
				else if (rgba == nullptr && !is_generating.load())
				{
					const wchar_t* status = L"The image will be generated here.\nOr drag and drop an image here to edit.";
					SetBkMode(hdc, TRANSPARENT);
					HFONT hProgressFont = CreateFontW(26, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
					HFONT hOldFont = (HFONT)SelectObject(hdc, hProgressFont);
					RECT textRect = { 0, 0, w2, h2 - splitter_thickness };
					RECT calcRect = textRect;
					DrawTextW(hdc, status, -1, &calcRect, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
					int textHeight = calcRect.bottom - calcRect.top;
					int containerHeight = textRect.bottom - textRect.top;
					int offset = (containerHeight - textHeight) / 2;
					textRect.top += offset;
					textRect.bottom = textRect.top + textHeight;
					RECT shadowRect = textRect;
					OffsetRect(&shadowRect, 2, 2);
					SetTextColor(hdc, RGB(10, 10, 10));
					DrawTextW(hdc, status, -1, &shadowRect, DT_CENTER | DT_WORDBREAK);
					SetTextColor(hdc, RGB(155, 155, 155));
					DrawTextW(hdc, status, -1, &textRect, DT_CENTER | DT_WORDBREAK);
					SelectObject(hdc, hOldFont);
					DeleteObject(hProgressFont);
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
					case IDC_CLEAR_BUTTON:
						if (rgba) { free(rgba); rgba = nullptr; }
						if (rgba2) { free(rgba2); rgba2 = nullptr; }
						push_history(nullptr, w, h);
						redraw();
						break;
					case IDC_GENERATE_BUTTON:
					case ID_ACCEL_GENERATE:
						trigger_generation();
						break; 
					case IDC_UNDO_BUTTON:
						handle_undo();
						break;
					case IDC_REDO_BUTTON:
						handle_redo();
						break;
					}
				}
				break; 
			
			case WM_KEYDOWN:
			if (GetKeyState(VK_CONTROL) & 0x8000)
			{
				if (wParam == 'V')
				{
					if (GetFocus() != hEdit) handle_paste_image(hWnd);
				}
				else if (wParam == 'Z')
				{
					if (GetFocus() != hEdit) handle_undo();
				}
				else if (wParam == 'Y')
				{
					if (GetFocus() != hEdit) handle_redo();
				}
			}
			break;

			case WM_CONTEXTMENU:
			{
				if ((HWND)wParam == window)
				{
					HMENU hMenu = CreatePopupMenu();

					// 1. Add Clipboard and Clear actions
					AppendMenuW(hMenu, MF_STRING, 1099, L"New image");
					AppendMenuW(hMenu, MF_STRING, 1100, L"Copy (Ctrl + C)");
					AppendMenuW(hMenu, MF_STRING, 1101, L"Paste (Ctrl + V)");
					AppendMenuW(hMenu, MF_STRING, 1102, L"50%");
					AppendMenuW(hMenu, MF_STRING, 1103, L"100%");
					AppendMenuW(hMenu, MF_STRING, 1104, L"200%");
					AppendMenuW(hMenu, MF_STRING, 1105, L"400%");

					// 2. Add Separator
					AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);

					// 3. Add Resolution Presets
					for (int i = 0; i < ARRAYSIZE(presets); ++i)
						AppendMenuW(hMenu, MF_STRING, 1000 + i, presets[i].name);

					POINT pt;
					GetCursorPos(&pt);
					int selection = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hWnd, NULL);

					// Handle Utility Selection
					if (selection == 1099) { // New image
						if (rgba) { free(rgba); rgba = nullptr; }
						if (rgba2) { free(rgba2); rgba2 = nullptr; }
						redraw();
					}
					else if (selection == 1100) { // Copy
						handle_copy_image(hWnd);
					}
					else if (selection == 1101) { // Paste
						handle_paste_image(hWnd);
					}
					else if (selection == 1102) { // 50%
						if (rgba2) { free(rgba2); rgba2 = nullptr; }
						w2 = w / 2;
						h2 = h / 2;
						resize();

						RECT rc = { 0, 0, w2, h2 + button_height + text_height };
						AdjustWindowRect(&rc, (DWORD)GetWindowLongPtr(hWnd, GWL_STYLE), GetMenu(hWnd) != NULL);
						SetWindowPos(hWnd, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED);
						if (rgba) redraw();
						redraw();
					}
					else if (selection == 1103) { // 100%
						if (rgba2) { free(rgba2); rgba2 = nullptr; }
						w2 = w;
						h2 = h;

						RECT rc = { 0, 0, w2, h2 + button_height + text_height };
						AdjustWindowRect(&rc, (DWORD)GetWindowLongPtr(hWnd, GWL_STYLE), GetMenu(hWnd) != NULL);
						SetWindowPos(hWnd, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED);
						if (rgba) redraw();
						redraw();
					}
					else if (selection == 1104) { // 200%
						if (rgba2) { free(rgba2); rgba2 = nullptr; }
						w2 = w * 2;
						h2 = h * 2;
						resize();

						RECT rc = { 0, 0, w2, h2 + button_height + text_height };
						AdjustWindowRect(&rc, (DWORD)GetWindowLongPtr(hWnd, GWL_STYLE), GetMenu(hWnd) != NULL);
						SetWindowPos(hWnd, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED);
						if (rgba) redraw();
						redraw();
					}
					else if (selection == 1105) { // 400%
						if (rgba2) { free(rgba2); rgba2 = nullptr; }
						w2 = w * 4;
						h2 = h * 4;
						resize();

						RECT rc = { 0, 0, w2, h2 + button_height + text_height };
						AdjustWindowRect(&rc, (DWORD)GetWindowLongPtr(hWnd, GWL_STYLE), GetMenu(hWnd) != NULL);
						SetWindowPos(hWnd, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED);
						if (rgba) redraw();
						redraw();
					}
					// Handle Resolution Selection
					else {
						selection -= 1000;
						if (selection >= 0 && selection < ARRAYSIZE(presets))
						{
							w2 = presets[selection].w;
							h2 = presets[selection].h;
							set_title();

							RECT rc = { 0, 0, w2, h2 + button_height + text_height };
							AdjustWindowRect(&rc, (DWORD)GetWindowLongPtr(hWnd, GWL_STYLE), GetMenu(hWnd) != NULL);
							SetWindowPos(hWnd, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED);
							if (rgba) redraw();
						}
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
					char filename[MAX_PATH] = {};
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
					if (rgba) push_history(rgba, w, h);
				}
				RECT rc = { 0, 0, w, h + button_height + text_height };
				AdjustWindowRect(&rc, (DWORD)GetWindowLongPtr(hWnd, GWL_STYLE), GetMenu(hWnd) != NULL);
				SetWindowPos(hWnd, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED);
				SetForegroundWindow(hWnd);
				redraw();
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
						if (hBtnLoad)    MoveWindow(hBtnLoad, 0, rc.bottom - button_height, square_width, button_height, TRUE);
						if (hBtnSave)    MoveWindow(hBtnSave, square_width, rc.bottom - button_height, square_width, button_height, TRUE);
						if (hBtnCopy)    MoveWindow(hBtnCopy, square_width * 2, rc.bottom - button_height, square_width, button_height, TRUE);
						if (hBtnClear)   MoveWindow(hBtnClear, square_width * 3, rc.bottom - button_height, square_width, button_height, TRUE);
						if (hBtnUndo)    MoveWindow(hBtnUndo, square_width * 4, rc.bottom - button_height, square_width, button_height, TRUE);
						if (hBtnRedo)    MoveWindow(hBtnRedo, square_width * 5, rc.bottom - button_height, square_width, button_height, TRUE);
						if (hBtnGenerate) MoveWindow(hBtnGenerate, square_width * 6, rc.bottom - button_height, rc.right - (square_width * 6), button_height, TRUE);

						InvalidateRect(hWnd, NULL, TRUE);
						if (rgba)
						{
							redraw();
						}
					}
				}
			}
			break;

			case WM_NOTIFY:
			{
				NMHDR* pNmhdr = (NMHDR*)lParam;
				if (pNmhdr->code == BCN_DROPDOWN && pNmhdr->idFrom == IDC_GENERATE_BUTTON)
				{
					HMENU hMenu = CreatePopupMenu();
					AppendMenuW(hMenu, MF_STRING | (mode == MODE_IMAGE_GENERATE ? MF_CHECKED : 0), 101, L"Generate New Image");
					AppendMenuW(hMenu, MF_STRING | (mode == MODE_IMAGE_EDIT ? MF_CHECKED : 0), 102, L"Edit Image");
					AppendMenuW(hMenu, MF_STRING | (mode == MODE_IMAGE_DESCRIBE ? MF_CHECKED : 0), 103, L"Describe Image");
					AppendMenuW(hMenu, MF_STRING | (mode == MODE_IMAGE_STORY ? MF_CHECKED : 0), 104, L"Story from Image");

					RECT rc;
					GetWindowRect(hBtnGenerate, &rc);
					int selection = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, rc.left, rc.bottom, 0, hWnd, NULL);

					switch (selection)
					{
					default:
					case 101:
						mode = MODE_IMAGE_GENERATE;
						SetWindowTextW(hBtnGenerate, L"\u2728 Generate \u2728");
						break;
					case 102:
						mode = MODE_IMAGE_EDIT;
						SetWindowTextW(hBtnGenerate, L"\u2728 Edit \u2728");
						break;
					case 103:
						mode = MODE_IMAGE_DESCRIBE;
						SetWindowTextW(hBtnGenerate, L"\u2728 Describe \u2728");
						break;
					case 104:
						mode = MODE_IMAGE_STORY;
						SetWindowTextW(hBtnGenerate, L"\u2728 Story \u2728");
						break;
					}

					DestroyMenu(hMenu);
					return 0;
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
	wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wcex.hbrBackground = CreateSolidBrush(RGB(30, 30, 30));
	wcex.lpszMenuName = NULL;
	wcex.lpszClassName = L"mini-ai";
	wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APPICON));
	wcex.hIconSm = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APPICON)); // Small icon (taskbar)
	RegisterClassExW(&wcex);

	RECT wr = { 0, 0, w, h + text_height + button_height };
	DWORD window_style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
	AdjustWindowRect(&wr, window_style, FALSE);
	int window_width = wr.right - wr.left;
	int window_height = wr.bottom - wr.top;

	window = CreateWindowW(L"mini-ai", L"mini-ai", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, 0, window_width, window_height, nullptr, nullptr, NULL, nullptr);
	hEdit = CreateWindowW(L"EDIT", NULL, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL, 0, 0, 0, 0, window, NULL, hInstance, NULL);
	SHAutoComplete(hEdit, SHACF_AUTOAPPEND_FORCE_OFF | SHACF_AUTOSUGGEST_FORCE_OFF);

	// Create UI Button Controls matching native modern font layouts
	hBtnLoad = CreateWindowW(L"BUTTON", L"\xE8B7", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, window, (HMENU)IDC_LOAD_BUTTON, hInstance, NULL);
	hBtnSave = CreateWindowW(L"BUTTON", L"\xE74E", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, window, (HMENU)IDC_SAVE_BUTTON, hInstance, NULL);
	hBtnCopy = CreateWindowW(L"BUTTON", L"\xE8C8", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, window, (HMENU)IDC_COPY_BUTTON, hInstance, NULL);
	hBtnClear = CreateWindowW(L"BUTTON", L"\xE74D", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, window, (HMENU)IDC_CLEAR_BUTTON, hInstance, NULL);
	hBtnGenerate = CreateWindowW(L"BUTTON", L"\u2728 Generate \u2728", WS_CHILD | WS_VISIBLE | BS_SPLITBUTTON, 0, 0, 0, 0, window, (HMENU)IDC_GENERATE_BUTTON, hInstance, NULL);
	hBtnUndo = CreateWindowW(L"BUTTON", L"\xE7A7", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, window, (HMENU)IDC_UNDO_BUTTON, hInstance, NULL);
	hBtnRedo = CreateWindowW(L"BUTTON", L"\xE7A6", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, window, (HMENU)IDC_REDO_BUTTON, hInstance, NULL);

	HFONT hFont = CreateFont(34, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Arial");
	SendMessage(hEdit, WM_SETFONT, (WPARAM)hFont, TRUE);

	// Create structural asset mapping definitions for MDL2 system icons
	HFONT hIconFont = CreateFontW(24, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe MDL2 Assets");
	SendMessageW(hBtnLoad, WM_SETFONT, (WPARAM)hIconFont, TRUE);
	SendMessageW(hBtnSave, WM_SETFONT, (WPARAM)hIconFont, TRUE);
	SendMessageW(hBtnCopy, WM_SETFONT, (WPARAM)hIconFont, TRUE);
	SendMessageW(hBtnClear, WM_SETFONT, (WPARAM)hIconFont, TRUE);
	SendMessageW(hBtnUndo, WM_SETFONT, (WPARAM)hIconFont, TRUE);
	SendMessageW(hBtnRedo, WM_SETFONT, (WPARAM)hIconFont, TRUE);
	HFONT hGenFont = CreateFontW(32, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Symbol");
	SendMessageW(hBtnGenerate, WM_SETFONT, (WPARAM)hGenFont, TRUE);

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
	SetWindowTheme(hBtnClear, L"DarkMode_Explorer", NULL);
	SetWindowTheme(hBtnUndo, L"DarkMode_Explorer", NULL);
	SetWindowTheme(hBtnRedo, L"DarkMode_Explorer", NULL);
	SetWindowTheme(hBtnGenerate, L"DarkMode_Explorer", NULL);
	SetWindowTheme(hEdit, L"DarkMode_Explorer", NULL);

	LoadPrompt(hEdit);

	// keyboard shortcuts
	ACCEL accels[] = {
		{ FCONTROL | FVIRTKEY, 'O', ID_ACCEL_LOAD },
		{ FCONTROL | FVIRTKEY, 'S', ID_ACCEL_SAVE },
		{ FCONTROL | FVIRTKEY, 'C', ID_ACCEL_COPY },
		{ FCONTROL | FVIRTKEY, VK_RETURN, ID_ACCEL_GENERATE }
	};
	HACCEL hAccel = CreateAcceleratorTableW(accels, ARRAYSIZE(accels));

	while (!exiting)
	{
		MSG msg = {};
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