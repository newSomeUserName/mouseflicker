// Тесты gesture.h на синтетической руке (камера не нужна).
// 1) признаки не меняются при повороте, масштабе и сдвиге руки;
// 2) классификатор учится отличать кулак от раскрытой и полусогнутой руки;
// 3) промежуточный сгиб даёт промежуточную вероятность (модель не «щёлкает» случайно).
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <vector>
#include "../gesture.h"
#include "../control.h"

static float Rnd() { return rand() / (float)RAND_MAX; }
static float Gauss() {
    float u = (rand() + 1.f) / (RAND_MAX + 2.f), v = Rnd();  // u строго в (0, 1)
    return sqrtf(-2 * logf(u)) * cosf(6.2831853f * v);
}

// Синтетическая правая рука в своей системе: запястье в 0, ладонь в плоскости XY, пальцы вдоль +Y.
// curl — угол сгиба каждого сустава (рад) вокруг оси X в сторону ладони (−Z).
static void MakeHand(float curl, float thumbCurl, float noise, float* p) {
    const float mcpX[4] = {-0.035f, -0.012f, 0.010f, 0.030f}, mcpY[4] = {0.090f, 0.092f, 0.086f, 0.078f};
    const float seg[4][3] = {{0.040f, 0.025f, 0.020f}, {0.045f, 0.028f, 0.021f}, {0.042f, 0.026f, 0.020f}, {0.033f, 0.020f, 0.018f}};
    p[0] = p[1] = p[2] = 0;
    // большой палец: 1..4
    float tx = -0.030f, ty = 0.030f, tz = 0, ang = 0.6f;
    for (int j = 0; j < 4; j++) {
        p[(1 + j) * 3] = tx; p[(1 + j) * 3 + 1] = ty; p[(1 + j) * 3 + 2] = tz;
        float a = ang + thumbCurl * (j + 1) * 0.5f;
        tx -= 0.03f * sinf(a) * (j == 0 ? 0.5f : 1.f);
        ty += 0.03f * cosf(a);
        tz -= 0.01f * thumbCurl * j;
    }
    for (int f = 0; f < 4; f++) {
        int base = 5 + f * 4;
        float x = mcpX[f], y = mcpY[f], z = 0, a = 0;
        p[base * 3] = x; p[base * 3 + 1] = y; p[base * 3 + 2] = z;
        for (int s = 0; s < 3; s++) {
            a += curl;
            y += seg[f][s] * cosf(a);
            z -= seg[f][s] * sinf(a);
            p[(base + 1 + s) * 3] = x; p[(base + 1 + s) * 3 + 1] = y; p[(base + 1 + s) * 3 + 2] = z;
        }
    }
    for (int i = 0; i < 63; i++) p[i] += noise * Gauss();
}

// Случайный поворот, масштаб и сдвиг (как если бы рука была в другом месте кадра).
static void Transform(const float* in, float* out, float s, float tx, float ty, float tz) {
    float a = Rnd() * 6.28f, b = Rnd() * 6.28f, c = Rnd() * 6.28f;
    float R[9] = {cosf(a) * cosf(b), cosf(a) * sinf(b) * sinf(c) - sinf(a) * cosf(c), cosf(a) * sinf(b) * cosf(c) + sinf(a) * sinf(c),
                  sinf(a) * cosf(b), sinf(a) * sinf(b) * sinf(c) + cosf(a) * cosf(c), sinf(a) * sinf(b) * cosf(c) - cosf(a) * sinf(c),
                  -sinf(b), cosf(b) * sinf(c), cosf(b) * cosf(c)};
    for (int i = 0; i < 21; i++) {
        const float* v = in + i * 3;
        for (int k = 0; k < 3; k++) out[i * 3 + k] = s * (R[k * 3] * v[0] + R[k * 3 + 1] * v[1] + R[k * 3 + 2] * v[2]) + (k == 0 ? tx : k == 1 ? ty : tz);
    }
}

// «Экранные» точки: та же рука в пикселях кропа (масштаб ~1000, другое положение и поворот).
static void ToScreen(const float* world, float* scr) { Transform(world, scr, 900 + 300 * Rnd(), 112, 112, 0); }

