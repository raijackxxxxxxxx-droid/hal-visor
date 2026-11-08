#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <obs-module.h>
#include <graphics/image-file.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>
#include <util/platform.h>

#include <math.h>
#include <string.h>

#include "face-detect.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("hal-visor", "en-US")

/* obs_module_load 時 (モジュール文脈が確実な場所) でモデルパスを解決しておく */
static char *g_model_path = NULL;

/* 検出結果が「新鮮」と言える上限 (ns) と、平滑化の係数 */
#define HAL_FRESH_NS 500000000ULL
#define HAL_SMOOTH 0.35f

/* 角度推定のゲインと上限 (rad)。最終調整で詰める前提の初期値 */
#define HAL_YAW_GAIN 2.2f
#define HAL_PITCH_GAIN 2.2f
#define HAL_YAW_MAX 0.9f
#define HAL_PITCH_MAX 0.6f

static float hv_clampf(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

/* 描画スレッド → ワーカー へのフレーム受け渡しスロット (lock で保護) */
struct hv_frame_slot {
	float *buf;
	int in_w;
	int in_h;
	int src_w;
	int src_h;
	bool pending;
	uint64_t seq;
};

/* ワーカー → 描画スレッド への結果スロット (lock で保護) */
struct hv_result_slot {
	struct hal_face_result res;
	uint64_t seq;
	uint64_t time_ns;
	bool valid;
};

struct hal_visor_data {
	obs_source_t *source;
	gs_image_file_t image;
	char *image_path;
	int pos_x;
	int pos_y;
	int scale;

	/* ステップ3: 追従の設定 */
	bool tracking;
	double width_factor;
	/* ステップ4: 変形の強さ (%) */
	int deform;
	/* 正面登録 (フィルター設定に永続化される) */
	bool calib_valid;
	double calib_eye_dist;
	double calib_nose_nx;
	double calib_nose_ny;

	struct hal_face_detector *detector;
	bool detector_tried;
	/* ステップ4: 変形描画用の頂点バッファ
	 * (GS_DUP_BUFFER なので vbd の所有権はフィルター側に残る) */
	gs_vertbuffer_t *quad_vb;
	struct gs_vb_data *quad_vbd;
	gs_texrender_t *capture;
	gs_stagesurf_t *stage;
	uint32_t stage_w;
	uint32_t stage_h;
	bool stage_pending;
	uint32_t frame_count;
	uint64_t last_log_ns;

	/* ワーカースレッドと受け渡しスロット */
	struct hv_frame_slot frame;
	struct hv_result_slot result;
	float *worker_buf;
	/* frame.buf と worker_buf の共通容量 (float 数)。
	 * モデルの実入力サイズに合わせて ensure_detector で拡張される */
	size_t frame_buf_cap;
	bool worker_started;
	bool worker_stop;
#ifdef _WIN32
	CRITICAL_SECTION lock;
	HANDLE frame_event;
	HANDLE worker_thread;
#endif

	/* 平滑化済みの追従状態 (描画スレッド専用) */
	bool have_track;
	bool track_lost;
	float sm_x;
	float sm_y;
	float sm_dist;
	float sm_cos;
	float sm_sin;
	float sm_nose_x;
	float sm_nose_y;
	uint64_t applied_seq;
};

static const char *hal_visor_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "HaL Visor";
}

static void hv_lock(struct hal_visor_data *filter)
{
#ifdef _WIN32
	EnterCriticalSection(&filter->lock);
#else
	UNUSED_PARAMETER(filter);
#endif
}

static void hv_unlock(struct hal_visor_data *filter)
{
#ifdef _WIN32
	LeaveCriticalSection(&filter->lock);
#else
	UNUSED_PARAMETER(filter);
#endif
}

