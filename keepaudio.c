// keepaudio.c - Headless Windows app to keep audio interface "awake".
// Subsystem: WINDOWS (no console window). Optional --console to show one.
// Defaults: 1 Hz, -100 dBFS, 48000 Hz, mono, format auto (float32 if available).
//
// Features:
//   --freq F_HZ
//   --db NEG_DBFS
//   --rate SR
//   --device N
//   --channels 1|2
//   --frames PER_BUFFER
//   --buffers K
//   --format auto|pcm16|float32
//   --chance PERCENT         (1..100) random early exit before opening audio
//   --list-devices           (shows a MessageBox with the device list; or console if --console)
//   --install [--install-copy] [--startup-name Name]
//   --uninstall [--startup-name Name]
//   --console                (attach a console and print logs/debug messages)
//
// Build (MSVC):
//   cl /O2 /W4 keepaudio.c /link winmm.lib /SUBSYSTEM:WINDOWS /ENTRY:WinMainCRTStartup
//
// Build (MinGW):
//   gcc -O2 keepaudio.c -lwinmm -o keepaudio.exe -mwindows
//
// Notes:
// - When started at logon via HKCU\...\Run, this build produces no window.
// - --list-devices without --console shows a simple MessageBox listing devices.
// - Logs go to OutputDebugString by default (view with DebugView). With --console
//   we allocate a console and print there.

#define _CRT_SECURE_NO_WARNINGS
#define _USE_MATH_DEFINES
#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include <stdbool.h>

#pragma comment(lib, "winmm.lib")

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef enum
{
    FMT_AUTO = 0,
    FMT_PCM16 = 1,
    FMT_FLOAT32 = 2
} AudioFormat;

typedef struct
{
    double freq;        // Hz
    double db;          // dBFS (negative)
    int rate;           // sample rate (Hz)
    int deviceIndex;    // -1 = WAVE_MAPPER
    int channels;       // 1 or 2
    int bufferFrames;   // frames per buffer
    int numBuffers;     // number of buffers
    int chance;         // 0=disabled; else 1..100
    AudioFormat reqFmt; // requested format
    // Install/uninstall
    BOOL doInstall;
    BOOL doInstallCopy;
    BOOL doUninstall;
    wchar_t startupName[128];
    BOOL wantConsole;
} Options;

static volatile LONG g_running = 1; // global run flag
static HANDLE g_audioThread = NULL;
static HWAVEOUT g_hwo = NULL;
static WAVEHDR *g_headers = NULL;
static void **g_buffers = NULL;
static int g_numBuffers = 0;
static int g_bufBytes = 0;
static BOOL g_usingFloat = FALSE;
static int g_bufferFrames = 0;
static int g_channels = 1;
static double g_phase = 0.0, g_phaseStep = 0.0;
static double g_db = -100.0;
static int g_rate = 48000;

static void dlog(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    OutputDebugStringA(buf);
    if (GetConsoleWindow())
    {
        DWORD w;
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        if (h && h != INVALID_HANDLE_VALUE)
        {
            WriteConsoleA(h, buf, (DWORD)strlen(buf), &w, NULL);
        }
    }
}

static double clamp_double(double v, double lo, double hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}
static int clamp_int(int v, int lo, int hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}
static int parse_int(const char *s, int def)
{
    if (!s)
        return def;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0')
        return def;
    return (int)v;
}
static double parse_double(const char *s, double def)
{
    if (!s)
        return def;
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s || *end != '\0')
        return def;
    return v;
}
static const char *next_arg_value(int i, int argc, char **argv)
{
    if (i + 1 < argc)
        return argv[i + 1];
    return NULL;
}
static int str_eq_ci(const char *a, const char *b)
{
    return _stricmp(a, b) == 0;
}

