// control.h — логика управления курсором и жестом клика.
// Без зависимостей от Windows, камеры и нейросети: на вход — центр ладони, размер ладони и
// метрика жеста за кадр, на выход — куда поставить курсор и когда нажать/отпустить кнопку.
// Поэтому её можно проверять на записанных данных (tools/replay.cpp).
#pragma once
#include <math.h>

// Фильтр One Euro: сильно сглаживает в покое, почти без задержки при быстром движении.
struct OneEuro {
    float minCut = 1.0f, beta = 0.05f, dCut = 1.0f;
    float x = 0, dx = 0;
    bool init = false;
    static float Alpha(float cut, float dt) { float tau = 1.f / (6.2831853f * cut); return 1.f / (1.f + tau / dt); }
    float Filter(float v, float dt) {
        if (!init) { init = true; x = v; dx = 0; return v; }
        float d = (v - x) / dt;
        dx += Alpha(dCut, dt) * (d - dx);
        float cut = minCut + beta * fabsf(dx);
        x += Alpha(cut, dt) * (v - x);
        return x;
    }
};

enum CtlEvent { EV_NONE, EV_PRESS, EV_RELEASE };

struct CtlParams {
    int mapping = 0;           // 0 — тачпад в воздухе (относительно), 1 — абсолютно (зона кадра = монитор)
    float sens = 1.f;          // множитель чувствительности
    float screenScale = 1.f;   // ширина основного монитора / 1920
    float deadZone = 3.f;      // абсолютный режим: мёртвая зона, px экрана
    float gOn = 1.05f;         // метрика жеста ниже — «сжато»
    float gOff = 1.35f;        // выше — «раскрыто»
    float relRelease = 0.30f;  // отпускание и при раскрытии на столько от самого сжатого положения
    float frameW = 640, frameH = 480;
    float monX = 0, monY = 0, monW = 1920, monH = 1080;  // монитор для абсолютного режима
    float zone = 0.65f;        // доля кадра, растягиваемая на монитор (абсолютный режим)
    float minCut = 0.8f;       // One Euro: срез в покое, Гц (меньше — спокойнее, но больше задержка)
    // Подобрано перебором на записанной руке (tools/replay.exe diag.csv sweep): втрое меньше
    // блуждания курсора в покое при той же задержке, что и у более «резких» вариантов.
    float beta = 0.1f;         // One Euro: насколько быстро срез растёт со скоростью (px кадра/с)
    float jitterSpeed = 0.15f; // тачпад: скорости ниже (ладоней/с) считаются дрожью
    // Кривая ускорения тачпада: px экрана на одну ладонь движения (до умножения на sens и screenScale).
    // Живой тест показал, что обычные движения — 2–10 ладоней/с, а при максимуме 1400 сдвиг руки
    // на 2 ладони уводил курсор на 4500 px. Теперь проход через весь кадр (~8 ладоней) ≈ 2 экрана.
    float gainMin = 150.f, gainMax = 600.f;
    float vLow = 0.3f, vHigh = 4.0f;  // ладоней/с: ниже vLow — gainMin, выше vHigh — gainMax
    int confirmFrames = 3;     // сколько кадров подряд жест должен держаться, чтобы нажать
    float calmSpeed = 1e9f;    // не нажимать при скорости выше (ладоней/с). Выключено: тест показал,
                               // что смаз не путает классификатор, а запрет только задерживал нажатие
};

struct CtlOut {
    bool setCursor = false;
    float x = 0, y = 0;
    CtlEvent ev = EV_NONE;
    float progress = 0;        // 0..1 — насколько рука близка к «нажатию» (для индикатора)
    bool pressed = false, dragging = false, hand = false;
    float dyPalm = 0;          // вертикальное смещение ладони за кадр (в ладонях) — для прокрутки
};

static inline float SmoothStep(float a, float b, float x) {
    float t = (x - a) / (b - a);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return t * t * (3 - 2 * t);
}

class Controller {
public:
    CtlParams P;
    float cx = 0, cy = 0;  // где сейчас курсор (с дробной частью)


    void SyncCursor(float x, float y) { cx = x; cy = y; hn = 0; }

