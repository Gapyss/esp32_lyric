#include "radar_screen.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pngle.h"

static const char *TAG = "radar_screen";

// ---------------------------------------------------------------- endpoints

// Keyless. The frame `path` is a hash, not a function of the timestamp, so the
// index has to be re-fetched before every tile -- there is no way to construct
// a tile URL from a clock. A full refresh is 3 requests: index, tile, forecast.
static const char *RAINVIEWER_INDEX_URL = "https://api.rainviewer.com/public/weather-maps.json";

// forecast_hours=4 anchors the array at the current hour, so [3] is literally
// +3h with no date parsing on the board. It was 8 while the panel carried a
// +7H slot; the motion ETA has that space now. `current` is what fills the
// centre marker; see the home-pixel note further down.
static const char *OPEN_METEO_URL =
    "https://api.open-meteo.com/v1/forecast"
    "?latitude=15.3919&longitude=99.8456"
    "&hourly=precipitation_probability"
    "&current=precipitation"
    "&forecast_hours=4"
    "&timezone=Asia%2FBangkok";

static const int HTTP_TIMEOUT_MS = 15000;
static const size_t HTTP_BUFFER_BYTES = 4096;
// The index measures 818 bytes and the forecast 629; 4 KB is ample headroom
// for either without giving a runaway response anywhere to go.
static const size_t MAX_API_BYTES = 4096;
static const size_t MAX_TILE_BYTES = 512U * 1024U;

// ---------------------------------------------------------------- geometry

// Home. Fixed, so every tile index below falls out of it at startup rather
// than being carried as a magic number.
static const double RADAR_HOME_LAT = 15.3919001;
static const double RADAR_HOME_LON = 99.8456348;
static const int RADAR_ZOOM = 7;
// One 512 px tile covers the whole scope with ~86 px to spare, so this is one
// fetch, one PNG, one decode. 256 px tiles would need nine of them.
static const int RADAR_TILE_PX = 512;

static const int SCOPE = 300;
static const int SCOPE_CENTER = 150;
static const int SCOPE_RADIUS = 148;  // 148 px x 0.59 km/px = ~88 km
static const int SCOPE_ROW_BYTES = (SCOPE + 7) / 8;
static const size_t SCOPE_BYTES = (size_t)SCOPE_ROW_BYTES * SCOPE;
// Ground scale is DERIVED from RADAR_ZOOM at startup, not pasted in. Every
// number the screen reports as a fact -- the range footer, and the storm speed
// the ETA is built on -- comes from these, so changing RADAR_ZOOM changes them
// together instead of leaving the labels quietly lying about the imagery.
//
// Still pixel-defined, and therefore changing MEANING with zoom rather than
// value: RADAR_MAX_SHIFT_PX (the searchable speed range), RADAR_CORRIDOR_HALF_PX
// (the corridor's width on the ground), and the blip size limits. The range
// rings and RADAR_BLIP_NEAR_PX are geometry -- quarters of the radius -- so
// they stay correct at any zoom; only what they measure changes.
//
// Landmarks do NOT follow: RADAR_LANDMARKS is a baked pixel table. Re-run
// tools/radar_landmarks.py with a matching ZOOM after changing this.
static float g_km_per_px;
static int g_range_km;

static const int PANEL_X = 300;
static const int PANEL_W = 100;

// ---------------------------------------------------------------- rendering

// RainViewer's palette is stable -- schemes 0/2/4/8 return byte-identical
// bytes -- so these thresholds can be baked. Within alpha 255 the intensity
// lives in the COLOUR, not the alpha.
static const uint8_t RADAR_BAYER[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};
// Tier 0 is the lightest-cyan band -- ~38% of all rain pixels on its own --
// so it is drawn at 2/16 rather than dropped. That reads as a sparse stipple
// instead of the grey wash a denser fill produced: on the 2026-08-23 tile it
// covers 15.9% of the scope but adds only 2.0% ink, and tier 1 another 0.9%.
// The tiers stay separable because each step roughly doubles the density.
static const uint8_t RADAR_TIER_DENSITY[4] = {2, 4, 8, 16};
// LIGHT+. Every return above the clutter band is drawn, so this guard never
// fires as it stands; it is kept as the one-line knob for putting the floor
// back at 2 (moderate and above) if the light band proves too busy on a
// heavy day, which the 2026-08-23 tile -- 4 heavy pixels -- could not test.
static const int RADAR_TIER_FLOOR = 0;

// ------------------------------------------------------------------ motion
//
// Every threshold in this block is a PLACEHOLDER. None of them has been
// measured against real weather over this tile; see radar-screen-design.md,
// "Unverified". They are named rather than inlined so they can be tuned from
// one place once the board has seen a wet day.

// Two floors, deliberately independent, for jobs that want opposite things.
// Correlation wants a well-populated, stable feature map, so it takes
// everything the scope draws. The arrival test wants a high bar, because tier
// 0 alone is 38% of all rain pixels -- an ETA keyed to it would sit pinned
// near zero for the whole monsoon and mean nothing.
static const int RADAR_CORR_TIER_FLOOR = RADAR_TIER_FLOOR;
static const int RADAR_ETA_TIER_FLOOR = 2;

// At 40 km/h over a 10-minute gap rain moves 6.7 km, which is ~11 px at
// 0.59 km/px. +/-20 px therefore covers up to ~72 km/h.
static const int RADAR_MAX_SHIFT_PX = 20;
// Ground scale is g_km_per_px, derived at init -- see the note by g_range_km.

// The mask is a DISC, not a rectangle: radar_png_draw rejects everything
// outside SCOPE_RADIUS. Shift a disc against a disc and the overlap area
// shrinks as |offset| grows, so a raw match count is maximised at dx=dy=0 by
// geometry alone -- it would report "stationary" on days when rain is racing
// across the scope. Correlating over an inner disc small enough that every
// candidate offset still lands inside the full mask removes the bias
// entirely, at the cost of 25% of the area, which is the least reliable 25%.
static const int RADAR_CORR_RADIUS = SCOPE_RADIUS - RADAR_MAX_SHIFT_PX;
// Sample every other pixel on both axes. Quarters the work for a picture the
// display cannot resolve the difference in at 0.59 km/px.
static const int RADAR_CORR_STEP = 2;
// Coarse-to-fine: 11x11 offsets at 4 px, then 7x7 at 1 px around the winner.
// 170 candidates instead of 1,681.
static const int RADAR_COARSE_STEP = 4;
static const int RADAR_COARSE_GRID = 2 * RADAR_MAX_SHIFT_PX / RADAR_COARSE_STEP + 1;

// Three gates, all of which must pass or nothing is drawn. Gate 3 is the one
// that matters: widespread stratiform rain passes the first two with room to
// spare and still has no recoverable displacement, because every offset
// matches about equally well. Only a distinct winner separates real motion
// from a coin flip.
static const int RADAR_GATE_MIN_CELLS = 200;      // sampled cells in the older mask
static const float RADAR_GATE_MATCH_FLOOR = 0.35f;
static const float RADAR_GATE_PEAK_MARGIN = 0.08f;
// A stalled cell has no arrival time, so below this the vector is discarded
// rather than drawn with an absurd ETA.
static const float RADAR_STALLED_KMH = 10.0f;
// Two frames further apart than this are not a motion pair. The screen only
// fetches while displayed, so returning to it after hours would otherwise
// correlate across a gap in which the weather was replaced, not moved.
static const int64_t RADAR_MAX_PAIR_GAP_SEC = 20 * 60;

// Past this the scope is timing an arrival it has no business timing. The
// radius is 87.3 km, so a 230-minute ETA means a cell crawling at ~23 km/h --
// a 6 px shift between frames, down at the noise floor of the search. And at
// that range and lead time weather develops and decays rather than merely
// translating, so the constant-velocity model the ETA rests on has stopped
// applying. radar-screen-design.md is explicit that the scope shows ~2 h of
// approach and must not be read as +3h.
//
// RADAR_STALLED_KMH alone does not catch this: it rejects a cell that is not
// moving, not one that is moving too slowly for the arithmetic to stay honest
// out to the rim. Without this bound the gates permit ETAs up to 524 minutes.
static const int RADAR_ETA_HORIZON_SEC = 2 * 60 * 60;

// Half-width of the upstream corridor walked back from home, ~9 km. Narrower
// and a cell that clips you is missed; wider and rain that passes to the side
// starts counting as an arrival.
static const int RADAR_CORRIDOR_HALF_PX = 15;

// ---------------------------------------------------------------- the beam

// 20 deg trailing wedge at 0.4 deg per ray. The step is what makes the fill
// gapless at the rim (148 px x 0.4 deg ~ 1.03 px). Both the width and the
// density are half what they were when the beam crossed the eye in 6 seconds:
// at 10 min/rev it dwells in a sector for over a minute, so it has to be far
// more polite about occluding the returns underneath it.
static const int BEAM_RAYS = 50;
static const float BEAM_STEP_DEG = 0.4f;
static const int BEAM_MAX_DENSITY = 4;
// One revolution per minute.
//
// This was 600 s -- one RainViewer frame interval -- which made the sweep's
// position a countdown to the next frame. That job is gone, and deliberately:
// the contacts gave the sweep a better one. The sweep is now the thing that PINGS
// the heavy-rain contacts, and a ping that happens once every ten minutes is a
// reveal nobody is in front of the screen to see. Once a minute is.
//
// Note this is NOT a revival of the old "doubles as a minute hand" claim,
// which was always false -- a 60 s revolution points at the current second,
// and nobody reads a scope for that. The sweep is justified here by what it
// does to the contacts, not by what it says about the time.
//
// Tip speed at the rim is 930/period px/s, so 60 s/rev is 15.5 px/s and needs
// RENDER_PERIOD_RADAR_MS to keep the step near the 1.55 px already accepted.
static const int BEAM_PERIOD_SEC = 60;

// The sweep carries exactly one fact: its POSITION is a countdown to the next
// frame -- 12 o'clock means one just landed, halfway round means ~5 minutes to
// the next. It is not a clock, and it is not a liveness cue: at 1.55 px/s the
// rim tip is at the threshold of human motion detection, so the DATA stamp and
// the age footer carry that instead, and there is no parked state any more.
//
// That fact is also derivable from the DATA stamp plus "frames come every ten
// minutes", so the sweep only saves the arithmetic. The honest remaining
// argument for it is that a radar scope without a sweep does not look like a
// radar scope -- an aesthetic call, and one that cannot be settled off the
// board. Hence a switch rather than a rewrite.
//
// false makes the scope fully static between frames: the memcmp in
// render_task then skips nearly every push, taking the screen from ~1 SPI
// transfer per second to roughly one per minute, when the ETA ticks over.
// 5.0 ms per 15 KB transfer at 24 MHz, so 0.5% duty against 0.008%.
static const bool BEAM_ENABLED = true;