static void list_devices_ui(BOOL console)
{
    UINT count = waveOutGetNumDevs();
    WAVEOUTCAPSW caps;
    if (console)
    {
        dlog("Playback devices:\n");
        for (UINT i = 0; i < count; ++i)
        {
            if (waveOutGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR)
            {
                char line[512];
                char name[512];
                WideCharToMultiByte(CP_UTF8, 0, caps.szPname, -1, name, sizeof(name), NULL, NULL);
                _snprintf(line, sizeof(line), "  [%u] %s\n", i, name);
                dlog("%s", line);
            }
        }
        if (count == 0)
            dlog("  (No waveOut devices found)\n");
    }
    else
    {
        wchar_t msg[4096];
        msg[0] = L'\0';
        wcscat(msg, L"Playback devices:\n");
        for (UINT i = 0; i < count; ++i)
        {
            if (waveOutGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR)
            {
                wchar_t line[512];
                _snwprintf(line, _countof(line), L"  [%u] %ls\n", i, caps.szPname);
                wcscat(msg, line);
            }
        }
        if (count == 0)
            wcscat(msg, L"  (No waveOut devices found)\n");
        MessageBoxW(NULL, msg, L"KeepAudio - Devices", MB_OK | MB_ICONINFORMATION);
    }
}

static BOOL get_exe_path_w(wchar_t *buf, DWORD cap)
{
    DWORD n = GetModuleFileNameW(NULL, buf, cap);
    return (n > 0 && n < cap);
}
static BOOL get_localappdata_w(wchar_t *buf, DWORD cap)
{
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, cap);
    return (n > 0 && n < cap);
}
static BOOL ensure_dir_exists_w(const wchar_t *path)
{
    if (CreateDirectoryW(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS)
        return TRUE;
    return FALSE;
}
static void wappend(wchar_t *dst, size_t cap, size_t *len, const wchar_t *src)
{
    while (*src && *len + 1 < cap)
    {
        dst[*len] = *src++;
        (*len)++;
    }
    dst[*len] = L'\0';
}
static void wappend_ch(wchar_t *dst, size_t cap, size_t *len, wchar_t ch)
{
    if (*len + 1 < cap)
    {
        dst[*len] = ch;
        (*len)++;
        dst[*len] = L'\0';
    }
}
static void append_quoted_arg_w(wchar_t *dst, size_t cap, size_t *len, const wchar_t *arg)
{
    BOOL needQuotes = wcspbrk(arg, L" \t\"") != NULL || arg[0] == L'\0';
    if (!needQuotes)
    {
        wappend(dst, cap, len, arg);
        return;
    }
    wappend_ch(dst, cap, len, L'"');
    for (const wchar_t *p = arg; *p; ++p)
    {
        if (*p == L'"')
        {
            wappend_ch(dst, cap, len, L'\\');
            wappend_ch(dst, cap, len, L'"');
        }
        else
            wappend_ch(dst, cap, len, *p);
    }
    wappend_ch(dst, cap, len, L'"');
}
static BOOL build_persisted_args_w(int argc, char **argv, wchar_t *out, size_t outcap)
{
    size_t len = 0;
    for (int i = 1; i < argc; ++i)
    {
        const char *a = argv[i];
        if (!a)
            continue;
        if (strcmp(a, "--install") == 0 || strcmp(a, "--install-copy") == 0 ||
            strcmp(a, "--uninstall") == 0 || strcmp(a, "--list-devices") == 0 ||
            strcmp(a, "--startup-name") == 0 || strcmp(a, "--console") == 0 ||
            strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0 || strcmp(a, "/?") == 0)
        {
            if (strcmp(a, "--startup-name") == 0 && i + 1 < argc && argv[i + 1] && argv[i + 1][0] != '-')
                ++i;
            continue;
        }
        // include known flags (+ value)
        if (strcmp(a, "--freq") == 0 || strcmp(a, "--db") == 0 || strcmp(a, "--rate") == 0 ||
            strcmp(a, "--device") == 0 || strcmp(a, "--channels") == 0 || strcmp(a, "--frames") == 0 ||
            strcmp(a, "--buffers") == 0 || strcmp(a, "--chance") == 0 || strcmp(a, "--format") == 0)
        {
            int w1 = MultiByteToWideChar(CP_UTF8, 0, a, -1, NULL, 0);
            wchar_t wflag[256] = {0};
            if (w1 > 0 && w1 < (int)_countof(wflag))
            {
                MultiByteToWideChar(CP_UTF8, 0, a, -1, wflag, w1);
                if (len)
                    wappend_ch(out, outcap, &len, L' ');
                append_quoted_arg_w(out, outcap, &len, wflag);
            }
            if (i + 1 < argc && argv[i + 1] && argv[i + 1][0] != '-')
            {
                const char *v = argv[++i];
                int w2 = MultiByteToWideChar(CP_UTF8, 0, v, -1, NULL, 0);
                wchar_t wval[512] = {0};
                if (w2 > 0 && w2 < (int)_countof(wval))
                {
                    MultiByteToWideChar(CP_UTF8, 0, v, -1, wval, w2);
                    wappend_ch(out, outcap, &len, L' ');
                    append_quoted_arg_w(out, outcap, &len, wval);
                }
            }
            continue;
        }
        int wlen = MultiByteToWideChar(CP_UTF8, 0, a, -1, NULL, 0);
        wchar_t warg[512] = {0};
        if (wlen > 0 && wlen < (int)_countof(warg))
        {
            MultiByteToWideChar(CP_UTF8, 0, a, -1, warg, wlen);
            if (len)
                wappend_ch(out, outcap, &len, L' ');
            append_quoted_arg_w(out, outcap, &len, warg);
        }
    }
    out[len] = L'\0';
    return TRUE;
}
static BOOL build_cmdline_for_run_w(const wchar_t *exePath, const wchar_t *args, wchar_t *out, size_t outcap)
{
    size_t len = 0;
    append_quoted_arg_w(out, outcap, &len, exePath);
    if (args && args[0])
    {
        wappend_ch(out, outcap, &len, L' ');
        wappend(out, outcap, &len, args);
    }
    out[len] = L'\0';
    return TRUE;
}
static BOOL reg_set_run_value_w(const wchar_t *name, const wchar_t *cmdline)
{
    HKEY hKey = NULL;
    LONG r = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                             0, NULL, 0, KEY_SET_VALUE, NULL, &hKey, NULL);
    if (r != ERROR_SUCCESS)
        return FALSE;
    r = RegSetValueExW(hKey, name, 0, REG_SZ, (const BYTE *)cmdline,
                       (DWORD)((lstrlenW(cmdline) + 1) * sizeof(wchar_t)));
    RegCloseKey(hKey);
    return (r == ERROR_SUCCESS);
}
static BOOL reg_delete_run_value_w(const wchar_t *name)
{
    HKEY hKey = NULL;
    LONG r = RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                           0, KEY_SET_VALUE, &hKey);
    if (r != ERROR_SUCCESS)
        return FALSE;
    r = RegDeleteValueW(hKey, name);
    RegCloseKey(hKey);
    return (r == ERROR_SUCCESS);
}