/* ---- ワーカースレッド: スロットのフレームを取り出して推論し、結果を返す ---- */
#ifdef _WIN32
static DWORD WINAPI hal_worker_proc(LPVOID param)
{
	struct hal_visor_data *filter = param;

	for (;;) {
		WaitForSingleObject(filter->frame_event, INFINITE);

		struct hal_face_detector *det = NULL;
		int in_w = 0, in_h = 0, src_w = 0, src_h = 0;
		bool stop = false;

		EnterCriticalSection(&filter->lock);
		if (filter->worker_stop) {
			stop = true;
		} else if (filter->frame.pending) {
			size_t n = (size_t)filter->frame.in_w *
				   filter->frame.in_h * 3;
			if (n > filter->frame_buf_cap) {
				/* prepare 側のガードで通常は起こらない。
				 * 念のための最終防御: 溢れさせるくらいなら落とす */
				blog(LOG_ERROR,
				     "[hal-visor] フレーム (%zu floats) がバッファ容量 (%zu floats) を超えています。破棄します",
				     n, filter->frame_buf_cap);
				filter->frame.pending = false;
			} else {
				memcpy(filter->worker_buf, filter->frame.buf,
				       n * sizeof(float));
				in_w = filter->frame.in_w;
				in_h = filter->frame.in_h;
				src_w = filter->frame.src_w;
				src_h = filter->frame.src_h;
				det = filter->detector;
				filter->frame.pending = false;
			}
		}
		LeaveCriticalSection(&filter->lock);

		if (stop)
			break;
		if (!det || in_w <= 0 || in_h <= 0)
			continue;

		struct hal_face_result res;
		memset(&res, 0, sizeof(res));
		hal_face_detector_infer(det, filter->worker_buf, in_w, in_h,
					src_w, src_h, &res);

		EnterCriticalSection(&filter->lock);
		filter->result.res = res;
		filter->result.seq++;
		filter->result.time_ns = os_gettime_ns();
		filter->result.valid = true;
		LeaveCriticalSection(&filter->lock);
	}
	return 0;
}
#endif

static void worker_start(struct hal_visor_data *filter)
{
	filter->worker_started = true;
#ifdef _WIN32
	InitializeCriticalSection(&filter->lock);
	filter->frame_event = CreateEventW(NULL, FALSE, FALSE, NULL);
	filter->worker_thread = filter->frame_event
					? CreateThread(NULL, 0, hal_worker_proc,
						       filter, 0, NULL)
					: NULL;
	if (!filter->worker_thread) {
		blog(LOG_ERROR,
		     "[hal-visor] ワーカースレッドを作成できませんでした");
		if (filter->frame_event) {
			CloseHandle(filter->frame_event);
			filter->frame_event = NULL;
		}
	}
#endif
}

static void worker_stop(struct hal_visor_data *filter)
{
	if (!filter->worker_started)
		return;
	filter->worker_started = false;
#ifdef _WIN32
	if (filter->worker_thread) {
		EnterCriticalSection(&filter->lock);
		filter->worker_stop = true;
		LeaveCriticalSection(&filter->lock);
		SetEvent(filter->frame_event);
		WaitForSingleObject(filter->worker_thread, INFINITE);
		CloseHandle(filter->worker_thread);
		filter->worker_thread = NULL;
	}
	if (filter->frame_event) {
		CloseHandle(filter->frame_event);
		filter->frame_event = NULL;
	}
	DeleteCriticalSection(&filter->lock);
#endif
}

/*
 * マップ済みフレームをワーカーへ渡す (描画スレッド)。
 * 縮小 (prepare) はここで行い、ワーカーが前フレームを未取得なら落とす。
 */
static void publish_frame(struct hal_visor_data *filter,
			  const uint8_t *pixels, int w, int h, int linesize)
{
	if (!filter->detector)
		return;
#ifdef _WIN32
	if (!filter->worker_thread || !filter->frame_event)
		return;
	EnterCriticalSection(&filter->lock);
	if (!filter->worker_stop && !filter->frame.pending) {
		int in_w = 0, in_h = 0;
		if (hal_face_detector_prepare(filter->detector, pixels, w, h,
					      linesize, filter->frame.buf,
					      filter->frame_buf_cap, &in_w,
					      &in_h)) {
			filter->frame.in_w = in_w;
			filter->frame.in_h = in_h;
			filter->frame.src_w = w;
			filter->frame.src_h = h;
			filter->frame.pending = true;
			filter->frame.seq++;
			SetEvent(filter->frame_event);
		}
	}
	LeaveCriticalSection(&filter->lock);
#else
	int in_w = 0, in_h = 0;
	if (hal_face_detector_prepare(filter->detector, pixels, w, h,
				      linesize, filter->frame.buf,
				      filter->frame_buf_cap, &in_w, &in_h)) {
		struct hal_face_result res;
		memset(&res, 0, sizeof(res));
		hal_face_detector_infer(filter->detector, filter->frame.buf,
					in_w, in_h, w, h, &res);
		filter->result.res = res;
		filter->result.seq++;
		filter->result.time_ns = os_gettime_ns();
		filter->result.valid = true;
	}
#endif
}

