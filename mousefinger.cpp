// MouseFinger — управление курсором рукой через веб-камеру.
//
// Архитектура:
//   * mousefinger.exe            — контроллер: иконка в трее + горячая клавиша Ctrl+Alt+M.
//                                  ~0.2 МБ, ничего не грузит.
//   * mousefinger.exe --track    — рабочий процесс: камера + нейросеть. Запускается по
//                                  горячей клавише и ПОЛНОСТЬЮ завершается при выключении.
//
// Распознавание: MediaPipe palm detection + hand landmarks (ONNX, OpenCV Zoo, Apache 2.0)
// через ONNX Runtime (onnxruntime.dll грузится динамически только в рабочем процессе).
// Курсор ведёт точка между кончиками большого и указательного пальцев (при щипке она
// почти не смещается, поэтому клик не «уводит» курсор). Щипок = нажатие, разжал = отпустил:
// короткий щипок — клик, щипок с движением — перетаскивание.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <psapi.h>
#include <math.h>
#include <stdlib.h>
#include "onnxruntime_c_api.h"
#include "control.h"
#include "gesture.h"

// ---------------------------------------------------------------- настройки
struct Settings {
    int sensitivity = 100;  // %
    int camera = 0;
    int showPreview = 1;
    int clickMode = 0;      // 0 левый (клик/перетаскивание), 1 правый, 2 двойной
    int scrollSpeed = 100;  // %
    int pinchOn = 25;       // щипок: зазор большой↔указательный < PinchClose% длины ладони
    int pinchOff = 40;      // отпускание: зазор > PinchOpen%
    int threads = 4;
    int mapping = 0;        // 0 тачпад в воздухе (относительно), 1 абсолютно (зона кадра = монитор)
    int gesture = 0;        // 0 кулак, 1 щипок
    int fistOn = 115;       // кулак: среднее «кончик→запястье / основание→запястье» < FistClose/100
    int fistOff = 140;      // ладонь раскрыта: > FistOpen/100
    int deadZone = 3;       // px экрана (абсолютный режим): микродрожь меньше этого игнорируется
} S;
static const int INI_VERSION = 4;

static wchar_t gIniPath[MAX_PATH], gExeDir[MAX_PATH];

static void InitPaths() {
    GetModuleFileNameW(NULL, gExeDir, MAX_PATH);
    lstrcpyW(gIniPath, gExeDir);
    wchar_t* dot = NULL;
    for (wchar_t* p = gIniPath; *p; ++p) if (*p == L'.') dot = p;
    if (dot) lstrcpyW(dot, L".ini"); else lstrcatW(gIniPath, L".ini");
    wchar_t* slash = NULL;
    for (wchar_t* p = gExeDir; *p; ++p) if (*p == L'\\') slash = p;
    if (slash) slash[1] = 0;
}

static void EnsureIni() {
    if (GetFileAttributesW(gIniPath) != INVALID_FILE_ATTRIBUTES) return;
    static const wchar_t txt[] =
        L"\xFEFF; Настройки MouseFinger. Применяются при следующем включении (Ctrl+Alt+M).\r\n"
        L"[MouseFinger]\r\n"
        L"Version=4\r\n"
        L"; Жест клика: 0 = кулак (сжал — нажато, раскрыл — отпущено), 1 = щипок большим и указательным\r\n"
        L"Gesture=0\r\n"
        L"; Управление: 0 = тачпад в воздухе (быстрое движение — далеко, медленное — точно), 1 = абсолютное\r\n"
        L"Mapping=0\r\n"
        L"; Чувствительность курсора в процентах (меньше = точнее и спокойнее, больше = быстрее)\r\n"
        L"Sensitivity=100\r\n"
        L"; Мёртвая зона в пикселях экрана (только для абсолютного режима)\r\n"
        L"DeadZone=3\r\n"
        L"; Пороги кулака (значение «кулак NN» видно в превью): меньше FistClose — нажато,\r\n"
        L"; больше FistOpen — отпущено. Ложные клики — уменьшите FistClose, не срабатывает — увеличьте.\r\n"
        L"; Подобрать по данным: mousefinger.exe --track --diag\r\n"
        L"FistClose=115\r\n"
        L"FistOpen=140\r\n"
        L"; Пороги щипка (если Gesture=1), в % от длины ладони\r\n"
        L"PinchClose=25\r\n"
        L"PinchOpen=40\r\n"
        L"; Что делает жест при старте: 0 левый клик/перетаскивание, 1 правый клик, 2 двойной клик\r\n"
        L"ClickMode=0\r\n"
        L"; Скорость прокрутки в процентах\r\n"
        L"ScrollSpeed=100\r\n"
        L"; Номер камеры (0 = первая)\r\n"
        L"Camera=0\r\n"
        L"; Показывать окно превью (1/0)\r\n"
        L"ShowPreview=1\r\n"
        L"; Потоков процессора для нейросети (1 = меньше нагрузка, 4 = меньше задержка)\r\n"
        L"Threads=4\r\n";
    HANDLE f = CreateFileW(gIniPath, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wr; WriteFile(f, txt, (DWORD)(sizeof(txt) - sizeof(wchar_t)), &wr, NULL);
    CloseHandle(f);
}

static int Clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void LoadSettings() {
    const wchar_t* s = L"MouseFinger";
    S.sensitivity = Clampi(GetPrivateProfileIntW(s, L"Sensitivity", 100, gIniPath), 10, 1000);
    S.camera      = Clampi(GetPrivateProfileIntW(s, L"Camera", 0, gIniPath), 0, 16);
    S.showPreview = GetPrivateProfileIntW(s, L"ShowPreview", 1, gIniPath) ? 1 : 0;
    S.clickMode   = Clampi(GetPrivateProfileIntW(s, L"ClickMode", 0, gIniPath), 0, 2);
    S.scrollSpeed = Clampi(GetPrivateProfileIntW(s, L"ScrollSpeed", 100, gIniPath), 10, 1000);
    S.pinchOn     = Clampi(GetPrivateProfileIntW(s, L"PinchClose", 25, gIniPath), 3, 150);
    S.pinchOff    = Clampi(GetPrivateProfileIntW(s, L"PinchOpen", 40, gIniPath), S.pinchOn + 2, 200);
    S.threads     = Clampi(GetPrivateProfileIntW(s, L"Threads", 4, gIniPath), 1, 16);
    S.mapping     = GetPrivateProfileIntW(s, L"Mapping", 0, gIniPath) ? 1 : 0;
    S.gesture     = GetPrivateProfileIntW(s, L"Gesture", 0, gIniPath) ? 1 : 0;
    S.fistOn      = Clampi(GetPrivateProfileIntW(s, L"FistClose", 115, gIniPath), 50, 300);
    S.fistOff     = Clampi(GetPrivateProfileIntW(s, L"FistOpen", 140, gIniPath), S.fistOn + 5, 400);
    S.deadZone    = Clampi(GetPrivateProfileIntW(s, L"DeadZone", 3, gIniPath), 0, 50);
}

// Старый ini (от прошлой версии с другими порогами) заменяем новым.
static void UpgradeIni() {
    if (GetFileAttributesW(gIniPath) == INVALID_FILE_ATTRIBUTES) return;
    if ((int)GetPrivateProfileIntW(L"MouseFinger", L"Version", 1, gIniPath) < INI_VERSION) DeleteFileW(gIniPath);
}

static void SetDpiAware() {
    typedef BOOL(WINAPI * Fn)(HANDLE);
    Fn f = (Fn)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
    if (!f || !f((HANDLE)-4 /* PER_MONITOR_AWARE_V2 */)) SetProcessDPIAware();
}

static void TrimMemory() { SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1); }

static const wchar_t* STOP_EVENT = L"Local\\MouseFinger.Stop";

// =================================================================== РАБОЧИЙ ПРОЦЕСС

static double gQpcFreq;
static double Now() { LARGE_INTEGER c; QueryPerformanceCounter(&c); return c.QuadPart / gQpcFreq; }

// Журнал запуска: mousefinger.log рядом с программой. По нему видно, на каком шаге и почему
// рабочий процесс закрылся, даже если сообщение об ошибке никто не увидел.
static void LogA(const char* fmt, ...) {
    wchar_t path[MAX_PATH];
    wsprintfW(path, L"%smousefinger.log", gExeDir);
    HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    char buf[1200];
    int n = wsprintfA(buf, "%04d-%02d-%02d %02d:%02d:%02d.%03d [%lu] ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
                      t.wMilliseconds, GetCurrentProcessId());
    va_list ap;
    va_start(ap, fmt);
    n += wvsprintfA(buf + n, fmt, ap);
    va_end(ap);
    lstrcpyA(buf + n, "\r\n");
    DWORD wr;
    WriteFile(f, buf, n + 2, &wr, NULL);
    CloseHandle(f);
}

static void Fail(const wchar_t* msg) {
    char u[1000];
    WideCharToMultiByte(CP_UTF8, 0, msg, -1, u, sizeof(u), NULL, NULL);
    LogA("ERROR: %s", u);
    MessageBoxW(NULL, msg, L"MouseFinger", MB_ICONERROR | MB_TOPMOST);
}