// RNG
static uint32_t g_rng_state = 1u;
static uint32_t rng_u32(void)
{
    uint32_t x = g_rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rng_state = x ? x : 0xA341316Cu;
    return g_rng_state;
}
static void rng_seed_from_system(void)
{
    LARGE_INTEGER qpc;
    FILETIME ft;
    ULONGLONG ticks = GetTickCount64();
    GetSystemTimeAsFileTime(&ft);
    QueryPerformanceCounter(&qpc);
    uint32_t s = (uint32_t)ticks ^ (uint32_t)(ticks >> 32);
    s ^= ft.dwLowDateTime ^ ft.dwHighDateTime;
    s ^= (uint32_t)qpc.LowPart ^ (uint32_t)qpc.HighPart;
    s ^= (uint32_t)(uintptr_t)&qpc;
    if (!s)
        s = 0xBEEF1234u;
    g_rng_state = s;
    for (int i = 0; i < 16; ++i)
        (void)rng_u32();
}
static int rng_roll_1_to_100(void)
{
    const uint32_t range = 100u;
    const uint32_t bound = (UINT32_MAX / range) * range;
    uint32_t r;
    do
    {
        r = rng_u32();
    } while (r >= bound);
    return (int)(r % range) + 1;
}

// Audio filling
static void fill_sine_i16(int16_t *out, int frames, int channels, double *phase, double step, int16_t amp)
{
    for (int i = 0; i < frames; ++i)
    {
        int16_t s = (int16_t)(sin(*phase) * amp);
        *phase += step;
        if (*phase >= 2 * M_PI)
            *phase -= 2 * M_PI;
        if (channels == 1)
            out[i] = s;
        else
        {
            out[2 * i] = s;
            out[2 * i + 1] = s;
        }
    }
}
static void fill_sine_f32(float *out, int frames, int channels, double *phase, double step, float amp)
{
    for (int i = 0; i < frames; ++i)
    {
        float s = (float)(sin(*phase) * amp);
        *phase += step;
        if (*phase >= 2 * M_PI)
            *phase -= 2 * M_PI;
        if (channels == 1)
            out[i] = s;
        else
        {
            out[2 * i] = s;
            out[2 * i + 1] = s;
        }
    }
}

