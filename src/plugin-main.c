#include <obs-module.h>
#include <graphics/image-file.h>
#include <util/platform.h>

#include "face-detect.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("hal-visor", "en-US")

/* obs_module_load 時 (モジュール文脈が確実な場所) でモデルパスを解決しておく */
static char *g_model_path = NULL;

struct hal_visor_data {
obs_source_t *source;
gs_image_file_t image;
char *image_path;
int pos_x;
int pos_y;
int scale;

/* ステップ2: 顔検出の配管確認 */
struct hal_face_detector *detector;
bool detector_tried;
gs_texrender_t *capture;
gs_stagesurface_t *stage;
uint32_t stage_w;
uint32_t stage_h;
bool stage_pending;
uint32_t frame_count;
struct hal_face_result face;
uint64_t last_log_ns;
};

static const char *hal_visor_get_name(void *unused)
{
UNUSED_PARAMETER(unused);
return "HaL Visor";
}

static void hal_visor_update(void *data, obs_data_t *settings)
{
struct hal_visor_data *filter = data;
const char *path = obs_data_get_string(settings, "image_path");

filter->pos_x = (int)obs_data_get_int(settings, "pos_x");
filter->pos_y = (int)obs_data_get_int(settings, "pos_y");
filter->scale = (int)obs_data_get_int(settings, "scale");

// 画像パスの更新
if (!filter->image_path || strcmp(filter->image_path, path)!= 0) {
bfree(filter->image_path);
filter->image_path = bstrdup(path);

obs_enter_graphics();
gs_image_file_free(&filter->image);
if (path && *path) {
gs_image_file_init(&filter->image, path);
}
obs_leave_graphics();
}
}

static void free_capture(struct hal_visor_data *filter)
{
if (filter->stage) {
gs_stagesurface_destroy(filter->stage);
filter->stage = NULL;
}
if (filter->capture) {
gs_texrender_destroy(filter->capture);
filter->capture = NULL;
}
filter->stage_pending = false;
filter->stage_w = 0;
filter->stage_h = 0;
}

static void hal_visor_destroy(void *data)
{
struct hal_visor_data *filter = data;

obs_enter_graphics();
gs_image_file_free(&filter->image);
free_capture(filter);
obs_leave_graphics();

hal_face_detector_destroy(filter->detector);
bfree(filter->image_path);
bfree(filter);
}

static void *hal_visor_create(obs_data_t *settings, obs_source_t *source)
{
struct hal_visor_data *filter = bzalloc(sizeof(struct hal_visor_data));
filter->source = source;

hal_visor_update(filter, settings);
return filter;
}

/* 検出器を遅延初期化する。失敗してもフィルターは従来動作を続ける */
static void ensure_detector(struct hal_visor_data *filter)
{
if (filter->detector || filter->detector_tried)
return;
filter->detector_tried = true;

if (!g_model_path) {
blog(LOG_ERROR,
"[hal-visor] モデルファイルのパスを解決できませんでした");
return;
}
filter->detector = hal_face_detector_create(g_model_path);
if (filter->detector)
blog(LOG_INFO, "[hal-visor] 顔検出の準備ができました: %s",
g_model_path);
}

/* キャプチャ用 texrender / stage surface をソースサイズに合わせて用意する */
static void ensure_capture(struct hal_visor_data *filter, uint32_t cx,
uint32_t cy)
{
if (filter->capture && filter->stage && filter->stage_w == cx &&
filter->stage_h == cy)
return;
free_capture(filter);
filter->capture = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
filter->stage = gs_stagesurface_create(cx, cy, GS_BGRA);
if (filter->capture && filter->stage) {
filter->stage_w = cx;
filter->stage_h = cy;
} else {
free_capture(filter);
}
}

/* バイザー画像を中心 (center_x, center_y) に倍率 s で描く */
static void draw_visor_image(struct hal_visor_data *filter, float center_x,
float center_y, float s)
{
gs_effect_t *draw_effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
gs_eparam_t *image_param =
gs_effect_get_param_by_name(draw_effect, "image");
gs_effect_set_texture(image_param, filter->image.texture);

gs_blend_state_push();
gs_enable_color(true, true, true, true);
gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

gs_matrix_push();

// 位置移動
gs_matrix_translate3f(center_x, center_y, 0.0f);

// 拡大縮小
gs_matrix_scale3f(s, s, 1.0f);

// 画像の中心を基準にする
float half_w = (float)filter->image.cx * 0.5f;
float half_h = (float)filter->image.cy * 0.5f;
gs_matrix_translate3f(-half_w, -half_h, 0.0f);

// 安全なエフェクトループで描画
while (gs_effect_loop(draw_effect, "Draw")) {
gs_draw_sprite(filter->image.texture, 0, filter->image.cx,
filter->image.cy);
}

gs_matrix_pop();
gs_blend_state_pop();
}

static void hal_visor_video_render(void *data, gs_effect_t *effect)
{
struct hal_visor_data *filter = data;

// OBSからeffectが渡ってこない場合の安全策
if (!effect)
effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);

obs_source_t *target = obs_filter_get_target(filter->source);
uint32_t cx = target? obs_source_get_base_width(target): 0;
uint32_t cy = target? obs_source_get_base_height(target): 0;

