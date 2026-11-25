// Прогоняет записанную диагностику (diag.csv) через логику управления control.h и печатает
// статистику: дрожь курсора, когда рука стоит, и поведение кликов. Камера и нейросеть не нужны.
// Использование: replay.exe diag.csv
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>
#include <algorithm>
#include "../control.h"

struct Row { double t; bool hand; float x, y, palm, metric; };

// Пороги жеста из заголовка журнала (новый формат); для старого — пороги щипка.
static float gOn = 0.25f, gOff = 0.40f, gRel = 0.15f;

// Понимает оба формата diag.csv:
//  v2: t_ms,infer_ms,palm_ran,palm_found,conf,pinch_scr,pinch_world,pinch,pinched,x5,y5
//  v4: t_ms,infer_ms,palm_ran,palm_found,conf,pinch_scr,pinch_world,fist_scr,fist_world,metric,pressed,dragging,px*10,py*10,palm*10,cur_x,cur_y
static std::vector<Row> Load(const char* path) {
    std::vector<Row> rows;
    FILE* f = fopen(path, "r");
    if (!f) { printf("cannot open %s\n", path); exit(1); }
    char line[512];
    bool v4 = false;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') {
            int ges, cl, op;
            const char* p = strstr(line, "gesture ");
            if (p && sscanf(p, "gesture %d, close %d, open %d", &ges, &cl, &op) == 3) {
                gOn = cl / 100.f; gOff = op / 100.f; gRel = ges ? 0.15f : 0.30f;
            }
            continue;
        }
        if (line[0] == 't') { v4 = strstr(line, "fist_scr") != NULL; continue; }
        int v[17] = {};
        int n = sscanf(line, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5],
                       &v[6], &v[7], &v[8], &v[9], &v[10], &v[11], &v[12], &v[13], &v[14], &v[15], &v[16]);
        Row r;
        r.t = v[0] / 1000.0;
        if (v4) {
            if (n != 17) continue;
            r.hand = v[9] >= 0;
            r.x = v[12] / 10.f; r.y = v[13] / 10.f; r.palm = v[14] / 10.f;
            r.metric = v[9] / 100.f;
        } else {
            if (n != 11) continue;
            r.hand = v[4] >= 50;
            r.x = (float)v[9]; r.y = (float)v[10];
            r.palm = 90.f;  // в старом журнале размера ладони нет — оценка по снимкам
            r.metric = (v[5] + v[6]) / 200.f;  // как в программе: среднее двух оценок щипка
        }
        rows.push_back(r);
    }
    fclose(f);
    printf("format %s, thresholds on %.2f off %.2f\n", v4 ? "v4" : "v2", gOn, gOff);
    return rows;
}

static float Pct(std::vector<float> v, float p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[(size_t)(p * (v.size() - 1))];
}

// «Рука стоит»: средняя скорость сырой точки в окне ±5 кадров меньше 0.3 ладони/с.
static std::vector<bool> StillMask(const std::vector<Row>& r, float palm) {
    std::vector<bool> m(r.size(), false);
    for (size_t i = 5; i + 5 < r.size(); i++) {
        bool ok = true;
        for (size_t j = i - 5; j <= i + 5; j++) ok = ok && r[j].hand;
        if (!ok) continue;
        float dx = r[i + 5].x - r[i - 5].x, dy = r[i + 5].y - r[i - 5].y;
        float v = sqrtf(dx * dx + dy * dy) / r[i].palm / (float)(r[i + 5].t - r[i - 5].t);
        m[i] = v < 0.3f;
    }
    return m;
}

static void Report(const char* name, const std::vector<Row>& r, const std::vector<bool>& still,
                   const std::vector<float>& cx, const std::vector<float>& cy) {
    std::vector<float> stillStep;
    float stillPath = 0, allPath = 0;
    int stillFrames = 0, frozen = 0;
    for (size_t i = 1; i < r.size(); i++) {
        float d = hypotf(cx[i] - cx[i - 1], cy[i] - cy[i - 1]);
        allPath += d;
        if (still[i]) { stillStep.push_back(d); stillPath += d; stillFrames++; if (d < 0.5f) frozen++; }
    }
    printf("%-34s still frames %d: cursor step px med %.2f p90 %.2f max %.1f | path while still %.0f px | total path %.0f px | still&motionless %d%%\n",
           name, stillFrames, Pct(stillStep, .5f), Pct(stillStep, .9f), Pct(stillStep, 1.f), stillPath, allPath,
           stillFrames ? frozen * 100 / stillFrames : 0);
}

// Задержка курсора: сдвиг (в кадрах), при котором скорость курсора лучше всего совпадает со скоростью руки.
static int LagFrames(const std::vector<Row>& r, const std::vector<float>& cx, const std::vector<float>& cy) {
    std::vector<float> hv(r.size(), 0), cv(r.size(), 0);
    for (size_t i = 1; i < r.size(); i++) {
        if (r[i].hand && r[i - 1].hand) hv[i] = hypotf(r[i].x - r[i - 1].x, r[i].y - r[i - 1].y);
        cv[i] = hypotf(cx[i] - cx[i - 1], cy[i] - cy[i - 1]);
    }
    int best = 0; double bc = -1e30;
    for (int lag = 0; lag <= 10; lag++) {
        double s = 0;
        for (size_t i = 0; i + lag < r.size(); i++) s += hv[i] * cv[i + lag];
        if (s > bc) { bc = s; best = lag; }
    }
    return best;
}