// Audio worker thread
static DWORD WINAPI AudioThreadProc(LPVOID lp)
{
    double lin = pow(10.0, g_db / 20.0);
    int bytesPerFrame; // derived from opened format
    bytesPerFrame = (g_usingFloat ? (int)(g_channels * 4) : (int)(g_channels * 2));

    // Prime and loop: headers already prepared/written by main before thread start.
    while (InterlockedCompareExchange(&g_running, 1, 1))
    {
        for (int i = 0; i < g_numBuffers; ++i)
        {
            if (g_headers[i].dwFlags & WHDR_DONE)
            {
                if (g_usingFloat)
                {
                    float amp = (float)lin;
                    fill_sine_f32((float *)g_headers[i].lpData, g_bufferFrames, g_channels, &g_phase, g_phaseStep, amp);
                }
                else
                {
                    double scaled = lin * 32767.0;
                    if (scaled < 1.0)
                        scaled = 1.0;
                    if (scaled > 32767.0)
                        scaled = 32767.0;
                    int16_t amp = (int16_t)floor(scaled + 0.5);
                    fill_sine_i16((int16_t *)g_headers[i].lpData, g_bufferFrames, g_channels, &g_phase, g_phaseStep, amp);
                }
                MMRESULT mmr = waveOutWrite(g_hwo, &g_headers[i], sizeof(WAVEHDR));
                if (mmr != MMSYSERR_NOERROR)
                {
                    dlog("waveOutWrite failed in loop: %u\n", (unsigned)mmr);
                    InterlockedExchange(&g_running, 0);
                    break;
                }
            }
        }
        Sleep(5);
    }
    return 0;
}

// Hidden message window to catch WM_ENDSESSION cleanly
static LRESULT CALLBACK HiddenWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_ENDSESSION:
        InterlockedExchange(&g_running, 0);
        return 0;
    case WM_CLOSE:
    case WM_QUIT:
        InterlockedExchange(&g_running, 0);
        return 0;
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

