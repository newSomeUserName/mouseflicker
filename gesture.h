// gesture.h — персональный классификатор жеста «кулак» поверх точек руки MediaPipe.
// Без зависимостей от Windows: признаки, обучение и предсказание. Проверяется tools/gesture_test.cpp
// и tools/calib_eval.cpp (на записи калибровки calib.csv).
#pragma once
#include <math.h>
#include <string.h>

// Признаки формы руки. Точки переводятся в «систему координат ладони»: начало — запястье,
// ось Y — к основанию среднего пальца, ось X — от основания указательного к мизинцу, Z — нормаль
// ладони; всё делится на длину ладони. Поэтому признаки не зависят от того, где рука в кадре,
// как далеко от камеры и как повёрнута — остаётся только форма (насколько согнуты пальцы).
enum { GF_POINTS = 60, GF_N = GF_POINTS * 2 + 8 };  // мировые + экранные точки + сгибы пальцев

static inline void GfSub(const float* a, int i, int j, float* o) {
    o[0] = a[i * 3] - a[j * 3]; o[1] = a[i * 3 + 1] - a[j * 3 + 1]; o[2] = a[i * 3 + 2] - a[j * 3 + 2];
}
static inline float GfDot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static inline float GfLen(const float* a) { return sqrtf(GfDot(a, a)); }

// 60 координат (точки 1..20) в системе ладони. Возвращает false, если ладонь вырождена.
static bool GfCanonical(const float* p, float* out) {
    float y[3], xt[3], x[3], z[3];
    GfSub(p, 9, 0, y);
    float scale = GfLen(y);
    if (scale < 1e-6f) return false;
    for (int k = 0; k < 3; k++) y[k] /= scale;
    GfSub(p, 17, 5, xt);
    float d = GfDot(xt, y);
    for (int k = 0; k < 3; k++) x[k] = xt[k] - d * y[k];
    float lx = GfLen(x);
    if (lx < 1e-6f) return false;
    for (int k = 0; k < 3; k++) x[k] /= lx;
    z[0] = x[1] * y[2] - x[2] * y[1];
    z[1] = x[2] * y[0] - x[0] * y[2];
    z[2] = x[0] * y[1] - x[1] * y[0];
    for (int i = 1; i <= 20; i++) {
        float v[3];
        GfSub(p, i, 0, v);
        out[(i - 1) * 3 + 0] = GfDot(v, x) / scale;
        out[(i - 1) * 3 + 1] = GfDot(v, y) / scale;
        out[(i - 1) * 3 + 2] = GfDot(v, z) / scale;
    }
    return true;
}

// Сгиб пальцев: «кончик→запястье / основание→запястье» для указательного..мизинца.
static void GfCurls(const float* p, float* out) {
    static const int f[4][2] = {{5, 8}, {9, 12}, {13, 16}, {17, 20}};
    for (int i = 0; i < 4; i++) {
        float a[3], b[3];
        GfSub(p, f[i][0], 0, a);
        GfSub(p, f[i][1], 0, b);
        float m = GfLen(a);
        out[i] = m > 1e-6f ? GfLen(b) / m : 0.f;
    }
}

// world — 63 «мировые» координаты модели (метры), scr — 63 координаты в пикселях кропа.
static bool HandFeatures(const float* world, const float* scr, float* f) {
    if (!GfCanonical(world, f) || !GfCanonical(scr, f + GF_POINTS)) return false;
    GfCurls(world, f + GF_POINTS * 2);
    GfCurls(scr, f + GF_POINTS * 2 + 4);
    for (int j = 0; j < GF_N; j++)  // нечисло от модели — кадр не годится (x - x != 0 только у NaN/inf)
        if (!(f[j] - f[j] == 0.f)) return false;
    return true;
}

// Логистическая регрессия на стандартизованных признаках с L2-регуляризацией.
struct GestureModel {
    char magic[4] = {'M', 'F', 'H', 'P'};
    int version = 1, n = GF_N;
    float mean[GF_N], stdv[GF_N], w[GF_N], b = 0;
    float valAcc = 0, valFalsePress = 0, valMiss = 0;  // результаты проверки на отложенных данных
    int samples = 0;

    bool Valid() const { return magic[0] == 'M' && magic[1] == 'F' && magic[2] == 'H' && magic[3] == 'P' && version == 1 && n == GF_N; }

    float Prob(const float* f) const {
        float z = b;
        for (int j = 0; j < GF_N; j++) z += w[j] * (f[j] - mean[j]) / stdv[j];
        if (z > 30) z = 30;
        if (z < -30) z = -30;
        return 1.f / (1.f + expf(-z));
    }

