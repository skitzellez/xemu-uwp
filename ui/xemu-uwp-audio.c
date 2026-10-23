/*
 * Audio output for the Xbox (UWP, Dev Mode) build of xemu: plays the APU's
 * SDL_AudioStream through XAudio2. See xemu-uwp-audio.h.
 *
 * Copyright (c) 2026 Skitzelles (https://github.com/skitzellez/xemu-uwp)
 * Licensed under the GNU GPL, version 2 or later, like the rest of xemu.
 */

#ifdef XEMU_UWP

#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <xaudio2.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#include "xemu-uwp-audio.h"

void uwp_log(const char *fmt, ...);

#define SAMPLE_RATE  48000
#define CHANNELS     2
#define FRAME_BYTES  (CHANNELS * 2)
#define CHUNK_FRAMES 1024          /* ~21 ms per buffer */
#define NUM_BUFFERS  8
#define MAX_QUEUED   4             /* keep ~85 ms queued: low latency, no gaps */

typedef HRESULT (WINAPI *XAudio2Create_fn)(IXAudio2 **, UINT32, XAUDIO2_PROCESSOR);

static SDL_AudioStream *stream;
/* for the log (ui/xemu.c): times the sound output ran dry, i.e. the game did not make sound fast enough */
volatile unsigned xemu_uwp_audio_dry;
static IXAudio2SourceVoice *voice;
/* headset (hw/xbox/xblc.c): the game's voice output on a second voice, 48 kHz mono */
SDL_AudioStream *xemu_uwp_headset_out;
SDL_AudioStream *xemu_uwp_headset_in;
static IXAudio2SourceVoice *headset_voice;
#define HEADSET_RATE 48000
#define HEADSET_CHUNK 480          /* 10 ms */
static Uint8 headset_buffers[NUM_BUFFERS][HEADSET_CHUNK * 2];
static Uint8 buffers[NUM_BUFFERS][CHUNK_FRAMES * FRAME_BYTES];