/*
 * 平滑化済みの状態から、正面登録を基準とした顔の向き (yaw/pitch, rad) を
 * 推定する。鼻のずれは目の線の角度ぶん逆回転させてから正規化し、
 * 顔自身の座標系で測る (ロールと切り離すため)。
 * strength は倍率 (1.0 = 推定そのまま)。登録が無ければ両方 0。
 * 座標系: x は右目→左目の向き、y は画像の下向き。
 *   yaw > 0 = 鼻が左目側へ寄った (= 被写体が自分の左を向いた) 向き。
 *   pitch > 0 = 鼻が目の線より下へ寄った (= うつむいた) 向き。
 */
static void estimate_angles(struct hal_visor_data *filter, float strength,
			    float *out_yaw, float *out_pitch)
{
	*out_yaw = 0.0f;
	*out_pitch = 0.0f;
	if (!filter->calib_valid || filter->sm_dist <= 0.0f)
		return;

	float vx = filter->sm_nose_x - filter->sm_x;
	float vy = filter->sm_nose_y - filter->sm_y;
	float rx = filter->sm_cos * vx + filter->sm_sin * vy;
	float ry = -filter->sm_sin * vx + filter->sm_cos * vy;
	float nx = rx / filter->sm_dist;
	float ny = ry / filter->sm_dist;

	*out_yaw = hv_clampf((nx - (float)filter->calib_nose_nx) *
				     HAL_YAW_GAIN,
			     -HAL_YAW_MAX, HAL_YAW_MAX) *
		   strength;
	*out_pitch = hv_clampf((ny - (float)filter->calib_nose_ny) *
				       HAL_PITCH_GAIN,
			       -HAL_PITCH_MAX, HAL_PITCH_MAX) *
		     strength;
}

/*
 * 最新の検出結果を取り込んで平滑化する (描画スレッド)。
 * 新しい結果が来た時だけ EMA を1回適用する。見失ったら変換は保持し、
 * 次に検出した時はスナップ (補間しない) する。
 */
static void consume_results(struct hal_visor_data *filter)
{
	struct hal_face_result res;
	memset(&res, 0, sizeof(res));
	uint64_t seq = 0, rtime = 0;
	bool valid = false;

	hv_lock(filter);
	if (filter->result.valid) {
		res = filter->result.res;
		seq = filter->result.seq;
		rtime = filter->result.time_ns;
		valid = true;
	}
	hv_unlock(filter);

	if (!valid || seq == filter->applied_seq)
		return;
	filter->applied_seq = seq;

	uint64_t now = os_gettime_ns();
	bool fresh = (now - rtime) <= HAL_FRESH_NS;
	if (!fresh || !res.found || res.eye_dist <= 0.0f) {
		filter->track_lost = true;
		return;
	}

	if (!filter->have_track || filter->track_lost) {
		filter->sm_x = res.eye_mid_x;
		filter->sm_y = res.eye_mid_y;
		filter->sm_dist = res.eye_dist;
		filter->sm_cos = cosf(res.eye_angle);
		filter->sm_sin = sinf(res.eye_angle);
		filter->sm_nose_x = res.nose_x;
		filter->sm_nose_y = res.nose_y;
	} else {
		const float a = HAL_SMOOTH;
		filter->sm_x += (res.eye_mid_x - filter->sm_x) * a;
		filter->sm_y += (res.eye_mid_y - filter->sm_y) * a;
		filter->sm_dist += (res.eye_dist - filter->sm_dist) * a;
		filter->sm_cos += (cosf(res.eye_angle) - filter->sm_cos) * a;
		filter->sm_sin += (sinf(res.eye_angle) - filter->sm_sin) * a;
		filter->sm_nose_x += (res.nose_x - filter->sm_nose_x) * a;
		filter->sm_nose_y += (res.nose_y - filter->sm_nose_y) * a;
	}
	filter->have_track = true;
	filter->track_lost = false;

	if (now - filter->last_log_ns >= 1000000000ULL) {
		filter->last_log_ns = now;
		blog(LOG_INFO,
		     "[hal-visor] 顔検出: score=%.2f 目の中心=(%.0f, %.0f) 目の間隔=%.0f",
		     res.score, res.eye_mid_x, res.eye_mid_y, res.eye_dist);
		if (filter->calib_valid) {
			double cnx = (double)(res.nose_x - res.eye_mid_x) /
				     (double)res.eye_dist;
			double cny = (double)(res.nose_y - res.eye_mid_y) /
				     (double)res.eye_dist;
			blog(LOG_INFO,
			     "[hal-visor] 鼻オフセット=(%.2f, %.2f) 正面登録との差=(%.2f, %.2f)",
			     cnx, cny, cnx - filter->calib_nose_nx,
			     cny - filter->calib_nose_ny);
			float eyaw = 0.0f, epitch = 0.0f;
			estimate_angles(filter, 1.0f, &eyaw, &epitch);
			blog(LOG_INFO,
			     "[hal-visor] 角度推定: yaw=%.0f° pitch=%.0f°",
			     (double)(eyaw * 57.29578f),
			     (double)(epitch * 57.29578f));
		}
	}
}

