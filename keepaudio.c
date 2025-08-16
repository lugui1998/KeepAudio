// gcc -O2 keepaudio.c -lwinmm -o keepaudio.exe

// keepaudio.c - Keep USB audio "awake" by playing a very low-level tone.
// Build (MSVC):  cl /O2 /W4 keepaudio.c /link winmm.lib
// Build (MinGW): gcc -O2 keepaudio.c -lwinmm -o keepaudio.exe
//
// Usage examples:
//   keepaudio.exe
//   keepaudio.exe --freq 25 --db -70
//   keepaudio.exe --rate 44100 --device 0
//   keepaudio.exe --list-devices
//
// Press Ctrl+C to stop.

#define _CRT_SECURE_NO_WARNINGS
#define _USE_MATH_DEFINES
#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <stdbool.h>

#pragma comment(lib, "winmm.lib")

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    double freq;          // Hz
    double db;            // dBFS (negative)
    int    rate;          // sample rate (Hz)
    int    deviceIndex;   // -1 = WAVE_MAPPER
    int    channels;      // 1 = mono
    int    bufferFrames;  // frames per buffer
    int    numBuffers;    // number of buffers
} Options;

static volatile LONG g_running = 1;

BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) {
    switch (ctrlType) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        InterlockedExchange(&g_running, 0);
        return TRUE;
    default:
        return FALSE;
    }
}

static void print_usage(void) {
    printf(
        "keepaudio - keep USB audio interface active by playing a very low-level tone\n"
        "Usage:\n"
        "  keepaudio.exe [--freq F_HZ] [--db NEG_DBFS] [--rate SR] [--device N]\n"
        "                [--buffers K] [--frames PER_BUFFER] [--channels 1|2]\n"
        "  keepaudio.exe --list-devices\n"
        "\n"
        "Defaults: --freq 1 --db -100 --rate 48000 --channels 1 --buffers 8 --frames 1024\n"
        "Notes:\n"
        "  * If your device still powers down, try a slightly higher level (e.g., --db -65).\n"
        "  * Use --list-devices to see indices, then --device N to pick one.\n"
        "  * Press Ctrl+C to stop.\n"
    );
}

static void list_devices(void) {
    UINT count = waveOutGetNumDevs();
    WAVEOUTCAPSW caps;
    printf("Playback devices:\n");
    for (UINT i = 0; i < count; ++i) {
        if (waveOutGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
            wprintf(L"  [%u] %ls\n", i, caps.szPname);
        }
    }
    if (count == 0) {
        printf("  (No waveOut devices found)\n");
    }
}