// ---------------------------------------------------------------- камера
struct Camera {
    IMFSourceReader* rd = NULL;
    GUID sub = {};
    UINT32 w = 0, h = 0;
    LONG stride = 0;
};
static const DWORD VS = (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM;

static bool IsYuv(const GUID& g) { return g == MFVideoFormat_NV12 || g == MFVideoFormat_YUY2; }

static HRESULT OpenCamera(int index, Camera& c) {
    IMFAttributes* a = NULL;
    IMFActivate** devs = NULL;
    UINT32 n = 0;
    IMFMediaSource* src = NULL;
    HRESULT hr = MFCreateAttributes(&a, 1);
    if (SUCCEEDED(hr)) hr = a->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (SUCCEEDED(hr)) hr = MFEnumDeviceSources(a, &devs, &n);
    if (a) { a->Release(); a = NULL; }
    if (SUCCEEDED(hr)) {
        if (n == 0) hr = MF_E_NOT_FOUND;
        else hr = devs[(UINT32)index < n ? index : 0]->ActivateObject(IID_PPV_ARGS(&src));
    }
    for (UINT32 i = 0; i < n; i++) devs[i]->Release();
    CoTaskMemFree(devs);
    if (FAILED(hr)) return hr;

    hr = MFCreateAttributes(&a, 1);
    if (SUCCEEDED(hr)) a->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    if (SUCCEEDED(hr)) hr = MFCreateSourceReaderFromMediaSource(src, a, &c.rd);
    if (a) a->Release();
    src->Release();
    if (FAILED(hr)) return hr;

    c.rd->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    c.rd->SetStreamSelection(VS, TRUE);

    // ~640x480, ≥25 fps, желательно несжатый YUV (без декодера — меньше памяти и CPU).
    IMFMediaType* best = NULL;
    int bestScore = -1000000;
    for (DWORD i = 0;; i++) {
        IMFMediaType* t = NULL;
        if (FAILED(c.rd->GetNativeMediaType(VS, i, &t))) break;
        GUID st = {};
        UINT32 w = 0, h = 0, fn = 0, fd = 1;
        t->GetGUID(MF_MT_SUBTYPE, &st);
        MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &w, &h);
        MFGetAttributeRatio(t, MF_MT_FRAME_RATE, &fn, &fd);
        double fps = fd ? (double)fn / fd : 0;
        int s = 0;
        if (IsYuv(st)) s += 400;
        else if (st == MFVideoFormat_MJPG) s += 200;
        else if (st == MFVideoFormat_RGB32 || st == MFVideoFormat_RGB24) s += 100;
        else s -= 500;
        // 60 fps заметно сглаживает курсор — ради него не жалко декодировать MJPG.
        if (fps >= 59) s += 1300; else if (fps >= 29) s += 1000; else if (fps >= 20) s += 600;
        if (w < 480) s -= 800;
        s -= abs((int)w - 640) / 2;
        if (s > bestScore) { if (best) best->Release(); best = t; bestScore = s; }
        else t->Release();
    }
    if (best) { c.rd->SetCurrentMediaType(VS, NULL, best); best->Release(); }

    IMFMediaType* cur = NULL;
    GUID st = {};
    if (SUCCEEDED(c.rd->GetCurrentMediaType(VS, &cur))) { cur->GetGUID(MF_MT_SUBTYPE, &st); cur->Release(); cur = NULL; }
    if (!IsYuv(st)) {
        const GUID* want[] = {&MFVideoFormat_NV12, &MFVideoFormat_YUY2, &MFVideoFormat_RGB32};
        hr = E_FAIL;
        for (int i = 0; i < 3 && FAILED(hr); i++) {
            IMFMediaType* t = NULL;
            if (FAILED(MFCreateMediaType(&t))) continue;
            t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            t->SetGUID(MF_MT_SUBTYPE, *want[i]);
            hr = c.rd->SetCurrentMediaType(VS, NULL, t);
            t->Release();
        }
        if (FAILED(hr)) return hr;
    }
    hr = c.rd->GetCurrentMediaType(VS, &cur);
    if (FAILED(hr)) return hr;
    cur->GetGUID(MF_MT_SUBTYPE, &c.sub);
    MFGetAttributeSize(cur, MF_MT_FRAME_SIZE, &c.w, &c.h);
    UINT32 st32 = 0;
    if (SUCCEEDED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &st32))) c.stride = (LONG)st32;
    else c.stride = (LONG)c.w * (c.sub == MFVideoFormat_YUY2 ? 2 : c.sub == MFVideoFormat_RGB32 ? 4 : 1);
    cur->Release();
    if (!c.w || !c.h || (c.w & 1)) return E_FAIL;
    return S_OK;
}

// ---------------------------------------------------------------- кадр → BGR (зеркально)
static int W, H, BS;  // размеры кадра и шаг строки BGR-буфера
static BYTE* gBGR;

static inline BYTE Clip(int v) { return (BYTE)(v < 0 ? 0 : v > 255 ? 255 : v); }
static inline void Yuv(BYTE* o, int y, int u, int v) {
    int c = 298 * (y - 16), d = u - 128, e = v - 128;
    o[0] = Clip((c + 516 * d + 128) >> 8);
    o[1] = Clip((c - 100 * d - 208 * e + 128) >> 8);
    o[2] = Clip((c + 409 * e + 128) >> 8);
}

static void ToBGR(const BYTE* scan0, LONG pitch, const GUID& sub) {
    for (int y = 0; y < H; y++) {
        const BYTE* row = scan0 + (LONG_PTR)y * pitch;
        BYTE* out = gBGR + y * BS;
        if (sub == MFVideoFormat_YUY2) {
            for (int x = 0; x < W; x += 2) {
                const BYTE* p = row + x * 2;
                Yuv(out + (W - 1 - x) * 3, p[0], p[1], p[3]);
                Yuv(out + (W - 2 - x) * 3, p[2], p[1], p[3]);
            }
        } else if (sub == MFVideoFormat_NV12) {
            const BYTE* uv = scan0 + (LONG_PTR)H * pitch + (LONG_PTR)(y / 2) * pitch;
            for (int x = 0; x < W; x++) Yuv(out + (W - 1 - x) * 3, row[x], uv[x & ~1], uv[x | 1]);
        } else {
            for (int x = 0; x < W; x++) {
                const BYTE* p = row + x * 4;
                BYTE* o = out + (W - 1 - x) * 3;
                o[0] = p[0]; o[1] = p[1]; o[2] = p[2];
            }
        }
    }
}

// Повёрнутый квадратный кроп с билинейной интерполяцией → float RGB [0..1], NHWC.
struct Roi { float cx, cy, size, rot; };

static void Crop(float* dst, int S2, const Roi& r) {
    float c = cosf(r.rot), s = sinf(r.rot), k = r.size / S2;
    for (int v = 0; v < S2; v++) {
        float fv = (v + 0.5f) * k - r.size * 0.5f;
        for (int u = 0; u < S2; u++, dst += 3) {
            float fu = (u + 0.5f) * k - r.size * 0.5f;
            float x = r.cx + fu * c - fv * s - 0.5f, y = r.cy + fu * s + fv * c - 0.5f;
            int x0 = (int)floorf(x), y0 = (int)floorf(y);
            float ax = x - x0, ay = y - y0;
            float acc[3] = {0, 0, 0};
            for (int j = 0; j < 2; j++) {
                int yy = y0 + j;
                if (yy < 0 || yy >= H) continue;
                float wy = j ? ay : 1 - ay;
                for (int i = 0; i < 2; i++) {
                    int xx = x0 + i;
                    if (xx < 0 || xx >= W) continue;
                    float wgt = wy * (i ? ax : 1 - ax);
                    const BYTE* p = gBGR + yy * BS + xx * 3;
                    acc[0] += wgt * p[2]; acc[1] += wgt * p[1]; acc[2] += wgt * p[0];
                }
            }
            dst[0] = acc[0] * (1.f / 255); dst[1] = acc[1] * (1.f / 255); dst[2] = acc[2] * (1.f / 255);
        }
    }
}

// ---------------------------------------------------------------- нейросеть (ONNX Runtime)
enum { PIN = 192, NA = 2016, HIN = 224 };
static const OrtApi* ort;
static OrtEnv* oEnv;
static OrtSession *oPalm, *oHand;
static OrtMemoryInfo* oMem;
static float inPalm[PIN * PIN * 3], outReg[NA * 18], outScore[NA];
static float inHand[HIN * HIN * 3], outLm[63], outFlag[1], outHanded[1], outWorld[63];
static OrtValue *vInPalm, *vReg, *vScore, *vInHand, *vLm, *vFlag, *vHanded, *vWorld;
static float gAnchors[NA][2];

static bool OrtCheck(OrtStatus* st, wchar_t* err, const wchar_t* what) {
    if (!st) return true;
    wchar_t m[300];
    MultiByteToWideChar(CP_UTF8, 0, ort->GetErrorMessage(st), -1, m, 300);
    wsprintfW(err, L"%s:\n%s", what, m);
    ort->ReleaseStatus(st);
    return false;
}

static bool MakeTensor(float* data, size_t bytes, const int64_t* shape, size_t n, OrtValue** v, wchar_t* err) {
    return OrtCheck(ort->CreateTensorWithDataAsOrtValue(oMem, data, bytes, shape, n, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, v),
                    err, L"Ошибка тензора");
}

