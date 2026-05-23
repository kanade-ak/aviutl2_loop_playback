#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <strsafe.h>
#include <atomic>
#include <cstring>
#include <cstdint>
#include <cwchar>
#include <vector>

#include "detours.h"
#include "plugin2.h"
#include "logger2.h"

namespace {

constexpr DWORD kRequiredVersion = 2003300;
constexpr UINT kFallbackWarpPlaybackMessage = WM_APP + 0x04c5;
constexpr UINT kFallbackStartLoopPlaybackMessage = WM_APP + 0x04c6;
constexpr UINT_PTR kWarpPlaybackTimerId = 0x4c50;
constexpr UINT kWarpPlaybackTimerMs = 10;
constexpr LPCWSTR kSettingsFileName = L"aviutl2_loop_playback.ini";

using PlaybackEndCheckFn = uint64_t(__fastcall *)(void *player, uint64_t a2, uint64_t a3, uint64_t a4);
using PlaybackStartFn = void(__fastcall *)(void *player, int mode, char repeat_buffer, uint64_t a4);
using ShortcutPlaybackFn = void(__fastcall *)(unsigned char stop_flag, unsigned char repeat_buffer);
using PreviewSeekFn = void(__fastcall *)(void *preview, int frame);
using SceneSeekFn = void(__fastcall *)(void *scene_preview_component, int frame, char keep_audio);

COMMON_PLUGIN_TABLE g_common_plugin_table = {
    L"ループ再生プラグイン",
    L"ループ再生を可能にします。プラグイン設定から切り替え",
};

HINSTANCE g_instance = nullptr;
EDIT_HANDLE *g_edit_handle = nullptr;
HWND g_host_window = nullptr;
WNDPROC g_original_host_wnd_proc = nullptr;
UINT g_warp_playback_message = 0;
UINT g_start_loop_playback_message = 0;
PlaybackStartFn g_true_playback_start = nullptr;
PlaybackEndCheckFn g_true_playback_end_check = nullptr;
ShortcutPlaybackFn g_shortcut_playback = nullptr;
PreviewSeekFn g_preview_seek = nullptr;
SceneSeekFn g_scene_seek = nullptr;
void **g_scene_preview_component_global = nullptr;
std::atomic_bool g_hook_installed{false};
std::atomic_bool g_hook_unresolved{false};
std::atomic_bool g_unresolved_dialog_shown{false};
std::atomic_bool g_loop_enabled{false};
std::atomic_bool g_host_subclassed{false};
std::atomic_bool g_loop_next_playback{false};
std::atomic_bool g_loop_playback_active{false};
std::atomic<void *> g_loop_player{nullptr};
std::atomic<int> g_loop_mode{0};
std::atomic<void *> g_preview_player{nullptr};
std::atomic<int> g_preview_start_frame{0};
std::atomic<void *> g_scene_player{nullptr};
std::atomic<int> g_scene_start_frame{0};
std::atomic<void *> g_pending_warp_player{nullptr};
std::atomic<int> g_pending_warp_mode{0};
std::atomic<int> g_pending_warp_frame{0};

int HexValue(char c)
{
    if ('0' <= c && c <= '9') {
        return c - '0';
    }
    if ('a' <= c && c <= 'f') {
        return c - 'a' + 10;
    }
    if ('A' <= c && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool ParsePattern(LPCSTR text, std::vector<BYTE> *bytes, std::vector<BYTE> *mask)
{
    bytes->clear();
    mask->clear();
    for (LPCSTR p = text; *p != '\0';) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            ++p;
        }
        if (*p == '\0') {
            break;
        }
        if (*p == '?') {
            ++p;
            if (*p == '?') {
                ++p;
            }
            bytes->push_back(0);
            mask->push_back(0);
            continue;
        }

        const int high = HexValue(p[0]);
        const int low = HexValue(p[1]);
        if (high < 0 || low < 0) {
            return false;
        }
        bytes->push_back(static_cast<BYTE>((high << 4) | low));
        mask->push_back(0xff);
        p += 2;
    }
    return !bytes->empty() && bytes->size() == mask->size();
}

bool GetTextSection(HMODULE module, BYTE **text, size_t *size)
{
    if (module == nullptr || text == nullptr || size == nullptr) {
        return false;
    }

    auto *base = reinterpret_cast<BYTE *>(module);
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }

    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }

    auto *section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (memcmp(section->Name, ".text", 5) == 0) {
            *text = base + section->VirtualAddress;
            *size = section->Misc.VirtualSize;
            return *size != 0;
        }
    }
    return false;
}

