// Разбор живого теста (test.csv, пишет mousefinger.exe --track --test) и подбор порогов по записи.
// Фазы: 1 — открытая ладонь в движении (кликов быть не должно), 2 — клики по сигналу (6 сигналов),
// 3 — «рука как при работе» (кликов быть не должно), 4 — рука неподвижна (дрожь курсора),
// 5 — два перетаскивания.
// Использование: test_eval.exe test.csv
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>
#include <algorithm>
#include "../control.h"

struct Row {
    double t; int phase, cue, hand, conf; float pfist, metric, curl; int pressed, dragging, ev;
    float px, py, palm, cx, cy; int infer;
};

static const double kCue[6] = {17, 19, 21, 23, 25, 27};

static float Pct(std::vector<float> v, float p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[(size_t)(p * (v.size() - 1))];
}

struct Score {
    int fp1 = 0, fp3 = 0, fp4 = 0, hits = 0, extra2 = 0, drags = 0, presses5 = 0; float lat = 0; std::vector<double> fpTimes;
    std::vector<float> pressDelay;  // от первого кадра с уверенным кулаком (p > 0.75) до нажатия, мс
    float cursorPath = 0, handPath = 0;  // путь курсора (px) и руки (ладоней), пока рука видна
};

// Параметры логики: «old» — как было до живого теста, иначе текущие умолчания control.h.
static bool gOld = false;
static int gConfirm = 3;