static bool InitNet(wchar_t* err) {
    wchar_t path[MAX_PATH];
    wsprintfW(path, L"%sonnxruntime.dll", gExeDir);
    HMODULE m = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) m = LoadLibraryExW(L"onnxruntime.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);  // есть в Windows 11
    if (!m) { lstrcpyW(err, L"Не найден onnxruntime.dll (положите его рядом с mousefinger.exe)."); return false; }
    typedef const OrtApiBase*(ORT_API_CALL * GetBase)(void);
    GetBase gb = (GetBase)(void*)GetProcAddress(m, "OrtGetApiBase");
    if (!gb) { lstrcpyW(err, L"onnxruntime.dll повреждён."); return false; }
    int apiVer = 0;
    for (int v = ORT_API_VERSION; v >= 14 && !ort; v--) { ort = gb()->GetApi(v); apiVer = v; }
    if (!ort) { lstrcpyW(err, L"Слишком старая версия onnxruntime.dll."); return false; }
    LogA("onnxruntime %s loaded (api %d)", gb()->GetVersionString(), apiVer);

    if (!OrtCheck(ort->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "mf", &oEnv), err, L"ONNX Runtime")) return false;
    OrtSessionOptions* so = NULL;
    if (!OrtCheck(ort->CreateSessionOptions(&so), err, L"ONNX Runtime")) return false;
    OrtStatus* st;
    if ((st = ort->SetIntraOpNumThreads(so, S.threads))) ort->ReleaseStatus(st);
    if ((st = ort->SetInterOpNumThreads(so, 1))) ort->ReleaseStatus(st);
    if ((st = ort->SetSessionGraphOptimizationLevel(so, ORT_ENABLE_ALL))) ort->ReleaseStatus(st);
    // Потоки засыпают между кадрами, а не крутятся вхолостую (иначе 4 потока жгут CPU впустую).
    if ((st = ort->AddSessionConfigEntry(so, "session.intra_op.allow_spinning", "0"))) ort->ReleaseStatus(st);

    wsprintfW(path, L"%smodels\\palm_detection.onnx", gExeDir);
    bool ok = OrtCheck(ort->CreateSession(oEnv, path, so, &oPalm), err, L"Модель ладони (models\\palm_detection.onnx)");
    wsprintfW(path, L"%smodels\\hand_landmark.onnx", gExeDir);
    if (ok) ok = OrtCheck(ort->CreateSession(oEnv, path, so, &oHand), err, L"Модель руки (models\\hand_landmark.onnx)");
    ort->ReleaseSessionOptions(so);
    if (!ok) return false;

    if (!OrtCheck(ort->CreateCpuMemoryInfo(OrtDeviceAllocator, OrtMemTypeDefault, &oMem), err, L"ONNX Runtime")) return false;
    const int64_t sIn[] = {1, PIN, PIN, 3}, sReg[] = {1, NA, 18}, sSc[] = {1, NA, 1};
    const int64_t sHin[] = {1, HIN, HIN, 3}, s63[] = {1, 63}, s1[] = {1, 1};
    if (!MakeTensor(inPalm, sizeof(inPalm), sIn, 4, &vInPalm, err) || !MakeTensor(outReg, sizeof(outReg), sReg, 3, &vReg, err) ||
        !MakeTensor(outScore, sizeof(outScore), sSc, 3, &vScore, err) || !MakeTensor(inHand, sizeof(inHand), sHin, 4, &vInHand, err) ||
        !MakeTensor(outLm, sizeof(outLm), s63, 2, &vLm, err) || !MakeTensor(outFlag, sizeof(outFlag), s1, 2, &vFlag, err) ||
        !MakeTensor(outHanded, sizeof(outHanded), s1, 2, &vHanded, err) || !MakeTensor(outWorld, sizeof(outWorld), s63, 2, &vWorld, err))
        return false;

    // SSD-якоря MediaPipe palm detection: 24x24x2 (шаг 8) + 12x12x6 (шаг 16).
    int k = 0;
    for (int y = 0; y < 24; y++) for (int x = 0; x < 24; x++) for (int a = 0; a < 2; a++) { gAnchors[k][0] = (x + 0.5f) / 24; gAnchors[k][1] = (y + 0.5f) / 24; k++; }
    for (int y = 0; y < 12; y++) for (int x = 0; x < 12; x++) for (int a = 0; a < 6; a++) { gAnchors[k][0] = (x + 0.5f) / 12; gAnchors[k][1] = (y + 0.5f) / 12; k++; }
    return true;
}

static void FreeNet() {
    OrtValue* vs[] = {vInPalm, vReg, vScore, vInHand, vLm, vFlag, vHanded, vWorld};
    for (OrtValue* v : vs) if (v) ort->ReleaseValue(v);
    if (oMem) ort->ReleaseMemoryInfo(oMem);
    if (oHand) ort->ReleaseSession(oHand);
    if (oPalm) ort->ReleaseSession(oPalm);
    if (oEnv) ort->ReleaseEnv(oEnv);
}

static float NormAngle(float a) {
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}

// Ищет ладонь во всём кадре; возвращает область для модели точек руки.
static bool DetectPalm(Roi& out) {
    Roi full = {W * 0.5f, H * 0.5f, (float)(W > H ? W : H), 0};
    Crop(inPalm, PIN, full);
    const char* in[] = {"input_1"};
    const char* on[] = {"Identity", "Identity_1"};
    OrtValue* outs[] = {vReg, vScore};
    OrtStatus* st = ort->Run(oPalm, NULL, in, &vInPalm, 1, on, 2, outs);
    if (st) { ort->ReleaseStatus(st); return false; }
    int best = -1;
    float bs = 0.45f;
    for (int i = 0; i < NA; i++) {
        float x = outScore[i];
        if (x < -80) continue;
        float p = 1.f / (1.f + expf(-x));
        if (p > bs) { bs = p; best = i; }
    }
    if (best < 0) return false;
    const float* r = outReg + best * 18;
    float ax = gAnchors[best][0], ay = gAnchors[best][1], sz = full.size;
    auto px = [&](float v, float a) { return (v / PIN + a - 0.5f) * sz; };
    float cx = full.cx + px(r[0], ax), cy = full.cy + px(r[1], ay);
    float bw = r[2] / PIN * sz, bh = r[3] / PIN * sz;
    float x0 = full.cx + px(r[4], ax), y0 = full.cy + px(r[5], ay);  // запястье
    float x2 = full.cx + px(r[8], ax), y2 = full.cy + px(r[9], ay);  // основание среднего пальца
    float rot = NormAngle(1.5707963f - atan2f(-(y2 - y0), x2 - x0));
    // Сдвиг на полвысоты к пальцам и расширение x2.6 — как в MediaPipe.
    out.cx = cx + 0.5f * bh * sinf(rot);
    out.cy = cy - 0.5f * bh * cosf(rot);
    out.size = (bw > bh ? bw : bh) * 2.6f;
    out.rot = rot;
    return out.size > 16;
}

struct Pt { float x, y; };

// Точки руки в области roi → 21 точка в координатах кадра. Возвращает уверенность.
static float DetectHand(const Roi& roi, Pt* lm) {
    Crop(inHand, HIN, roi);
    const char* in[] = {"input_1"};
    const char* on[] = {"Identity", "Identity_1", "Identity_2", "Identity_3"};
    OrtValue* outs[] = {vLm, vFlag, vHanded, vWorld};
    OrtStatus* st = ort->Run(oHand, NULL, in, &vInHand, 1, on, 4, outs);
    if (st) { ort->ReleaseStatus(st); return 0; }
    float conf = outFlag[0];
    if (conf < 0 || conf > 1) conf = 1.f / (1.f + expf(-conf));
    float c = cosf(roi.rot), s = sinf(roi.rot), k = roi.size / HIN;
    for (int i = 0; i < 21; i++) {
        float u = outLm[i * 3] * k - roi.size * 0.5f, v = outLm[i * 3 + 1] * k - roi.size * 0.5f;
        lm[i].x = roi.cx + u * c - v * s;
        lm[i].y = roi.cy + u * s + v * c;
    }
    return conf;
}

// Следующая область по найденным точкам (без повторного поиска ладони).
static Roi RoiFromLandmarks(const Pt* lm) {
    static const int ids[] = {0, 1, 2, 3, 5, 6, 9, 10, 13, 14, 17, 18};
    float rot = NormAngle(1.5707963f - atan2f(-(lm[9].y - lm[0].y), lm[9].x - lm[0].x));
    float c = cosf(rot), s = sinf(rot);
    float mina = 1e9f, maxa = -1e9f, minb = 1e9f, maxb = -1e9f;
    for (int id : ids) {
        float a = lm[id].x * c + lm[id].y * s, b = -lm[id].x * s + lm[id].y * c;
        if (a < mina) mina = a; if (a > maxa) maxa = a;
        if (b < minb) minb = b; if (b > maxb) maxb = b;
    }
    float w = maxa - mina, h = maxb - minb;
    float ca = (mina + maxa) * 0.5f, cb = (minb + maxb) * 0.5f - 0.1f * h;
    Roi r;
    r.cx = ca * c - cb * s;
    r.cy = ca * s + cb * c;
    r.size = (w > h ? w : h) * 2.0f;
    r.rot = rot;
    return r;
}