static DWORD WINAPI feeder_thread(LPVOID unused)
{
    int next = 0;
    Uint64 next_report = SDL_GetTicks() + 2000;
    uwp_log("audio: feeder thread started");

    for (;;) {
        XAUDIO2_VOICE_STATE state;
        IXAudio2SourceVoice_GetState(voice, &state, 0);
        if (SDL_GetTicks() >= next_report) {
            uwp_log("audio: stream %d bytes, xaudio queued %u, played %llu samples",
                    SDL_GetAudioStreamAvailable(stream), state.BuffersQueued,
                    (unsigned long long)state.SamplesPlayed);
            next_report += 2000;
        }
        int avail = SDL_GetAudioStreamAvailable(stream);
        static UINT32 last_queued = 1;
        if (state.BuffersQueued == 0 && last_queued != 0 && state.SamplesPlayed) {
            xemu_uwp_audio_dry++;
        }
        last_queued = state.BuffersQueued;
        if (headset_voice) {
            static int hnext;
            XAUDIO2_VOICE_STATE hs;
            IXAudio2SourceVoice_GetState(headset_voice, &hs, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            int havail = SDL_GetAudioStreamAvailable(xemu_uwp_headset_out);
            if (hs.BuffersQueued < MAX_QUEUED && havail >= (int)sizeof(headset_buffers[0])) {
                int hgot = SDL_GetAudioStreamData(xemu_uwp_headset_out, headset_buffers[hnext],
                                                  sizeof(headset_buffers[0]));
                if (hgot > 0) {
                    XAUDIO2_BUFFER hb = { 0 };
                    hb.AudioBytes = hgot;
                    hb.pAudioData = headset_buffers[hnext];
                    IXAudio2SourceVoice_SubmitSourceBuffer(headset_voice, &hb, NULL);
                    hnext = (hnext + 1) % NUM_BUFFERS;
                }
            }
        }
        if (state.BuffersQueued >= MAX_QUEUED || avail < FRAME_BYTES) {
            SDL_Delay(2);
            continue;
        }
        /* take what is there (up to one chunk): never wait on the APU, which
         * itself waits for the stream to drain */
        int want = avail < (int)sizeof(buffers[0]) ? avail - avail % FRAME_BYTES
                                                  : (int)sizeof(buffers[0]);
        int got = SDL_GetAudioStreamData(stream, buffers[next], want);
        if (got <= 0) {
            SDL_Delay(2);
            continue;
        }
        XAUDIO2_BUFFER buf = { 0 };
        buf.AudioBytes = got;
        buf.pAudioData = buffers[next];
        IXAudio2SourceVoice_SubmitSourceBuffer(voice, &buf, NULL);
        next = (next + 1) % NUM_BUFFERS;
    }
    return 0;
}

SDL_AudioStream *xemu_uwp_audio_start(void)
{
    const SDL_AudioSpec spec = { SDL_AUDIO_S16LE, CHANNELS, SAMPLE_RATE };
    IXAudio2 *xaudio = NULL;
    IXAudio2MasteringVoice *master = NULL;
    HRESULT hr;

    HMODULE lib = LoadLibraryExW(L"xaudio2_9.dll", NULL, 0);
    XAudio2Create_fn create = lib ? (XAudio2Create_fn)(void *)GetProcAddress(lib, "XAudio2Create") : NULL;
    if (!create) {
        uwp_log("audio: xaudio2_9.dll not available (error %lu)", GetLastError());
        return NULL;
    }
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    hr = create(&xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr)) {
        uwp_log("audio: XAudio2Create failed 0x%08lx", hr);
        return NULL;
    }
    hr = IXAudio2_CreateMasteringVoice(xaudio, &master, XAUDIO2_DEFAULT_CHANNELS,
                                       XAUDIO2_DEFAULT_SAMPLERATE, 0, NULL, NULL,
                                       AudioCategory_GameEffects);
    if (FAILED(hr)) {
        uwp_log("audio: CreateMasteringVoice failed 0x%08lx", hr);
        return NULL;
    }
    WAVEFORMATEX wfx = { 0 };
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = CHANNELS;
    wfx.nSamplesPerSec = SAMPLE_RATE;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = FRAME_BYTES;
    wfx.nAvgBytesPerSec = SAMPLE_RATE * FRAME_BYTES;
    hr = IXAudio2_CreateSourceVoice(xaudio, &voice, &wfx, 0,
                                    XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL);
    if (FAILED(hr)) {
        uwp_log("audio: CreateSourceVoice failed 0x%08lx", hr);
        return NULL;
    }
    stream = SDL_CreateAudioStream(&spec, &spec);
    if (!stream) {
        uwp_log("audio: SDL_CreateAudioStream failed: %s", SDL_GetError());
        return NULL;
    }
    IXAudio2SourceVoice_Start(voice, 0, XAUDIO2_COMMIT_NOW);
    /* the headset's sound: the game writes at its own rate, XBLC sets the input format */
    const SDL_AudioSpec hspec = { SDL_AUDIO_S16LE, 1, HEADSET_RATE };
    WAVEFORMATEX hwfx = wfx;
    hwfx.nChannels = 1;
    hwfx.nSamplesPerSec = HEADSET_RATE;
    hwfx.nBlockAlign = 2;
    hwfx.nAvgBytesPerSec = HEADSET_RATE * 2;
    xemu_uwp_headset_out = SDL_CreateAudioStream(&hspec, &hspec);
    xemu_uwp_headset_in = SDL_CreateAudioStream(&hspec, &hspec);
    if (xemu_uwp_headset_out &&
        SUCCEEDED(IXAudio2_CreateSourceVoice(xaudio, &headset_voice, &hwfx, 0,
                                             XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL))) {
        IXAudio2SourceVoice_Start(headset_voice, 0, XAUDIO2_COMMIT_NOW);
    } else {
        headset_voice = NULL;
        uwp_log("headset: no voice output");
    }
    /* Win32 thread: SDL_CreateThread needs the caller's _beginthreadex, which
     * SDL3-uwp refuses for code built outside it */
    HANDLE t = CreateThread(NULL, 0, feeder_thread, NULL, 0, NULL);
    if (!t) {
        uwp_log("audio: CreateThread failed (error %lu)", GetLastError());
        return NULL;
    }
    SetThreadPriority(t, THREAD_PRIORITY_HIGHEST);
    CloseHandle(t);
    uwp_log("audio: XAudio2 output running (48 kHz stereo)");
    return stream;
}

/*
 * Headset microphone: the console's default recording device through WASAPI, written into
 * xemu_uwp_headset_in (48 kHz mono). ActivateAudioInterfaceAsync is called from the UI thread
 * (it may ask the player's permission) and never waited for: the result arrives on a COM thread.
 */
static const GUID uwp_IID_IAudioClient = { 0x1cb9ad4c, 0xdbfa, 0x4c32, { 0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2 } };
static const GUID uwp_IID_IAudioCaptureClient = { 0xc8adbd64, 0xe71e, 0x48a0, { 0xa4, 0xde, 0x18, 0x5c, 0x39, 0x5c, 0xd3, 0x17 } };
static const GUID uwp_IID_IActivateDone = { 0x41d949ab, 0x9862, 0x444a, { 0x80, 0xf6, 0xc2, 0x61, 0x33, 0x4d, 0xa5, 0xeb } };
static const GUID uwp_IID_IAgileObject = { 0x94ea2b94, 0xe9cc, 0x49e0, { 0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90 } };
static const GUID uwp_IID_IUnknown = { 0x00000000, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };

static IAudioClient *mic_client;
static IAudioCaptureClient *mic_capture;
volatile unsigned xemu_uwp_mic_bytes;

static DWORD WINAPI mic_thread(LPVOID unused)
{
    static const Uint8 zero[HEADSET_CHUNK * 2];
    bool logged = false;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    for (;;) {
        UINT32 frames = 0;
        if (FAILED(IAudioCaptureClient_GetNextPacketSize(mic_capture, &frames))) {
            uwp_log("headset: microphone stopped");
            return 0;
        }
        if (frames == 0) {
            SDL_Delay(5);
            continue;
        }
        BYTE *data;
        DWORD flags;
        if (FAILED(IAudioCaptureClient_GetBuffer(mic_capture, &data, &frames, &flags, NULL, NULL))) {
            SDL_Delay(5);
            continue;
        }
        if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
            SDL_PutAudioStreamData(xemu_uwp_headset_in, data, frames * 2);
        } else {
            for (UINT32 left = frames * 2; left > 0;) {
                UINT32 n = left < sizeof(zero) ? left : sizeof(zero);
                SDL_PutAudioStreamData(xemu_uwp_headset_in, zero, n);
                left -= n;
            }
        }
        /* keep at most ~200 ms: nothing reads it until a game uses the headset */
        if (SDL_GetAudioStreamAvailable(xemu_uwp_headset_in) > HEADSET_RATE * 2 / 5) {
            SDL_ClearAudioStream(xemu_uwp_headset_in);
        }
        xemu_uwp_mic_bytes += frames * 2;
        IAudioCaptureClient_ReleaseBuffer(mic_capture, frames);
        if (!logged) {
            logged = true;
            uwp_log("headset: microphone running (%u frames per packet)", frames);
        }
    }
}

typedef struct {
    IActivateAudioInterfaceCompletionHandler iface;
    LONG refs;
    bool control;   /* the sound output, asked the same way: shows whether the request mechanism works */
} MicDone;

static volatile LONG mic_answered;