// ----------------------------------------------------------------- blips
//
// Heavy rain is drawn as a radar CONTACT -- a solid dot with a ring pulsing
// outward from it as the sweep passes -- rather than as its true footprint.
//
// Measured on a live frame, that abstraction costs nothing. tier 3 broke into
// 4 connected clusters, only 2 of them 8 px or larger: 50 px (radius 4.0 px)
// at 45 km and 10 px (radius 1.8 px) at 86 km. At 8 px across there is no
// shape to lose, so a symbol is not hiding anything. tier 2+ is a different
// story -- 31 clusters, the largest 2,633 px at radius 29 px -- which is real
// storm AREA and would be a lie as a dot. Hence tier 3 only.
//
// The dither already draws tier 3 solid at 16/16, so this adds emphasis, not
// information. What it buys is that a 4 px blob inside a 300 px field carrying
// 17% tier-0 stipple everywhere is very easy to miss, and heavy rain is the
// one thing on the scope that should never be missed.
static const int RADAR_BLIP_TIER_FLOOR = 3;
// Clusters smaller than this are speckle, not weather. On the measured frame
// this keeps 2 contacts and drops 2.
static const int RADAR_BLIP_MIN_PX = 8;
// Cells this far apart still count as one contact, so a ragged core does not
// come apart into a cloud of dots.
static const int RADAR_BLIP_LINK_PX = 2;
static const int RADAR_BLIP_MAX = 16;
// Flood fill is iterative with an explicit stack rather than recursive: an
// 88 km scope of heavy rain would blow the task stack. Overflow splits a
// contact into two rather than corrupting anything.
static const int RADAR_BLIP_STACK = 4096;

// The dot is a marker, not a footprint, so it is deliberately drawn LARGER
// than the returns it stands for -- padded, then clamped so a tiny cell still
// reads and a huge one does not become a blob.
static const int RADAR_BLIP_PAD_PX = 2;
static const int RADAR_BLIP_MIN_RADIUS = 4;
static const int RADAR_BLIP_MAX_RADIUS = 12;

// Phosphor decay, in two timescales, because one does not work.
//
// The RING is a ping: a quarter turn, 15 s of every minute at 60 s/rev. Short
// and sharp is what makes it read as a pulse.
//
// The DOT decays over three quarters of a turn -- 45 s of every minute. Fading
// it on the ring's schedule would leave heavy rain visible only 15 s in 60,
// which defeats the reason for marking it at all: a contact you have a 75%
// chance of missing is worse than one drawn permanently. Three quarters keeps
// it swept into existence, which is the effect wanted, while leaving only a
// short blind window before the sweep comes round again.
static const float RADAR_BLIP_PERSIST_DEG = 90.0f;
static const float RADAR_BLIP_DOT_PERSIST_DEG = 270.0f;
static const int RADAR_BLIP_DOT_DENSITY = 16;
// Below this the dot is too sparse to carry a hard edge, so the knockout is
// dropped and it simply thins into the field -- which is what decay looks
// like. Above it the edge is what makes the dot a contact rather than a patch
// of heavier stipple.
static const int RADAR_BLIP_EDGE_DENSITY = 8;

// A contact this close to home gets an alert ring drawn around the centre.
// 37 px is the FIRST RANGE RING -- 22 km -- deliberately, so the threshold is
// already drawn on the scope and needs no explaining: the alert means "there
// is a heavy cell inside the inner ring". Distance is centroid to centre, so
// a large cell whose edge is closer still trips it late; the ETA is what
// reports approach, this only reports arrival in the neighbourhood.
// The bearing mark lives at the RIM, among the echoes it describes. It used
// to run the full 148 px from the centre, which on hardware read as a slash
// across the whole scope -- more ink than the rain it was pointing at, and
// crossing every range ring on the way. The chevron is the indicator; the tail
// only has to be long enough to say "this is a direction, not a tick".
static const int RADAR_BEARING_APEX_PX = 14;
static const int RADAR_BEARING_TAIL_PX = 30;

static const int RADAR_BLIP_NEAR_PX = 37;
static const int RADAR_ALERT_RING_A = 10;
static const int RADAR_ALERT_RING_B = 13;
static const int RADAR_BLIP_RING_GAP = 3;
static const int RADAR_BLIP_RING_EXPAND = 14;
static const int RADAR_BLIP_RING_DENSITY = 16;

// Newest frame older than this and the scope is showing rain that has moved
// on. RainViewer publishes every 10 minutes.
static const int64_t STALE_AGE_SEC = 25 * 60;
// Matches clock_screen: anything below this is a clock SNTP has not set yet.
static const time_t CLOCK_VALID_EPOCH = 1700000000;

// Polling is aligned to the wall clock, not free-running. Every frame stamp
// lands on a 10-minute boundary -- verified, 13 consecutive frames spaced
// exactly 600 s -- but publication trails the stamp: 102 s measured on
// 2026-08-23, one sample. So the earliest moment a new frame can plausibly
// exist is the boundary plus that lag, and asking before it is asking for the
// frame already in hand.
static const int64_t FRAME_PERIOD_SEC = 600;
static const int64_t PUBLISH_LAG_SEC = 120;
// If publication runs later than the estimate, the index reports the frame
// already held, the tile fetch short-circuits, and this is the wait before
// asking again. It is what stops a late publish from either spinning or
// costing a whole skipped frame.
static const int64_t ALIGNED_RETRY_US = 60LL * 1000000LL;
// Fallback only, for before SNTP has set the clock: with no wall time there
// is nothing to align to.
static const int64_t REFRESH_INTERVAL_US = 5LL * 60LL * 1000000LL;
static const int64_t RETRY_INTERVAL_US = 60LL * 1000000LL;

typedef struct {
    double lat;
    double lon;
    const char *label;
} RadarLandmark;

// Projected once at init. Splitting the table from its pixels is the point:
// WHICH towns needs a geocoding API and the quadrant rule, so it stays in the
// script; WHERE they land needs only RADAR_ZOOM, so it belongs on the board.
typedef struct {
    int16_t x;
    int16_t y;
} RadarLandmarkPx;

// Selected by tools/radar_landmarks.py, which resolves names through
// Open-Meteo's geocoding API. Picked one per bearing quadrant: sorting by
// population alone piles every label into the south-east and leaves half the
// scope with nothing to read against.
//
// These are COORDINATES, not pixels. The board projects them at init with the
// same radar_project() that places home, so they follow RADAR_ZOOM on their
// own. The old baked pixel table could -- and did -- drift out of step with
// the zoom it was generated at.
static const RadarLandmark RADAR_LANDMARKS[] = {
    {15.7047, 100.1372, "NAKHON SAWAN"},  // N   47.1 km   41.9 deg
    {15.3794, 100.0245, "UTHAI THANI"},   // E   19.4 km   93.5 deg
    {14.8879, 100.4046, "SING BURI"},     // E   82.1 km  132.9 deg
    {14.8418,  99.6976, "DAN CHANG"},     // S   62.9 km  194.4 deg
    {15.4529,  99.5761, "LAN SAK"},       // W   29.6 km  283.7 deg
};
static const int RADAR_LANDMARK_COUNT = sizeof(RADAR_LANDMARKS) / sizeof(RADAR_LANDMARKS[0]);
static RadarLandmarkPx g_landmark_px[sizeof(RADAR_LANDMARKS) / sizeof(RADAR_LANDMARKS[0])];

typedef enum {
    RADAR_EMPTY,
    RADAR_LOADING,
    RADAR_READY,
    RADAR_ERROR,
} RadarStatus;

typedef enum {
    RADAR_ETA_NONE = 0,  // no confident vector: draw nothing, claim nothing
    RADAR_ETA_CLEAR,     // vector good, corridor empty to the rim
    RADAR_ETA_MINUTES,   // vector good, something inbound
} RadarEtaKind;

typedef struct {
    bool valid;            // false means the wedge AND the number are withheld
    float bearing_rad;     // direction rain comes FROM, screen frame (+x = east)
    float speed_kmh;
    RadarEtaKind eta_kind;
    int eta_sec;           // seconds from frame_time to arrival; decays on screen
} RadarMotion;

typedef struct {
    int16_t x;          // centroid
    int16_t y;
    uint8_t radius;     // symbol size, not the cluster's true extent
    float bearing_rad;  // precomputed at refresh; render only compares angles
} RadarBlip;

// Written by the radar task and read by the render task, both under
// g_radar.mutex. Kept out of RadarState so the wholesale snapshot copy in
// radar_render_current does not drag it onto the render task's stack.
static RadarBlip g_blips[RADAR_BLIP_MAX];
static int g_blip_count;

typedef struct {
    SemaphoreHandle_t mutex;
    uint8_t *scope;      // 300x300 1bpp XBM: rain, already dithered and masked
    time_t frame_time;   // RainViewer timestamp of what `scope` holds
    int probability_3h;  // percent, or -1 when not known
    bool raining_home;
    RadarMotion motion;
    char error[48];
    RadarStatus status;
} RadarState;

static RadarState g_radar;
static TaskHandle_t g_radar_task;
static int64_t g_last_attempt_us;

// Tile indices and the crop origin inside that tile. Derived once at init from
// the home coordinates rather than pasted in, so moving home is a two-constant
// change and the landmark script stays in agreement.
static int g_tile_x;
static int g_tile_y;
static int g_crop_x;
static int g_crop_y;

// ---------------------------------------------------------------- projection

// Web Mercator to global pixel coordinates at RADAR_ZOOM. Mirrored by
// project() in tools/radar_landmarks.py; the two must stay in step, because
// the landmark table is generated there and consumed here.
static void radar_project(double lat, double lon, double *out_x, double *out_y)
{
    const double span = (double)(1 << RADAR_ZOOM) * (double)RADAR_TILE_PX;
    const double lat_rad = lat * M_PI / 180.0;
    *out_x = (lon + 180.0) / 360.0 * span;
    *out_y = (1.0 - log(tan(lat_rad) + 1.0 / cos(lat_rad)) / M_PI) / 2.0 * span;
}

// ---------------------------------------------------------------- state

static void set_status(RadarStatus status, const char *error)
{
    xSemaphoreTake(g_radar.mutex, portMAX_DELAY);
    g_radar.status = status;
    snprintf(g_radar.error, sizeof(g_radar.error), "%s", error == NULL ? "" : error);
    xSemaphoreGive(g_radar.mutex);
}