static float Dist3(const float* a, int i, int j) {
    float dx = a[i * 3] - a[j * 3], dy = a[i * 3 + 1] - a[j * 3 + 1], dz = a[i * 3 + 2] - a[j * 3 + 2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

// Расстояние от точки p до отрезка s0–s1 (3D).
static float SegDist3(const float* a, int p, int s0, int s1) {
    float vx = a[s1 * 3] - a[s0 * 3], vy = a[s1 * 3 + 1] - a[s0 * 3 + 1], vz = a[s1 * 3 + 2] - a[s0 * 3 + 2];
    float wx = a[p * 3] - a[s0 * 3], wy = a[p * 3 + 1] - a[s0 * 3 + 1], wz = a[p * 3 + 2] - a[s0 * 3 + 2];
    float vv = vx * vx + vy * vy + vz * vz;
    float t = vv > 1e-12f ? (wx * vx + wy * vy + wz * vz) / vv : 0.f;
    if (t < 0) t = 0; if (t > 1) t = 1;
    float dx = wx - t * vx, dy = wy - t * vy, dz = wz - t * vz;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

// Зазор между большим и указательным пальцами в долях длины ладони (запястье 0 → основание
// указательного 5). Берём минимум из «кончик–кончик» и «кончик большого – последняя фаланга
// указательного (7–8)»: при щипке пальцы касаются подушечками, и кончики-точки модели
// остаются на расстоянии, а до фаланги — почти ноль. Не зависит от расстояния до камеры.
static float PinchMetric(const float* a) {
    float ref = Dist3(a, 0, 5);
    if (ref < 1e-6f) return -1.f;
    float d = Dist3(a, 4, 8), s = SegDist3(a, 4, 7, 8);
    return (d < s ? d : s) / ref;
}

static float gPinchScr, gPinchWorld;  // для диагностики

// Усредняем две независимые оценки: по точкам на изображении (x, y, z в пикселях кропа)
// и по «мировым» 3D-точкам модели (в метрах). Каждая по отдельности шумит по-своему.
static float PinchRatio() {
    gPinchScr = PinchMetric(outLm);
    gPinchWorld = PinchMetric(outWorld);
    if (gPinchScr < 0) return gPinchWorld < 0 ? 9.f : gPinchWorld;
    if (gPinchWorld < 0) return gPinchScr;
    return 0.5f * (gPinchScr + gPinchWorld);
}

// Насколько вытянуты пальцы: для указательного, среднего, безымянного и мизинца берём
// «расстояние кончик→запястье / основание пальца→запястье» и усредняем.
// Раскрытая ладонь ≈ 1.8–2.0, кулак ≈ 0.9–1.2. Меняется форма всей руки, а не двух
// кончиков, поэтому жест почти не зависит от угла и от того, что пальцы закрывают друг друга.
static float FistMetric(const float* a) {
    static const int f[4][2] = {{5, 8}, {9, 12}, {13, 16}, {17, 20}};
    float sum = 0;
    for (auto& p : f) {
        float m = Dist3(a, 0, p[0]);
        if (m < 1e-6f) return -1.f;
        sum += Dist3(a, 0, p[1]) / m;
    }
    return sum * 0.25f;
}

static float gFistScr, gFistWorld;

static float FistRatio() {
    gFistScr = FistMetric(outLm);
    gFistWorld = FistMetric(outWorld);
    if (gFistScr < 0) return gFistWorld < 0 ? 9.f : gFistWorld;
    if (gFistWorld < 0) return gFistScr;
    return 0.5f * (gFistScr + gFistWorld);
}

// ---------------------------------------------------------------- ввод мыши
static bool gDry = false, gDiag = false, gCalib = false, gTest = false, gManaged = false;

static void SendOne(DWORD flags, DWORD data = 0) {
    if (gDry) return;
    INPUT in = {};
    in.type = INPUT_MOUSE; in.mi.dwFlags = flags; in.mi.mouseData = data;
    SendInput(1, &in, sizeof(INPUT));
}
static void Click(DWORD down, DWORD up, int times) {
    for (int i = 0; i < times; i++) { SendOne(down); SendOne(up); }
}

// ---------------------------------------------------------------- окна (превью и индикатор щипка)
static HWND gPrev, gRing;
static int PVW = 256, PVH = 192, BAR = 36;
static HDC gMemDC;
static HBITMAP gMemBmp;
static BYTE* gMemBits;
static BITMAPINFO gBmi;

// Снимок окна превью в BMP (для диагностики).
static void SavePreviewBmp(const wchar_t* path) {
    if (!gMemBits) return;
    GdiFlush();
    int h = PVH + BAR;
    DWORD img = (DWORD)PVW * h * 4;
    BITMAPFILEHEADER fh = {};
    BITMAPINFOHEADER ih = {};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + img;
    ih.biSize = sizeof(ih); ih.biWidth = PVW; ih.biHeight = -h; ih.biPlanes = 1; ih.biBitCount = 32;
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wr;
    WriteFile(f, &fh, sizeof(fh), &wr, NULL);
    WriteFile(f, &ih, sizeof(ih), &wr, NULL);
    WriteFile(f, gMemBits, img, &wr, NULL);
    CloseHandle(f);
}
static volatile int gHot;

enum { HK_QUIT = 1, HK_RESET, HK_MODE, HK_SCROLL, HK_PREVIEW };

static LRESULT CALLBACK WorkerProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_HOTKEY: gHot = (int)w; return 0;
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); return 0; }
    }
    return DefWindowProcW(h, m, w, l);
}

static const wchar_t* kModeNames[] = {L"Левый клик", L"Правый клик", L"Двойной клик"};
static const COLORREF kModeColors[] = {RGB(40, 220, 90), RGB(255, 150, 30), RGB(40, 200, 255)};
static const COLORREF KEY = RGB(255, 0, 255);
enum { RING = 30 };

static void DrawRing(float p, COLORREF col, int x, int y) {
    if (p < 0.15f) { if (IsWindowVisible(gRing)) ShowWindow(gRing, SW_HIDE); return; }
    SetWindowPos(gRing, HWND_TOPMOST, x + 14, y + 14, RING, RING, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    HDC dc = GetDC(gRing);
    RECT r = {0, 0, RING, RING};
    HBRUSH kb = CreateSolidBrush(KEY), cb = CreateSolidBrush(col), db = CreateSolidBrush(RGB(30, 30, 30));
    FillRect(dc, &r, kb);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    HGDIOBJ ob = SelectObject(dc, db);
    Ellipse(dc, 0, 0, RING + 1, RING + 1);
    SelectObject(dc, cb);
    if (p >= 0.995f) Ellipse(dc, 2, 2, RING - 1, RING - 1);
    else {
        float c = RING / 2.f, a0 = -1.5707963f, a1 = a0 + p * 6.2831853f;
        SetArcDirection(dc, AD_CLOCKWISE);
        Pie(dc, 2, 2, RING - 1, RING - 1, (int)(c + cosf(a0) * 20), (int)(c + sinf(a0) * 20),
            (int)(c + cosf(a1) * 20), (int)(c + sinf(a1) * 20));
    }
    SelectObject(dc, kb);
    Ellipse(dc, 9, 9, RING - 8, RING - 8);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(kb); DeleteObject(cb); DeleteObject(db);
    ReleaseDC(gRing, dc);
}

static void CreateWindows(HINSTANCE hi) {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WorkerProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"MouseFingerWnd";
    RegisterClassW(&wc);
    DWORD ex = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;

    if (gCalib || gTest) PVW = 480;  // калибровка/тест: окно крупнее и по центру — там инструкции
    PVH = PVW * H / W;
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int wx = wa.right - PVW - 12, wy = wa.bottom - PVH - BAR - 12;
    if (gCalib || gTest) { wx = (wa.left + wa.right - PVW) / 2; wy = (wa.top + wa.bottom - PVH - BAR) / 2; }
    gPrev = CreateWindowExW(ex, wc.lpszClassName, L"MouseFinger", WS_POPUP, wx, wy, PVW, PVH + BAR, NULL, NULL, hi, NULL);
    SetLayeredWindowAttributes(gPrev, 0, 230, LWA_ALPHA);
    gRing = CreateWindowExW(ex, wc.lpszClassName, L"", WS_POPUP, 0, 0, RING, RING, NULL, NULL, hi, NULL);
    SetLayeredWindowAttributes(gRing, KEY, 0, LWA_COLORKEY);

    HDC sdc = GetDC(NULL);
    gMemDC = CreateCompatibleDC(sdc);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = PVW;
    bi.bmiHeader.biHeight = -(PVH + BAR);
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    gMemBmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, (void**)&gMemBits, NULL, 0);
    SelectObject(gMemDC, gMemBmp);
    ReleaseDC(NULL, sdc);
    SelectObject(gMemDC, GetStockObject(DEFAULT_GUI_FONT));
    SetBkMode(gMemDC, TRANSPARENT);
    SetStretchBltMode(gMemDC, HALFTONE);

    gBmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    gBmi.bmiHeader.biWidth = W;
    gBmi.bmiHeader.biHeight = -H;
    gBmi.bmiHeader.biPlanes = 1;
    gBmi.bmiHeader.biBitCount = 24;

    if (S.showPreview || gCalib || gTest) ShowWindow(gPrev, SW_SHOWNOACTIVATE);
}

static const int kBones[][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 4}, {0, 5}, {5, 6}, {6, 7}, {7, 8}, {5, 9}, {9, 10}, {10, 11},
                                {11, 12}, {9, 13}, {13, 14}, {14, 15}, {15, 16}, {13, 17}, {0, 17}, {17, 18}, {18, 19}, {19, 20}};

static HFONT gBigFont;