static void parse_options(int argc, char **argv, Options *opt, BOOL *listOnly)
{
    opt->freq = 1.0;
    opt->db = -100.0;
    opt->rate = 48000;
    opt->deviceIndex = -1;
    opt->channels = 1;
    opt->bufferFrames = 1024;
    opt->numBuffers = 8;
    opt->chance = 0;
    opt->reqFmt = FMT_AUTO;
    opt->doInstall = FALSE;
    opt->doInstallCopy = FALSE;
    opt->doUninstall = FALSE;
    opt->wantConsole = FALSE;
    *listOnly = FALSE;
    wcsncpy(opt->startupName, L"KeepAudio", _countof(opt->startupName));
    opt->startupName[_countof(opt->startupName) - 1] = L'\0';

    for (int i = 1; i < argc; ++i)
    {
        const char *a = argv[i];
        if (!a)
            continue;
        if (str_eq_ci(a, "--console"))
        {
            opt->wantConsole = TRUE;
            continue;
        }
        if (str_eq_ci(a, "--list-devices"))
        {
            *listOnly = TRUE;
            continue;
        }
        if (str_eq_ci(a, "--help") || str_eq_ci(a, "-h") || str_eq_ci(a, "/?"))
        {
            *listOnly = TRUE;
            continue;
        } // show devices/usage via console/MB
        if (str_eq_ci(a, "--install"))
        {
            opt->doInstall = TRUE;
            continue;
        }
        if (str_eq_ci(a, "--install-copy"))
        {
            opt->doInstallCopy = TRUE;
            continue;
        }
        if (str_eq_ci(a, "--uninstall"))
        {
            opt->doUninstall = TRUE;
            continue;
        }
        if (str_eq_ci(a, "--startup-name"))
        {
            const char *v = next_arg_value(i, argc, argv);
            if (v)
            {
                int w = MultiByteToWideChar(CP_UTF8, 0, v, -1, opt->startupName, (int)_countof(opt->startupName));
                if (w > 0)
                    opt->startupName[w - 1] = L'\0';
            }
            ++i;
            continue;
        }
        if (str_eq_ci(a, "--freq"))
        {
            opt->freq = parse_double(next_arg_value(i, argc, argv), opt->freq);
            ++i;
            continue;
        }
        if (str_eq_ci(a, "--db"))
        {
            opt->db = parse_double(next_arg_value(i, argc, argv), opt->db);
            ++i;
            continue;
        }
        if (str_eq_ci(a, "--rate"))
        {
            opt->rate = parse_int(next_arg_value(i, argc, argv), opt->rate);
            ++i;
            continue;
        }
        if (str_eq_ci(a, "--device"))
        {
            opt->deviceIndex = parse_int(next_arg_value(i, argc, argv), opt->deviceIndex);
            ++i;
            continue;
        }
        if (str_eq_ci(a, "--channels"))
        {
            opt->channels = parse_int(next_arg_value(i, argc, argv), opt->channels);
            ++i;
            continue;
        }
        if (str_eq_ci(a, "--frames"))
        {
            opt->bufferFrames = parse_int(next_arg_value(i, argc, argv), opt->bufferFrames);
            ++i;
            continue;
        }
        if (str_eq_ci(a, "--buffers"))
        {
            opt->numBuffers = parse_int(next_arg_value(i, argc, argv), opt->numBuffers);
            ++i;
            continue;
        }
        if (str_eq_ci(a, "--chance"))
        {
            opt->chance = parse_int(next_arg_value(i, argc, argv), opt->chance);
            ++i;
            continue;
        }
        if (str_eq_ci(a, "--format"))
        {
            const char *v = next_arg_value(i, argc, argv);
            ++i;
            if (v)
            {
                if (str_eq_ci(v, "auto"))
                    opt->reqFmt = FMT_AUTO;
                else if (str_eq_ci(v, "pcm16"))
                    opt->reqFmt = FMT_PCM16;
                else if (str_eq_ci(v, "float32"))
                    opt->reqFmt = FMT_FLOAT32;
            }
            continue;
        }
    }
    opt->freq = clamp_double(opt->freq, 0.1, 2000.0);
    opt->db = clamp_double(opt->db, -150.0, -10.0);
    if (opt->rate < 8000)
        opt->rate = 8000;
    if (opt->rate > 192000)
        opt->rate = 192000;
    if (opt->channels != 1 && opt->channels != 2)
        opt->channels = 1;
    opt->bufferFrames = clamp_int(opt->bufferFrames, 128, 8192);
    opt->numBuffers = clamp_int(opt->numBuffers, 2, 32);
    if (opt->chance != 0)
        opt->chance = clamp_int(opt->chance, 1, 100);
}