static void *alloc_external(size_t bytes)
{
    void *memory = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return memory == NULL ? malloc(bytes) : memory;
}

static uint8_t *alloc_scope(void)
{
    uint8_t *bitmap = (uint8_t *)heap_caps_calloc(1, SCOPE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (bitmap == NULL) {
        bitmap = (uint8_t *)calloc(1, SCOPE_BYTES);
    }
    return bitmap;
}

// ---------------------------------------------------------------- http

static esp_err_t open_https(const char *url, esp_http_client_handle_t *client, int64_t *content_length)
{
    esp_http_client_config_t config = {};
    config.url = url;
    config.timeout_ms = HTTP_TIMEOUT_MS;
    config.buffer_size = 2048;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    *client = esp_http_client_init(&config);
    if (*client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_open(*client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(*client);
        *client = NULL;
        return err;
    }

    *content_length = esp_http_client_fetch_headers(*client);
    const int status = esp_http_client_get_status_code(*client);
    if (status != 200) {
        ESP_LOGW(TAG, "GET %s returned HTTP %d", url, status);
        esp_http_client_close(*client);
        esp_http_client_cleanup(*client);
        *client = NULL;
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static esp_err_t fetch_json(const char *url, char *json, size_t json_size)
{
    esp_http_client_handle_t client = NULL;
    int64_t content_length = -1;
    esp_err_t err = open_https(url, &client, &content_length);
    if (err != ESP_OK) {
        return err;
    }
    if (content_length >= (int64_t)json_size) {
        err = ESP_ERR_INVALID_SIZE;
    }

    size_t used = 0;
    while (err == ESP_OK) {
        const int read = esp_http_client_read(client, json + used, json_size - used - 1);
        if (read < 0) {
            err = ESP_FAIL;
            break;
        }
        if (read == 0) {
            break;
        }
        used += (size_t)read;
        if (used + 1 >= json_size) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
    }
    json[used] = '\0';
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

// ---------------------------------------------------------------- decode

typedef struct {
    uint8_t *bitmap;     // dithered, for the screen
    uint8_t *corr_mask;  // undithered, at RADAR_CORR_TIER_FLOOR, for correlation
    uint8_t *eta_mask;   // undithered, at RADAR_ETA_TIER_FLOOR, for the corridor
    uint8_t *heavy_mask; // undithered, at RADAR_BLIP_TIER_FLOOR, for the contacts
    bool size_ok;
    bool done;
} RadarDecode;

// Two interleaved ramps. The blue one runs cyan (light) -> dark blue (heavy)
// and is ordered by the GREEN channel: luminance is not a valid ordering here
// because the warm ramp interleaves with it.
static int radar_tier(const uint8_t rgba[4])
{
    const int r = rgba[0];
    const int g = rgba[1];
    const int b = rgba[2];
    if (b > g && b >= r) {
        if (g >= 200) {
            return 0;  // lightest cyan: light rain, the band that washes out
        }
        return g >= 160 ? 1 : 2;
    }
    // Not the blue ramp means the warm one -- yellow, orange, red -- and all
    // of it is heavy. Thresholding within it only creates a hole for the
    // darkest reds, which are the strongest returns on the tile.
    return 3;
}

static void radar_png_init(pngle_t *png, uint32_t width, uint32_t height)
{
    RadarDecode *ctx = (RadarDecode *)pngle_get_user_data(png);
    ctx->size_ok = width == (uint32_t)RADAR_TILE_PX && height == (uint32_t)RADAR_TILE_PX;
    if (!ctx->size_ok) {
        ESP_LOGW(TAG, "unexpected tile size %ux%u", (unsigned)width, (unsigned)height);
    }
}

// Without this a connection dropped mid-tile would look like success: the
// header has already been seen, pngle reports no error on truncation, and the
// read loop simply ends. The frame would then commit half-decoded and be
// stamped as current.
static void radar_png_done(pngle_t *png)
{
    RadarDecode *ctx = (RadarDecode *)pngle_get_user_data(png);
    ctx->done = true;
}

static void radar_png_draw(pngle_t *png,
                           uint32_t x,
                           uint32_t y,
                           uint32_t width,
                           uint32_t height,
                           const uint8_t rgba[4])
{
    RadarDecode *ctx = (RadarDecode *)pngle_get_user_data(png);
    if (!ctx->size_ok || ctx->bitmap == NULL) {
        return;
    }
    // Alpha 130-190 is RainViewer's clutter/haze band (7 beige tones); only
    // alpha 255 is real rain. Cheapest possible reject, and it removes most of
    // the tile before anything else is computed.
    if (rgba[3] != 255) {
        return;
    }
    const int tier = radar_tier(rgba);
    if (tier < RADAR_TIER_FLOOR && tier < RADAR_CORR_TIER_FLOOR) {
        return;
    }
    const int density = RADAR_TIER_DENSITY[tier];

    // pngle reports multi-pixel rects on interlaced PNGs, so honour w/h rather
    // than treating this as a single pixel.
    for (uint32_t py = y; py < y + height; py++) {
        const int sy = (int)py - g_crop_y;
        if (sy < 0 || sy >= SCOPE) {
            continue;
        }
        const int dy = sy - SCOPE_CENTER;
        for (uint32_t px = x; px < x + width; px++) {
            const int sx = (int)px - g_crop_x;
            if (sx < 0 || sx >= SCOPE) {
                continue;
            }
            const int dx = sx - SCOPE_CENTER;
            if (dx * dx + dy * dy > SCOPE_RADIUS * SCOPE_RADIUS) {
                continue;
            }
            const size_t bit = (size_t)sy * SCOPE_ROW_BYTES + (size_t)(sx / 8);
            const uint8_t set = (uint8_t)(1U << (sx & 7));
            // The Bayer matrix is anchored to screen space, not regenerated
            // per frame: a rotating beam over per-pixel noise boils.
            if (tier >= RADAR_TIER_FLOOR && RADAR_BAYER[sy & 3][sx & 3] < density) {
                ctx->bitmap[bit] |= set;
            }
            // The masks are undithered on purpose. Correlating stipple would
            // measure the Bayer phase as much as the rain: the matrix is
            // screen-anchored, so a storm shifted 11 px re-stipples against a
            // different phase and matches itself badly.
            if (ctx->corr_mask != NULL && tier >= RADAR_CORR_TIER_FLOOR) {
                ctx->corr_mask[bit] |= set;
            }
            if (ctx->eta_mask != NULL && tier >= RADAR_ETA_TIER_FLOOR) {
                ctx->eta_mask[bit] |= set;
            }
            if (ctx->heavy_mask != NULL && tier >= RADAR_BLIP_TIER_FLOOR) {
                ctx->heavy_mask[bit] |= set;
            }
        }
    }
}

static esp_err_t fetch_tile(const char *url,
                            uint8_t *bitmap,
                            uint8_t *corr_mask,
                            uint8_t *eta_mask,
                            uint8_t *heavy_mask)
{
    esp_http_client_handle_t client = NULL;
    int64_t content_length = -1;
    esp_err_t err = open_https(url, &client, &content_length);
    if (err != ESP_OK) {
        return err;
    }
    if (content_length > (int64_t)MAX_TILE_BYTES) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_SIZE;
    }

    RadarDecode ctx = {};
    ctx.bitmap = bitmap;
    ctx.corr_mask = corr_mask;
    ctx.eta_mask = eta_mask;
    ctx.heavy_mask = heavy_mask;

    pngle_t *png = pngle_new();
    uint8_t *buffer = png == NULL ? NULL : (uint8_t *)alloc_external(HTTP_BUFFER_BYTES);
    if (buffer == NULL) {
        if (png != NULL) {
            pngle_destroy(png);
        }
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }
    pngle_set_user_data(png, &ctx);
    pngle_set_init_callback(png, radar_png_init);
    pngle_set_draw_callback(png, radar_png_draw);
    pngle_set_done_callback(png, radar_png_done);

    size_t remain = 0;
    size_t total = 0;
    while (err == ESP_OK && !ctx.done) {
        const int read = esp_http_client_read(client, (char *)buffer + remain, HTTP_BUFFER_BYTES - remain);
        if (read < 0) {
            err = ESP_FAIL;
            break;
        }
        if (read == 0) {
            break;
        }
        total += (size_t)read;
        if (total > MAX_TILE_BYTES) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        const size_t available = remain + (size_t)read;
        const int consumed = pngle_feed(png, buffer, available);
        if (consumed < 0) {
            ESP_LOGW(TAG, "tile decode failed: %s", pngle_error(png));
            err = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        remain = available - (size_t)consumed;
        if (remain > 0 && consumed > 0) {
            memmove(buffer, buffer + consumed, remain);
        }
        if (remain == HTTP_BUFFER_BYTES) {
            err = ESP_ERR_INVALID_RESPONSE;
            break;
        }
    }
    if (err == ESP_OK && (!ctx.done || !ctx.size_ok)) {
        err = ctx.size_ok ? ESP_ERR_INVALID_RESPONSE : ESP_ERR_INVALID_SIZE;
    }
    free(buffer);
    pngle_destroy(png);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

// ---------------------------------------------------------------- refresh

// ----------------------------------------------------------------- blips

static inline bool mask_get(const uint8_t *mask, int x, int y)
{
    return ((mask[(size_t)y * SCOPE_ROW_BYTES + (size_t)(x / 8)] >> (x & 7)) & 1U) != 0U;
}

static inline void mask_set(uint8_t *mask, int x, int y)
{
    mask[(size_t)y * SCOPE_ROW_BYTES + (size_t)(x / 8)] |= (uint8_t)(1U << (x & 7));
}

typedef struct {
    int16_t *stack;  // x,y pairs
    int depth;
} BlipFill;

// Iterative flood fill over the heavy mask. Returns the cluster's pixel count
// and accumulates its centroid; `seen` stops a cell being visited twice.
static int blip_flood(const uint8_t *heavy, uint8_t *seen, BlipFill *fill, int sx, int sy,
                      int32_t *sum_x, int32_t *sum_y)
{
    int count = 0;
    fill->depth = 0;
    fill->stack[fill->depth * 2] = (int16_t)sx;
    fill->stack[fill->depth * 2 + 1] = (int16_t)sy;
    fill->depth++;
    mask_set(seen, sx, sy);

    while (fill->depth > 0) {
        fill->depth--;
        const int x = fill->stack[fill->depth * 2];
        const int y = fill->stack[fill->depth * 2 + 1];
        count++;
        *sum_x += x;
        *sum_y += y;
        for (int dy = -RADAR_BLIP_LINK_PX; dy <= RADAR_BLIP_LINK_PX; dy++) {
            for (int dx = -RADAR_BLIP_LINK_PX; dx <= RADAR_BLIP_LINK_PX; dx++) {
                const int nx = x + dx;
                const int ny = y + dy;
                if (nx < 0 || nx >= SCOPE || ny < 0 || ny >= SCOPE) {
                    continue;
                }
                if (!mask_get(heavy, nx, ny) || mask_get(seen, nx, ny)) {
                    continue;
                }
                if (fill->depth >= RADAR_BLIP_STACK) {
                    // Out of stack. Splitting the contact is the graceful
                    // failure; the alternative is scribbling past the buffer.
                    continue;
                }
                mask_set(seen, nx, ny);
                fill->stack[fill->depth * 2] = (int16_t)nx;
                fill->stack[fill->depth * 2 + 1] = (int16_t)ny;
                fill->depth++;
            }
        }
    }
    return count;
}

// Rebuilt from scratch every frame, on the refresh path, never in a render
// frame. Connected components over ~90,000 bit tests, once per ten minutes.
static void radar_blips_update(const uint8_t *heavy_mask)
{
    uint8_t *seen = alloc_scope();
    int16_t *stack = (int16_t *)alloc_external((size_t)RADAR_BLIP_STACK * 2 * sizeof(int16_t));
    if (seen == NULL || stack == NULL) {
        free(seen);
        free(stack);
        xSemaphoreTake(g_radar.mutex, portMAX_DELAY);
        g_blip_count = 0;
        xSemaphoreGive(g_radar.mutex);
        return;
    }
    BlipFill fill = {stack, 0};

    RadarBlip found[RADAR_BLIP_MAX];
    int found_count = 0;
    int smallest = 0;  // index of the weakest contact held, for eviction

    for (int y = 0; y < SCOPE; y++) {
        for (int x = 0; x < SCOPE; x++) {
            if (!mask_get(heavy_mask, x, y) || mask_get(seen, x, y)) {
                continue;
            }
            int32_t sum_x = 0;
            int32_t sum_y = 0;
            const int area = blip_flood(heavy_mask, seen, &fill, x, y, &sum_x, &sum_y);
            if (area < RADAR_BLIP_MIN_PX) {
                continue;
            }
            // Symbol size from area, padded and clamped: the dot stands for
            // the contact, it does not measure it.
            int radius = (int)lroundf(sqrtf((float)area / (float)M_PI)) + RADAR_BLIP_PAD_PX;
            if (radius < RADAR_BLIP_MIN_RADIUS) {
                radius = RADAR_BLIP_MIN_RADIUS;
            }
            if (radius > RADAR_BLIP_MAX_RADIUS) {
                radius = RADAR_BLIP_MAX_RADIUS;
            }
            RadarBlip blip;
            blip.x = (int16_t)(sum_x / area);
            blip.y = (int16_t)(sum_y / area);
            blip.radius = (uint8_t)radius;
            blip.bearing_rad =
                atan2f((float)(blip.y - SCOPE_CENTER), (float)(blip.x - SCOPE_CENTER));

            if (found_count < RADAR_BLIP_MAX) {
                found[found_count++] = blip;
            } else {
                // Full. Keep the biggest contacts -- on a scope this crowded
                // the small ones are the ones you can afford to lose.
                for (int i = 0; i < found_count; i++) {
                    if (found[i].radius < found[smallest].radius) {
                        smallest = i;
                    }
                }
                if (blip.radius > found[smallest].radius) {
                    found[smallest] = blip;
                }
            }
        }
    }

    xSemaphoreTake(g_radar.mutex, portMAX_DELAY);
    memcpy(g_blips, found, sizeof(RadarBlip) * (size_t)found_count);
    g_blip_count = found_count;
    xSemaphoreGive(g_radar.mutex);

    free(seen);
    free(stack);
    if (found_count > 0) {
        ESP_LOGI(TAG, "blips: %d contact(s)", found_count);
    }
}

// ---------------------------------------------------------------- motion

// The partner frame for the next correlation. Owned entirely by the radar
// task, so it needs no mutex: the render task never sees a mask, only the
// RadarMotion that falls out of one.
static uint8_t *g_prev_mask;
static time_t g_prev_mask_time;

// Set cells of `older` inside the inner disc, sampled. Independent of the
// candidate offset -- which is the whole point of the inner disc -- so it is
// computed once and reused as the denominator for every score.
static int corr_reference(const uint8_t *older)
{
    int count = 0;
    for (int y = SCOPE_CENTER - RADAR_CORR_RADIUS; y <= SCOPE_CENTER + RADAR_CORR_RADIUS;
         y += RADAR_CORR_STEP) {
        const int dy = y - SCOPE_CENTER;
        for (int x = SCOPE_CENTER - RADAR_CORR_RADIUS; x <= SCOPE_CENTER + RADAR_CORR_RADIUS;
             x += RADAR_CORR_STEP) {
            const int dx = x - SCOPE_CENTER;
            if (dx * dx + dy * dy > RADAR_CORR_RADIUS * RADAR_CORR_RADIUS) {
                continue;
            }
            if (mask_get(older, x, y)) {
                count++;
            }
        }
    }
    return count;
}

// Cells that are set in `older` and still set in `newer` once shifted by
// (sx, sy). Every sample sits inside the inner disc, so `newer` is read at
// most RADAR_MAX_SHIFT_PX outside it -- still comfortably within the mask.
static int corr_matched(const uint8_t *older, const uint8_t *newer, int sx, int sy)
{
    int count = 0;
    for (int y = SCOPE_CENTER - RADAR_CORR_RADIUS; y <= SCOPE_CENTER + RADAR_CORR_RADIUS;
         y += RADAR_CORR_STEP) {
        const int dy = y - SCOPE_CENTER;
        for (int x = SCOPE_CENTER - RADAR_CORR_RADIUS; x <= SCOPE_CENTER + RADAR_CORR_RADIUS;
             x += RADAR_CORR_STEP) {
            const int dx = x - SCOPE_CENTER;
            if (dx * dx + dy * dy > RADAR_CORR_RADIUS * RADAR_CORR_RADIUS) {
                continue;
            }
            if (mask_get(older, x, y) && mask_get(newer, x + sx, y + sy)) {
                count++;
            }
        }
    }
    return count;
}

// Coarse pass, three gates, then a fine pass around the winner. Returns false
// -- meaning nothing gets drawn -- unless all three gates pass.
static bool corr_solve(const uint8_t *older, const uint8_t *newer, int *out_dx, int *out_dy)
{
    const int reference = corr_reference(older);
    // Gate 1: enough signal. No rain, no vector.
    if (reference < RADAR_GATE_MIN_CELLS) {
        ESP_LOGD(TAG, "motion: only %d reference cells", reference);
        return false;
    }

    int score[RADAR_COARSE_GRID][RADAR_COARSE_GRID];
    int best = -1;
    int best_i = 0;
    int best_j = 0;
    for (int j = 0; j < RADAR_COARSE_GRID; j++) {
        const int sy = -RADAR_MAX_SHIFT_PX + j * RADAR_COARSE_STEP;
        for (int i = 0; i < RADAR_COARSE_GRID; i++) {
            const int sx = -RADAR_MAX_SHIFT_PX + i * RADAR_COARSE_STEP;
            const int matched = corr_matched(older, newer, sx, sy);
            score[j][i] = matched;
            if (matched > best) {
                best = matched;
                best_i = i;
                best_j = j;
            }
        }
    }

    // Gate 2: the winner actually matches.
    if ((float)best < RADAR_GATE_MATCH_FLOOR * (float)reference) {
        ESP_LOGD(TAG, "motion: best %d/%d below match floor", best, reference);
        return false;
    }

    // Gate 3: the winner is DISTINCT. Evaluated on the coarse grid, which is
    // the scale at which a flat correlation surface actually looks flat, and
    // against non-adjacent candidates only -- the cells touching the peak are
    // part of the same peak, not rivals to it.
    int rival = 0;
    for (int j = 0; j < RADAR_COARSE_GRID; j++) {
        for (int i = 0; i < RADAR_COARSE_GRID; i++) {
            const int di = i - best_i < 0 ? best_i - i : i - best_i;
            const int dj = j - best_j < 0 ? best_j - j : j - best_j;
            if (di <= 1 && dj <= 1) {
                continue;
            }
            if (score[j][i] > rival) {
                rival = score[j][i];
            }
        }
    }
    if ((float)(best - rival) < RADAR_GATE_PEAK_MARGIN * (float)reference) {
        ESP_LOGD(TAG, "motion: peak %d vs rival %d is not distinct", best, rival);
        return false;
    }

    // Fine pass. The coarse step is 4, so +/-3 covers every offset the coarse
    // grid skipped without re-testing its neighbours' territory.
    int fine_dx = -RADAR_MAX_SHIFT_PX + best_i * RADAR_COARSE_STEP;
    int fine_dy = -RADAR_MAX_SHIFT_PX + best_j * RADAR_COARSE_STEP;
    int fine_best = best;
    for (int sy = fine_dy - RADAR_COARSE_STEP + 1; sy <= fine_dy + RADAR_COARSE_STEP - 1; sy++) {
        if (sy < -RADAR_MAX_SHIFT_PX || sy > RADAR_MAX_SHIFT_PX) {
            continue;
        }
        for (int sx = fine_dx - RADAR_COARSE_STEP + 1; sx <= fine_dx + RADAR_COARSE_STEP - 1; sx++) {
            if (sx < -RADAR_MAX_SHIFT_PX || sx > RADAR_MAX_SHIFT_PX) {
                continue;
            }
            const int matched = corr_matched(older, newer, sx, sy);
            if (matched > fine_best) {
                fine_best = matched;
                fine_dx = sx;
                fine_dy = sy;
            }
        }
    }
    *out_dx = fine_dx;
    *out_dy = fine_dy;
    return true;
}

// Walks back upstream from home along `u`, sweeping a corridor either side,
// and returns how far away the first qualifying cell is. Anything outside the
// corridor is rain that misses the house and is correctly ignored.
static RadarEtaKind eta_scan(const uint8_t *eta_mask, float ux, float uy, float px_per_sec, int *out_sec)
{
    for (int step = 1; step <= SCOPE_RADIUS; step++) {
        const float cx = (float)SCOPE_CENTER + ux * (float)step;
        const float cy = (float)SCOPE_CENTER + uy * (float)step;
        for (int w = -RADAR_CORRIDOR_HALF_PX; w <= RADAR_CORRIDOR_HALF_PX; w++) {
            // Perpendicular to (ux, uy) is (-uy, ux).
            const int x = (int)lroundf(cx - uy * (float)w);
            const int y = (int)lroundf(cy + ux * (float)w);
            if (x < 0 || x >= SCOPE || y < 0 || y >= SCOPE) {
                continue;
            }
            const int dx = x - SCOPE_CENTER;
            const int dy = y - SCOPE_CENTER;
            if (dx * dx + dy * dy > SCOPE_RADIUS * SCOPE_RADIUS) {
                continue;
            }
            if (mask_get(eta_mask, x, y)) {
                *out_sec = (int)lroundf((float)step / px_per_sec);
                return RADAR_ETA_MINUTES;
            }
        }
    }
    // Genuinely useful, and the common case: nothing inbound out to 88 km.
    return RADAR_ETA_CLEAR;
}

// Takes ownership of `corr_mask`, which becomes the partner for the next
// frame. `eta_mask` is only needed here and stays the caller's to free.
static void radar_motion_update(uint8_t *corr_mask,
                                const uint8_t *eta_mask,
                                time_t frame_time,
                                RadarMotion *out)
{
    *out = RadarMotion();

    const int64_t gap = g_prev_mask_time > 0 ? (int64_t)frame_time - (int64_t)g_prev_mask_time : 0;
    if (g_prev_mask != NULL && gap > 0 && gap <= RADAR_MAX_PAIR_GAP_SEC) {
        int dx = 0;
        int dy = 0;
        if (corr_solve(g_prev_mask, corr_mask, &dx, &dy)) {
            const float px = sqrtf((float)(dx * dx + dy * dy));
            const float px_per_sec = px / (float)gap;
            const float kmh = px_per_sec * g_km_per_px * 3600.0f;
            if (kmh >= RADAR_STALLED_KMH) {
                out->valid = true;
                out->speed_kmh = kmh;
                // Rain arrives FROM the direction opposite its travel, so the
                // marker goes at -v. Putting it on the rim among the echoes it
                // describes is what removes the from/toward ambiguity: there
                // is no convention to remember when the mark sits on top of
                // the stipple it is pointing at.
                out->bearing_rad = atan2f(-(float)dy, -(float)dx);
                out->eta_kind = eta_scan(eta_mask,
                                         cosf(out->bearing_rad),
                                         sinf(out->bearing_rad),
                                         px_per_sec,
                                         &out->eta_sec);
                ESP_LOGI(TAG,
                         "motion %+d,%+d over %llds = %.0f km/h, eta kind %d (%d s)",
                         dx,
                         dy,
                         (long long)gap,
                         (double)kmh,
                         (int)out->eta_kind,
                         out->eta_sec);
            }
        }
    }

    free(g_prev_mask);
    g_prev_mask = corr_mask;
    g_prev_mask_time = frame_time;
}

static esp_err_t refresh_scope(char *json)
{
    esp_err_t err = fetch_json(RAINVIEWER_INDEX_URL, json, MAX_API_BYTES + 1);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON *host = cJSON_GetObjectItemCaseSensitive(root, "host");
    const cJSON *radar = cJSON_GetObjectItemCaseSensitive(root, "radar");
    const cJSON *past = radar == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(radar, "past");
    const int frame_count = cJSON_IsArray(past) ? cJSON_GetArraySize(past) : 0;
    if (!cJSON_IsString(host) || host->valuestring == NULL || frame_count == 0) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    // `nowcast` is always empty for this region, so the newest past frame is
    // the newest frame there is: the scope shows now and the past only.
    const cJSON *newest = cJSON_GetArrayItem(past, frame_count - 1);
    const cJSON *path = cJSON_GetObjectItemCaseSensitive(newest, "path");
    const cJSON *stamp = cJSON_GetObjectItemCaseSensitive(newest, "time");
    if (!cJSON_IsString(path) || path->valuestring == NULL || !cJSON_IsNumber(stamp)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    char url[256];
    // The trailing "/0/0_0.png" is colour scheme 0 and smoothing OFF. Leave
    // smoothing off: it interpolates between palette entries, which is exactly
    // what the tier thresholds above key on.
    const int written = snprintf(url,
                                 sizeof(url),
                                 "%s%s/%d/%d/%d/%d/0/0_0.png",
                                 host->valuestring,
                                 path->valuestring,
                                 RADAR_TILE_PX,
                                 RADAR_ZOOM,
                                 g_tile_x,
                                 g_tile_y);
    const time_t frame_time = (time_t)stamp->valuedouble;
    cJSON_Delete(root);
    if (written <= 0 || written >= (int)sizeof(url)) {
        return ESP_ERR_INVALID_SIZE;
    }

    // Polling runs at 5 minutes against frames published every 10, so roughly
    // every other pass sees a frame already in hand. Re-fetching it would
    // re-download a PNG and re-decode it into a byte-identical bitmap. It
    // would also hand the motion solver a frame to correlate against itself,
    // which is a confident zero -- so this guard is load-bearing, not just an
    // economy.
    xSemaphoreTake(g_radar.mutex, portMAX_DELAY);
    const bool already_held = g_radar.scope != NULL && g_radar.frame_time == frame_time;
    xSemaphoreGive(g_radar.mutex);
    if (already_held) {
        return ESP_OK;
    }

    uint8_t *bitmap = alloc_scope();
    uint8_t *corr_mask = alloc_scope();
    uint8_t *eta_mask = alloc_scope();
    uint8_t *heavy_mask = alloc_scope();
    if (bitmap == NULL || corr_mask == NULL || eta_mask == NULL || heavy_mask == NULL) {
        free(bitmap);
        free(corr_mask);
        free(eta_mask);
        free(heavy_mask);
        return ESP_ERR_NO_MEM;
    }
    err = fetch_tile(url, bitmap, corr_mask, eta_mask, heavy_mask);
    if (err != ESP_OK) {
        free(bitmap);
        free(corr_mask);
        free(eta_mask);
        free(heavy_mask);
        return err;
    }

    // Steady state costs no extra fetch: the partner frame is the one already
    // in hand from the last refresh, and the real gap between their stamps is
    // what turns a pixel displacement into a speed.
    RadarMotion motion;
    radar_motion_update(corr_mask, eta_mask, frame_time, &motion);
    free(eta_mask);
    radar_blips_update(heavy_mask);
    free(heavy_mask);

    xSemaphoreTake(g_radar.mutex, portMAX_DELAY);
    free(g_radar.scope);
    g_radar.scope = bitmap;
    g_radar.frame_time = frame_time;
    g_radar.motion = motion;
    xSemaphoreGive(g_radar.mutex);
    ESP_LOGI(TAG, "radar frame %lld decoded", (long long)frame_time);
    return ESP_OK;
}

static esp_err_t refresh_forecast(char *json)
{
    esp_err_t err = fetch_json(OPEN_METEO_URL, json, MAX_API_BYTES + 1);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON *hourly = cJSON_GetObjectItemCaseSensitive(root, "hourly");
    const cJSON *probability =
        hourly == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(hourly, "precipitation_probability");
    if (!cJSON_IsArray(probability) || cJSON_GetArraySize(probability) < 4) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON *at_3h = cJSON_GetArrayItem(probability, 3);
    if (!cJSON_IsNumber(at_3h)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    // Radar and Open-Meteo genuinely disagree at the home pixel: drizzle at
    // 0.1 mm sits below what the radar resolves, so the tile can be empty for
    // 3 px around home while the forecast reports rain. The centre marker is
    // therefore driven by this number, not by the imagery.
    const cJSON *current = cJSON_GetObjectItemCaseSensitive(root, "current");
    const cJSON *precipitation =
        current == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(current, "precipitation");
    const bool raining_home = cJSON_IsNumber(precipitation) && precipitation->valuedouble > 0.0;

    const int value_3h = at_3h->valueint;
    cJSON_Delete(root);

    xSemaphoreTake(g_radar.mutex, portMAX_DELAY);
    g_radar.probability_3h = value_3h;
    g_radar.raining_home = raining_home;
    xSemaphoreGive(g_radar.mutex);
    return ESP_OK;
}

static void radar_task(void *arg)
{
    (void)arg;
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        set_status(RADAR_LOADING, NULL);

        char *json = (char *)alloc_external(MAX_API_BYTES + 1);
        if (json == NULL) {
            set_status(RADAR_ERROR, "NO MEMORY");
            continue;
        }
        // Commit the two halves independently: a forecast outage should not
        // throw away a good radar frame, and vice versa.
        const esp_err_t scope_err = refresh_scope(json);
        if (scope_err != ESP_OK) {
            ESP_LOGW(TAG, "scope refresh failed: %s", esp_err_to_name(scope_err));
        }
        const esp_err_t forecast_err = refresh_forecast(json);
        if (forecast_err != ESP_OK) {
            ESP_LOGW(TAG, "forecast refresh failed: %s", esp_err_to_name(forecast_err));
        }
        free(json);

        const esp_err_t err = scope_err != ESP_OK ? scope_err : forecast_err;
        set_status(err == ESP_OK ? RADAR_READY : RADAR_ERROR, err == ESP_OK ? NULL : esp_err_to_name(err));
        ESP_LOGI(TAG, "radar stack reserve: %u bytes", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
}

// Asks the worker for fresh data, but only when something is actually due.
// Called from the render task, which runs at 1 Hz on this screen: without the
// interval check every frame would re-arm the fetch and hammer the API.
//
// Aligned to the frame clock rather than free-running, so a poll lands just
// after publication (~:02, :12, :22) instead of somewhere random in the cycle.
// The old 5-minute poll asked twice per frame and threw one of the answers
// away.
// The attempt is stamped at notify time rather than on completion, so a run of
// failures backs off instead of retrying flat out.
static void request_refresh_if_due(RadarStatus status)
{
    if (g_radar_task == NULL) {
        return;
    }
    const int64_t now_us = esp_timer_get_time();
    struct timeval tv = {};
    gettimeofday(&tv, NULL);
    const bool clock_valid = tv.tv_sec > CLOCK_VALID_EPOCH;

    bool due = false;
    xSemaphoreTake(g_radar.mutex, portMAX_DELAY);
    if (g_last_attempt_us == 0) {
        // First look at this screen. Do not make it wait for a boundary.
        due = true;
    } else if (status == RADAR_ERROR) {
        due = now_us - g_last_attempt_us >= RETRY_INTERVAL_US;
    } else if (!clock_valid) {
        due = now_us - g_last_attempt_us >= REFRESH_INTERVAL_US;
    } else if (now_us - g_last_attempt_us >= ALIGNED_RETRY_US) {
        // The newest stamp that should exist by now. Comparing it against the
        // frame in hand is self-correcting in both directions: an early ask
        // is not made at all, and a publish later than PUBLISH_LAG_SEC simply
        // stays due and is retried a minute later instead of being missed
        // until the next period.
        const int64_t expected =
            ((int64_t)tv.tv_sec - PUBLISH_LAG_SEC) / FRAME_PERIOD_SEC * FRAME_PERIOD_SEC;
        due = expected > (int64_t)g_radar.frame_time;
    }
    if (due) {
        g_last_attempt_us = now_us;
    }
    xSemaphoreGive(g_radar.mutex);
    if (due) {
        xTaskNotifyGive(g_radar_task);
    }
}

// ---------------------------------------------------------------- drawing

// The panel's memory-in-pixel controller renders draw colour 1 as the light
// state and 0 as dark ink, which every other screen uses to paint a paper
// card. The scope wants the opposite, and gets it for free: the render task
// has already cleared the buffer to all-0, which is a black field, so this is
// a colour change with no fill pass. See clock_screen.cpp for the light one.
static void begin_scope_theme(u8g2_t *u8)
{
    u8g2_SetDrawColor(u8, 1);
    u8g2_SetFontMode(u8, 1);
}

static void draw_furniture(u8g2_t *u8)
{
    // 22 / 44 / 66 / 88 km.
    for (int radius = 37; radius <= SCOPE_RADIUS; radius += 37) {
        u8g2_DrawCircle(u8, SCOPE_CENTER, SCOPE_CENTER, radius, U8G2_DRAW_ALL);
    }
    u8g2_DrawHLine(u8, SCOPE_CENTER - SCOPE_RADIUS, SCOPE_CENTER, SCOPE_RADIUS * 2);
    u8g2_DrawVLine(u8, SCOPE_CENTER, SCOPE_CENTER - SCOPE_RADIUS, SCOPE_RADIUS * 2);

    for (int bearing = 0; bearing < 360; bearing += 15) {
        const float angle = (float)bearing * (float)M_PI / 180.0f;
        const int length = bearing % 45 == 0 ? 8 : 4;
        const float ca = cosf(angle);
        const float sa = sinf(angle);
        u8g2_DrawLine(u8,
                      SCOPE_CENTER + (int)((SCOPE_RADIUS - length) * ca),
                      SCOPE_CENTER + (int)((SCOPE_RADIUS - length) * sa),
                      SCOPE_CENTER + (int)(SCOPE_RADIUS * ca),
                      SCOPE_CENTER + (int)(SCOPE_RADIUS * sa));
    }
}

static void draw_home_marker(u8g2_t *u8, bool raining)
{
    // Filled means it is raining at home right now. This is the one place on
    // screen that reports current rain, and it sits at the exact pixel the eye
    // checks first.
    if (raining) {
        u8g2_DrawDisc(u8, SCOPE_CENTER, SCOPE_CENTER, 3, U8G2_DRAW_ALL);
    } else {
        u8g2_DrawCircle(u8, SCOPE_CENTER, SCOPE_CENTER, 3, U8G2_DRAW_ALL);
    }
    u8g2_DrawHLine(u8, SCOPE_CENTER - 7, SCOPE_CENTER, 15);
    u8g2_DrawVLine(u8, SCOPE_CENTER, SCOPE_CENTER - 7, 15);
}

static void draw_landmarks(u8g2_t *u8)
{
    u8g2_SetFont(u8, u8g2_font_5x7_tf);
    for (int i = 0; i < RADAR_LANDMARK_COUNT; i++) {
        const RadarLandmark *mark = &RADAR_LANDMARKS[i];
        const RadarLandmarkPx *at = &g_landmark_px[i];
        // Off the scope at this zoom -- a tighter zoom can push a town past
        // the rim, and a label hanging in the corner would be a lie.
        const int off_x = at->x - SCOPE_CENTER;
        const int off_y = at->y - SCOPE_CENTER;
        if (off_x * off_x + off_y * off_y > SCOPE_RADIUS * SCOPE_RADIUS) {
            continue;
        }
        const int label_width = (int)u8g2_GetUTF8Width(u8, mark->label);
        // Place the label on the side AWAY from home, so its knockout never
        // lands in the corridor rain travels down to reach the centre. A
        // westerly landmark labelled to its right blanks exactly the strip a
        // cell crosses on approach, which hides the thing the scope is for.
        // Flip back only when the outward side does not fit.
        const bool outward_left = at->x < SCOPE_CENTER;
        int label_x = outward_left ? at->x - 5 - label_width : at->x + 5;
        if (label_x < 2 || label_x + label_width > SCOPE - 2) {
            label_x = outward_left ? at->x + 5 : at->x - 5 - label_width;
        }
        // Knock the field back to black behind the mark first. A 5x7 label
        // laid straight over dithered returns is unreadable, and heavy rain is
        // exactly the situation where the landmarks earn their place. This
        // also clips the range rings where they pass behind a label, which is
        // the cheaper of the two losses.
        u8g2_SetDrawColor(u8, 0);
        u8g2_DrawBox(u8, label_x - 1, at->y - 4, label_width + 2, 9);
        u8g2_DrawBox(u8, at->x - 2, at->y - 2, 5, 5);
        u8g2_SetDrawColor(u8, 1);
        u8g2_DrawFrame(u8, at->x - 1, at->y - 1, 3, 3);
        u8g2_DrawUTF8(u8, label_x, at->y + 3, mark->label);
    }
}

// A dithered circle. u8g2_DrawCircle is solid-only, and a ping that cannot
// fade is not a ping.
static void draw_dithered_circle(u8g2_t *u8, int cx, int cy, int radius, int density)
{
    if (radius <= 0 || density <= 0) {
        return;
    }
    // One step per rim pixel, so the ring is gapless without being redrawn.
    const int steps = radius * 7;
    for (int i = 0; i < steps; i++) {
        const float angle = (float)i * 2.0f * (float)M_PI / (float)steps;
        const int x = cx + (int)lroundf(cosf(angle) * (float)radius);
        const int y = cy + (int)lroundf(sinf(angle) * (float)radius);
        if (x < 0 || x >= SCOPE || y < 0 || y >= SCOPE) {
            continue;
        }
        const int dx = x - SCOPE_CENTER;
        const int dy = y - SCOPE_CENTER;
        if (dx * dx + dy * dy > SCOPE_RADIUS * SCOPE_RADIUS) {
            continue;
        }
        if (RADAR_BAYER[y & 3][x & 3] < density) {
            u8g2_DrawPixel(u8, x, y);
        }
    }
}

// Same halo trick as the bearing line: a 1 px knockout either side so the ring
// reads over any density of stipple underneath it.
static void draw_haloed_circle(u8g2_t *u8, int cx, int cy, int radius)
{
    u8g2_SetDrawColor(u8, 0);
    u8g2_DrawCircle(u8, cx, cy, radius - 1, U8G2_DRAW_ALL);
    u8g2_DrawCircle(u8, cx, cy, radius + 1, U8G2_DRAW_ALL);
    u8g2_SetDrawColor(u8, 1);
    u8g2_DrawCircle(u8, cx, cy, radius, U8G2_DRAW_ALL);
}

// True when a heavy contact has reached the inner range ring.
static bool blips_near_home(void)
{
    for (int i = 0; i < g_blip_count; i++) {
        const int dx = g_blips[i].x - SCOPE_CENTER;
        const int dy = g_blips[i].y - SCOPE_CENTER;
        if (dx * dx + dy * dy <= RADAR_BLIP_NEAR_PX * RADAR_BLIP_NEAR_PX) {
            return true;
        }
    }
    return false;
}

// Heavy rain has arrived in the neighbourhood: two rings around home, just
// outside the centre marker's 15 px crosshair.
//
// Drawn solid and constant rather than pulsed with the sweep. The contacts
// themselves are painted by the beam and decay, which is right for a target
// -- but an alert that blinks out for a quarter of every minute is an alert
// you can miss, and this is the one condition on the scope that matters most.
// It also survives BEAM_ENABLED = false for the same reason.
static void draw_close_alert(u8g2_t *u8)
{
    draw_haloed_circle(u8, SCOPE_CENTER, SCOPE_CENTER, RADAR_ALERT_RING_A);
    draw_haloed_circle(u8, SCOPE_CENTER, SCOPE_CENTER, RADAR_ALERT_RING_B);
}

static void draw_dithered_disc(u8g2_t *u8, int cx, int cy, int radius, int density)
{
    if (radius <= 0 || density <= 0) {
        return;
    }
    for (int y = cy - radius; y <= cy + radius; y++) {
        if (y < 0 || y >= SCOPE) {
            continue;
        }
        const int dy = y - cy;
        for (int x = cx - radius; x <= cx + radius; x++) {
            if (x < 0 || x >= SCOPE) {
                continue;
            }
            const int dx = x - cx;
            if (dx * dx + dy * dy > radius * radius) {
                continue;
            }
            const int ox = x - SCOPE_CENTER;
            const int oy = y - SCOPE_CENTER;
            if (ox * ox + oy * oy > SCOPE_RADIUS * SCOPE_RADIUS) {
                continue;
            }
            if (RADAR_BAYER[y & 3][x & 3] < density) {
                u8g2_DrawPixel(u8, x, y);
            }
        }
    }
}

// Heavy rain as a radar contact, painted by the sweep rather than standing
// there permanently: the dot is lit as the beam crosses it and decays behind
// it, with a ring that leaves it, expands and fades much faster.
//
// With no sweep -- BEAM_ENABLED off, or the clock unset -- there is nothing to
// paint the contacts, so they are drawn solid and static instead. Turning the
// beam off should cost the animation, not the information.
static void draw_blips(u8g2_t *u8, bool swept, float lead_rad)
{
    static const float TWO_PI = 6.28318530718f;
    const float ring_persist = RADAR_BLIP_PERSIST_DEG * (float)M_PI / 180.0f;
    const float dot_persist = RADAR_BLIP_DOT_PERSIST_DEG * (float)M_PI / 180.0f;

    for (int i = 0; i < g_blip_count; i++) {
        const RadarBlip *blip = &g_blips[i];
        int density = RADAR_BLIP_DOT_DENSITY;
        float behind = 0.0f;

        if (swept) {
            // How far the sweep has travelled since it crossed this contact.
            behind = lead_rad - blip->bearing_rad;
            while (behind < 0.0f) {
                behind += TWO_PI;
            }
            while (behind >= TWO_PI) {
                behind -= TWO_PI;
            }
            if (behind > dot_persist) {
                continue;  // faded out; the sweep has not come round again yet
            }
            density = (int)((float)RADAR_BLIP_DOT_DENSITY * (1.0f - behind / dot_persist));
            if (density <= 0) {
                continue;
            }
        }

        // Knock the field back only while the dot is solid enough to carry an
        // edge. Once it is thinning, the knockout would read as a hole punched
        // in the rain rather than as a contact fading out.
        if (density >= RADAR_BLIP_EDGE_DENSITY) {
            u8g2_SetDrawColor(u8, 0);
            u8g2_DrawDisc(u8, blip->x, blip->y, blip->radius + 1, U8G2_DRAW_ALL);
            u8g2_SetDrawColor(u8, 1);
        }
        draw_dithered_disc(u8, blip->x, blip->y, blip->radius, density);

        if (!swept || behind > ring_persist) {
            continue;
        }
        const float progress = behind / ring_persist;
        draw_dithered_circle(u8,
                             blip->x,
                             blip->y,
                             blip->radius + RADAR_BLIP_RING_GAP +
                                 (int)(progress * (float)RADAR_BLIP_RING_EXPAND),
                             (int)((float)RADAR_BLIP_RING_DENSITY * (1.0f - progress)));
    }
}

// A 1 px line over dithered stipple is unreadable, and heavy rain is exactly
// when the bearing matters. Knocking a 1 px halo out either side costs two
// extra line draws and makes the mark legible over any density underneath.
static void draw_haloed_line(u8g2_t *u8, int x0, int y0, int x1, int y1, int nx, int ny)
{
    u8g2_SetDrawColor(u8, 0);
    u8g2_DrawLine(u8, x0 + nx, y0 + ny, x1 + nx, y1 + ny);
    u8g2_DrawLine(u8, x0 - nx, y0 - ny, x1 - nx, y1 - ny);
    u8g2_SetDrawColor(u8, 1);
    u8g2_DrawLine(u8, x0, y0, x1, y1);
}

// Where the rain is coming from. A thin radial line plus a chevron at the rim
// whose apex points inward at home -- "this is heading your way".
//
// Deliberately not the filled wedge shape the sweep uses: that geometry is
// tolerable only because it moves. Static, it would permanently obliterate a
// sector, and it would obliterate the UPSTREAM sector -- the one carrying the
// rain you actually want to look at. This costs one pixel per radius step.
static void draw_bearing(u8g2_t *u8, float bearing_rad)
{
    const float ca = cosf(bearing_rad);
    const float sa = sinf(bearing_rad);
    const int nx = (int)lroundf(-sa);
    const int ny = (int)lroundf(ca);

    // Apex inboard, arms out to the rim: a V opening outward, so it reads as
    // an arrowhead aimed at the centre.
    const int apex_radius = SCOPE_RADIUS - RADAR_BEARING_APEX_PX;
    const int apex_x = SCOPE_CENTER + (int)lroundf(ca * (float)apex_radius);
    const int apex_y = SCOPE_CENTER + (int)lroundf(sa * (float)apex_radius);

    // A short tail inward from the apex, in the direction the arrow points,
    // rather than a line all the way back to home.
    const int tail_radius = apex_radius - RADAR_BEARING_TAIL_PX;
    draw_haloed_line(u8,
                     SCOPE_CENTER + (int)lroundf(ca * (float)tail_radius),
                     SCOPE_CENTER + (int)lroundf(sa * (float)tail_radius),
                     apex_x,
                     apex_y,
                     nx,
                     ny);
    for (int side = -1; side <= 1; side += 2) {
        const float arm = bearing_rad + (float)side * 0.20f;  // ~11 deg
        draw_haloed_line(u8,
                         apex_x,
                         apex_y,
                         SCOPE_CENTER + (int)lroundf(cosf(arm) * (float)SCOPE_RADIUS),
                         SCOPE_CENTER + (int)lroundf(sinf(arm) * (float)SCOPE_RADIUS),
                         nx,
                         ny);
    }
}

// Sweeps clockwise from 12 o'clock, once a minute. Unlike the 600 s version
// this replaced, the position carries no meaning: it is not a countdown, and
// it is emphatically not a clock. The sweep earns its place by being what
// pings the heavy-rain contacts -- see BEAM_PERIOD_SEC.
//
// Phase still comes from frame age rather than the wall clock, which at this
// period is a distinction without a difference: RainViewer stamps every frame
// on a 10-minute boundary, so frame_time % 60 is always 0 and the two agree
// exactly. Frame age is kept because it is the honest expression of what the
// beam is anchored to, and because it survives a period change back.
//
// It is read by motion now, not by position: 15.5 px/s at the rim is plainly
// visible, which is the whole point of the change. Live-vs-stale is still
// carried by the DATA stamp and the age footer, which say how stale rather
// than merely that it is -- at one revolution a minute the beam's position
// cannot say anything about a 25-minute staleness threshold, so there is no
// parked angle and no attempt at one.
static void draw_beam(u8g2_t *u8, double seconds_since_frame)
{
    const double phase = fmod(seconds_since_frame, (double)BEAM_PERIOD_SEC);
    const float lead_deg = -90.0f + (float)(phase / (double)BEAM_PERIOD_SEC * 360.0);
    for (int i = 0; i < BEAM_RAYS; i++) {
        // Iterate rays, not pixels. The per-pixel form needs an atan2 across
        // 90,000 pixels, which does not fit in a 70 ms frame on this chip;
        // this is ~100 sin/cos plus 14,800 integer steps.
        // Rounded up, so the ray behind the leading edge actually reaches the
        // specified maximum instead of truncating to one step below it.
        const int density =
            i == 0 ? 16
                   : (BEAM_MAX_DENSITY * (BEAM_RAYS - i) + BEAM_RAYS - 1) / BEAM_RAYS;
        if (density <= 0) {
            continue;
        }
        const float angle = (lead_deg - (float)i * BEAM_STEP_DEG) * (float)M_PI / 180.0f;
        const float ca = cosf(angle);
        const float sa = sinf(angle);
        for (int r = 1; r <= SCOPE_RADIUS; r++) {
            const int x = SCOPE_CENTER + (int)(ca * (float)r);
            const int y = SCOPE_CENTER + (int)(sa * (float)r);
            if (RADAR_BAYER[y & 3][x & 3] < density) {
                u8g2_DrawPixel(u8, x, y);
            }
        }
    }
}

static void draw_centered_in_panel(u8g2_t *u8, const char *text, int baseline)
{
    const int width = (int)u8g2_GetUTF8Width(u8, text);
    u8g2_DrawUTF8(u8, PANEL_X + (PANEL_W - width) / 2, baseline, text);
}

static void draw_probability(u8g2_t *u8, int baseline, int percent)
{
    if (percent < 0) {
        u8g2_SetFont(u8, u8g2_font_helvB18_tf);
        draw_centered_in_panel(u8, "--", baseline);
        return;
    }
    char digits[8];
    // Clamped rather than trusted: a percentage outside 0-100 is a malformed
    // response, and letting it through would only wreck the panel layout.
    snprintf(digits, sizeof(digits), "%d", percent > 100 ? 100 : percent);
    // logisoso28 is a digits-only face, so the per-cent sign is set separately
    // in a text font and the pair is centred as one unit.
    u8g2_SetFont(u8, u8g2_font_logisoso28_tn);
    const int digits_width = (int)u8g2_GetUTF8Width(u8, digits);
    u8g2_SetFont(u8, u8g2_font_helvB10_tf);
    const int sign_width = (int)u8g2_GetUTF8Width(u8, "%");
    const int x = PANEL_X + (PANEL_W - digits_width - 2 - sign_width) / 2;
    u8g2_SetFont(u8, u8g2_font_logisoso28_tn);
    u8g2_DrawUTF8(u8, x, baseline, digits);
    u8g2_SetFont(u8, u8g2_font_helvB10_tf);
    u8g2_DrawUTF8(u8, x + digits_width + 2, baseline, "%");
}

// The figure was computed for frame_time and frames are 10 minutes apart, so
// it has to decay with the clock. Left alone, "RAIN 25M" would still read 25M
// when it means 15M -- a headline number wrong by up to ten minutes, on the
// one value whose entire worth is its precision.
//
// The panel therefore changes once a minute, which at RENDER_PERIOD_STATIC_MS
// is free: the memcmp in render_task absorbs the other 59 renders.
static void draw_eta(u8g2_t *u8, const RadarState *s, bool live, time_t now, int baseline)
{
    char text[8];
    // Stale imagery has a stale vector. Left ungated, a frame stranded by a
    // dead router keeps decaying until `remaining` goes negative and the panel
    // reads a confident "NOW" forever, next to a footer quietly saying -64M.
    // The motion is only as current as the frame it was measured from.
    if (!live || !s->motion.valid) {
        // No confident vector. The wedge and the number vanish together --
        // never a bearing without an ETA, never an ETA without a bearing, on a
        // screen only ever read at a glance. "--" rather than a blank, which
        // under a RAIN eyebrow would read as a bug.
        snprintf(text, sizeof(text), "--");
    } else if (s->motion.eta_kind == RADAR_ETA_MINUTES) {
        // `live` already implies a set clock and a real frame_time.
        long long remaining = (long long)s->motion.eta_sec - ((long long)now - (long long)s->frame_time);
        if (remaining <= 30) {
            snprintf(text, sizeof(text), "NOW");
        } else if (remaining > RADAR_ETA_HORIZON_SEC) {
            // Something IS inbound, so this is not CLEAR -- but it is further
            // out than this instrument can honestly time, so it is not a
            // number either. The +3H figure below is the better answer at that
            // horizon, which is the panel's whole division of labour: radar
            // owns minutes, Open-Meteo owns hours.
            //
            // Tested on the decayed value rather than at scan time, so a
            // measurement that starts beyond the horizon resolves into a real
            // figure by itself once it is close enough to be credible.
            snprintf(text, sizeof(text), ">2H");
        } else {
            // The horizon bounds this to 120, but the clamp stays: it is what
            // makes the width provable to the compiler.
            const long long minutes = (remaining + 59) / 60;
            snprintf(text, sizeof(text), "%dM", minutes > 999 ? 999 : (int)minutes);
        }
    } else if (s->raining_home) {
        // Precedence, not a source change: Open-Meteo owns current conditions.
        // The radar cannot resolve 0.1 mm drizzle -- measured, 0/29 wet pixels
        // within 3 px of home while the forecast reported rain -- so it must
        // never be allowed to print CLEAR over a wet home reading.
        snprintf(text, sizeof(text), "NOW");
    } else {
        snprintf(text, sizeof(text), "CLEAR");
    }

    // CLEAR is five characters where the percentages are three, so the large
    // face is used only when it actually fits the 100 px column.
    u8g2_SetFont(u8, u8g2_font_helvB18_tf);
    if ((int)u8g2_GetUTF8Width(u8, text) > PANEL_W - 8) {
        u8g2_SetFont(u8, u8g2_font_helvB10_tf);
    }
    draw_centered_in_panel(u8, text, baseline);
}

static void draw_panel(u8g2_t *u8,
                       const RadarState *snapshot,
                       bool live,
                       int64_t age_sec,
                       time_t now)
{
    u8g2_DrawVLine(u8, PANEL_X, 0, SCOPE);

    u8g2_SetFont(u8, u8g2_font_6x12_tf);
    draw_centered_in_panel(u8, "+3H", 60);
    draw_probability(u8, snapshot->probability_3h < 0 ? 100 : 102, snapshot->probability_3h);

    u8g2_DrawHLine(u8, PANEL_X + 12, 140, PANEL_W - 24);

    // Radar owns minutes, Open-Meteo owns hours -- the ownership boundary the
    // two-source design settles on, made visible in the layout. +7H used to
    // sit here; seven hours out, as a probability, is a question a phone
    // answers better, and three numbers on a 100 px column is one too many to
    // take in on a glance.
    u8g2_SetFont(u8, u8g2_font_6x12_tf);
    draw_centered_in_panel(u8, "RAIN", 180);
    draw_eta(u8, snapshot, live, now, 218);

    // Capture time of the frame on screen -- not the time it was fetched.
    // RainViewer stamps every frame on a 10-minute boundary, and that stamp is
    // what the imagery actually shows. It comes from the API rather than the
    // board clock, so it should still read correctly if SNTP has not set the
    // time -- the case where the beam parks and the age line says nothing.
    // That path has not been exercised: a frame only exists after a fetch, and
    // by then SNTP has normally run. Labelled so it is not read as a clock.
    if (snapshot->scope != NULL) {
        char stamp[8] = "--:--";
        const time_t frame = snapshot->frame_time;
        struct tm local = {};
        if (frame > 0 && localtime_r(&frame, &local) != NULL) {
            snprintf(stamp, sizeof(stamp), "%02d:%02d", local.tm_hour, local.tm_min);
        }
        u8g2_SetFont(u8, u8g2_font_5x7_tf);
        draw_centered_in_panel(u8, "DATA", 258);
        u8g2_SetFont(u8, u8g2_font_6x12_tf);
        draw_centered_in_panel(u8, stamp, 272);
    }

    // Range normally, frame age when the imagery has gone stale. The stopped
    // beam is the other half of that signal.
    char footer[16];
    if (snapshot->scope == NULL) {
        snprintf(footer, sizeof(footer), "%s", snapshot->status == RADAR_ERROR ? "NO LINK" : "SYNC");
    } else if (live) {
        snprintf(footer, sizeof(footer), "%d KM", g_range_km);
    } else {
        // Once the frame is a day old the exact figure stops meaning
        // anything; the label just has to keep saying "not live".
        const long long minutes = (long long)(age_sec / 60);
        snprintf(footer, sizeof(footer), "-%dM", minutes > 999 ? 999 : (int)minutes);
    }
    u8g2_SetFont(u8, u8g2_font_5x7_tf);
    draw_centered_in_panel(u8, footer, 290);
}

// ---------------------------------------------------------------- public

void radar_screen_init(void)
{
    memset(&g_radar, 0, sizeof(g_radar));
    g_radar.mutex = xSemaphoreCreateMutex();
    g_radar.probability_3h = -1;

    // Web Mercator ground resolution at this latitude and zoom. The trailing
    // divisor is the tile size: a 512 px tile covers the same ground as a
    // 256 px one at the same zoom, so its pixels are half the size.
    const double metres_per_px = 156543.03392 * cos(RADAR_HOME_LAT * M_PI / 180.0) /
                                 (double)(1 << RADAR_ZOOM) / ((double)RADAR_TILE_PX / 256.0);
    g_km_per_px = (float)(metres_per_px / 1000.0);
    g_range_km = (int)lround((double)SCOPE_RADIUS * metres_per_px / 1000.0);

    double home_x = 0.0;
    double home_y = 0.0;
    radar_project(RADAR_HOME_LAT, RADAR_HOME_LON, &home_x, &home_y);
    const int pixel_x = (int)lround(home_x);
    const int pixel_y = (int)lround(home_y);
    g_tile_x = pixel_x / RADAR_TILE_PX;
    g_tile_y = pixel_y / RADAR_TILE_PX;
    // Crop so home lands exactly on the scope centre. This puts the window at
    // x 106..406 rather than the 105..405 quoted in radar-screen-design.md --
    // deriving it is what keeps the centre pixel-exact, and the 1 px shift is
    // the rounding the table did by hand.
    g_crop_x = pixel_x - g_tile_x * RADAR_TILE_PX - SCOPE_CENTER;
    g_crop_y = pixel_y - g_tile_y * RADAR_TILE_PX - SCOPE_CENTER;
    // Landmarks project through the same maths as home, against the same crop
    // origin, so they follow RADAR_ZOOM without the table being regenerated.
    for (int i = 0; i < RADAR_LANDMARK_COUNT; i++) {
        double mark_x = 0.0;
        double mark_y = 0.0;
        radar_project(RADAR_LANDMARKS[i].lat, RADAR_LANDMARKS[i].lon, &mark_x, &mark_y);
        g_landmark_px[i].x = (int16_t)lround(mark_x - (double)(g_tile_x * RADAR_TILE_PX + g_crop_x));
        g_landmark_px[i].y = (int16_t)lround(mark_y - (double)(g_tile_y * RADAR_TILE_PX + g_crop_y));
        ESP_LOGI(TAG,
                 "landmark %-13s -> (%d, %d)",
                 RADAR_LANDMARKS[i].label,
                 g_landmark_px[i].x,
                 g_landmark_px[i].y);
    }

    ESP_LOGI(TAG, "scale %.3f km/px, range %d km", (double)g_km_per_px, g_range_km);
    ESP_LOGI(TAG,
             "scope tile /%d/%d/%d/%d/, crop x %d..%d y %d..%d",
             RADAR_TILE_PX,
             RADAR_ZOOM,
             g_tile_x,
             g_tile_y,
             g_crop_x,
             g_crop_x + SCOPE,
             g_crop_y,
             g_crop_y + SCOPE);
}

void radar_screen_start(void)
{
    if (g_radar_task != NULL) {
        return;
    }
    // Matches the daily-images task: pngle and cJSON are the same shape of
    // load, and the high-water mark is logged after every refresh.
    const BaseType_t ok = xTaskCreate(radar_task, "radar", 12288, NULL, 4, &g_radar_task);
    if (ok != pdPASS) {
        g_radar_task = NULL;
        set_status(RADAR_ERROR, "NO MEMORY");
    }
}

void radar_refresh(void)
{
    if (g_radar_task == NULL) {
        return;
    }
    // Explicit user request: bypass the interval, but still restart it so the
    // automatic poll does not fire immediately afterwards.
    xSemaphoreTake(g_radar.mutex, portMAX_DELAY);
    g_last_attempt_us = esp_timer_get_time();
    xSemaphoreGive(g_radar.mutex);
    xTaskNotifyGive(g_radar_task);
}

void radar_render_current(u8g2_t *u8)
{
    begin_scope_theme(u8);

    struct timeval now_tv = {};
    gettimeofday(&now_tv, NULL);
    const bool clock_valid = now_tv.tv_sec > CLOCK_VALID_EPOCH;

    xSemaphoreTake(g_radar.mutex, portMAX_DELAY);
    const RadarStatus status = g_radar.status;
    const bool has_scope = g_radar.scope != NULL;
    // An unset clock cannot tell live imagery from stale, and cannot phase the
    // sweep either, since both are measured from frame age.
    int64_t age_sec = 0;
    bool live = false;
    if (has_scope && clock_valid && g_radar.frame_time > 0) {
        age_sec = (int64_t)now_tv.tv_sec - (int64_t)g_radar.frame_time;
        if (age_sec < 0) {
            age_sec = 0;
        }
        live = age_sec < STALE_AGE_SEC;
    }

    if (has_scope) {
        u8g2_DrawXBM(u8, 0, 0, SCOPE, SCOPE, g_radar.scope);
    }
    // Drawn before the furniture so the rings and ticks stay readable through
    // it, matching the proof render. The beam keeps winding whether the frame
    // is live or stale -- being past 2.5 revolutions IS the stale condition,
    // so there is no separate parked state to hold.
    const bool swept = BEAM_ENABLED && has_scope && clock_valid && g_radar.frame_time > 0;
    float blip_lead_rad = 0.0f;
    if (swept) {
        const double seconds = (double)age_sec + (double)now_tv.tv_usec / 1000000.0;
        draw_beam(u8, seconds);
        const double phase = fmod(seconds, (double)BEAM_PERIOD_SEC);
        blip_lead_rad =
            (float)((-90.0 + phase / (double)BEAM_PERIOD_SEC * 360.0) * M_PI / 180.0);
    }
    draw_furniture(u8);
    draw_landmarks(u8);
    // After the furniture, not before it: a contact near home was being
    // crossed out by the full-width crosshair and the range rings. A contact
    // outranks a ring -- the landmarks already take the same liberty -- and
    // the home marker still draws last, so home is never hidden by one.
    if (has_scope) {
        draw_blips(u8, swept, blip_lead_rad);
    }
    // After the furniture: the bearing is the one mark on the scope that must
    // not be read as a range ring.
    // Withheld on stale imagery for the same reason the ETA is: the wedge and
    // the number vanish together, always.
    if (has_scope && live && g_radar.motion.valid) {
        draw_bearing(u8, g_radar.motion.bearing_rad);
    }
    if (has_scope && blips_near_home()) {
        draw_close_alert(u8);
    }
    draw_home_marker(u8, g_radar.raining_home);

    if (!has_scope) {
        u8g2_SetFont(u8, u8g2_font_helvB14_tf);
        const char *message = status == RADAR_ERROR ? "RADAR UNAVAILABLE" : "ACQUIRING RADAR";
        const int width = (int)u8g2_GetUTF8Width(u8, message);
        u8g2_DrawUTF8(u8, (SCOPE - width) / 2, SCOPE_CENTER + 60, message);
        if (status == RADAR_ERROR && g_radar.error[0] != '\0') {
            u8g2_SetFont(u8, u8g2_font_5x7_tf);
            const int error_width = (int)u8g2_GetUTF8Width(u8, g_radar.error);
            u8g2_DrawUTF8(u8, (SCOPE - error_width) / 2, SCOPE_CENTER + 76, g_radar.error);
        }
    }

    RadarState snapshot = g_radar;
    xSemaphoreGive(g_radar.mutex);

    draw_panel(u8, &snapshot, live, age_sec, (time_t)now_tv.tv_sec);
    request_refresh_if_due(status);
}