// big — крупный заголовок поверх видео (для калибровки), progress — полоса внизу видео (0..1, <0 — нет).
static void DrawPreview(const Pt* lm, bool hand, bool pinched, float cx, float cy, const wchar_t* line1, const wchar_t* line2,
                        const wchar_t* big = NULL, float progress = -1.f) {
    if (!IsWindowVisible(gPrev)) return;
    StretchDIBits(gMemDC, 0, 0, PVW, PVH, 0, 0, W, H, gBGR, &gBmi, DIB_RGB_COLORS, SRCCOPY);
    if (hand) {
        float s = (float)PVW / W;
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(80, 230, 120));
        HGDIOBJ op = SelectObject(gMemDC, pen);
        for (auto& b : kBones) {
            MoveToEx(gMemDC, (int)(lm[b[0]].x * s), (int)(lm[b[0]].y * s), NULL);
            LineTo(gMemDC, (int)(lm[b[1]].x * s), (int)(lm[b[1]].y * s));
        }
        HBRUSH br = CreateSolidBrush(pinched ? RGB(255, 60, 60) : RGB(255, 220, 0));
        HGDIOBJ ob = SelectObject(gMemDC, br);
        SelectObject(gMemDC, GetStockObject(NULL_PEN));
        // Точка, за которой идёт курсор (центр ладони): жёлтая, при нажатии — красная.
        Ellipse(gMemDC, (int)(cx * s) - 6, (int)(cy * s) - 6, (int)(cx * s) + 7, (int)(cy * s) + 7);
        SelectObject(gMemDC, op); SelectObject(gMemDC, ob);
        DeleteObject(pen); DeleteObject(br);
    }
    if (big) {
        if (!gBigFont)
            gBigFont = CreateFontW(-22, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        RECT band = {0, 0, PVW, 40};
        FillRect(gMemDC, &band, (HBRUSH)GetStockObject(BLACK_BRUSH));
        HGDIOBJ of = SelectObject(gMemDC, gBigFont);
        SetTextColor(gMemDC, RGB(255, 230, 80));
        DrawTextW(gMemDC, big, -1, &band, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(gMemDC, of);
    }
    if (progress >= 0) {
        RECT pb = {0, PVH - 8, PVW, PVH}, pf = {0, PVH - 8, (int)(PVW * (progress > 1 ? 1 : progress)), PVH};
        FillRect(gMemDC, &pb, (HBRUSH)GetStockObject(DKGRAY_BRUSH));
        HBRUSH g = CreateSolidBrush(RGB(60, 220, 100));
        FillRect(gMemDC, &pf, g);
        DeleteObject(g);
    }
    RECT bar = {0, PVH, PVW, PVH + BAR};
    FillRect(gMemDC, &bar, (HBRUSH)GetStockObject(BLACK_BRUSH));
    SetTextColor(gMemDC, RGB(240, 240, 240));
    TextOutW(gMemDC, 6, PVH + 3, line1, lstrlenW(line1));
    SetTextColor(gMemDC, RGB(150, 150, 150));
    TextOutW(gMemDC, 6, PVH + 19, line2, lstrlenW(line2));
    HDC dc = GetDC(gPrev);
    BitBlt(dc, 0, 0, PVW, PVH + BAR, gMemDC, 0, 0, SRCCOPY);
    ReleaseDC(gPrev, dc);
}

// ---------------------------------------------------------------- кадр и распознавание
// Снять кадр с камеры в gBGR: 1 — есть кадр, 0 — пока нет, -1 — камера отключилась.
static int GrabFrame(Camera& cam) {
    DWORD si, flags = 0;
    LONGLONG ts;
    IMFSample* sample = NULL;
    HRESULT hr = cam.rd->ReadSample(VS, 0, &si, &flags, &ts, &sample);
    if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR))) {
        if (sample) sample->Release();
        return -1;
    }
    if (!sample) return 0;
    IMFMediaBuffer* buf = NULL;
    if (SUCCEEDED(sample->GetBufferByIndex(0, &buf))) {
        IMF2DBuffer* b2 = NULL;
        BYTE* p; LONG pitch;
        if (SUCCEEDED(buf->QueryInterface(IID_PPV_ARGS(&b2))) && SUCCEEDED(b2->Lock2D(&p, &pitch))) {
            ToBGR(p, pitch, cam.sub);
            b2->Unlock2D();
        } else {
            DWORD len;
            if (SUCCEEDED(buf->Lock(&p, NULL, &len))) {
                pitch = cam.stride;
                if (pitch < 0) p += (LONG_PTR)(-pitch) * (cam.h - 1);
                ToBGR(p, pitch, cam.sub);
                buf->Unlock();
            }
        }
        if (b2) b2->Release();
        buf->Release();
    }
    sample->Release();
    return 1;
}

struct HandTracker {
    Roi roi = {};
    bool haveRoi = false;
    bool palmRan = false, palmFound = false;
    float conf = 0;
    // Ладонь ищем только если руку потеряли, дальше ведём по точкам. Новую руку принимаем
    // при уверенности ≥0.6, а уже ведомую держим до 0.35 — так рука не «мигает».
    bool Step(Pt* lm) {
        palmRan = !haveRoi; palmFound = false;
        if (!haveRoi) haveRoi = palmFound = DetectPalm(roi);
        conf = 0;
        if (!haveRoi) return false;
        conf = DetectHand(roi, lm);
        if (conf >= (palmFound ? 0.6f : 0.35f)) { roi = RoiFromLandmarks(lm); return true; }
        haveRoi = false;
        return false;
    }
};

// Центр ладони (среднее запястья и оснований четырёх пальцев) и её размер.
static void PalmCenter(const Pt* lm, float& px, float& py, float& palm) {
    px = (lm[0].x + lm[5].x + lm[9].x + lm[13].x + lm[17].x) * 0.2f;
    py = (lm[0].y + lm[5].y + lm[9].y + lm[13].y + lm[17].y) * 0.2f;
    palm = sqrtf((lm[0].x - lm[9].x) * (lm[0].x - lm[9].x) + (lm[0].y - lm[9].y) * (lm[0].y - lm[9].y));
}

// ---------------------------------------------------------------- профиль руки
static GestureModel gModel;

static void ProfilePath(wchar_t* p) { wsprintfW(p, L"%shand_profile.bin", gExeDir); }

static bool LoadProfile(GestureModel& m) {
    wchar_t p[MAX_PATH];
    ProfilePath(p);
    HANDLE f = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD rd = 0;
    BOOL ok = ReadFile(f, &m, sizeof(m), &rd, NULL);
    CloseHandle(f);
    return ok && rd == sizeof(m) && m.Valid();
}

static bool SaveProfile(const GestureModel& m) {
    wchar_t p[MAX_PATH];
    ProfilePath(p);
    HANDLE f = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    BOOL ok = WriteFile(f, &m, sizeof(m), &wr, NULL);
    CloseHandle(f);
    return ok && wr == sizeof(m);
}

// ---------------------------------------------------------------- калибровка («обучить на моей руке»)
// Три фазы по CAL_PER кадров с рукой: раскрытая ладонь, кулак, «рабочая» рука. На них обучается
// классификатор (gesture.h); проверка — на последней четверти каждой фазы. Сырые точки
// сохраняются в calib.csv, чтобы обучение можно было повторить и проверить офлайн.
enum { CAL_PER = 200, CAL_RAW = 126 };

static int CalibLoop(Camera& cam, HANDLE stop) {
    struct Phase { int label; const wchar_t* title; const wchar_t* hint; };
    static const Phase ph[3] = {
        {0, L"1/3  ЛАДОНЬ РАСКРЫТА", L"Пальцы врозь. Водите рукой по кадру и поворачивайте её"},
        {1, L"2/3  КУЛАК", L"Сожмите кулак. Водите им по кадру и поворачивайте"},
        {0, L"3/3  РУКА КАК ПРИ РАБОТЕ", L"Расслабленная, полусогнутая, как обычно держите. Двигайте"},
    };
    const double PREP = 3.0, TIMEOUT = 45.0;
    const int maxRows = 3 * CAL_PER;
    HANDLE heap = GetProcessHeap();
    float* raw = (float*)HeapAlloc(heap, 0, sizeof(float) * maxRows * CAL_RAW);
    float* X = (float*)HeapAlloc(heap, 0, sizeof(float) * maxRows * GF_N);
    int* y = (int*)HeapAlloc(heap, 0, sizeof(int) * maxRows);
    int* phase = (int*)HeapAlloc(heap, 0, sizeof(int) * maxRows);
    unsigned char* use = (unsigned char*)HeapAlloc(heap, 0, maxRows);
    if (!raw || !X || !y || !phase || !use) return 6;

    HandTracker tr;
    Pt lm[21] = {};
    int rows = 0, rc = 0;
    wchar_t big[96], l1[160], l2[160];
    for (int p = 0; p < 3 && rc == 0; p++) {
        double start = Now();
        int got = 0;
        while (got < CAL_PER) {
            if (stop && WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) { rc = 5; break; }
            MSG msg;
            while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
            if (gHot == HK_QUIT) { rc = 5; break; }
            gHot = 0;
            int g = GrabFrame(cam);
            if (g < 0) { rc = 6; break; }
            if (g == 0) continue;
            bool hand = tr.Step(lm);
            double el = Now() - start;
            bool rec = el >= PREP;
            if (rec && hand && tr.conf >= 0.6f && HandFeatures(outWorld, outLm, X + rows * GF_N)) {
                CopyMemory(raw + rows * CAL_RAW, outWorld, sizeof(float) * 63);
                CopyMemory(raw + rows * CAL_RAW + 63, outLm, sizeof(float) * 63);
                y[rows] = ph[p].label; phase[rows] = p;
                rows++; got++;
            }
            if (el > PREP + TIMEOUT) {
                Fail(L"Калибровка остановлена: руку почти не было видно.\n\nСядьте так, чтобы рука целиком была в кадре, и запустите снова.");
                rc = 6;
                break;
            }
            if (rec) lstrcpyW(big, ph[p].title);
            else wsprintfW(big, L"Приготовьтесь… %d", (int)(PREP - el) + 1);
            lstrcpyW(l1, ph[p].hint);
            if (!hand) lstrcpyW(l2, L"Рука не видна — поднимите её в кадр целиком");
            else wsprintfW(l2, L"Записано %d из %d · Ctrl+Alt+M — отмена", got, CAL_PER);
            float px = 0, py = 0, palm = 0;
            if (hand) PalmCenter(lm, px, py, palm);
            DrawPreview(lm, hand, false, px, py, l1, l2, big, rec ? (float)got / CAL_PER : 0.f);
        }
    }

    if (rc == 0) {
        DrawPreview(lm, false, false, 0, 0, L"Обучение на ваших кадрах…", L"", L"ГОТОВО, ОБУЧАЮ", 1.f);
        // Честная проверка: учим на первых 75% каждой фазы, проверяем на последних 25%.
        TemporalSplit(phase, rows, use);
        gModel = GestureModel();
        gModel.Fit(X, y, rows, use);
        gModel.Evaluate(X, y, rows, use, 0.75f);
        float acc = gModel.valAcc, fp = gModel.valFalsePress, miss = gModel.valMiss;
        // Итоговая модель — на всех кадрах; результаты проверки сохраняем в профиль.
        gModel.Fit(X, y, rows, NULL);
        gModel.valAcc = acc; gModel.valFalsePress = fp; gModel.valMiss = miss;

        // Сырые точки — в calib.csv (для офлайн-проверки tools/calib_eval.exe).
        wchar_t cp[MAX_PATH];
        wsprintfW(cp, L"%scalib.csv", gExeDir);
        HANDLE f = CreateFileW(cp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            char line[2400];
            DWORD wr;
            lstrcpyA(line, "# label,phase, 63 world coords x1e5, 63 screen coords x100\r\n");
            WriteFile(f, line, lstrlenA(line), &wr, NULL);
            for (int i = 0; i < rows; i++) {
                int n = wsprintfA(line, "%d,%d", y[i], phase[i]);
                for (int j = 0; j < CAL_RAW; j++) {
                    float v = raw[i * CAL_RAW + j] * (j < 63 ? 100000.f : 100.f);
                    n += wsprintfA(line + n, ",%d", (int)(v < 0 ? v - 0.5f : v + 0.5f));
                }
                lstrcpyA(line + n, "\r\n");
                WriteFile(f, line, n + 2, &wr, NULL);
            }
            CloseHandle(f);
        }

        wchar_t msg[600];
        if (!SaveProfile(gModel)) {
            Fail(L"Не удалось сохранить hand_profile.bin рядом с программой.");
            rc = 6;
        } else {
            wsprintfW(msg,
                      L"Готово! Профиль вашей руки сохранён.\n\n"
                      L"Проверка на отложенных кадрах (%d кадров всего):\n"
                      L"  точность: %d%%\n  ложные нажатия: %d.%d%%\n  пропуски кулака: %d.%d%%\n\n%s",
                      rows, (int)(acc * 100 + 0.5f), (int)(fp * 100), (int)(fp * 1000) % 10, (int)(miss * 100),
                      (int)(miss * 1000) % 10,
                      acc >= 0.95f ? L"Сейчас включится управление."
                                   : L"Точность невысокая. Лучше повторить калибровку при хорошем свете, "
                                     L"держа руку целиком в кадре. Управление всё равно включится.");
            MessageBoxW(NULL, msg, L"MouseFinger — обучение", MB_ICONINFORMATION | MB_TOPMOST);
        }
    }
    HeapFree(heap, 0, raw); HeapFree(heap, 0, X); HeapFree(heap, 0, y); HeapFree(heap, 0, phase); HeapFree(heap, 0, use);
    return rc;
}