static void install_startup(int argc, char **argv, const Options *opt)
{
    wchar_t exePath[MAX_PATH] = {0};
    if (!get_exe_path_w(exePath, _countof(exePath)))
    {
        dlog("Install: cannot get exe path\n");
        return;
    }

    wchar_t useExe[MAX_PATH] = {0};
    wcscpy(useExe, exePath);

    if (opt->doInstallCopy)
    {
        wchar_t lad[MAX_PATH] = {0}, targetDir[MAX_PATH] = {0}, targetExe[MAX_PATH] = {0};
        if (!get_localappdata_w(lad, _countof(lad)))
        {
            dlog("Install: cannot resolve LOCALAPPDATA\n");
            return;
        }
        _snwprintf(targetDir, _countof(targetDir), L"%ls\\KeepAudio", lad);
        if (!ensure_dir_exists_w(targetDir))
        {
            dlog("Install: cannot create dir\n");
            return;
        }
        _snwprintf(targetExe, _countof(targetExe), L"%ls\\keepaudio.exe", targetDir);
        if (!CopyFileW(exePath, targetExe, FALSE))
        {
            dlog("Install: copy failed (%lu)\n", GetLastError());
            return;
        }
        wcsncpy(useExe, targetExe, _countof(useExe));
        useExe[_countof(useExe) - 1] = L'\0';
    }

    wchar_t args[8192] = {0};
    build_persisted_args_w(argc, argv, args, _countof(args));

    wchar_t cmdline[9000] = {0};
    build_cmdline_for_run_w(useExe, args, cmdline, _countof(cmdline));

    if (reg_set_run_value_w(opt->startupName, cmdline))
    {
        dlog("Startup entry set: %ls\n", cmdline);
    }
    else
    {
        dlog("Install: failed to set Run value\n");
    }
}

static void uninstall_startup(const Options *opt)
{
    if (reg_delete_run_value_w(opt->startupName))
    {
        dlog("Startup entry removed: %ls\n", opt->startupName);
    }
    else
    {
        dlog("Uninstall: entry not found or failed\n");
    }
    wchar_t lad[MAX_PATH] = {0}, targetDir[MAX_PATH] = {0}, targetExe[MAX_PATH] = {0};
    if (get_localappdata_w(lad, _countof(lad)))
    {
        _snwprintf(targetDir, _countof(targetDir), L"%ls\\KeepAudio", lad);
        _snwprintf(targetExe, _countof(targetExe), L"%ls\\KeepAudio\\keepaudio.exe", lad);
        DeleteFileW(targetExe);
        RemoveDirectoryW(targetDir);
    }
}