static void hal_visor_update(void *data, obs_data_t *settings)
{
	struct hal_visor_data *filter = data;
	const char *path = obs_data_get_string(settings, "image_path");

	filter->pos_x = (int)obs_data_get_int(settings, "pos_x");
	filter->pos_y = (int)obs_data_get_int(settings, "pos_y");
	filter->scale = (int)obs_data_get_int(settings, "scale");
	filter->tracking = obs_data_get_bool(settings, "tracking");
	filter->width_factor = obs_data_get_double(settings, "width_factor");
	filter->deform = (int)obs_data_get_int(settings, "deform");
	filter->calib_valid = obs_data_get_bool(settings, "calib_valid");
	filter->calib_eye_dist = obs_data_get_double(settings, "calib_eye_dist");
	filter->calib_nose_nx = obs_data_get_double(settings, "calib_nose_nx");
	filter->calib_nose_ny = obs_data_get_double(settings, "calib_nose_ny");

	// 画像パスの更新
	if (!filter->image_path || strcmp(filter->image_path, path) != 0) {
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

	/* 先にワーカーを止めてから検出器や画像を解放する */
	worker_stop(filter);

	obs_enter_graphics();
	gs_image_file_free(&filter->image);
	free_capture(filter);
	if (filter->quad_vb) {
		gs_vertexbuffer_destroy(filter->quad_vb);
		filter->quad_vb = NULL;
	}
	obs_leave_graphics();

	if (filter->quad_vbd) {
		gs_vbdata_destroy(filter->quad_vbd);
		filter->quad_vbd = NULL;
	}

	hal_face_detector_destroy(filter->detector);
	bfree(filter->frame.buf);
	bfree(filter->worker_buf);
	bfree(filter->image_path);
	bfree(filter);
}

static void *hal_visor_create(obs_data_t *settings, obs_source_t *source)
{
	struct hal_visor_data *filter = bzalloc(sizeof(struct hal_visor_data));
	filter->source = source;
	filter->frame_buf_cap = HAL_FACE_INPUT_MAX_FLOATS;
	filter->frame.buf =
		bmalloc(sizeof(float) * filter->frame_buf_cap);
	filter->worker_buf =
		bmalloc(sizeof(float) * filter->frame_buf_cap);

	worker_start(filter);
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
	if (filter->detector) {
		blog(LOG_INFO, "[hal-visor] 顔検出の準備ができました: %s",
		     g_model_path);
		/* フレームバッファをモデルの実入力サイズに合わせる。
		 * 固定サイズ入力 (YuNet は 640x640) は初期確保より大きい。
		 * ここは生成直後でまだフレームを発行していないため、
		 * ワーカーはどちらのバッファにも触れていない */
		size_t need = hal_face_detector_input_capacity(filter->detector);
		if (need > filter->frame_buf_cap) {
			filter->frame.buf = brealloc(
				filter->frame.buf, sizeof(float) * need);
			filter->worker_buf = brealloc(
				filter->worker_buf, sizeof(float) * need);
			filter->frame_buf_cap = need;
			blog(LOG_INFO,
			     "[hal-visor] 入力バッファを %zu floats に拡張しました",
			     need);
		}
	}
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

/* バイザー画像を中心 (center_x, center_y) に倍率 s・回転 angle_rad で描く */
static void draw_visor_image(struct hal_visor_data *filter, float center_x,
			     float center_y, float s, float angle_rad)
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

	// 中心まわりの回転 (目の線の傾き)
	if (angle_rad != 0.0f)
		gs_matrix_rotaa4f(0.0f, 0.0f, 1.0f, angle_rad);

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

/*
 * 変形描画用の頂点バッファを用意する (描画文脈で呼ぶ)。
 * 4頂点 (位置 + UV) を GS_DUP_BUFFER | GS_DYNAMIC で作り、毎フレーム
 * gs_vertexbuffer_get_data で位置だけ書き換える (libobs の subregion と同方式)。
 */
static bool ensure_quad_vb(struct hal_visor_data *filter)
{
	if (filter->quad_vb)
		return true;

	struct gs_vb_data *vbd = gs_vbdata_create();
	vbd->num = 4;
	vbd->points = bzalloc(sizeof(struct vec3) * 4);
	vbd->num_tex = 1;
	vbd->tvarray = bzalloc(sizeof(struct gs_tvertarray));
	vbd->tvarray[0].width = 2;
	vbd->tvarray[0].array = bzalloc(sizeof(struct vec2) * 4);

	/* UV は gs_draw_sprite (反転なし) と同じ割り当て: TL, TR, BL, BR */
	struct vec2 *uvs = vbd->tvarray[0].array;
	uvs[1].x = 1.0f;
	uvs[2].y = 1.0f;
	uvs[3].x = 1.0f;
	uvs[3].y = 1.0f;

	filter->quad_vb =
		gs_vertexbuffer_create(vbd, GS_DUP_BUFFER | GS_DYNAMIC);
	if (!filter->quad_vb) {
		gs_vbdata_destroy(vbd);
		return false;
	}
	filter->quad_vbd = vbd;
	return true;
}

/*
 * バイザーを顔の向き (yaw/pitch) に合わせて変形して描く。
 * ローカルの隅 (±w/2, ±h/2, 0) を Y軸まわり (yaw) → X軸まわり (pitch) の
 * 順に回し、焦点距離 f = 4w の弱い透視で投影した四角形を描く。
 * 座標系は estimate_angles と同じ (x: 右目→左目、y: 下、z: カメラ向きが正)。
 *   yaw 回転:   x1 = x・cos(yaw),        z1 = -x・sin(yaw)
 *   pitch 回転: y2 = y・cos(pitch) + z1・sin(pitch),
 *               z2 = -y・sin(pitch) + z1・cos(pitch)
 *   投影: s = f / (f - z2) (手前ほど大きい)
 * 目の線の回転と目の中心への移動は、スプライト描画と同じ行列で行う。
 * 頂点の順序と UV は gs_draw_sprite (反転なし) と同一: TL, TR, BL, BR。
 */
static bool draw_visor_warped(struct hal_visor_data *filter, float center_x,
			      float center_y, float w, float h,
			      float roll_rad, float yaw, float pitch)
{
	if (w <= 0.0f || h <= 0.0f || !ensure_quad_vb(filter))
		return false;

	const float lx[4] = {-0.5f, 0.5f, -0.5f, 0.5f}; /* TL, TR, BL, BR */
	const float ly[4] = {-0.5f, -0.5f, 0.5f, 0.5f};
	const float cyw = cosf(yaw), syw = sinf(yaw);
	const float cpt = cosf(pitch), spt = sinf(pitch);
	const float f = 4.0f * w;
	float px[4], py[4];
	float min_x = 0.0f, max_x = 0.0f, min_y = 0.0f, max_y = 0.0f;

	for (int i = 0; i < 4; i++) {
		float x = lx[i] * w;
		float y = ly[i] * h;
		float x1 = x * cyw;
		float z1 = -x * syw;
		float y2 = y * cpt + z1 * spt;
		float z2 = -y * spt + z1 * cpt;
		float s = f / (f - z2);
		px[i] = x1 * s;
		py[i] = y2 * s;
		if (i == 0) {
			min_x = max_x = px[i];
			min_y = max_y = py[i];
		} else {
			if (px[i] < min_x)
				min_x = px[i];
			if (px[i] > max_x)
				max_x = px[i];
			if (py[i] < min_y)
				min_y = py[i];
			if (py[i] > max_y)
				max_y = py[i];
		}
	}

	/* 退化ガード: 投影後の幅・高さが元の2割未満なら重心まわりに広げる */
	float pw = max_x - min_x, ph = max_y - min_y;
	float pcx = (min_x + max_x) * 0.5f, pcy = (min_y + max_y) * 0.5f;
	if (pw < 0.2f * w) {
		if (pw > 1e-6f) {
			float k = (0.2f * w) / pw;
			for (int i = 0; i < 4; i++)
				px[i] = pcx + (px[i] - pcx) * k;
		} else {
			for (int i = 0; i < 4; i++)
				px[i] = pcx + lx[i] * 0.2f * w;
		}
	}
	if (ph < 0.2f * h) {
		if (ph > 1e-6f) {
			float k = (0.2f * h) / ph;
			for (int i = 0; i < 4; i++)
				py[i] = pcy + (py[i] - pcy) * k;
		} else {
			for (int i = 0; i < 4; i++)
				py[i] = pcy + ly[i] * 0.2f * h;
		}
	}

	struct gs_vb_data *data = gs_vertexbuffer_get_data(filter->quad_vb);
	if (!data)
		return false;
	for (int i = 0; i < 4; i++)
		vec3_set(data->points + i, px[i], py[i], 0.0f);
	gs_vertexbuffer_flush(filter->quad_vb);

	gs_effect_t *draw_effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *image_param =
		gs_effect_get_param_by_name(draw_effect, "image");
	gs_effect_set_texture(image_param, filter->image.texture);

	gs_blend_state_push();
	gs_enable_color(true, true, true, true);
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

	gs_matrix_push();
	gs_matrix_translate3f(center_x, center_y, 0.0f);
	if (roll_rad != 0.0f)
		gs_matrix_rotaa4f(0.0f, 0.0f, 1.0f, roll_rad);

	gs_load_vertexbuffer(filter->quad_vb);
	gs_load_indexbuffer(NULL);
	while (gs_effect_loop(draw_effect, "Draw")) {
		gs_draw(GS_TRISTRIP, 0, 0);
	}

	gs_matrix_pop();
	gs_blend_state_pop();
	return true;
}

static void hal_visor_video_render(void *data, gs_effect_t *effect)
{
	struct hal_visor_data *filter = data;

	// OBSからeffectが渡ってこない場合の安全策
	if (!effect)
		effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);

	obs_source_t *target = obs_filter_get_target(filter->source);
	uint32_t cx = target ? obs_source_get_base_width(target) : 0;
	uint32_t cy = target ? obs_source_get_base_height(target) : 0;

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

	// ---- ステップ3: 顔検出 (推論はワーカースレッド) ----
	filter->frame_count++;

	// 検出器の用意はマップの外で行う (セッション生成は重い処理なので)
	ensure_detector(filter);

	// 前回コピーしたフレームをマップしてワーカーへ渡す
	if (filter->stage_pending && filter->stage) {
		uint8_t *pixels = NULL;
		uint32_t linesize = 0;
		if (gs_stagesurface_map(filter->stage, &pixels, &linesize) &&
		    pixels) {
			if (filter->detector)
				publish_frame(filter, pixels, (int)cx, (int)cy,
					      (int)linesize);
			gs_stagesurface_unmap(filter->stage);
		}
		filter->stage_pending = false;
	}

	// 3回に1回、キャプチャを stage surface へコピーする (次回マップする)
	if (captured && filter->stage && (filter->frame_count % 3) == 0) {
		gs_stage_texture(filter->stage,
				 gs_texrender_get_texture(filter->capture));
		filter->stage_pending = true;
	}

	// 最新の検出結果を取り込んで平滑化する
	consume_results(filter);

	// バイザー画像を描画
	if (filter->image.loaded && filter->image.texture) {
		if (filter->tracking && filter->have_track &&
		    filter->sm_dist > 0.0f) {
			// 追従: 平滑化済みの目位置・間隔・角度で1回だけ描く。
			// 検出が途切れても最後の変換を保持する (隠し続ける)。
			float ang = atan2f(filter->sm_sin, filter->sm_cos);
			bool drawn = false;
			if (filter->calib_valid && filter->deform > 0) {
				// 正面登録があれば、顔の向きで変形して描く
				float yaw = 0.0f, pitch = 0.0f;
				estimate_angles(filter,
						(float)filter->deform / 100.0f,
						&yaw, &pitch);
				float w = (float)((double)filter->sm_dist *
						  filter->width_factor);
				float h = w * (float)filter->image.cy /
					  (float)filter->image.cx;
				drawn = draw_visor_warped(filter, filter->sm_x,
							  filter->sm_y, w, h,
							  ang, yaw, pitch);
			}
			if (!drawn) {
				float ts = (float)((double)filter->sm_dist *
						   filter->width_factor) /
					   (float)filter->image.cx;
				draw_visor_image(filter, filter->sm_x,
						 filter->sm_y, ts, ang);
			}
		} else {
			// 従来のスライダー位置
			float s = (float)filter->scale / 100.0f;
			draw_visor_image(filter, (float)filter->pos_x,
					 (float)filter->pos_y, s, 0.0f);
		}
	}
}

/* 「正面を登録」ボタン: 最新の検出形状を正面として設定へ保存する */
static bool hal_calib_clicked(obs_properties_t *props,
			      obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	struct hal_visor_data *filter = data;
	if (!filter)
		return false;

	struct hal_face_result res;
	memset(&res, 0, sizeof(res));
	uint64_t rtime = 0;
	bool valid = false;

	hv_lock(filter);
	if (filter->result.valid) {
		res = filter->result.res;
		rtime = filter->result.time_ns;
		valid = true;
	}
	hv_unlock(filter);

	uint64_t now = os_gettime_ns();
	if (!valid || !res.found || res.eye_dist <= 0.0f ||
	    (now - rtime) > HAL_FRESH_NS) {
		blog(LOG_WARNING,
		     "[hal-visor] 正面登録に失敗しました: 顔が検出できていません");
		return false;
	}

	double nx = (double)(res.nose_x - res.eye_mid_x) /
		    (double)res.eye_dist;
	double ny = (double)(res.nose_y - res.eye_mid_y) /
		    (double)res.eye_dist;

	filter->calib_valid = true;
	filter->calib_eye_dist = (double)res.eye_dist;
	filter->calib_nose_nx = nx;
	filter->calib_nose_ny = ny;

	obs_data_t *settings = obs_source_get_settings(filter->source);
	obs_data_set_bool(settings, "calib_valid", true);
	obs_data_set_double(settings, "calib_eye_dist",
			    filter->calib_eye_dist);
	obs_data_set_double(settings, "calib_nose_nx", nx);
	obs_data_set_double(settings, "calib_nose_ny", ny);
	obs_data_release(settings);

	blog(LOG_INFO,
	     "[hal-visor] 正面を登録しました: 目の間隔=%.0f 鼻オフセット=(%.2f, %.2f)",
	     res.eye_dist, nx, ny);
	return false;
}

static obs_properties_t *hal_visor_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_path(props, "image_path", "バイザー画像",
				OBS_PATH_FILE,
				"Image Files (*.png *.jpg *.jpeg *.bmp *.webp);;All Files (*.*)",
				NULL);

	obs_properties_add_int_slider(props, "pos_x", "位置 X", 0, 1920, 1);
	obs_properties_add_int_slider(props, "pos_y", "位置 Y", 0, 1080, 1);
	obs_properties_add_int_slider(props, "scale", "サイズ (%)", 10, 500, 1);

	obs_properties_add_bool(props, "tracking", "顔追従");
	obs_properties_add_float_slider(props, "width_factor", "幅の倍率",
					1.0, 6.0, 0.1);
	obs_properties_add_int_slider(props, "deform", "変形の強さ", 0, 200,
				      1);
	obs_properties_add_button2(props, "calib_button", "正面を登録",
				   hal_calib_clicked, data);

	return props;
}

static void hal_visor_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "pos_x", 960);
	obs_data_set_default_int(settings, "pos_y", 540);
	obs_data_set_default_int(settings, "scale", 100);
	obs_data_set_default_bool(settings, "tracking", true);
	obs_data_set_default_double(settings, "width_factor", 3.0);
	obs_data_set_default_int(settings, "deform", 100);
	obs_data_set_default_bool(settings, "calib_valid", false);
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