static int TrackLoop(Camera& cam, HANDLE stop);

// Сценарий живого теста (--test): фазы с командами на экране; в фазе 2 — сигналы «СЕЙЧАС!».
struct TestStep { double t0, t1; int phase; const wchar_t* title; const wchar_t* hint; };
static const TestStep kTest[] = {
    {0, 5, 0, L"ПРИГОТОВЬТЕСЬ", L"Поднимите руку в кадр, ладонью к камере"},
    {5, 15, 1, L"1/5  ЛАДОНЬ ОТКРЫТА, ВОДИТЕ", L"Пальцы врозь, кулак НЕ сжимать. Водите рукой по кадру"},
    {15, 29, 2, L"2/5  КЛИК КУЛАКОМ ПО СИГНАЛУ", L"Сожмите и сразу разожмите кулак на «СЕЙЧАС!»"},
    {29, 41, 3, L"3/5  РУКА КАК ПРИ РАБОТЕ", L"Двигайте, поворачивайте, расслабьте пальцы — но НЕ кликайте"},
    {41, 49, 4, L"4/5  ДЕРЖИТЕ РУКУ НЕПОДВИЖНО", L"Ладонь раскрыта, рука спокойно в кадре"},
    {49, 61, 5, L"5/5  ПЕРЕТАСКИВАНИЕ × 2", L"Сожмите кулак, ведите вправо, разожмите. Два раза"},
};
enum { TEST_STEPS = sizeof(kTest) / sizeof(kTest[0]), TEST_CUES = 6 };
static const double kTestCue[TEST_CUES] = {17, 19, 21, 23, 25, 27};

// ---------------------------------------------------------------- главный цикл
static int RunWorker(HINSTANCE hi) {
    LARGE_INTEGER fq; QueryPerformanceFrequency(&fq); gQpcFreq = (double)fq.QuadPart;
    LogA("worker start: managed %d, test %d, diag %d, calibrate %d, dry %d", gManaged, gTest, gDiag, gCalib, gDry);
    // Сигнал «стоп» слушаем, только если нас запустил контроллер. Запущенные вручную тест,
    // диагностика или --track не должны закрываться от чужого (возможно, оставшегося) сигнала.
    HANDLE stop = gManaged ? OpenEventW(SYNCHRONIZE, FALSE, STOP_EVENT) : NULL;
    wchar_t err[512];

    if (!InitNet(err)) { Fail(err); return 4; }
    LogA("models loaded");
    if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED))) return 2;
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) { CoUninitialize(); return 2; }

    Camera cam;
    HRESULT hr = OpenCamera(S.camera, cam);
    if (FAILED(hr)) {
        if (hr == E_ACCESSDENIED || hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED))
            lstrcpyW(err, L"Нет доступа к камере.\n\nПараметры → Конфиденциальность → Камера → разрешите доступ классическим приложениям.");
        else if (hr == MF_E_NOT_FOUND)
            lstrcpyW(err, L"Камера не найдена.");
        else
            wsprintfW(err, L"Не удалось открыть камеру (код 0x%08X).\nВозможно, она занята другой программой.", (unsigned)hr);
        Fail(err);
        if (cam.rd) cam.rd->Release();
        MFShutdown(); CoUninitialize(); FreeNet();
        return 3;
    }

    W = (int)cam.w; H = (int)cam.h;
    {
        DWORD fcc = cam.sub.Data1;
        LogA("camera opened: %dx%d %c%c%c%c", W, H, (char)(fcc & 255), (char)((fcc >> 8) & 255), (char)((fcc >> 16) & 255), (char)(fcc >> 24));
    }
    BS = (W * 3 + 3) & ~3;
    gBGR = (BYTE*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (SIZE_T)BS * H);

    CreateWindows(hi);
    RegisterHotKey(gPrev, HK_QUIT, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'M');  // только если контроллер не запущен
    RegisterHotKey(gPrev, HK_RESET, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'R');
    RegisterHotKey(gPrev, HK_MODE, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'C');
    RegisterHotKey(gPrev, HK_SCROLL, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'S');
    RegisterHotKey(gPrev, HK_PREVIEW, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'P');

    LogA("window created, entering %s loop", gCalib ? "calibration" : gTest ? "test" : gDiag ? "diag" : "tracking");
    int rc = gCalib ? CalibLoop(cam, stop) : TrackLoop(cam, stop);
    LogA("loop finished, rc %d", rc);

    DestroyWindow(gRing); DestroyWindow(gPrev);
    cam.rd->Release();
    MFShutdown();
    CoUninitialize();
    FreeNet();
    if (stop) CloseHandle(stop);
    return rc;
}