    // Один кадр. t — секунды; hand — найдена ли рука; px, py — центр ладони (px кадра);
    // palm — размер ладони (px кадра); metric — метрика жеста (меньше = сильнее сжато).
    CtlOut Step(double t, bool hand, float px, float py, float palm, float metric) {
        CtlOut o;
        float dt = haveT ? (float)(t - lastT) : 1.f / 30;
        if (dt < 0.005f) dt = 0.005f;
        if (dt > 0.2f) dt = 0.2f;
        lastT = t; haveT = true;

        if (!hand) {
            // Рука пропала на 3+ кадра: отпускаем кнопку и всё сбрасываем. Курсор остаётся на месте.
            if (++missFrames >= 3) {
                if (pressed) { o.ev = EV_RELEASE; pressed = false; }
                haveP = false; fx.init = fy.init = false;
                g = 9.f; closeFrames = openFrames = 0; approachT = -1;
                locked = dragging = false; spd = 0; offX = offY = 0;
            }
            o.pressed = pressed;
            return o;
        }
        if (missFrames >= 3 || !haveP) handSince = t;
        missFrames = 0;
        o.hand = true;
        if (palm < 1) palm = 1;

        g = g > 5.f ? metric : g + 0.5f * (metric - g);  // лёгкое сглаживание метрики
        fx.minCut = fy.minCut = P.minCut;
        fx.beta = fy.beta = P.beta;
        float ffx = fx.Filter(px, dt), ffy = fy.Filter(py, dt);
        float dxp = 0, dyp = 0, v = 0;
        if (haveP) {
            dxp = (ffx - pfx) / palm; dyp = (ffy - pfy) / palm;
            v = sqrtf(dxp * dxp + dyp * dyp) / dt;  // скорость в ладонях в секунду
            spd += 0.4f * (v - spd);
        }
        o.dyPalm = dyp;

        // ---- жест: подтверждение 3 кадра; не в первые 0.3 с и не при быстром движении (смаз)
        bool calm = (t - handSince) > 0.3 && spd < P.calmSpeed;
        if (!pressed) {
            if (g < P.gOff) { if (approachT < 0) approachT = t; }
            else approachT = -1;
            closeFrames = (g < P.gOn && calm) ? closeFrames + 1 : 0;
            if (closeFrames >= P.confirmFrames) {
                pressed = true; closeFrames = openFrames = 0; pressMin = g;
                // Защёлка: кликаем туда, где курсор был ДО начала сжатия (сжатие сдвигает ладонь).
                double back = approachT - 0.08;
                if (back < t - 0.5) back = t - 0.5;
                LookupHist(back, lockX, lockY);
                // Защёлка отменяет только небольшой сдвиг от самого жеста. Если курсор за это
                // время ушёл далеко, значит рука целенаправленно двигалась — кликаем где сейчас.
                float lim = 50.f * P.screenScale;
                if ((lockX - cx) * (lockX - cx) + (lockY - cy) * (lockY - cy) > lim * lim) { lockX = cx; lockY = cy; }
                cx = lockX; cy = lockY;
                locked = true; dragging = false;
                pressPx = ffx; pressPy = ffy;
                o.setCursor = true;
                o.ev = EV_PRESS;  // приложение сначала ставит курсор, потом нажимает
            }
        } else {
            if (g < pressMin) pressMin = g;
            float thr = pressMin + P.relRelease;
            if (thr > P.gOff) thr = P.gOff;
            openFrames = g > thr ? openFrames + 1 : 0;
            if (openFrames >= 2) {
                pressed = false; openFrames = 0; approachT = -1;
                o.ev = EV_RELEASE;
                if (locked) holdUntil = t + 0.15;  // раскрытие ладони тоже сдвигает её — держим курсор
            }
        }
        o.progress = pressed ? 1.f : SmoothStep(0, 1, (P.gOff - g) / (P.gOff - P.gOn));

        // Перетаскивание начинается, только если сжатую руку сдвинули на треть ладони.
        if (pressed && locked && !dragging) {
            float d = sqrtf((ffx - pressPx) * (ffx - pressPx) + (ffy - pressPy) * (ffy - pressPy)) / palm;
            if (d > 0.33f) { dragging = true; locked = false; StartAbsOffset(ffx, ffy); }
        }
        if (!pressed && locked && t >= holdUntil) { locked = false; StartAbsOffset(ffx, ffy); }
        if (!pressed) dragging = false;

        // ---- движение курсора
        if (haveP && !locked) {
            // Рука начинает сжиматься — курсор притормаживает, чтобы клик был точным.
            float damp = pressed ? 1.f : 1.f - 0.7f * o.progress;
            if (P.mapping == 0) {
                // Тачпад в воздухе: усиление растёт со скоростью (как баллистика мыши в Windows).
                // Медленно — ~180 px экрана на ладонь (точность), быстро — ~1400 (через весь экран).
                float vEff = v - P.jitterSpeed;  // медленнее — дрожь модели, а не движение руки
                float scale = (v > 1e-6f && vEff > 0) ? vEff / v : 0.f;
                float gain = (P.gainMin + (P.gainMax - P.gainMin) * SmoothStep(P.vLow, P.vHigh, v)) * P.sens * P.screenScale;
                cx += dxp * scale * gain * damp;
                cy += dyp * scale * gain * damp;
            } else {
                float z0x = 0.5f - P.zone * 0.5f, z0y = 0.5f - P.zone * 0.5f;
                float nx = (ffx / P.frameW - z0x) / P.zone, ny = (ffy / P.frameH - z0y) / P.zone;
                nx = nx < 0 ? 0 : nx > 1 ? 1 : nx;
                ny = ny < 0 ? 0 : ny > 1 ? 1 : ny;
                float tx = P.monX + nx * (P.monW - 1) + offX, ty = P.monY + ny * (P.monH - 1) + offY;
                float decay = expf(-dt / 0.3f);  // сдвиг после защёлки плавно уходит за ~0.3 с
                offX *= decay; offY *= decay;
                float ddx = tx - cx, ddy = ty - cy, dd = sqrtf(ddx * ddx + ddy * ddy);
                if (dd > P.deadZone) {
                    float k = (dd - P.deadZone) / dd * damp;
                    cx += ddx * k; cy += ddy * k;
                }
            }
            o.setCursor = true;
        }
        pfx = ffx; pfy = ffy; haveP = true;
        PushHist(t, cx, cy);
        o.x = cx; o.y = cy;
        o.pressed = pressed; o.dragging = dragging;
        return o;
    }

private:
    OneEuro fx, fy;
    bool haveP = false, haveT = false;
    float pfx = 0, pfy = 0;
    double lastT = 0, handSince = 0, approachT = -1, holdUntil = 0;
    int missFrames = 0, closeFrames = 0, openFrames = 0;
    float g = 9.f, spd = 0, pressMin = 9.f;
    bool pressed = false, locked = false, dragging = false;
    float lockX = 0, lockY = 0, pressPx = 0, pressPy = 0, offX = 0, offY = 0;