BYTE *FindUniquePattern(BYTE *text, size_t text_size, LPCSTR pattern_text)
{
    std::vector<BYTE> bytes;
    std::vector<BYTE> mask;
    if (!ParsePattern(pattern_text, &bytes, &mask)) {
        return nullptr;
    }

    BYTE *match = nullptr;
    unsigned count = 0;
    if (bytes.size() <= text_size) {
        for (size_t i = 0; i <= text_size - bytes.size(); ++i) {
            bool ok = true;
            for (size_t j = 0; j < bytes.size(); ++j) {
                if (mask[j] != 0 && text[i + j] != bytes[j]) {
                    ok = false;
                    break;
                }
            }
            if (ok) {
                match = text + i;
                ++count;
                if (count > 1) {
                    break;
                }
            }
        }
    }

    if (count != 1) {
        return nullptr;
    }

    return match;
}

void **DecodeRipRelativeDataTarget(BYTE *instruction)
{
    const int32_t displacement = *reinterpret_cast<int32_t *>(instruction + 3);
    return reinterpret_cast<void **>(instruction + 7 + displacement);
}

bool ResolveHostSymbols()
{
    g_hook_unresolved.store(false);

    HMODULE host = GetModuleHandleW(nullptr);
    if (host == nullptr) {
        g_hook_unresolved.store(true);
        return false;
    }

    BYTE *text = nullptr;
    size_t text_size = 0;
    if (!GetTextSection(host, &text, &text_size)) {
        g_hook_unresolved.store(true);
        return false;
    }

    BYTE *playback_start = FindUniquePattern(
        text, text_size,
        "4C 8B DC 49 89 5B 18 49 89 73 20 57 48 81 EC A0");
    BYTE *playback_end_check = FindUniquePattern(
        text, text_size,
        "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 "
        "48 83 EC 40 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 30 "
        "48 8B F9 80 B9 51 03 00 00 00 75 07 32 C0 E9 3E 01 00 00");
    BYTE *shortcut_playback = FindUniquePattern(
        text, text_size,
        "4C 8B 05 ?? ?? ?? ?? 44 0F B6 CA 0F B6 D1 "
        "41 83 B8 50 03 00 00 00 74 42 48 8B 0D ?? ?? ?? ?? "
        "83 B9 58 01 00 00 00");
    BYTE *preview_seek = FindUniquePattern(
        text, text_size,
        "48 89 5C 24 18 55 56 57 48 83 EC 30 48 8B 81 00 08 00 00 "
        "48 8B F1 8B 78 04");
    BYTE *scene_seek = FindUniquePattern(
        text, text_size,
        "48 89 5C 24 18 56 48 81 EC 80 00 00 00 83 B9 58 01 00 00 00 "
        "48 8B D9 41 0F B6 F0");

    if (playback_start == nullptr || playback_end_check == nullptr || shortcut_playback == nullptr ||
        preview_seek == nullptr || scene_seek == nullptr) {
        g_hook_unresolved.store(true);
        return false;
    }

    g_true_playback_start = reinterpret_cast<PlaybackStartFn>(playback_start);
    g_true_playback_end_check = reinterpret_cast<PlaybackEndCheckFn>(playback_end_check);
    g_shortcut_playback = reinterpret_cast<ShortcutPlaybackFn>(shortcut_playback);
    g_preview_seek = reinterpret_cast<PreviewSeekFn>(preview_seek);
    g_scene_seek = reinterpret_cast<SceneSeekFn>(scene_seek);
    g_scene_preview_component_global = DecodeRipRelativeDataTarget(shortcut_playback + 24);

    g_hook_unresolved.store(false);
    return true;
}

bool GetSettingsPath(wchar_t path[MAX_PATH])
{
    if (g_instance == nullptr) {
        return false;
    }

    DWORD length = GetModuleFileNameW(g_instance, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return false;
    }

    wchar_t *slash = wcsrchr(path, L'\\');
    if (slash == nullptr) {
        return false;
    }
    *(slash + 1) = L'\0';
    return SUCCEEDED(StringCchCatW(path, MAX_PATH, kSettingsFileName));
}

void LoadSettings()
{
    wchar_t path[MAX_PATH] = {};
    if (!GetSettingsPath(path)) {
        return;
    }

    const UINT enabled = GetPrivateProfileIntW(L"Settings", L"LoopNormalPlayback", 0, path);
    g_loop_enabled.store(enabled != 0);
}