// ---------------------------------------------------------------- рабочий режим: рука ведёт курсор
static int TrackLoop(Camera& cam, HANDLE stop) {
    // Вся логика курсора и жеста — в control.h (её же проверяет tools/replay.exe на записях).
    POINT cp0; GetCursorPos(&cp0);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint(cp0, MONITOR_DEFAULTTOPRIMARY), &mi);  // монитор с курсором
    const int vsx = GetSystemMetrics(SM_XVIRTUALSCREEN), vsy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vsw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vsh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    Controller ctl;
    ctl.P.mapping = S.mapping;
    ctl.P.sens = S.sensitivity / 100.f;
    ctl.P.screenScale = GetSystemMetrics(SM_CXSCREEN) / 1920.f;
    ctl.P.deadZone = (float)S.deadZone;
    ctl.P.gOn = (S.gesture ? S.pinchOn : S.fistOn) / 100.f;
    ctl.P.gOff = (S.gesture ? S.pinchOff : S.fistOff) / 100.f;
    ctl.P.relRelease = S.gesture ? 0.15f : 0.30f;
    ctl.P.frameW = (float)W; ctl.P.frameH = (float)H;
    ctl.P.monX = (float)mi.rcMonitor.left; ctl.P.monY = (float)mi.rcMonitor.top;
    ctl.P.monW = (float)(mi.rcMonitor.right - mi.rcMonitor.left);
    ctl.P.monH = (float)(mi.rcMonitor.bottom - mi.rcMonitor.top);
    float zone = 0.65f / ctl.P.sens;  // абсолютный режим: доля кадра на весь монитор
    if (zone < 0.15f) zone = 0.15f; if (zone > 0.95f) zone = 0.95f;
    ctl.P.zone = zone;
    ctl.SyncCursor((float)cp0.x, (float)cp0.y);
    const wchar_t* gName = S.gesture ? L"щипок" : L"кулак";

    // Персональный профиль руки (если калибровали): пороги — по вероятности кулака.
    // Нажатие при p > 0.75, отпускание при p < 0.4 или при падении на 0.35 от максимума.
    const bool useModel = S.gesture == 0 && LoadProfile(gModel);
    // Классификатор уверенный (тест: p≈1 даже на 12 ладонях/с) — хватает 2 кадров подтверждения.
    if (useModel) { ctl.P.gOn = 0.25f; ctl.P.gOff = 0.60f; ctl.P.relRelease = 0.35f; ctl.P.confirmFrames = 2; }

    HandTracker tr;
    bool hand = false, scroll = false, btnDown = false;
    int mode = S.clickMode;
    Pt lm[21] = {};
    float wheelAcc = 0;
    POINT lastSet = cp0;
    wchar_t l1[160], l2[128];
    if (S.gesture == 0 && !useModel) lstrcpyW(l2, L"Совет: трей → «Обучить на моей руке» — клик станет точнее");
    else lstrcpyW(l2, L"Ctrl+Alt: C режим · S скролл · P окно · R сброс");

    // Диагностика: 30 секунд пишем значения в diag.csv и каждые 2.5 с — снимок превью.
    HANDLE diag = INVALID_HANDLE_VALUE;
    const double diagStart = Now();
    double nextShot = 2.0;
    int shot = 0;
    if (gDiag) {
        wchar_t p[MAX_PATH];
        wsprintfW(p, L"%sdiag.csv", gExeDir);
        diag = CreateFileW(p, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD fcc = cam.sub.Data1, wr;
        char hdr[400];
        wsprintfA(hdr, "# camera %dx%d %c%c%c%c, threads %d, gesture %d, close %d, open %d, mapping %d, sens %d, model %d\r\n"
                       "t_ms,infer_ms,palm_ran,palm_found,conf,pinch_scr,pinch_world,fist_scr,fist_world,metric,pressed,dragging,px,py,palm,cur_x,cur_y\r\n",
                  W, H, (char)(fcc & 255), (char)((fcc >> 8) & 255), (char)((fcc >> 16) & 255), (char)(fcc >> 24), S.threads,
                  S.gesture, (int)(ctl.P.gOn * 100 + 0.5f), (int)(ctl.P.gOff * 100 + 0.5f), S.mapping, S.sensitivity, useModel ? 1 : 0);
        WriteFile(diag, hdr, lstrlenA(hdr), &wr, NULL);
        lstrcpyW(l2, S.gesture ? L"ДИАГНОСТИКА 30 с: сводите пальцы, водите рукой"
                               : L"ДИАГНОСТИКА 30 с: сжимайте кулак, водите рукой");
        ShowWindow(gPrev, SW_SHOWNOACTIVATE);
    }
    if (gTest) {
        wchar_t p[MAX_PATH];
        wsprintfW(p, L"%stest.csv", gExeDir);
        diag = CreateFileW(p, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        char hdr[400];
        DWORD wr;
        int n = wsprintfA(hdr, "# test v1, camera %dx%d, model %d, gOn %d, gOff %d, rel %d, mapping %d, sens %d\r\n"
                               "t_ms,phase,cue,hand,conf,pfist_x1000,metric_x1000,curl_x100,pressed,dragging,ev,px10,py10,palm10,cur_x,cur_y,infer_ms\r\n",
                          W, H, useModel ? 1 : 0, (int)(ctl.P.gOn * 100 + 0.5f), (int)(ctl.P.gOff * 100 + 0.5f),
                          (int)(ctl.P.relRelease * 100 + 0.5f), S.mapping, S.sensitivity);
        WriteFile(diag, hdr, n, &wr, NULL);
    }
    TrimMemory();

    for (;;) {
        if (stop && WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) break;
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        int hk = gHot; gHot = 0;
        if (hk == HK_QUIT) break;
        if (hk == HK_RESET) tr.haveRoi = false;
        if (hk == HK_MODE || hk == HK_SCROLL) {
            if (btnDown) { SendOne(MOUSEEVENTF_LEFTUP); btnDown = false; }
            if (hk == HK_MODE) mode = (mode + 1) % 3; else { scroll = !scroll; wheelAcc = 0; }
        }
        if (hk == HK_PREVIEW) ShowWindow(gPrev, IsWindowVisible(gPrev) ? SW_HIDE : SW_SHOWNOACTIVATE);

        int gr = GrabFrame(cam);
        if (gr < 0) break;  // камеру отключили
        if (gr == 0) continue;
        double now = Now();

        hand = tr.Step(lm);
        bool palmRan = tr.palmRan, palmFound = tr.palmFound;
        float conf = tr.conf;
        int inferMs = (int)((Now() - now) * 1000);

        // Центр ладони: среднее запястья и оснований четырёх пальцев. Эти точки почти не
        // двигаются при сжатии кулака, а усреднение пяти точек гасит дрожь модели.
        float px = 0, py = 0, palm = 0, metric = 9.f, pFist = 0;
        if (hand) {
            PalmCenter(lm, px, py, palm);
            if (useModel) {
                // Персональный профиль: метрика = 1 − вероятность кулака (меньше = «сильнее сжато»).
                float feat[GF_N];
                pFist = HandFeatures(outWorld, outLm, feat) ? gModel.Prob(feat) : 0.f;
                metric = 1.f - pFist;
            } else {
                metric = S.gesture ? PinchRatio() : FistRatio();
            }
            if (gDiag || gTest) { PinchRatio(); FistRatio(); }  // в журнал пишем и геометрические метрики
        }

        // Двигали обычную мышь — подхватываем её позицию. В режиме прокрутки курсор не наш.
        POINT cp; GetCursorPos(&cp);
        if (scroll || (!gDry && (abs(cp.x - lastSet.x) > 2 || abs(cp.y - lastSet.y) > 2))) {
            ctl.SyncCursor((float)cp.x, (float)cp.y);
            lastSet = cp;
        }

        CtlOut o = ctl.Step(now, hand, px, py, palm, metric);

        if (o.setCursor && !scroll) {
            LONG x = (LONG)floorf(o.x + 0.5f), y = (LONG)floorf(o.y + 0.5f);
            POINT a = {x, y};
            if (!gDry) {
                SetCursorPos(x, y);
                GetCursorPos(&a);
            } else {
                if (a.x < vsx) a.x = vsx; if (a.x > vsx + vsw - 1) a.x = vsx + vsw - 1;
                if (a.y < vsy) a.y = vsy; if (a.y > vsy + vsh - 1) a.y = vsy + vsh - 1;
            }
            // Windows не пускает курсор за край монитора — не копим «долг» за краем.
            if (abs(a.x - x) > 1 || abs(a.y - y) > 1) ctl.SyncCursor((float)a.x, (float)a.y);
            lastSet = a;
        }
        // Курсор уже стоит в точке клика (защёлка) — теперь нажимаем.
        if (o.ev == EV_PRESS && !scroll) {
            if (mode == 0) { SendOne(MOUSEEVENTF_LEFTDOWN); btnDown = true; }
            else if (mode == 1) Click(MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP, 1);
            else Click(MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP, 2);
        }
        if (o.ev == EV_RELEASE && btnDown) { SendOne(MOUSEEVENTF_LEFTUP); btnDown = false; }
        if (scroll && o.pressed) {  // «схватил страницу и тянешь»: рука вниз — страница вниз
            wheelAcc += o.dyPalm * 600.f * S.scrollSpeed / 100.f;
            if (wheelAcc >= 40.f || wheelAcc <= -40.f) {
                int d = (int)wheelAcc;
                SendOne(MOUSEEVENTF_WHEEL, (DWORD)d);
                wheelAcc -= d;
            }
        }

        if (!hand) lstrcpyW(l1, L"Покажите руку ладонью к камере");
        else if (scroll) wsprintfW(l1, L"ПРОКРУТКА: %s + тяни · %s %d", gName, gName, (int)(metric * 100));
        else if (useModel) wsprintfW(l1, L"%s%s · кулак %d%% (профиль)", kModeNames[mode],
                                     o.dragging ? L" · тащу" : o.pressed ? L" ●" : L"", (int)(pFist * 100 + 0.5f));
        else wsprintfW(l1, L"%s%s · %s %d (нажать < %d)", kModeNames[mode],
                       o.dragging ? L" · тащу" : o.pressed ? L" ●" : L"", gName, (int)(metric * 100),
                       (int)(ctl.P.gOn * 100 + 0.5f));
        if (!scroll) DrawRing(o.hand ? o.progress : 0, o.pressed ? RGB(255, 60, 60) : kModeColors[mode], lastSet.x, lastSet.y);
        else DrawRing(0, 0, 0, 0);

        // Живой тест: команды на экране и покадровый журнал test.csv.
        const wchar_t* big = NULL;
        float prog = -1.f;
        if (gTest) {
            double el = now - diagStart;
            int si = 0;
            while (si < TEST_STEPS - 1 && el >= kTest[si].t1) si++;
            const TestStep& st = kTest[si];
            int cue = 0;
            if (st.phase == 2)  // сигналы «СЕЙЧАС!» каждые 2 с, по 0.8 с
                for (int k = 0; k < TEST_CUES; k++)
                    if (el >= kTestCue[k] && el < kTestCue[k] + 0.8) cue = 1;
            big = cue ? L"▶  СЕЙЧАС!  ◀" : st.title;
            lstrcpyW(l2, st.hint);
            prog = (float)((el - st.t0) / (st.t1 - st.t0));
            char line[240];
            DWORD wr;
            int n = wsprintfA(line, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\r\n", (int)(el * 1000), st.phase, cue,
                              hand ? 1 : 0, (int)(conf * 100), hand && useModel ? (int)(pFist * 1000) : -1,
                              hand ? (int)(metric * 1000) : -1, hand ? (int)(gFistScr * 50 + gFistWorld * 50) : -1,
                              o.pressed, o.dragging, (int)o.ev, (int)(px * 10), (int)(py * 10), (int)(palm * 10),
                              (int)o.x, (int)o.y, inferMs);
            WriteFile(diag, line, n, &wr, NULL);
            if (el >= kTest[TEST_STEPS - 1].t1) {
                PROCESS_MEMORY_COUNTERS_EX pm = {};
                pm.cb = sizeof(pm);
                GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof(pm));
                n = wsprintfA(line, "# memory: working set %d KB, private %d KB\r\n", (int)(pm.WorkingSetSize / 1024),
                              (int)(pm.PrivateUsage / 1024));
                WriteFile(diag, line, n, &wr, NULL);
                CloseHandle(diag);
                DrawPreview(lm, hand, o.pressed, px, py, L"Спасибо! Результаты записаны.", L"", L"ТЕСТ ЗАВЕРШЁН", 1.f);
                Sleep(1500);
                break;
            }
        }
        DrawPreview(lm, hand, o.pressed, px, py, l1, l2, big, prog);

        if (gDiag) {
            double el = now - diagStart;
            char line[240];
            DWORD wr;
            wsprintfA(line, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\r\n", (int)(el * 1000), inferMs, palmRan, palmFound,
                      (int)(conf * 100), hand ? (int)(gPinchScr * 100) : -1, hand ? (int)(gPinchWorld * 100) : -1,
                      hand ? (int)(gFistScr * 100) : -1, hand ? (int)(gFistWorld * 100) : -1,
                      hand ? (int)(metric * 100) : -1, o.pressed, o.dragging, (int)(px * 10), (int)(py * 10),
                      (int)(palm * 10), (int)o.x, (int)o.y);
            WriteFile(diag, line, lstrlenA(line), &wr, NULL);
            if (el >= nextShot && shot < 12) {
                wchar_t p[MAX_PATH];
                wsprintfW(p, L"%sdiag_%02d.bmp", gExeDir, ++shot);
                SavePreviewBmp(p);
                nextShot += 2.5;
            }
            if (el >= 30.0) {
                PROCESS_MEMORY_COUNTERS_EX pm = {};
                pm.cb = sizeof(pm);
                GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof(pm));
                wsprintfA(line, "# memory: working set %d KB, private %d KB, peak WS %d KB\r\n", (int)(pm.WorkingSetSize / 1024),
                          (int)(pm.PrivateUsage / 1024), (int)(pm.PeakWorkingSetSize / 1024));
                WriteFile(diag, line, lstrlenA(line), &wr, NULL);
                CloseHandle(diag);
                MessageBoxW(NULL, L"Диагностика записана в diag.csv и diag_*.bmp рядом с программой.", L"MouseFinger",
                            MB_ICONINFORMATION | MB_TOPMOST);
                break;
            }
        }
    }

    if (btnDown) SendOne(MOUSEEVENTF_LEFTUP);
    return 0;
}

