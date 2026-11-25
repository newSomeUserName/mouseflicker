// Офлайн-проверка калибровки: читает calib.csv (пишет программа при «Обучить на моей руке»),
// заново считает признаки, учит классификатор на первых 75% каждой фазы и проверяет на последних 25%.
// Для сравнения — старый способ: порог по геометрической метрике сгиба пальцев (FistClose=115).
// Использование: calib_eval.exe calib.csv
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <algorithm>
#include "../gesture.h"

static float Pct(std::vector<float> v, float p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[(size_t)(p * (v.size() - 1))];
}

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "calib.csv";
    FILE* f = fopen(path, "r");
    if (!f) { printf("cannot open %s\n", path); return 1; }
    std::vector<float> X;
    std::vector<int> y, phase;
    std::vector<float> geo;  // старая метрика: среднее сгибов (мировые + экранные)
    static char line[8192];
    int bad = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        int v[128], n = 0;
        char* p = line;
        while (n < 128) {
            char* e;
            long x = strtol(p, &e, 10);
            if (e == p) break;
            v[n++] = (int)x;
            p = e;
            if (*p == ',') p++;
        }
        if (n != 128) { bad++; continue; }
        float world[63], scr[63], feat[GF_N];
        for (int j = 0; j < 63; j++) { world[j] = v[2 + j] / 100000.f; scr[j] = v[65 + j] / 100.f; }
        if (!HandFeatures(world, scr, feat)) { bad++; continue; }
        X.insert(X.end(), feat, feat + GF_N);
        y.push_back(v[0]); phase.push_back(v[1]);
        const float* c = feat + GF_POINTS * 2;  // 4 мировых + 4 экранных сгиба
        geo.push_back((c[0] + c[1] + c[2] + c[3] + c[4] + c[5] + c[6] + c[7]) / 8.f);
    }
    fclose(f);
    int rows = (int)y.size();
    printf("rows %d (skipped %d)\n", rows, bad);
    if (rows < 30) { printf("too few rows\n"); return 1; }

    std::vector<unsigned char> use(rows);
    TemporalSplit(phase.data(), rows, use.data());
    static GestureModel m;
    m.Fit(X.data(), y.data(), rows, use.data());
    m.Evaluate(X.data(), y.data(), rows, use.data(), 0.75f);
    printf("PROFILE (held-out last 25%% of each phase): accuracy %.1f%%, false press %.1f%%, miss %.1f%%\n",
           m.valAcc * 100, m.valFalsePress * 100, m.valMiss * 100);

    // Распределение вероятности кулака по фазам на отложенных кадрах.
    const char* names[3] = {"open palm", "fist", "working hand"};
    for (int ph = 0; ph < 3; ph++) {
        std::vector<float> ps;
        for (int i = 0; i < rows; i++) if (!use[i] && phase[i] == ph) ps.push_back(m.Prob(&X[i * GF_N]));
        if (ps.empty()) continue;
        printf("  %-13s p(fist): min %.2f p10 %.2f med %.2f p90 %.2f max %.2f\n", names[ph], Pct(ps, 0), Pct(ps, .1f),
               Pct(ps, .5f), Pct(ps, .9f), Pct(ps, 1));
    }

    // Старый способ на тех же отложенных кадрах.
    int neg = 0, fp = 0, pos = 0, miss = 0;
    for (int i = 0; i < rows; i++) {
        if (use[i]) continue;
        bool press = geo[i] < 1.15f;
        if (y[i]) { pos++; if (!press) miss++; } else { neg++; if (press) fp++; }
    }
    printf("OLD threshold (curl < 1.15): false press %.1f%%, miss %.1f%%\n", neg ? 100.f * fp / neg : 0, pos ? 100.f * miss / pos : 0);
    for (int ph = 0; ph < 3; ph++) {
        std::vector<float> g;
        for (int i = 0; i < rows; i++) if (phase[i] == ph) g.push_back(geo[i]);
        if (!g.empty()) printf("  %-13s curl metric: min %.2f med %.2f max %.2f\n", names[ph], Pct(g, 0), Pct(g, .5f), Pct(g, 1));
    }
    return 0;
}