int main() {
    srand(12345);
    int fails = 0;

    // ---- 1. инвариантность
    {
        float h[63], t[63], s1[63], s2[63], f1[GF_N], f2[GF_N];
        float maxd = 0;
        for (int k = 0; k < 200; k++) {
            MakeHand(Rnd() * 1.4f, Rnd(), 0.f, h);
            Transform(h, t, 0.5f + 2 * Rnd(), Gauss(), Gauss(), Gauss());
            ToScreen(h, s1); ToScreen(t, s2);
            if (!HandFeatures(h, s1, f1) || !HandFeatures(t, s2, f2)) { printf("FAIL: degenerate features\n"); fails++; break; }
            for (int j = 0; j < GF_N; j++) maxd = fmaxf(maxd, fabsf(f1[j] - f2[j]));
        }
        printf("invariance: max feature difference after rotate/scale/shift = %.2e  %s\n", maxd, maxd < 1e-3f ? "OK" : "FAIL");
        if (maxd >= 1e-3f) fails++;
    }

    // ---- 2. обучение: «калибровка» как в программе — 3 фазы по 200 кадров
    std::vector<float> X;
    std::vector<int> y, phase;
    auto addPhase = [&](int ph, int label, float cMin, float cMax, float tMin, float tMax) {
        float h[63], w[63], s[63], f[GF_N];
        for (int k = 0; k < 200; k++) {
            MakeHand(cMin + (cMax - cMin) * Rnd(), tMin + (tMax - tMin) * Rnd(), 0.004f, h);  // шум ~4 мм
            Transform(h, w, 1.f, 0, 0, 0);
            ToScreen(h, s);
            if (!HandFeatures(w, s, f)) continue;
            X.insert(X.end(), f, f + GF_N); y.push_back(label); phase.push_back(ph);
        }
    };
    addPhase(0, 0, 0.00f, 0.25f, 0.0f, 0.4f);  // ладонь раскрыта
    addPhase(1, 1, 1.20f, 1.55f, 0.6f, 1.2f);  // кулак
    addPhase(2, 0, 0.30f, 0.75f, 0.0f, 0.8f);  // «рабочая» полусогнутая рука
    int rows = (int)y.size();
    std::vector<unsigned char> use(rows);
    TemporalSplit(phase.data(), rows, use.data());
    static GestureModel m;
    m.Fit(X.data(), y.data(), rows, use.data());
    m.Evaluate(X.data(), y.data(), rows, use.data(), 0.75f);
    printf("training: rows %d, held-out accuracy %.1f%%, false press %.1f%%, miss %.1f%%  %s\n", rows, m.valAcc * 100,
           m.valFalsePress * 100, m.valMiss * 100, (m.valAcc > 0.97f && m.valFalsePress < 0.02f) ? "OK" : "FAIL");
    if (!(m.valAcc > 0.97f && m.valFalsePress < 0.02f)) fails++;

    // ---- 3. монотонность: вероятность растёт со сгибом
    {
        float h[63], s[63], f[GF_N], prev = -1;
        bool mono = true;
        printf("p(fist) by curl angle:");
        for (int k = 0; k <= 14; k++) {
            float c = k * 0.1f;
            MakeHand(c, c * 0.7f, 0.f, h);
            ToScreen(h, s);
            HandFeatures(h, s, f);
            float p = m.Prob(f);
            printf(" %.1f:%.2f", c, p);
            if (p < prev - 0.05f) mono = false;
            prev = p;
        }
        printf("  %s\n", mono ? "OK" : "FAIL");
        if (!mono) fails++;
    }

    // ---- 4. сохранение/загрузка модели побайтно
    {
        static GestureModel m2;
        memcpy(&m2, &m, sizeof(m));
        float h[63], s[63], f[GF_N];
        MakeHand(0.9f, 0.5f, 0.f, h); ToScreen(h, s); HandFeatures(h, s, f);
        bool ok = m2.Valid() && fabsf(m2.Prob(f) - m.Prob(f)) < 1e-6f;
        printf("model copy/valid: %s (size %zu bytes)\n", ok ? "OK" : "FAIL", sizeof(GestureModel));
        if (!ok) fails++;
    }
    // ---- 6. связка «вероятность профиля → нажатие» в control.h с порогами программы (0.25/0.60/0.35)
    {
        auto run = [](const std::vector<float>& p, int& presses, int& releases, bool& stillDown) {
            Controller c;
            c.P.gOn = 0.25f; c.P.gOff = 0.60f; c.P.relRelease = 0.35f;
            c.SyncCursor(500, 500);
            presses = releases = 0;
            bool down = false;
            for (size_t i = 0; i < p.size(); i++) {
                CtlOut o = c.Step(i / 30.0, true, 320, 240, 90, 1.f - p[i]);  // рука стоит на месте
                if (o.ev == EV_PRESS) { presses++; down = true; }
                if (o.ev == EV_RELEASE) { releases++; down = false; }
            }
            stillDown = down;
        };
        std::vector<float> clean, noisy, partial;
        for (int i = 0; i < 30; i++) clean.push_back(0.02f);             // 1 с ладонь
        for (int i = 0; i < 15; i++) clean.push_back(0.97f);             // 0.5 с кулак
        for (int i = 0; i < 30; i++) clean.push_back(0.03f);             // ладонь
        for (int i = 0; i < 300; i++) noisy.push_back(0.5f + 0.2f * Gauss() * 0.5f);  // неуверенность около 0.5
        for (int i = 0; i < 30; i++) partial.push_back(0.02f);
        for (int i = 0; i < 15; i++) partial.push_back(0.98f);
        for (int i = 0; i < 30; i++) partial.push_back(0.62f);           // разжал не до конца (p не упала ниже 0.4)
        int pr, rl; bool dn;
        run(clean, pr, rl, dn);
        bool ok1 = pr == 1 && rl == 1 && !dn;
        printf("click from profile: presses %d releases %d  %s\n", pr, rl, ok1 ? "OK" : "FAIL");
        run(noisy, pr, rl, dn);
        bool ok2 = pr == 0;
        printf("noisy p~0.5 for 10 s: presses %d (expect 0)  %s\n", pr, ok2 ? "OK" : "FAIL");
        run(partial, pr, rl, dn);
        bool ok3 = pr == 1 && rl == 1 && !dn;
        printf("partial opening releases button: presses %d releases %d  %s\n", pr, rl, ok3 ? "OK" : "FAIL");
        if (!ok1) fails++;
        if (!ok2) fails++;
        if (!ok3) fails++;
    }

    // ---- 5. синтетический calib.csv в формате программы (для проверки tools/calib_eval.exe)
    {
        FILE* f = fopen("tools/synthetic_calib.csv", "w");
        if (f) {
            fprintf(f, "# label,phase, 63 world coords x1e5, 63 screen coords x100\n");
            float h[63], w[63], s[63];
            const float cr[3][4] = {{0.f, 0.25f, 0.f, 0.4f}, {1.2f, 1.55f, 0.6f, 1.2f}, {0.3f, 0.75f, 0.f, 0.8f}};
            for (int ph = 0; ph < 3; ph++)
                for (int k = 0; k < 200; k++) {
                    MakeHand(cr[ph][0] + (cr[ph][1] - cr[ph][0]) * Rnd(), cr[ph][2] + (cr[ph][3] - cr[ph][2]) * Rnd(), 0.004f, h);
                    Transform(h, w, 1.f, 0.f, 0.f, 0.5f);
                    ToScreen(h, s);
                    fprintf(f, "%d,%d", ph == 1 ? 1 : 0, ph);
                    for (int j = 0; j < 63; j++) fprintf(f, ",%d", (int)lroundf(w[j] * 100000.f));
                    for (int j = 0; j < 63; j++) fprintf(f, ",%d", (int)lroundf(s[j] * 100.f));
                    fprintf(f, "\n");
                }
            fclose(f);
            printf("wrote tools/synthetic_calib.csv\n");
        }
    }
    printf(fails ? "\n%d TEST(S) FAILED\n" : "\nALL TESTS PASSED\n", fails);
    return fails;
}