// =================================================================== КОНТРОЛЛЕР (трей + хоткей)

#define WM_TRAY (WM_APP + 1)
enum { ID_TOGGLE = 1, ID_SETTINGS, ID_EXIT, ID_CALIB, HK_TOGGLE = 100 };
static HANDLE gWorker, gStop, gJob;
static NOTIFYICONDATAW gNid;
static HICON gIcoOn, gIcoOff;
static UINT gTaskbarCreated;

static HICON MakeIcon(COLORREF c) {
    int s = GetSystemMetrics(SM_CXSMICON);
    HDC sdc = GetDC(NULL), dc = CreateCompatibleDC(sdc);
    HBITMAP col = CreateCompatibleBitmap(sdc, s, s), mask = CreateBitmap(s, s, 1, 1, NULL);
    ReleaseDC(NULL, sdc);
    RECT r = {0, 0, s, s};
    HBRUSH b = CreateSolidBrush(c);
    HGDIOBJ ob = SelectObject(dc, col), op = SelectObject(dc, GetStockObject(NULL_PEN)), obr = SelectObject(dc, b);
    FillRect(dc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
    Ellipse(dc, 1, 1, s, s);
    SelectObject(dc, GetStockObject(WHITE_BRUSH));
    Ellipse(dc, s / 2 - 2, s / 2 - 2, s / 2 + 3, s / 2 + 3);
    SelectObject(dc, mask);
    FillRect(dc, &r, (HBRUSH)GetStockObject(WHITE_BRUSH));
    SelectObject(dc, GetStockObject(BLACK_BRUSH));
    Ellipse(dc, 1, 1, s, s);
    SelectObject(dc, ob); SelectObject(dc, op); SelectObject(dc, obr);
    DeleteObject(b); DeleteDC(dc);
    ICONINFO ii = {TRUE, 0, 0, mask, col};
    HICON ic = CreateIconIndirect(&ii);
    DeleteObject(col); DeleteObject(mask);
    return ic;
}

static bool WorkerRunning() { return gWorker && WaitForSingleObject(gWorker, 0) == WAIT_TIMEOUT; }

static void UpdateTray() {
    bool on = WorkerRunning();
    gNid.uFlags = NIF_ICON | NIF_TIP;
    gNid.hIcon = on ? gIcoOn : gIcoOff;
    lstrcpynW(gNid.szTip, on ? L"MouseFinger: ВКЛ (Ctrl+Alt+M)" : L"MouseFinger: выкл (Ctrl+Alt+M)", 128);
    Shell_NotifyIconW(NIM_MODIFY, &gNid);
}

static bool gCalibRun;  // сейчас идёт калибровка — после успеха сразу включим управление

static void StartWorker(bool calib = false) {
    ResetEvent(gStop);
    gCalibRun = calib;
    wchar_t exe[MAX_PATH], cmd[MAX_PATH + 32];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wsprintfW(cmd, calib ? L"\"%s\" --track --managed --calibrate" : L"\"%s\" --track --managed", exe);
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi;
    if (CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi)) {
        AssignProcessToJobObject(gJob, pi.hProcess);
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        gWorker = pi.hProcess;
    }
}

static void StopWorker() {
    gCalibRun = false;
    if (!gWorker) return;
    SetEvent(gStop);
    if (WaitForSingleObject(gWorker, 3000) != WAIT_OBJECT_0) TerminateProcess(gWorker, 1);
    ResetEvent(gStop);  // не оставляем сигнал выставленным — иначе следующий запуск сразу закроется
    CloseHandle(gWorker);
    gWorker = NULL;
}

static void Toggle() {
    if (WorkerRunning()) StopWorker();
    else {
        if (gWorker) { CloseHandle(gWorker); gWorker = NULL; }
        StartWorker();
    }
    UpdateTray();
}

static void AddTray(HWND h) {
    gNid.cbSize = sizeof(gNid);
    gNid.hWnd = h;
    gNid.uID = 1;
    gNid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    gNid.uCallbackMessage = WM_TRAY;
    gNid.hIcon = gIcoOff;
    lstrcpyW(gNid.szTip, L"MouseFinger: выкл (Ctrl+Alt+M)");
    Shell_NotifyIconW(NIM_ADD, &gNid);
    UpdateTray();
}

static LRESULT CALLBACK CtlProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == gTaskbarCreated && m) { AddTray(h); return 0; }
    switch (m) {
    case WM_HOTKEY: if (w == HK_TOGGLE) Toggle(); return 0;
    case WM_TRAY:
        if (LOWORD(l) == WM_LBUTTONUP) Toggle();
        else if (LOWORD(l) == WM_RBUTTONUP) {
            HMENU mn = CreatePopupMenu();
            AppendMenuW(mn, MF_STRING, ID_TOGGLE, WorkerRunning() ? L"Выключить\tCtrl+Alt+M" : L"Включить\tCtrl+Alt+M");
            AppendMenuW(mn, MF_STRING, ID_CALIB, L"Обучить на моей руке…");
            AppendMenuW(mn, MF_STRING, ID_SETTINGS, L"Настройки…");
            AppendMenuW(mn, MF_SEPARATOR, 0, NULL);
            AppendMenuW(mn, MF_STRING, ID_EXIT, L"Выход");
            POINT p; GetCursorPos(&p);
            SetForegroundWindow(h);
            int cmd = TrackPopupMenu(mn, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, p.x, p.y, 0, h, NULL);
            DestroyMenu(mn);
            if (cmd == ID_TOGGLE) Toggle();
            else if (cmd == ID_CALIB) {
                StopWorker();
                if (gWorker) { CloseHandle(gWorker); gWorker = NULL; }
                StartWorker(true);
                UpdateTray();
            }
            else if (cmd == ID_SETTINGS) { EnsureIni(); ShellExecuteW(NULL, L"open", L"notepad.exe", gIniPath, NULL, SW_SHOWNORMAL); }
            else if (cmd == ID_EXIT) DestroyWindow(h);
            TrimMemory();
        }
        return 0;
    case WM_DESTROY:
        StopWorker();
        Shell_NotifyIconW(NIM_DELETE, &gNid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static int RunController(HINSTANCE hi) {
    HANDLE mtx = CreateMutexW(NULL, TRUE, L"Local\\MouseFinger.Controller");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"MouseFinger уже запущен (иконка в трее).", L"MouseFinger", MB_ICONINFORMATION);
        return 0;
    }
    EnsureIni();
    gStop = CreateEventW(NULL, TRUE, FALSE, STOP_EVENT);
    gJob = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jl = {};
    jl.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(gJob, JobObjectExtendedLimitInformation, &jl, sizeof(jl));

    gIcoOn = MakeIcon(RGB(40, 200, 90));
    gIcoOff = MakeIcon(RGB(120, 120, 120));
    gTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSW wc = {};
    wc.lpfnWndProc = CtlProc;
    wc.hInstance = hi;
    wc.lpszClassName = L"MouseFingerCtl";
    RegisterClassW(&wc);
    HWND h = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"MouseFinger", WS_POPUP, 0, 0, 0, 0, NULL, NULL, hi, NULL);
    AddTray(h);
    if (!RegisterHotKey(h, HK_TOGGLE, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'M'))
        MessageBoxW(NULL, L"Сочетание Ctrl+Alt+M занято другой программой.\nВключайте кликом по иконке в трее.",
                    L"MouseFinger", MB_ICONWARNING);
    TrimMemory();

    for (;;) {
        DWORD n = gWorker ? 1 : 0;
        DWORD r = MsgWaitForMultipleObjects(n, &gWorker, FALSE, INFINITE, QS_ALLINPUT);
        if (n && r == WAIT_OBJECT_0) {
            DWORD code = 1;
            GetExitCodeProcess(gWorker, &code);
            CloseHandle(gWorker); gWorker = NULL;
            // Калибровка прошла успешно — сразу включаем управление с новым профилем.
            if (gCalibRun && code == 0) StartWorker(false);
            gCalibRun = false;
            UpdateTray();
            TrimMemory();
            continue;
        }
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { CloseHandle(gJob); CloseHandle(mtx); return 0; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, LPWSTR, int) {
    SetDpiAware();
    InitPaths();
    bool track = false;
    int argc = 0;
    LPWSTR* av = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; i++) {
        if (!lstrcmpiW(av[i], L"--track")) track = true;
        if (!lstrcmpiW(av[i], L"--dry")) gDry = true;
        if (!lstrcmpiW(av[i], L"--diag")) gDiag = gDry = true;  // диагностика никогда не трогает мышь
        if (!lstrcmpiW(av[i], L"--calibrate")) gCalib = gDry = true;  // калибровка тоже не трогает мышь
        if (!lstrcmpiW(av[i], L"--test")) gTest = gDry = true;         // живой тест по сценарию, мышь не трогает
        if (!lstrcmpiW(av[i], L"--managed")) gManaged = true;          // запущен контроллером — слушать сигнал «стоп»
    }
    LocalFree(av);
    if (!track) { UpgradeIni(); EnsureIni(); }
    LoadSettings();
    return track ? RunWorker(hi) : RunController(hi);
}