static HRESULT STDMETHODCALLTYPE mic_qi(IActivateAudioInterfaceCompletionHandler *This, REFIID riid,
                                        void **out)
{
    if (IsEqualGUID(riid, &uwp_IID_IUnknown) || IsEqualGUID(riid, &uwp_IID_IActivateDone) ||
        IsEqualGUID(riid, &uwp_IID_IAgileObject)) {
        *out = This;
        IActivateAudioInterfaceCompletionHandler_AddRef(This);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE mic_addref(IActivateAudioInterfaceCompletionHandler *This)
{
    return InterlockedIncrement(&((MicDone *)This)->refs);
}

static ULONG STDMETHODCALLTYPE mic_release(IActivateAudioInterfaceCompletionHandler *This)
{
    LONG n = InterlockedDecrement(&((MicDone *)This)->refs);
    if (n == 0) {
        free(This);
    }
    return n;
}

static HRESULT STDMETHODCALLTYPE mic_done(IActivateAudioInterfaceCompletionHandler *This,
                                          IActivateAudioInterfaceAsyncOperation *op)
{
    HRESULT hr, ahr = E_FAIL;
    IUnknown *unk = NULL;

    hr = IActivateAudioInterfaceAsyncOperation_GetActivateResult(op, &ahr, &unk);
    if (((MicDone *)This)->control) {
        uwp_log("headset: sound output request answered (0x%08lx 0x%08lx)", hr, ahr);
        if (unk) {
            IUnknown_Release(unk);
        }
        return S_OK;
    }
    if (InterlockedExchange(&mic_answered, 1)) {
        if (unk) {
            IUnknown_Release(unk);   /* the second request's answer: the first one is in use */
        }
        return S_OK;
    }
    if (FAILED(hr) || FAILED(ahr) || !unk) {
        uwp_log("headset: microphone not allowed or missing (0x%08lx 0x%08lx)", hr, ahr);
        return S_OK;
    }
    hr = IUnknown_QueryInterface(unk, &uwp_IID_IAudioClient, (void **)&mic_client);
    IUnknown_Release(unk);
    if (FAILED(hr)) {
        uwp_log("headset: microphone gave no audio client (0x%08lx)", hr);
        return S_OK;
    }
    WAVEFORMATEX wfx = { 0 };
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 1;
    wfx.nSamplesPerSec = HEADSET_RATE;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = 2;
    wfx.nAvgBytesPerSec = HEADSET_RATE * 2;
    WAVEFORMATEX *mix = NULL;
    if (SUCCEEDED(IAudioClient_GetMixFormat(mic_client, &mix)) && mix) {
        uwp_log("headset: microphone format %lu Hz, %u channel(s), %u bits, tag 0x%x",
                mix->nSamplesPerSec, mix->nChannels, mix->wBitsPerSample, mix->wFormatTag);
    }
    hr = IAudioClient_Initialize(mic_client, AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                 200 * 10000, 0, &wfx, NULL);
    if (FAILED(hr) && mix) {
        /* test: the microphone's own format, to tell a refused format from refused access */
        uwp_log("headset: microphone setup refused (0x%08lx); trying its own format", hr);
        hr = IAudioClient_Initialize(mic_client, AUDCLNT_SHAREMODE_SHARED, 0, 200 * 10000, 0, mix, NULL);
        if (SUCCEEDED(hr)) {
            uwp_log("headset: microphone accepts only its own format (conversion not built yet)");
            CoTaskMemFree(mix);
            return S_OK;
        }
    }
    CoTaskMemFree(mix);
    const char *step = "setup";
    if (SUCCEEDED(hr)) {
        step = "capture service";
        hr = IAudioClient_GetService(mic_client, &uwp_IID_IAudioCaptureClient, (void **)&mic_capture);
    }
    if (SUCCEEDED(hr)) {
        step = "start";
        hr = IAudioClient_Start(mic_client);
    }
    if (FAILED(hr)) {
        uwp_log("headset: microphone not started at %s (0x%08lx%s)", step, hr,
                hr == E_ACCESSDENIED ? ": the console does not let apps use the microphone" : "");
        return S_OK;
    }
    HANDLE t = CreateThread(NULL, 0, mic_thread, NULL, 0, NULL);
    if (t) {
        CloseHandle(t);
    }
    uwp_log("headset: microphone opened (48 kHz mono)");
    return S_OK;
}

static IActivateAudioInterfaceCompletionHandlerVtbl mic_vtbl = {
    mic_qi, mic_addref, mic_release, mic_done
};

typedef HRESULT (WINAPI *ActivateAudioInterfaceAsync_fn)(LPCWSTR, REFIID, PROPVARIANT *,
                                                         IActivateAudioInterfaceCompletionHandler *,
                                                         IActivateAudioInterfaceAsyncOperation **);

static void mic_request(bool control)
{
    HMODULE lib = LoadLibraryExW(L"mmdevapi.dll", NULL, 0);
    ActivateAudioInterfaceAsync_fn activate =
        lib ? (ActivateAudioInterfaceAsync_fn)(void *)GetProcAddress(lib, "ActivateAudioInterfaceAsync") : NULL;
    if (!activate) {
        uwp_log("headset: no microphone support (error %lu)", GetLastError());
        return;
    }
    MicDone *done = calloc(1, sizeof(*done));
    if (!done) {
        return;
    }
    done->iface.lpVtbl = &mic_vtbl;
    done->refs = 1;
    done->control = control;
    IActivateAudioInterfaceAsyncOperation *op = NULL;
    /* DEVINTERFACE_AUDIO_CAPTURE: the default recording device, following the player's choice */
    /* DEVINTERFACE_AUDIO_RENDER for the control request */
    HRESULT hr = activate(control ? L"{E6327CAD-DCEC-4949-AE8A-991E976A79D2}"
                                  : L"{2EEF81BE-33FA-4800-9670-1CD474972C3F}",
                          &uwp_IID_IAudioClient, NULL, &done->iface, &op);
    if (FAILED(hr)) {
        uwp_log("headset: %s request refused (0x%08lx)", control ? "sound output" : "microphone", hr);
    } else {
        uwp_log("headset: %s requested", control ? "sound output" : "microphone");
    }
    if (op) {
        IActivateAudioInterfaceAsyncOperation_Release(op);
    }
    IActivateAudioInterfaceCompletionHandler_Release(&done->iface);
}

void xemu_uwp_mic_start(void)
{
    if (xemu_uwp_headset_in) {
        mic_request(false);
    }
}

void xemu_uwp_mic_tick(void)
{
    static int ticks;
    if (!xemu_uwp_headset_in) {
        return;
    }
    ticks++;
    if (ticks == 1) {
        /* again with the window up, and the control request */
        mic_request(true);
        if (!mic_answered) {
            mic_request(false);
        }
    } else if (ticks == 5 && !mic_answered) {
        uwp_log("headset: the microphone request has no answer after 15 s");
    } else if (ticks % 20 == 0 && mic_answered) {
        uwp_log("headset: microphone delivered %u bytes", xemu_uwp_mic_bytes);
    }
}

#endif /* XEMU_UWP */
