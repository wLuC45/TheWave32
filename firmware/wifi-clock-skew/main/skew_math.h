/*
 * TheWave32 / wifi-clock-skew - pure regression + clone-decision math.
 *
 * Dependency-free on purpose: no ESP-IDF, no FreeRTOS, only <math.h>.
 * That lets the host build a unit test against this header (see
 * tests/host/test_skew_math.c) and exercise the skew/clone logic
 * deterministically, without RF hardware.
 *
 * Streaming linear regression via the weighted Welford / West update.
 * State is O(1) and the sample series is never stored. The previous
 * implementation used raw power sums (Sxx, Sxy, ...) and computed the
 * fit as a difference of large numbers, which loses precision through
 * catastrophic cancellation on long captures. The centred form here is
 * numerically stable: den > 0 and RSS >= 0 hold by construction, so the
 * defensive guards the old code needed disappear.
 *
 * An optional exponential forgetting factor lambda in (0, 1] lets the
 * estimate track a crystal whose skew drifts slowly with temperature.
 * lambda == 1.0 is exact ordinary least squares.
 */
#ifndef TW32_SKEW_MATH_H
#define TW32_SKEW_MATH_H

#include <math.h>
#include <stdbool.h>

/* Running regression state. x is local elapsed time (s), y is the
 * accumulated AP-clock drift (us); slope is therefore us/s == ppm. */
typedef struct {
    double w;              /* effective weight (== sample count when lambda==1) */
    double mx, my;         /* running weighted means of x and y */
    double cxx, cxy, cyy;  /* weighted central (co)moments */
} skew_reg_t;

static inline void skew_reg_reset(skew_reg_t *r)
{
    r->w = r->mx = r->my = r->cxx = r->cxy = r->cyy = 0.0;
}

static inline double skew_reg_weight(const skew_reg_t *r) { return r->w; }

/*
 * Fold one (x, y) sample in. lambda in (0,1]; pass 1.0 to disable
 * forgetting. Derivation: decay the prior accumulators by lambda, then
 * apply the standard West weighted-comoment increment with the new
 * sample's weight 1:
 *   C_new = lambda*C + (lambda*W / (lambda*W + 1)) * dx * dy
 * which, at lambda == 1, is the plain Welford update.
 */
static inline void skew_reg_add(skew_reg_t *r, double x, double y, double lambda)
{
    double w_old = r->w * lambda;
    double w_new = w_old + 1.0;
    double f     = w_old / w_new;     /* in [0,1); 0 on the very first sample */
    double dx    = x - r->mx;
    double dy    = y - r->my;

    r->cxx = r->cxx * lambda + f * dx * dx;
    r->cxy = r->cxy * lambda + f * dx * dy;
    r->cyy = r->cyy * lambda + f * dy * dy;
    r->mx  = r->mx + dx / w_new;
    r->my  = r->my + dy / w_new;
    r->w   = w_new;
}

typedef struct {
    double slope;      /* ppm */
    double intercept;  /* us */
    double resid;      /* residual standard error, us */
    bool   valid;
} skew_fit_t;

static inline skew_fit_t skew_reg_fit(const skew_reg_t *r)
{
    skew_fit_t f = { 0.0, 0.0, 0.0, false };
    if (r->w < 3.0 || r->cxx <= 0.0) {
        return f;                       /* need > 2 effective points */
    }
    f.slope     = r->cxy / r->cxx;
    f.intercept = r->my - f.slope * r->mx;
    double rss  = r->cyy - f.slope * r->cxy;   /* == Cyy - Cxy^2/Cxx >= 0 */
    if (rss < 0.0) rss = 0.0;                  /* clamp fp round-off only */
    f.resid     = sqrt(rss / (r->w - 2.0));
    f.valid     = true;
    return f;
}

/*
 * Clone decision with hysteresis. The old code latched `clone_flagged`
 * the first time the residual crossed the threshold and never cleared
 * it, so a single delayed beacon (whose squared residual inflates the
 * fit) permanently mislabelled an honest AP. Here a strike counter
 * requires the residual to stay high for `strikes_set` emits before the
 * flag rises, and decays it back down so the flag clears when the AP
 * settles. Persistence, not a single sample, makes the call - which is
 * exactly what distinguishes one transmitter (a tight line, transient
 * spikes) from two (a persistently large residual).
 */
typedef struct {
    int  strikes;
    bool flagged;
} clone_det_t;

static inline void clone_det_reset(clone_det_t *c) { c->strikes = 0; c->flagged = false; }

/* Returns +1 on a rising edge (just flagged), -1 on a falling edge
 * (just cleared), 0 otherwise. */
static inline int clone_det_update(clone_det_t *c, double resid, double abs_us,
                                   int strikes_set, int strikes_clear)
{
    if (resid > abs_us) {
        if (c->strikes < strikes_set) c->strikes++;
    } else {
        if (c->strikes > 0) c->strikes--;
    }
    if (!c->flagged && c->strikes >= strikes_set) { c->flagged = true;  return  1; }
    if ( c->flagged && c->strikes <= strikes_clear) { c->flagged = false; return -1; }
    return 0;
}

#endif /* TW32_SKEW_MATH_H */