// Прогон записи через Controller с заданными порогами; события считаем по фазам.
static Score Simulate(const std::vector<Row>& r, float gOn, float gOff, float rel, bool verbose) {
    Controller c;
    c.P.gOn = gOn; c.P.gOff = gOff; c.P.relRelease = rel;
    c.P.confirmFrames = gConfirm;
    if (gOld) { c.P.gainMin = 180; c.P.gainMax = 1400; c.P.vLow = 0.3f; c.P.vHigh = 2.5f; c.P.calmSpeed = 2.5f; c.P.confirmFrames = 3; }
    c.P.screenScale = 2560.f / 1920.f;
    c.SyncCursor(1280, 720);
    Score s;
    std::vector<double> presses2;
    bool dragSeen = false;
    double sureSince = -1;
    float lx = 0, ly = 0, hx = 0, hy = 0;
    bool havePrev = false;
    for (auto& w : r) {
        float before = c.cx, beforeY = c.cy;
        CtlOut o = c.Step(w.t, w.hand != 0, w.px, w.py, w.palm, w.metric);
        if (w.hand && w.metric < 0.25f) { if (sureSince < 0) sureSince = w.t; }
        else if (!o.pressed) sureSince = -1;
        if (o.ev == EV_PRESS && sureSince >= 0) s.pressDelay.push_back((float)((w.t - sureSince) * 1000));
        if (w.hand && havePrev) {
            s.cursorPath += hypotf(c.cx - before, c.cy - beforeY);
            s.handPath += hypotf(w.px - hx, w.py - hy) / w.palm;
        }
        havePrev = w.hand != 0; hx = w.px; hy = w.py;
        (void)lx; (void)ly;
        if (o.ev == EV_PRESS) {
            if (w.phase == 1) { s.fp1++; s.fpTimes.push_back(w.t); }
            else if (w.phase == 3) { s.fp3++; s.fpTimes.push_back(w.t); }
            else if (w.phase == 4) { s.fp4++; s.fpTimes.push_back(w.t); }
            else if (w.phase == 2) presses2.push_back(w.t);
            else if (w.phase == 5) { s.presses5++; dragSeen = false; }
        }
        if (w.phase == 5 && o.dragging && !dragSeen) { dragSeen = true; s.drags++; }
    }
    // Сопоставляем нажатия с сигналами: нажатие в окне [сигнал, сигнал + 1.8 с].
    std::vector<bool> used(presses2.size(), false);
    float latSum = 0;
    for (double cue : kCue) {
        for (size_t i = 0; i < presses2.size(); i++) {
            if (!used[i] && presses2[i] >= cue && presses2[i] <= cue + 1.8) {
                used[i] = true; s.hits++; latSum += (float)(presses2[i] - cue);
                if (verbose) printf("    cue %.0fs -> press after %.0f ms\n", cue, (presses2[i] - cue) * 1000);
                break;
            }
        }
    }
    for (bool u : used) if (!u) s.extra2++;
    s.lat = s.hits ? latSum / s.hits : 0;
    return s;
}

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "test.csv";
    FILE* f = fopen(path, "r");
    if (!f) { printf("cannot open %s\n", path); return 1; }
    std::vector<Row> r;
    char line[1024];
    float hdrOn = 0.25f, hdrOff = 0.6f, hdrRel = 0.35f;
    int model = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') {
            printf("%s", line);
            const char* p = strstr(line, "model ");
            int a, b, c, d;
            if (p && sscanf(p, "model %d, gOn %d, gOff %d, rel %d", &a, &b, &c, &d) == 4) {
                model = a; hdrOn = b / 100.f; hdrOff = c / 100.f; hdrRel = d / 100.f;
            }
            continue;
        }
        if (line[0] == 't') continue;
        int v[17];
        if (sscanf(line, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6],
                   &v[7], &v[8], &v[9], &v[10], &v[11], &v[12], &v[13], &v[14], &v[15], &v[16]) != 17) continue;
        Row w;
        w.t = v[0] / 1000.0; w.phase = v[1]; w.cue = v[2]; w.hand = v[3]; w.conf = v[4];
        w.pfist = v[5] / 1000.f; w.metric = v[6] / 1000.f; w.curl = v[7] / 100.f;
        w.pressed = v[8]; w.dragging = v[9]; w.ev = v[10];
        w.px = v[11] / 10.f; w.py = v[12] / 10.f; w.palm = v[13] / 10.f; w.cx = (float)v[14]; w.cy = (float)v[15]; w.infer = v[16];
        r.push_back(w);
    }
    fclose(f);
    if (r.size() < 100) { printf("too few rows (%zu)\n", r.size()); return 1; }
    printf("rows %zu, %.1f s, %.1f fps, model %d\n\n", r.size(), r.back().t, r.size() / r.back().t, model);

    // ---- по фазам: присутствие руки, вероятность кулака, задержка нейросети
    const char* names[6] = {"prepare", "1 open palm moving", "2 clicks on cue", "3 working hand", "4 hold still", "5 drag x2"};
    for (int ph = 0; ph <= 5; ph++) {
        int n = 0, h = 0;
        std::vector<float> p, curl, inf;
        for (auto& w : r) if (w.phase == ph) {
            n++; inf.push_back((float)w.infer);
            if (w.hand) { h++; p.push_back(model ? w.pfist : w.metric); curl.push_back(w.curl); }
        }
        if (!n) continue;
        printf("%-20s frames %4d  hand %3d%%  %s p10 %.2f med %.2f p90 %.2f max %.2f | curl med %.2f min %.2f | infer med %.0f ms\n",
               names[ph], n, h * 100 / n, model ? "p(fist)" : "metric ", Pct(p, .1f), Pct(p, .5f), Pct(p, .9f), Pct(p, 1),
               Pct(curl, .5f), Pct(curl, 0), Pct(inf, .5f));
    }

    // ---- старая и новая логика на одной и той же записи
    if (model) gConfirm = 2;
    for (int old = 1; old >= 0; old--) {
        gOld = old != 0;
        Score q = Simulate(r, hdrOn, hdrOff, hdrRel, false);
        printf("\n%s: press delay after fist is visible: med %.0f ms max %.0f ms | cursor %.0f px per palm of hand travel | presses: cues %d/6, drag phase %d, false %d/%d/%d\n",
               old ? "OLD LOGIC" : "NEW LOGIC", Pct(q.pressDelay, .5f), Pct(q.pressDelay, 1.f),
               q.handPath > 0 ? q.cursorPath / q.handPath : 0, q.hits, q.presses5, q.fp1, q.fp3, q.fp4);
    }
    gOld = false;

    // ---- что было на самом деле (события программы во время теста)
    printf("\nAS RECORDED (thresholds gOn %.2f gOff %.2f rel %.2f):\n", hdrOn, hdrOff, hdrRel);
    Score s = Simulate(r, hdrOn, hdrOff, hdrRel, true);
    printf("  false presses: open palm %d, working hand %d, still %d | cue hits %d/6 (avg latency %.0f ms), extra presses %d | drags %d/2 (presses %d)\n",
           s.fp1, s.fp3, s.fp4, s.hits, s.lat * 1000, s.extra2, s.drags, s.presses5);
    for (double t : s.fpTimes) printf("    false press at %.2f s\n", t);

    // ---- дрожь курсора в фазе 4
    {
        std::vector<float> step;
        float path = 0;
        for (size_t i = 1; i < r.size(); i++)
            if (r[i].phase == 4 && r[i - 1].phase == 4 && r[i].hand && r[i - 1].hand) {
                float d = hypotf(r[i].cx - r[i - 1].cx, r[i].cy - r[i - 1].cy);
                step.push_back(d); path += d;
            }
        printf("  hold still: cursor step med %.2f p90 %.2f max %.1f px, path %.0f px over %zu frames\n", Pct(step, .5f),
               Pct(step, .9f), Pct(step, 1), path, step.size());
    }

    // ---- подбор порогов по записи: ошибки = ложные нажатия + пропущенные сигналы + лишние + несостоявшиеся перетаскивания
    printf("\nTHRESHOLD SEARCH (errors = false + missed cues + extra + missed drags):\n");
    float bestOn = hdrOn, bestOff = hdrOff, bestRel = hdrRel;
    int bestErr = 1 << 30;
    float bestLat = 9;
    const float ons[] = {0.10f, 0.15f, 0.20f, 0.25f, 0.30f, 0.35f, 0.40f, 0.50f};
    const float offs[] = {0.50f, 0.60f, 0.70f, 0.80f};
    const float rels[] = {0.25f, 0.35f, 0.45f};
    for (float on : ons) for (float off : offs) for (float rel : rels) {
        if (off <= on + 0.1f) continue;
        Score q = Simulate(r, on, off, rel, false);
        int err = q.fp1 + q.fp3 + q.fp4 + (6 - q.hits) + q.extra2 + (2 - (q.drags < 2 ? q.drags : 2));
        if (err < bestErr || (err == bestErr && q.lat < bestLat)) { bestErr = err; bestOn = on; bestOff = off; bestRel = rel; bestLat = q.lat; }
    }
    Score b = Simulate(r, bestOn, bestOff, bestRel, false);
    printf("  best: gOn %.2f gOff %.2f rel %.2f -> errors %d | false %d/%d/%d, cues %d/6 (%.0f ms), extra %d, drags %d/2\n", bestOn,
           bestOff, bestRel, bestErr, b.fp1, b.fp3, b.fp4, b.hits, b.lat * 1000, b.extra2, b.drags);
    return 0;
}