static double clamp_double(double v, double lo, double hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int parse_int(const char* s, int def) {
    if (!s) return def;
    char* end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0') return def;
    return (int)v;
}

static double parse_double(const char* s, double def) {
    if (!s) return def;
    char* end = NULL;
    double v = strtod(s, &end);
    if (end == s || *end != '\0') return def;
    return v;
}

static const char* next_arg_value(int i, int argc, char** argv) {
    if (i + 1 < argc) return argv[i + 1];
    return NULL;
}

static void parse_options(int argc, char** argv, Options* opt, bool* listOnly) {
    // Defaults
    opt->freq = 1.0;
    opt->db = -100.0;
    opt->rate = 48000;
    opt->deviceIndex = -1;     // WAVE_MAPPER
    opt->channels = 1;         // mono
    opt->bufferFrames = 1024;
    opt->numBuffers = 8;
    *listOnly = false;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0 || strcmp(a, "/?") == 0) {
            print_usage();
            exit(0);
        } else if (strcmp(a, "--list-devices") == 0) {
            *listOnly = true;
        } else if (strcmp(a, "--freq") == 0) {
            opt->freq = parse_double(next_arg_value(i, argc, argv), opt->freq);
            ++i;
        } else if (strcmp(a, "--db") == 0) {
            opt->db = parse_double(next_arg_value(i, argc, argv), opt->db);
            ++i;
        } else if (strcmp(a, "--rate") == 0) {
            opt->rate = parse_int(next_arg_value(i, argc, argv), opt->rate);
            ++i;
        } else if (strcmp(a, "--device") == 0) {
            opt->deviceIndex = parse_int(next_arg_value(i, argc, argv), opt->deviceIndex);
            ++i;
        } else if (strcmp(a, "--channels") == 0) {
            opt->channels = parse_int(next_arg_value(i, argc, argv), opt->channels);
            ++i;
        } else if (strcmp(a, "--frames") == 0) {
            opt->bufferFrames = parse_int(next_arg_value(i, argc, argv), opt->bufferFrames);
            ++i;
        } else if (strcmp(a, "--buffers") == 0) {
            opt->numBuffers = parse_int(next_arg_value(i, argc, argv), opt->numBuffers);
            ++i;
        } else {
            // Unknown flag: ignore silently
        }
    }

    // Sanity clamps
    opt->freq = clamp_double(opt->freq, 1.0, 2000.0);
    opt->db = clamp_double(opt->db, -120.0, -10.0);
    if (opt->rate < 8000) opt->rate = 8000;
    if (opt->rate > 192000) opt->rate = 192000;
    if (opt->channels != 1 && opt->channels != 2) opt->channels = 1;
    if (opt->bufferFrames < 128) opt->bufferFrames = 128;
    if (opt->bufferFrames > 8192) opt->bufferFrames = 8192;
    if (opt->numBuffers < 2) opt->numBuffers = 2;
    if (opt->numBuffers > 32) opt->numBuffers = 32;
}

static void fill_sine_i16(int16_t* out, int frames, int channels, double* phase, double phaseStep, int16_t amp) {
    // Generate mono sine, duplicate to channels if needed.
    for (int i = 0; i < frames; ++i) {
        int16_t s = (int16_t)(sin(*phase) * amp);
        *phase += phaseStep;
        if (*phase >= 2.0 * M_PI) *phase -= 2.0 * M_PI;
        if (channels == 1) {
            out[i] = s;
        } else {
            out[2 * i + 0] = s;
            out[2 * i + 1] = s;
        }
    }
}