void SaveSettings()
{
    wchar_t path[MAX_PATH] = {};
    if (!GetSettingsPath(path)) {
        return;
    }

    const bool enabled = g_loop_enabled.load();
    WritePrivateProfileStringW(L"Settings", L"LoopNormalPlayback", enabled ? L"1" : L"0", path);
}

void ShowUnresolvedStartupDialog()
{
    if (!g_hook_unresolved.load() || g_unresolved_dialog_shown.exchange(true)) {
        return;
    }

    MessageBoxW(g_host_window,
                L"このバージョンでは動作しません!!",
                L"ループ再生プラグイン",
                MB_OK | MB_ICONWARNING);
}

void WarpToPendingFrame()
{
    void *player = g_pending_warp_player.exchange(nullptr);
    const int mode = g_pending_warp_mode.exchange(0);
    const int frame = g_pending_warp_frame.load();

    if (player == nullptr || mode == 0) {
        return;
    }

    bool seek_called = false;
    if (mode == 3 && g_preview_seek != nullptr) {
        void *preview = *static_cast<void **>(player);
        if (preview != nullptr) {
            g_preview_seek(preview, frame);
            seek_called = true;
        }
    }
    else if (mode == 4 && g_scene_seek != nullptr && g_scene_preview_component_global != nullptr) {
        void *scene_preview_component = *g_scene_preview_component_global;
        if (scene_preview_component != nullptr) {
            g_scene_seek(scene_preview_component, frame, 0);
            seek_called = true;
        }
    }

    if (!seek_called || g_true_playback_start == nullptr) {
        return;
    }

    const uint64_t restart_a4 = 0;
    g_loop_player.store(player);
    g_loop_mode.store(mode);
    g_loop_playback_active.store(true);
    g_true_playback_start(player, mode, 0, restart_a4);
}

void StartLoopPlaybackNow()
{
    if (!g_hook_installed.load()) {
        return;
    }

    int edit_state = -1;
    if (g_edit_handle != nullptr && g_edit_handle->get_edit_state != nullptr) {
        edit_state = g_edit_handle->get_edit_state();
    }

    if (edit_state == EDIT_HANDLE::EDIT_STATE_PLAY) {
        g_loop_next_playback.store(false);
        g_loop_playback_active.store(false);
        g_loop_player.store(nullptr);
        g_loop_mode.store(0);
        g_shortcut_playback(0, 0);
        return;
    }

    g_loop_next_playback.store(true);
    g_shortcut_playback(0, 0);
    if (g_loop_next_playback.exchange(false)) {
        return;
    }
}

LRESULT CALLBACK HostWndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == g_start_loop_playback_message && g_start_loop_playback_message != 0) {
        StartLoopPlaybackNow();
        return 0;
    }
    if (message == g_warp_playback_message && g_warp_playback_message != 0) {
        const UINT_PTR timer = SetTimer(hwnd, kWarpPlaybackTimerId, kWarpPlaybackTimerMs, nullptr);
        if (timer == 0) {
            WarpToPendingFrame();
        }
        return 0;
    }
    if (message == WM_TIMER && wparam == kWarpPlaybackTimerId) {
        KillTimer(hwnd, kWarpPlaybackTimerId);
        WarpToPendingFrame();
        return 0;
    }

    if (g_original_host_wnd_proc != nullptr) {
        return CallWindowProcW(g_original_host_wnd_proc, hwnd, message, wparam, lparam);
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

bool SubclassHostWindow(HOST_APP_TABLE *host)
{
    if (g_host_subclassed.load()) {
        return true;
    }
    if (host == nullptr || host->create_edit_handle == nullptr) {
        return false;
    }

    if (g_edit_handle == nullptr) {
        g_edit_handle = host->create_edit_handle();
    }
    if (g_edit_handle == nullptr || g_edit_handle->get_host_app_window == nullptr) {
        return false;
    }

    g_host_window = g_edit_handle->get_host_app_window();
    if (g_host_window == nullptr) {
        return false;
    }
    if (g_warp_playback_message == 0) {
        g_warp_playback_message = RegisterWindowMessageW(L"AviUtl2LoopPlayback.WarpPlayback.v1");
        if (g_warp_playback_message == 0) {
            g_warp_playback_message = kFallbackWarpPlaybackMessage;
        }
    }
    if (g_start_loop_playback_message == 0) {
        g_start_loop_playback_message = RegisterWindowMessageW(L"AviUtl2LoopPlayback.StartLoopPlayback.v1");
        if (g_start_loop_playback_message == 0) {
            g_start_loop_playback_message = kFallbackStartLoopPlaybackMessage;
        }
    }

    SetLastError(ERROR_SUCCESS);
    LONG_PTR original = SetWindowLongPtrW(g_host_window, GWLP_WNDPROC,
                                          reinterpret_cast<LONG_PTR>(HostWndProc));
    if (original == 0 && GetLastError() != ERROR_SUCCESS) {
        g_host_window = nullptr;
        return false;
    }

    g_original_host_wnd_proc = reinterpret_cast<WNDPROC>(original);
    g_host_subclassed.store(true);
    return true;
}

void UnsubclassHostWindow()
{
    HWND hwnd = g_host_window;
    if (g_host_subclassed.load() && hwnd != nullptr && IsWindow(hwnd) && g_original_host_wnd_proc != nullptr) {
        if (reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC)) == HostWndProc) {
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_original_host_wnd_proc));
        }
    }

    g_host_subclassed.store(false);
    g_host_window = nullptr;
    g_original_host_wnd_proc = nullptr;
    g_warp_playback_message = 0;
    g_start_loop_playback_message = 0;
    g_pending_warp_player.store(nullptr);
    g_pending_warp_mode.store(0);
    if (hwnd != nullptr && IsWindow(hwnd)) {
        KillTimer(hwnd, kWarpPlaybackTimerId);
    }
}