    struct Hist { double t; float x, y; };
    enum { HN = 128 };
    Hist hist[HN];
    int hn = 0, hpos = 0;

    void PushHist(double t, float x, float y) {
        hist[hpos] = {t, x, y};
        hpos = (hpos + 1) % HN;
        if (hn < HN) hn++;
    }
    // Позиция курсора на момент t (последняя запись не позже t); если такой нет — самая старая.
    void LookupHist(double t, float& x, float& y) {
        x = cx; y = cy;
        for (int i = 1; i <= hn; i++) {
            const Hist& h = hist[(hpos - i + HN) % HN];
            x = h.x; y = h.y;
            if (h.t <= t) return;
        }
    }
    // В абсолютном режиме после защёлки курсор не прыгает к руке, а плавно догоняет её.
    void StartAbsOffset(float ffx, float ffy) {
        if (P.mapping != 1) return;
        float z0 = 0.5f - P.zone * 0.5f;
        float nx = (ffx / P.frameW - z0) / P.zone, ny = (ffy / P.frameH - z0) / P.zone;
        nx = nx < 0 ? 0 : nx > 1 ? 1 : nx;
        ny = ny < 0 ? 0 : ny > 1 ? 1 : ny;
        offX = cx - (P.monX + nx * (P.monW - 1));
        offY = cy - (P.monY + ny * (P.monH - 1));
    }
};