bool captured = false;
if (cx && cy) {
ensure_capture(filter, cx, cy);
if (filter->capture) {
gs_texrender_reset(filter->capture);
if (gs_texrender_begin(filter->capture, cx, cy)) {
struct vec4 clear_color;
vec4_set(&clear_color, 0.0f, 0.0f, 0.0f, 0.0f);
gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f,
0);
// 親ソースを自前 texrender へ描く
if (obs_source_process_filter_begin(
filter->source, GS_RGBA,
OBS_NO_DIRECT_RENDERING)) {
obs_source_process_filter_end(
filter->source,
obs_get_base_effect(
OBS_EFFECT_DEFAULT),
(int)cx, (int)cy);
captured = true;
}
gs_texrender_end(filter->capture);
}
}
}

if (captured) {
// キャプチャした映像をフィルターの出力として描く
gs_effect_t *draw_effect =
obs_get_base_effect(OBS_EFFECT_DEFAULT);
gs_eparam_t *image_param =
gs_effect_get_param_by_name(draw_effect, "image");
gs_texture_t *tex = gs_texrender_get_texture(filter->capture);
gs_effect_set_texture(image_param, tex);

gs_blend_state_push();
gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);
while (gs_effect_loop(draw_effect, "Draw")) {
gs_draw_sprite(tex, 0, cx, cy);
}
gs_blend_state_pop();
} else {
// 従来どおり: 親ソースをそのまま出力する
if (!obs_source_process_filter_begin(filter->source, GS_RGBA,
OBS_NO_DIRECT_RENDERING))
return;
obs_source_process_filter_end(filter->source, effect, 0, 0);
}

// ---- ステップ2: 顔検出の配管 ----
filter->frame_count++;

// 前回コピーしたフレームをマップして推論する
if (filter->stage_pending && filter->stage) {
uint8_t *pixels = NULL;
uint32_t linesize = 0;
if (gs_stagesurface_map(filter->stage, &pixels, &linesize) &&
pixels) {
ensure_detector(filter);
if (filter->detector) {
struct hal_face_result res;
if (hal_face_detector_run(
filter->detector, pixels, (int)cx,
(int)cy, (int)linesize, &res)) {
filter->face = res;
uint64_t now = os_gettime_ns();
if (now - filter->last_log_ns >=
1000000000ULL) {
filter->last_log_ns = now;
blog(LOG_INFO,
"[hal-visor] 顔検出: score=%.2f 目の中心=(%.0f, %.0f) 目の間隔=%.0f",
res.score,
res.eye_mid_x,
res.eye_mid_y,
res.eye_dist);
}
} else {
filter->face.found = false;
}
}
gs_stagesurface_unmap(filter->stage);
}
filter->stage_pending = false;
}

// 3回に1回、キャプチャを stage surface へコピーする (次回マップする)
if (captured && filter->stage && (filter->frame_count % 3) == 0) {
gs_copy_texture((gs_texture_t *)filter->stage,
gs_texrender_get_texture(filter->capture));
filter->stage_pending = true;
}

// バイザー画像を描画 (従来のスライダー位置)
if (filter->image.loaded && filter->image.texture) {
float s = (float)filter->scale / 100.0f;
draw_visor_image(filter, (float)filter->pos_x,
(float)filter->pos_y, s);

// 配管確認用マーカー: 検出した目の中心にもう1枚描く
// (幅が目の間隔の約3倍になる倍率)
if (filter->face.found && filter->face.eye_dist > 0.0f) {
float ms = (3.0f * filter->face.eye_dist) /
(float)filter->image.cx;
draw_visor_image(filter, filter->face.eye_mid_x,
filter->face.eye_mid_y, ms);
}
}
}

static obs_properties_t *hal_visor_properties(void *data)
{
UNUSED_PARAMETER(data);
obs_properties_t *props = obs_properties_create();

obs_properties_add_path(props, "image_path", "バイザー画像",
OBS_PATH_FILE,
"Image Files (*.png *.jpg *.jpeg *.bmp *.webp);;All Files (*.*)",
NULL);

obs_properties_add_int_slider(props, "pos_x", "位置 X", 0, 1920, 1);
obs_properties_add_int_slider(props, "pos_y", "位置 Y", 0, 1080, 1);
obs_properties_add_int_slider(props, "scale", "サイズ (%)", 10, 500, 1);

return props;
}

static void hal_visor_defaults(obs_data_t *settings)
{
obs_data_set_default_int(settings, "pos_x", 960);
obs_data_set_default_int(settings, "pos_y", 540);
obs_data_set_default_int(settings, "scale", 100);
}

struct obs_source_info hal_visor_filter_info = {
.id = "hal_visor_filter",
.type = OBS_SOURCE_TYPE_FILTER,
.output_flags = OBS_SOURCE_VIDEO,
.get_name = hal_visor_get_name,
.create = hal_visor_create,
.destroy = hal_visor_destroy,
.update = hal_visor_update,
.get_defaults = hal_visor_defaults,
.get_properties = hal_visor_properties,
.video_render = hal_visor_video_render,
};

bool obs_module_load(void)
{
obs_register_source(&hal_visor_filter_info);
g_model_path = obs_module_file("face_detection_yunet_2023mar.onnx");
return true;
}

void obs_module_unload(void)
{
bfree(g_model_path);
g_model_path = NULL;
}