void QueueWarp(void *player, int mode)
{
    if (g_host_window == nullptr || g_warp_playback_message == 0 || player == nullptr) {
        return;
    }

    int frame = *reinterpret_cast<int *>(static_cast<BYTE *>(player) + 0x32c);
    if (mode == 3 && g_preview_player.load() == player) {
        frame = g_preview_start_frame.load();
    }
    else if (mode == 4 && g_scene_player.load() == player) {
        frame = g_scene_start_frame.load();
    }

    g_pending_warp_frame.store(frame);
    g_pending_warp_mode.store(mode);
    g_pending_warp_player.store(player);
    PostMessageW(g_host_window, g_warp_playback_message, 0, 0);
}

void __fastcall HookPlaybackStart(void *player, int mode, char repeat_buffer, uint64_t a4)
{
    if (g_true_playback_start == nullptr) {
        return;
    }

    g_true_playback_start(player, mode, repeat_buffer, a4);

    if (player == nullptr || repeat_buffer != 0 || (mode != 3 && mode != 4)) {
        return;
    }

    const int start_frame = *reinterpret_cast<int *>(static_cast<BYTE *>(player) + 0x32c);
    if (mode == 3) {
        g_preview_player.store(player);
        g_preview_start_frame.store(start_frame);
    }
    else {
        g_scene_player.store(player);
        g_scene_start_frame.store(start_frame);
    }

    const bool requested_loop = g_loop_next_playback.exchange(false);
    const bool global_loop = g_loop_enabled.load();
    const bool loop_this_playback = requested_loop || global_loop;
    if (loop_this_playback) {
        g_loop_player.store(player);
        g_loop_mode.store(mode);
        g_loop_playback_active.store(true);
    }
    else if (g_loop_player.load() == player) {
        g_loop_playback_active.store(false);
        g_loop_mode.store(0);
    }
}

uint64_t __fastcall HookPlaybackEndCheck(void *player, uint64_t a2, uint64_t a3, uint64_t a4)
{
    const bool active_for_player = g_loop_playback_active.load() && g_loop_player.load() == player;
    const bool global_loop = g_loop_enabled.load();
    if (g_true_playback_end_check == nullptr || player == nullptr || (!global_loop && !active_for_player)) {
        if (g_true_playback_end_check == nullptr) {
            return 0;
        }
        return g_true_playback_end_check(player, a2, a3, a4);
    }

    auto *bytes = static_cast<BYTE *>(player);
    const int mode = *reinterpret_cast<int *>(bytes + 0x320);
    const BYTE repeat_buffer_flag = *(bytes + 0x351);

    if (repeat_buffer_flag != 0 || (mode != 3 && mode != 4)) {
        return g_true_playback_end_check(player, a2, a3, a4);
    }

    uint64_t result = g_true_playback_end_check(player, a2, a3, a4);
    const unsigned result_low = static_cast<unsigned>(result & 0xff);
    if (result_low == 0) {
        QueueWarp(player, mode);
    }
    return result;
}