static BOOL open_audio(const Options *opt)
{
    // Decide format
    AudioFormat useFmt = opt->reqFmt;
    if (useFmt == FMT_AUTO)
        useFmt = (opt->db <= -96.0) ? FMT_FLOAT32 : FMT_PCM16;

    UINT deviceId = (opt->deviceIndex < 0) ? WAVE_MAPPER : (UINT)opt->deviceIndex;
    WAVEFORMATEX wfx = {0};
    MMRESULT mmr;

    g_rate = opt->rate;
    g_channels = opt->channels;
    g_phase = 0.0;
    g_phaseStep = 2.0 * M_PI * opt->freq / (double)opt->rate;
    g_db = opt->db;
    g_bufferFrames = opt->bufferFrames;

    if (useFmt == FMT_FLOAT32)
    {
        wfx.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        wfx.nChannels = (WORD)opt->channels;
        wfx.nSamplesPerSec = (DWORD)opt->rate;
        wfx.wBitsPerSample = 32;
        wfx.nBlockAlign = (WORD)(wfx.nChannels * (wfx.wBitsPerSample / 8));
        wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
        mmr = waveOutOpen(&g_hwo, deviceId, &wfx, 0, 0, CALLBACK_NULL);
        if (mmr == MMSYSERR_NOERROR)
        {
            g_usingFloat = TRUE;
        }
        else
        {
            // Fallback to PCM16
            wfx.wFormatTag = WAVE_FORMAT_PCM;
            wfx.wBitsPerSample = 16;
            wfx.nBlockAlign = (WORD)(wfx.nChannels * (wfx.wBitsPerSample / 8));
            wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
            mmr = waveOutOpen(&g_hwo, deviceId, &wfx, 0, 0, CALLBACK_NULL);
            if (mmr != MMSYSERR_NOERROR)
                return FALSE;
            g_usingFloat = FALSE;
        }
    }
    else
    {
        wfx.wFormatTag = WAVE_FORMAT_PCM;
        wfx.nChannels = (WORD)opt->channels;
        wfx.nSamplesPerSec = (DWORD)opt->rate;
        wfx.wBitsPerSample = 16;
        wfx.nBlockAlign = (WORD)(wfx.nChannels * (wfx.wBitsPerSample / 8));
        wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
        mmr = waveOutOpen(&g_hwo, deviceId, &wfx, 0, 0, CALLBACK_NULL);
        if (mmr != MMSYSERR_NOERROR && opt->reqFmt == FMT_AUTO)
        {
            // try float
            wfx.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
            wfx.wBitsPerSample = 32;
            wfx.nBlockAlign = (WORD)(wfx.nChannels * (wfx.wBitsPerSample / 8));
            wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
            mmr = waveOutOpen(&g_hwo, deviceId, &wfx, 0, 0, CALLBACK_NULL);
            if (mmr == MMSYSERR_NOERROR)
                g_usingFloat = TRUE;
        }
        if (mmr != MMSYSERR_NOERROR)
            return FALSE;
    }

    // Allocate buffers/headers and prime
    g_numBuffers = opt->numBuffers;
    int bytesPerFrame = (g_usingFloat ? (int)(g_channels * 4) : (int)(g_channels * 2));
    g_bufBytes = g_bufferFrames * bytesPerFrame;

    g_buffers = (void **)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(void *) * g_numBuffers);
    g_headers = (WAVEHDR *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(WAVEHDR) * g_numBuffers);
    if (!g_buffers || !g_headers)
        return FALSE;

    double lin = pow(10.0, g_db / 20.0);

    for (int i = 0; i < g_numBuffers; ++i)
    {
        g_buffers[i] = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, g_bufBytes);
        if (!g_buffers[i])
            return FALSE;
        ZeroMemory(&g_headers[i], sizeof(WAVEHDR));
        g_headers[i].lpData = (LPSTR)g_buffers[i];
        g_headers[i].dwBufferLength = (DWORD)g_bufBytes;

        if (g_usingFloat)
        {
            float amp = (float)lin;
            fill_sine_f32((float *)g_buffers[i], g_bufferFrames, g_channels, &g_phase, g_phaseStep, amp);
        }
        else
        {
            double scaled = lin * 32767.0;
            if (scaled < 1.0)
                scaled = 1.0;
            if (scaled > 32767.0)
                scaled = 32767.0;
            int16_t amp = (int16_t)floor(scaled + 0.5);
            fill_sine_i16((int16_t *)g_buffers[i], g_bufferFrames, g_channels, &g_phase, g_phaseStep, amp);
        }

        MMRESULT mmr2 = waveOutPrepareHeader(g_hwo, &g_headers[i], sizeof(WAVEHDR));
        if (mmr2 != MMSYSERR_NOERROR)
            return FALSE;
        mmr2 = waveOutWrite(g_hwo, &g_headers[i], sizeof(WAVEHDR));
        if (mmr2 != MMSYSERR_NOERROR)
            return FALSE;
    }
    return TRUE;
}

static void close_audio(void)
{
    if (g_hwo)
    {
        waveOutReset(g_hwo);
        for (int i = 0; i < g_numBuffers; ++i)
        {
            int spins = 0;
            while (!(g_headers[i].dwFlags & WHDR_DONE) && spins++ < 200)
                Sleep(5);
            waveOutUnprepareHeader(g_hwo, &g_headers[i], sizeof(WAVEHDR));
            if (g_buffers && g_buffers[i])
                HeapFree(GetProcessHeap(), 0, g_buffers[i]);
        }
        if (g_buffers)
            HeapFree(GetProcessHeap(), 0, g_buffers);
        if (g_headers)
            HeapFree(GetProcessHeap(), 0, g_headers);
        g_buffers = NULL;
        g_headers = NULL;
        waveOutClose(g_hwo);
        g_hwo = NULL;
    }
}