    // X — n строк по GF_N признаков, y — 1 «кулак» / 0 «не кулак»; use — какие строки брать (или NULL).
    void Fit(const float* X, const int* y, int rows, const unsigned char* use, int iters = 400, float l2 = 0.01f) {
        int cnt = 0, pos = 0;
        for (int j = 0; j < GF_N; j++) { mean[j] = 0; stdv[j] = 0; w[j] = 0; }
        b = 0;
        for (int i = 0; i < rows; i++) {
            if (use && !use[i]) continue;
            cnt++; pos += y[i] ? 1 : 0;
            for (int j = 0; j < GF_N; j++) mean[j] += X[i * GF_N + j];
        }
        if (cnt == 0 || pos == 0 || pos == cnt) return;
        for (int j = 0; j < GF_N; j++) mean[j] /= cnt;
        for (int i = 0; i < rows; i++) {
            if (use && !use[i]) continue;
            for (int j = 0; j < GF_N; j++) { float d = X[i * GF_N + j] - mean[j]; stdv[j] += d * d; }
        }
        for (int j = 0; j < GF_N; j++) { stdv[j] = sqrtf(stdv[j] / cnt); if (stdv[j] < 1e-3f) stdv[j] = 1e-3f; }
        // Веса классов выравнивают их вклад, даже если кадров кулака меньше.
        float wPos = cnt / (2.f * pos), wNeg = cnt / (2.f * (cnt - pos));
        // Adam.
        float m[GF_N + 1] = {}, v[GF_N + 1] = {}, g[GF_N + 1];
        const float lr = 0.05f, b1 = 0.9f, b2 = 0.999f;
        float b1t = 1, b2t = 1;
        for (int it = 0; it < iters; it++) {
            for (int j = 0; j <= GF_N; j++) g[j] = 0;
            for (int i = 0; i < rows; i++) {
                if (use && !use[i]) continue;
                const float* x = X + i * GF_N;
                float z = b;
                for (int j = 0; j < GF_N; j++) z += w[j] * (x[j] - mean[j]) / stdv[j];
                if (z > 30) z = 30;
                if (z < -30) z = -30;
                float p = 1.f / (1.f + expf(-z));
                float e = (p - (y[i] ? 1.f : 0.f)) * (y[i] ? wPos : wNeg) / cnt;
                for (int j = 0; j < GF_N; j++) g[j] += e * (x[j] - mean[j]) / stdv[j];
                g[GF_N] += e;
            }
            for (int j = 0; j < GF_N; j++) g[j] += l2 * w[j];
            b1t *= b1; b2t *= b2;
            for (int j = 0; j <= GF_N; j++) {
                m[j] = b1 * m[j] + (1 - b1) * g[j];
                v[j] = b2 * v[j] + (1 - b2) * g[j] * g[j];
                float step = lr * (m[j] / (1 - b1t)) / (sqrtf(v[j] / (1 - b2t)) + 1e-8f);
                if (j < GF_N) w[j] -= step; else b -= step;
            }
        }
        samples = cnt;
    }

    // Проверка: доля верных ответов (порог 0.5), ложные «нажатия» (не-кулак с p > pressP)
    // и пропуски (кулак с p < pressP) среди строк, где use[i] == 0 (отложенные данные).
    void Evaluate(const float* X, const int* y, int rows, const unsigned char* use, float pressP) {
        int n0 = 0, ok = 0, neg = 0, fp = 0, pos = 0, miss = 0;
        for (int i = 0; i < rows; i++) {
            if (use && use[i]) continue;
            float p = Prob(X + i * GF_N);
            n0++;
            if ((p >= 0.5f) == (y[i] != 0)) ok++;
            if (y[i]) { pos++; if (p < pressP) miss++; }
            else { neg++; if (p > pressP) fp++; }
        }
        valAcc = n0 ? (float)ok / n0 : 0;
        valFalsePress = neg ? (float)fp / neg : 0;
        valMiss = pos ? (float)miss / pos : 0;
    }
};

// Разбиение «по времени»: в каждой фазе первые 75% кадров — обучение, последние 25% — проверка.
// Так проверка честнее, чем случайная: соседние кадры почти одинаковые.
static void TemporalSplit(const int* phase, int rows, unsigned char* use) {
    int start = 0;
    while (start < rows) {
        int end = start;
        while (end < rows && phase[end] == phase[start]) end++;
        int cut = start + (end - start) * 3 / 4;
        for (int i = start; i < end; i++) use[i] = i < cut ? 1 : 0;
        start = end;
    }
}