bool AttachHook()
{
    if (g_hook_installed.load()) {
        return true;
    }
    if (!ResolveHostSymbols()) {
        return false;
    }

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    LONG error = DetourAttach(reinterpret_cast<PVOID *>(&g_true_playback_start), HookPlaybackStart);
    if (error == NO_ERROR) {
        error = DetourAttach(reinterpret_cast<PVOID *>(&g_true_playback_end_check), HookPlaybackEndCheck);
    }
    if (error == NO_ERROR) {
        error = DetourTransactionCommit();
    }
    else {
        DetourTransactionAbort();
    }

    if (error != NO_ERROR) {
        g_true_playback_start = nullptr;
        g_true_playback_end_check = nullptr;
        g_shortcut_playback = nullptr;
        g_hook_unresolved.store(true);
        return false;
    }

    g_hook_installed.store(true);
    return true;
}

void DetachHook()
{
    if (!g_hook_installed.load()) {
        return;
    }

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    LONG error = NO_ERROR;
    if (g_true_playback_start != nullptr) {
        error = DetourDetach(reinterpret_cast<PVOID *>(&g_true_playback_start), HookPlaybackStart);
    }
    if (error == NO_ERROR && g_true_playback_end_check != nullptr) {
        error = DetourDetach(reinterpret_cast<PVOID *>(&g_true_playback_end_check), HookPlaybackEndCheck);
    }
    if (error == NO_ERROR) {
        error = DetourTransactionCommit();
    }
    else {
        DetourTransactionAbort();
    }

    if (error == NO_ERROR) {
        g_hook_installed.store(false);
        g_true_playback_start = nullptr;
        g_true_playback_end_check = nullptr;
        g_shortcut_playback = nullptr;
    }
}

void LoopPlaybackEditMenu(EDIT_SECTION *)
{
    if (g_host_window != nullptr && g_start_loop_playback_message != 0) {
        if (PostMessageW(g_host_window, g_start_loop_playback_message, 0, 0)) {
            return;
        }
    }

    StartLoopPlaybackNow();
}

void ToggleLoopPlayback(HWND hwnd, HINSTANCE)
{
    const bool current_enabled = g_loop_enabled.load();
    const int result = MessageBoxW(hwnd,
                                   L"通常再生のループを有効化しますか？\n\n"
                                   L"Yes: 有効化\n"
                                   L"No: 無効化",
                                   L"ループ再生プラグイン",
                                   MB_YESNO | MB_ICONQUESTION |
                                       (current_enabled ? MB_DEFBUTTON1 : MB_DEFBUTTON2));
    const bool enabled = result == IDYES;
    g_loop_enabled.store(enabled);
    SaveSettings();
    MessageBoxW(hwnd,
                enabled
                    ? L"ループ再生を有効化しました \n 通常再生でループします"
                    : L"ループ再生を無効化しました \n 通常再生でループしません",
                L"ループ再生プラグイン",
                MB_OK | MB_ICONINFORMATION);
}

}

extern "C" __declspec(dllexport) DWORD RequiredVersion()
{
    return kRequiredVersion;
}

extern "C" __declspec(dllexport) void InitializeLogger(LOG_HANDLE *logger)
{
    UNREFERENCED_PARAMETER(logger);
}

extern "C" __declspec(dllexport) bool InitializePlugin(DWORD)
{
    LoadSettings();
    return true;
}

extern "C" __declspec(dllexport) void UninitializePlugin()
{
    DetachHook();
    UnsubclassHostWindow();
}

extern "C" __declspec(dllexport) COMMON_PLUGIN_TABLE *GetCommonPluginTable()
{
    return &g_common_plugin_table;
}

extern "C" __declspec(dllexport) void RegisterPlugin(HOST_APP_TABLE *host)
{
    if (host != nullptr && host->register_config_menu != nullptr) {
        host->register_config_menu(L"ループ再生プラグイン\\設定", ToggleLoopPlayback);
    }
    if (host != nullptr && host->register_edit_menu != nullptr) {
        host->register_edit_menu(L"ループ再生プラグイン\\設定", LoopPlaybackEditMenu);
    }
    SubclassHostWindow(host);
    AttachHook();
    ShowUnresolvedStartupDialog();
} //過去の実装直してないだけ

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID)
{
    if (DetourIsHelperProcess()) {
        return TRUE;
    }
    if (reason == DLL_PROCESS_ATTACH) {
        g_instance = hinst;
        DisableThreadLibraryCalls(hinst);
        DetourRestoreAfterWith();
    }
    return TRUE;
}