static int show_usage(BOOL console)
{
    const char *txt =
        "KeepAudio (headless) - keep USB audio interface awake with a near-inaudible tone\n"
        "Flags:\n"
        "  --freq F_HZ  --db NEG_DBFS  --rate SR  --device N  --channels 1|2\n"
        "  --frames N   --buffers K    --format auto|pcm16|float32\n"
        "  --chance P   --list-devices --install [--install-copy] [--startup-name Name]\n"
        "  --uninstall  --console\n";
    if (console)
        dlog("%s", txt);
    else
        MessageBoxA(NULL, txt, "KeepAudio - Help", MB_OK | MB_ICONINFORMATION);
    return 0;
}

int APIENTRY WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmdLine, int nShowCmd)
{
    int argc = 0;
    LPWSTR *argvw = CommandLineToArgvW(GetCommandLineW(), &argc);
    char **argv = (char **)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(char *) * (argc > 0 ? argc : 1));
    if (!argvw || !argv)
        return 1;
    for (int i = 0; i < argc; ++i)
    {
        int len = WideCharToMultiByte(CP_UTF8, 0, argvw[i], -1, NULL, 0, NULL, NULL);
        argv[i] = (char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, len);
        WideCharToMultiByte(CP_UTF8, 0, argvw[i], -1, argv[i], len, NULL, NULL);
    }

    Options opt;
    BOOL listOnly = FALSE;
    parse_options(argc, argv, &opt, &listOnly);

    if (opt.wantConsole)
    {
        AllocConsole();
        FILE *f;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
        freopen_s(&f, "CONIN$", "r", stdin);
    }

    if (listOnly)
    {
        // If user asked for --help specifically, show usage. If --list-devices, list them.
        BOOL askedHelp = FALSE;
        for (int i = 1; i < argc; ++i)
        {
            if (_stricmp(argv[i], "--help") == 0 || _stricmp(argv[i], "-h") == 0 || _stricmp(argv[i], "/?") == 0)
            {
                askedHelp = TRUE;
                break;
            }
        }
        if (askedHelp)
        {
            show_usage(opt.wantConsole);
        }
        else
        {
            list_devices_ui(opt.wantConsole);
        }
        goto cleanup;
    }

    if (opt.doUninstall)
    {
        uninstall_startup(&opt);
        goto cleanup;
    }
    if (opt.doInstall || opt.doInstallCopy)
    {
        install_startup(argc, argv, &opt);
        goto cleanup;
    }

    if (opt.chance > 0)
    {
        rng_seed_from_system();
        int roll = rng_roll_1_to_100();
        if (roll <= opt.chance)
            goto cleanup; // early silent exit
    }

    // Register hidden window class and create a message-only window.
    const wchar_t *clsName = L"KeepAudioHiddenClass";
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = HiddenWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = clsName;
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, clsName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hInst, NULL);

    // Open audio + prime buffers
    if (!open_audio(&opt))
    {
        dlog("Audio open failed. Try different --rate/--channels/--device or --format.\n");
        goto cleanup;
    }

    // Start audio worker
    g_audioThread = CreateThread(NULL, 0, AudioThreadProc, NULL, 0, NULL);

    // Pump messages until shutdown/logoff/quit
    MSG msg;
    while (InterlockedCompareExchange(&g_running, 1, 1))
    {
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
                InterlockedExchange(&g_running, 0);
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        Sleep(10);
    }

    // Stop thread + audio
    if (g_audioThread)
    {
        WaitForSingleObject(g_audioThread, 2000);
        CloseHandle(g_audioThread);
        g_audioThread = NULL;
    }
    close_audio();

cleanup:
    if (opt.wantConsole)
    {
        dlog("KeepAudio exiting.\n");
        Sleep(200);
        FreeConsole();
    }
    for (int i = 0; i < argc; ++i)
        if (argv[i])
            HeapFree(GetProcessHeap(), 0, argv[i]);
    if (argv)
        HeapFree(GetProcessHeap(), 0, argv);
    if (argvw)
        LocalFree(argvw);
    return 0;
}