int main(int argc, char** argv) {
    Options opt;
    bool listOnly = false;
    parse_options(argc, argv, &opt, &listOnly);

    if (listOnly) {
        list_devices();
        return 0;
    }

    if (!SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE)) {
        fprintf(stderr, "Warning: failed to install Ctrl+C handler.\n");
    }

    // Convert dBFS to linear amplitude for 16-bit PCM.
    // db is negative, e.g., -75 dB => linear ~ 10^(-75/20) ≈ 0.0001778
    double lin = pow(10.0, opt.db / 20.0);
    double scaled = lin * 32767.0;
    if (scaled < 1.0) scaled = 1.0; // ensure it's not exactly zero (some devices treat pure zero as silence)
    if (scaled > 32767.0) scaled = 32767.0;
    int16_t amp = (int16_t)floor(scaled + 0.5);

    // Prepare wave format
    WAVEFORMATEX wfx;
    ZeroMemory(&wfx, sizeof(wfx));
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = (WORD)opt.channels;
    wfx.nSamplesPerSec = (DWORD)opt.rate;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = (WORD)(wfx.nChannels * (wfx.wBitsPerSample / 8));
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    // Open device
    HWAVEOUT hwo = NULL;
    MMRESULT mmr;
    UINT deviceId = (opt.deviceIndex < 0) ? WAVE_MAPPER : (UINT)opt.deviceIndex;
    mmr = waveOutOpen(&hwo, deviceId, &wfx, 0, 0, CALLBACK_NULL);
    if (mmr != MMSYSERR_NOERROR) {
        fprintf(stderr, "Error: waveOutOpen failed (code %u). Try a different --rate/--channels/--device.\n", (unsigned)mmr);
        fprintf(stderr, "Tip: run with --list-devices to see available outputs.\n");
        return 1;
    }

    // Allocate buffers
    const int bytesPerFrame = wfx.nBlockAlign;
    const int bufBytes = opt.bufferFrames * bytesPerFrame;

    int16_t** buffers = (int16_t**)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(int16_t*) * opt.numBuffers);
    WAVEHDR* headers = (WAVEHDR*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(WAVEHDR) * opt.numBuffers);
    if (!buffers || !headers) {
        fprintf(stderr, "Error: memory allocation failed.\n");
        if (hwo) waveOutClose(hwo);
        return 1;
    }

    // Tone generator state
    double phase = 0.0;
    double phaseStep = 2.0 * M_PI * opt.freq / (double)opt.rate;

    // Prepare + prime the queue
    for (int i = 0; i < opt.numBuffers; ++i) {
        buffers[i] = (int16_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bufBytes);
        if (!buffers[i]) {
            fprintf(stderr, "Error: buffer allocation failed.\n");
            InterlockedExchange(&g_running, 0);
            break;
        }
        ZeroMemory(&headers[i], sizeof(WAVEHDR));
        headers[i].lpData = (LPSTR)buffers[i];
        headers[i].dwBufferLength = (DWORD)bufBytes;

        // Optional gentle fade-in on the very first buffers to avoid a start pop.
        fill_sine_i16(buffers[i], opt.bufferFrames, opt.channels, &phase, phaseStep, amp);

        mmr = waveOutPrepareHeader(hwo, &headers[i], sizeof(WAVEHDR));
        if (mmr != MMSYSERR_NOERROR) {
            fprintf(stderr, "Error: waveOutPrepareHeader failed (code %u).\n", (unsigned)mmr);
            InterlockedExchange(&g_running, 0);
            break;
        }
        mmr = waveOutWrite(hwo, &headers[i], sizeof(WAVEHDR));
        if (mmr != MMSYSERR_NOERROR) {
            fprintf(stderr, "Error: waveOutWrite failed (code %u).\n", (unsigned)mmr);
            InterlockedExchange(&g_running, 0);
            break;
        }
    }

    printf("Playing %.2f Hz at %.1f dBFS, %d Hz, %s, device=%s\n",
           opt.freq, opt.db, opt.rate,
           (opt.channels == 1 ? "mono" : "stereo"),
           (opt.deviceIndex < 0 ? "default" : "selected"));

    printf("Press Ctrl+C to stop...\n");

    // Main refill loop
    while (InterlockedCompareExchange(&g_running, 1, 1)) {
        for (int i = 0; i < opt.numBuffers; ++i) {
            if (headers[i].dwFlags & WHDR_DONE) {
                // Refill and requeue
                fill_sine_i16((int16_t*)headers[i].lpData, opt.bufferFrames, opt.channels, &phase, phaseStep, amp);
                headers[i].dwFlags &= ~WHDR_DONE; // we'll re-prepare to be safe
                mmr = waveOutWrite(hwo, &headers[i], sizeof(WAVEHDR));
                if (mmr != MMSYSERR_NOERROR) {
                    fprintf(stderr, "Error: waveOutWrite failed during loop (code %u).\n", (unsigned)mmr);
                    InterlockedExchange(&g_running, 0);
                    break;
                }
            }
        }
        Sleep(5); // very light polling
    }

    // Shutdown: stop and clean up
    waveOutReset(hwo);

    // Wait for buffers to be marked done, then unprepare and free
    for (int i = 0; i < opt.numBuffers; ++i) {
        int spins = 0;
        while (!(headers[i].dwFlags & WHDR_DONE) && spins++ < 200) {
            Sleep(5);
        }
        waveOutUnprepareHeader(hwo, &headers[i], sizeof(WAVEHDR));
        if (buffers[i]) HeapFree(GetProcessHeap(), 0, buffers[i]);
    }
    HeapFree(GetProcessHeap(), 0, buffers);
    HeapFree(GetProcessHeap(), 0, headers);
    waveOutClose(hwo);

    printf("Stopped.\n");
    return 0;
}