static void Sweep(const std::vector<Row>& r, const std::vector<bool>& still, float palm) {
    printf("minCut beta  jitter | still p90 px  still path | moving path | lag frames | clicks\n");
    float mcs[] = {0.5f, 0.8f, 1.2f}, bs[] = {0.1f, 0.3f, 0.6f}, js[] = {0.06f, 0.1f, 0.15f};
    for (float mc : mcs) for (float b : bs) for (float j : js) {
        Controller c;
        c.P.mapping = 0; c.P.screenScale = 2560.f / 1920.f;
        c.P.gOn = gOn; c.P.gOff = gOff; c.P.relRelease = gRel;
        c.P.minCut = mc; c.P.beta = b; c.P.jitterSpeed = j;
        c.SyncCursor(1280, 720);
        std::vector<float> X, Y;
        int presses = 0;
        for (auto& w : r) {
            CtlOut o = c.Step(w.t, w.hand, w.x, w.y, w.palm, w.metric);
            if (o.ev == EV_PRESS) presses++;
            X.push_back(c.cx); Y.push_back(c.cy);
        }
        std::vector<float> st;
        float sp = 0, mp = 0;
        for (size_t i = 1; i < r.size(); i++) {
            float d = hypotf(X[i] - X[i - 1], Y[i] - Y[i - 1]);
            if (still[i]) { st.push_back(d); sp += d; } else mp += d;
        }
        printf("%5.1f %5.1f %6.2f | %10.2f %10.0f | %11.0f | %10d | %d\n", mc, b, j, Pct(st, .9f), sp, mp, LagFrames(r, X, Y), presses);
    }
}

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "diag.csv";
    std::vector<Row> r = Load(path);
    const float palm = 90.f;  // размер ладони в кадре 640x480 (оценка по снимкам диагностики)
    printf("rows %zu, duration %.1f s\n", r.size(), r.empty() ? 0 : r.back().t);
    std::vector<bool> still = StillMask(r, palm);
    if (argc > 2 && !strcmp(argv[2], "sweep")) { Sweep(r, still, palm); return 0; }

    // ---- старая схема (v3): абсолютно, One Euro 0.6/0.04 по точке, зона 65% на виртуальный экран 4000x1966
    {
        OneEuro ex, ey; ex.minCut = ey.minCut = 0.6f; ex.beta = ey.beta = 0.04f;
        float cx = 0, cy = 0, zone = 0.65f, z0 = 0.5f - zone / 2;
        std::vector<float> X, Y;
        double last = 0;
        for (auto& w : r) {
            float dt = (float)(w.t - last); last = w.t;
            if (dt < 0.005f) dt = 0.005f; if (dt > 0.2f) dt = 0.2f;
            if (w.hand) {
                float fx = ex.Filter(w.x, dt), fy = ey.Filter(w.y, dt);
                float nx = (fx / 640 - z0) / zone, ny = (fy / 480 - z0) / zone;
                nx = nx < 0 ? 0 : nx > 1 ? 1 : nx; ny = ny < 0 ? 0 : ny > 1 ? 1 : ny;
                float tx = -1440 + nx * 3999, ty = -526 + ny * 1965;
                float ddx = tx - cx, ddy = ty - cy, dd = hypotf(ddx, ddy);
                if (dd > 3) { float k = (dd - 3) / dd; cx += ddx * k; cy += ddy * k; }
            }
            X.push_back(cx); Y.push_back(cy);
        }
        Report("OLD absolute (virtual 4000px)", r, still, X, Y);
    }

    // ---- новая логика, оба режима
    for (int mapping = 0; mapping <= 1; mapping++) {
        Controller c;
        c.P.mapping = mapping;
        c.P.screenScale = 2560.f / 1920.f;
        c.P.monX = 0; c.P.monY = 0; c.P.monW = 2560; c.P.monH = 1440;
        c.P.gOn = gOn; c.P.gOff = gOff; c.P.relRelease = gRel;
        c.SyncCursor(1280, 720);
        std::vector<float> X, Y;
        int presses = 0, releases = 0, drags = 0;
        std::vector<float> lockShift, heldMove;
        float pressX = 0, pressY = 0, maxHeld = 0;
        bool wasDrag = false;
        for (size_t i = 0; i < r.size(); i++) {
            float beforeX = c.cx, beforeY = c.cy;
            CtlOut o = c.Step(r[i].t, r[i].hand, r[i].x, r[i].y, r[i].palm, r[i].metric);
            if (o.ev == EV_PRESS) {
                presses++;
                lockShift.push_back(hypotf(o.x - beforeX, o.y - beforeY));  // на сколько защёлка вернула курсор
                pressX = o.x; pressY = o.y; maxHeld = 0; wasDrag = false;
            }
            if (o.pressed) {
                maxHeld = fmaxf(maxHeld, hypotf(c.cx - pressX, c.cy - pressY));
                wasDrag = wasDrag || o.dragging;
            }
            if (o.ev == EV_RELEASE) {
                releases++;
                if (wasDrag) drags++; else heldMove.push_back(maxHeld);
            }
            X.push_back(c.cx); Y.push_back(c.cy);
        }
        Report(mapping ? "NEW absolute (primary 2560px)" : "NEW air-trackpad (relative)", r, still, X, Y);
        printf("    presses %d, releases %d, became drags %d | latch pulled cursor back px: med %.1f max %.1f | cursor move during click (non-drag): max %.1f px\n",
               presses, releases, drags, Pct(lockShift, .5f), Pct(lockShift, 1.f), Pct(heldMove, 1.f));
    }
    return 0;
}
