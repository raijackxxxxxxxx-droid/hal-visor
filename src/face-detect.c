#include "face-detect.h"

#include <obs-module.h>
#include <util/platform.h>
#include <onnxruntime_c_api.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

/*
 * YuNet (opencv_zoo, face_detection_yunet_2023mar) の推論ラッパ。
 * 入力: float32 NCHW (1,3,H,W), 生の画素値 0..255, BGR 順。
 * 出力: cls_<s> / obj_<s> / bbox_<s> / kps_<s> (s = 8, 16, 32)。
 *   score = sqrt(cls * obj)
 *   box   = ((gx - l), (gy - t), (gx + r), (gy + b)) * stride
 *   kps_k = ((gx + dx_k), (gy + dy_k)) * stride  (5点: 右目, 左目, 鼻, 口角2つ)
 * しきい値 0.6 で候補を集め、IoU 0.3 の greedy NMS のち最良の1顔を返す。
 * 出力テンソルのレイアウト (NCHW / NHWC / フラット化) は実行時に形状から判定する。
 */

#define HAL_MAX_IN 320
#define HAL_SCORE_THRESH 0.6f
#define HAL_NMS_IOU 0.3f
#define HAL_MAX_CAND 4096

struct hal_face_detector {
	const OrtApi *api;
	OrtEnv *env;
	OrtSession *session;
	OrtMemoryInfo *mem_info;
	char *input_name;
	char **output_names;
	size_t num_outputs;
	int fixed_in_w; /* モデル入力が固定サイズなら >0 */
	int fixed_in_h;
	float *input_buf; /* HAL_MAX_IN * HAL_MAX_IN * 3 */
	bool logged_io;
};

static bool ort_ok(struct hal_face_detector *d, OrtStatus *st,
		   const char *what)
{
	if (!st)
		return true;
	blog(LOG_ERROR, "[hal-visor] ONNX Runtime エラー (%s): %s", what,
	     d->api->GetErrorMessage(st));
	d->api->ReleaseStatus(st);
	return false;
}

struct hal_face_detector *hal_face_detector_create(const char *model_path)
{
	struct hal_face_detector *d = bzalloc(sizeof(*d));
	const OrtApi *api = OrtGetApiBase()->GetApi(ORT_API_VERSION);
	d->api = api;
	OrtStatus *st = NULL;
	OrtSessionOptions *opts = NULL;
	OrtAllocator *alloc = NULL;

	st = api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "hal-visor", &d->env);
	if (!ort_ok(d, st, "CreateEnv"))
		goto fail;
	st = api->CreateSessionOptions(&opts);
	if (!ort_ok(d, st, "CreateSessionOptions"))
		goto fail;
	ort_ok(d, api->SetIntraOpNumThreads(opts, 1), "SetIntraOpNumThreads");
	ort_ok(d,
	       api->SetSessionGraphOptimizationLevel(opts, ORT_ENABLE_ALL),
	       "SetSessionGraphOptimizationLevel");

#ifdef _WIN32
	{
		wchar_t *wpath = NULL;
		os_utf8_to_wcs_ptr(model_path, 0, &wpath);
		st = api->CreateSession(d->env, wpath, opts, &d->session);
		bfree(wpath);
	}
#else
	st = api->CreateSession(d->env, model_path, opts, &d->session);
#endif
	if (!ort_ok(d, st, "CreateSession"))
		goto fail;
	api->ReleaseSessionOptions(opts);
	opts = NULL;

	st = api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault,
				      &d->mem_info);
	if (!ort_ok(d, st, "CreateCpuMemoryInfo"))
		goto fail;

	st = api->GetAllocatorWithDefaultOptions(&alloc);
	if (!ort_ok(d, st, "GetAllocatorWithDefaultOptions"))
		goto fail;

	{
		char *name = NULL;
		st = api->SessionGetInputName(d->session, 0, alloc, &name);
		if (!ort_ok(d, st, "SessionGetInputName"))
			goto fail;
		d->input_name = bstrdup(name);
		api->AllocatorFree(alloc, name);
	}

	/* モデル入力が固定サイズならそれに従う (動的なら -1 が返る) */
	{
		OrtTypeInfo *tinfo = NULL;
		const OrtTensorTypeAndShapeInfo *tsinfo = NULL;
		st = api->SessionGetInputTypeInfo(d->session, 0, &tinfo);
		if (ort_ok(d, st, "SessionGetInputTypeInfo")) {
			st = api->CastTypeInfoToTensorInfo(tinfo, &tsinfo);
			if (ort_ok(d, st, "CastTypeInfoToTensorInfo")) {
				size_t n = 0;
				api->GetDimensionsCount(tsinfo, &n);
				if (n == 4) {
					int64_t dims[4] = {0, 0, 0, 0};
					api->GetDimensions(tsinfo, dims, 4);
					if (dims[2] > 0 && dims[3] > 0) {
						d->fixed_in_h = (int)dims[2];
						d->fixed_in_w = (int)dims[3];
					}
					blog(LOG_INFO,
					     "[hal-visor] YuNet入力: %s 形状=(%lld,%lld,%lld,%lld)",
					     d->input_name,
					     (long long)dims[0],
					     (long long)dims[1],
					     (long long)dims[2],
					     (long long)dims[3]);
				}
				/* Cast で得た info は TypeInfo 側が所有するので個別解放しない */
			}
			api->ReleaseTypeInfo(tinfo);
		}
	}

	{
		size_t nout = 0;
		st = api->SessionGetOutputCount(d->session, &nout);
		if (!ort_ok(d, st, "SessionGetOutputCount"))
			goto fail;
		d->num_outputs = nout;
		d->output_names = bzalloc(sizeof(char *) * nout);
		for (size_t i = 0; i < nout; i++) {
			char *name = NULL;
			st = api->SessionGetOutputName(d->session, i, alloc,
						       &name);
			if (!ort_ok(d, st, "SessionGetOutputName"))
				goto fail;
			d->output_names[i] = bstrdup(name);
			api->AllocatorFree(alloc, name);
		}
	}

	d->input_buf = bmalloc(sizeof(float) * HAL_MAX_IN * HAL_MAX_IN * 3);
	return d;

fail:
	blog(LOG_ERROR,
	     "[hal-visor] 顔検出の初期化に失敗しました。検出なしで動作を続けます。");
	if (opts)
		api->ReleaseSessionOptions(opts);
	hal_face_detector_destroy(d);
	return NULL;
}

void hal_face_detector_destroy(struct hal_face_detector *det)
{
	if (!det)
		return;
	const OrtApi *api = det->api;
	if (api) {
		if (det->session)
			api->ReleaseSession(det->session);
		if (det->mem_info)
			api->ReleaseMemoryInfo(det->mem_info);
		if (det->env)
			api->ReleaseEnv(det->env);
	}
	if (det->output_names) {
		for (size_t i = 0; i < det->num_outputs; i++)
			bfree(det->output_names[i]);
		bfree(det->output_names);
	}
	bfree(det->input_name);
	bfree(det->input_buf);
	bfree(det);
}

/* ---- 出力テンソルのレイアウト判定と要素アクセス ---- */

enum hal_layout { HAL_LAYOUT_NCHW = 0, HAL_LAYOUT_NHWC = 1 };

struct hal_view {
	const float *data;
	int channels;
	int gh;
	int gw;
	enum hal_layout layout;
};

static bool view_init(struct hal_view *v, const float *data,
		      const int64_t *dims, size_t rank, int channels, int gh,
		      int gw)
{
	int64_t cells = (int64_t)gh * gw;
	v->data = data;
	v->channels = channels;
	v->gh = gh;
	v->gw = gw;
	if (rank == 4) {
		if (dims[1] == channels && dims[2] == gh && dims[3] == gw) {
			v->layout = HAL_LAYOUT_NCHW;
			return true;
		}
		if (dims[1] == gh && dims[2] == gw && dims[3] == channels) {
			v->layout = HAL_LAYOUT_NHWC;
			return true;
		}
	} else if (rank == 3) {
		if (dims[1] == cells && dims[2] == channels) {
			v->layout = HAL_LAYOUT_NHWC;
			return true;
		}
		if (dims[1] == channels && dims[2] == cells) {
			v->layout = HAL_LAYOUT_NCHW;
			return true;
		}
	} else if (rank == 2) {
		if (dims[1] == cells * channels) {
			v->layout = HAL_LAYOUT_NHWC;
			return true;
		}
	} else if (rank == 1) {
		if (dims[0] == cells * channels) {
			v->layout = HAL_LAYOUT_NHWC;
			return true;
		}
	}
	return false;
}

static float view_at(const struct hal_view *v, int gy, int gx, int c)
{
	if (v->layout == HAL_LAYOUT_NCHW)
		return v->data[(c * v->gh + gy) * v->gw + gx];
	return v->data[(gy * v->gw + gx) * v->channels + c];
}

/* ---- 候補と NMS ---- */

struct hal_cand {
	float score;
	float x0, y0, x1, y1; /* 入力画像座標 */
	float kps[10];
	bool suppressed;
};

static int cand_cmp(const void *a, const void *b)
{
	float sa = ((const struct hal_cand *)a)->score;
	float sb = ((const struct hal_cand *)b)->score;
	if (sa < sb)
		return 1;
	if (sa > sb)
		return -1;
	return 0;
}

static float box_iou(const struct hal_cand *a, const struct hal_cand *b)
{
	float x0 = a->x0 > b->x0 ? a->x0 : b->x0;
	float y0 = a->y0 > b->y0 ? a->y0 : b->y0;
	float x1 = a->x1 < b->x1 ? a->x1 : b->x1;
	float y1 = a->y1 < b->y1 ? a->y1 : b->y1;
	float iw = x1 - x0, ih = y1 - y0;
	if (iw <= 0 || ih <= 0)
		return 0.0f;
	float inter = iw * ih;
	float area_a = (a->x1 - a->x0) * (a->y1 - a->y0);
	float area_b = (b->x1 - b->x0) * (b->y1 - b->y0);
	float uni = area_a + area_b - inter;
	return uni > 0 ? inter / uni : 0.0f;
}

static int find_output(struct hal_face_detector *d, const char *prefix,
		       int stride)
{
	char want[32];
	snprintf(want, sizeof(want), "%s_%d", prefix, stride);
	for (size_t i = 0; i < d->num_outputs; i++) {
		if (strcmp(d->output_names[i], want) == 0)
			return (int)i;
	}
	return -1;
}

bool hal_face_detector_run(struct hal_face_detector *det,
			   const unsigned char *bgra, int src_w, int src_h,
			   int linesize, struct hal_face_result *out)
{
	out->found = false;
	if (!det || !bgra || src_w <= 0 || src_h <= 0)
		return false;
	const OrtApi *api = det->api;
	OrtStatus *st = NULL;

	/* 入力サイズ: 幅 320 目安、高さは 32 の倍数 (固定サイズモデルならそれ) */
	int in_w = HAL_MAX_IN;
	int in_h = (int)lrint((double)HAL_MAX_IN * src_h / src_w / 32.0) * 32;
	if (in_h < 32)
		in_h = 32;
	if (in_h > HAL_MAX_IN)
		in_h = HAL_MAX_IN;
	if (det->fixed_in_w > 0 && det->fixed_in_h > 0) {
		in_w = det->fixed_in_w;
		in_h = det->fixed_in_h;
	}

	/* BGRA → BGR float (0..255), 等間隔サンプリングで縮小 */
	{
		const int plane = in_w * in_h;
		for (int iy = 0; iy < in_h; iy++) {
			int sy = (int)((int64_t)iy * src_h / in_h);
			const unsigned char *row = bgra + sy * linesize;
			for (int ix = 0; ix < in_w; ix++) {
				int sx = (int)((int64_t)ix * src_w / in_w);
				const unsigned char *p = row + sx * 4;
				float *dst = det->input_buf + iy * in_w + ix;
				dst[0 * plane] = (float)p[0]; /* B */
				dst[1 * plane] = (float)p[1]; /* G */
				dst[2 * plane] = (float)p[2]; /* R */
			}
		}
	}

	OrtValue *in_tensor = NULL;
	OrtValue **outputs = bzalloc(sizeof(OrtValue *) * det->num_outputs);
	{
		int64_t shape[4] = {1, 3, in_h, in_w};
		st = api->CreateTensorWithDataAsOrtValue(
			det->mem_info, det->input_buf,
			sizeof(float) * 3 * (size_t)in_w * in_h, shape, 4,
			ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in_tensor);
		if (!ort_ok(det, st, "CreateTensorWithDataAsOrtValue"))
			goto done;
	}
	{
		const char *in_names[1] = {det->input_name};
		st = api->Run(det->session, NULL, in_names,
			      (const OrtValue *const *)&in_tensor, 1,
			      (const char *const *)det->output_names,
			      det->num_outputs, outputs);
		if (!ort_ok(det, st, "Run"))
			goto done;
	}

	/* 出力ごとに data ポインタと形状を取り出す */
	{
		static const int strides[3] = {8, 16, 32};
		struct hal_cand *cands = bmalloc(sizeof(struct hal_cand) *
						 HAL_MAX_CAND);
		int ncand = 0;

		for (int si = 0; si < 3; si++) {
			int stride = strides[si];
			int gh = in_h / stride;
			int gw = in_w / stride;
			if (gh <= 0 || gw <= 0)
				continue;
			int idx_cls = find_output(det, "cls", stride);
			int idx_obj = find_output(det, "obj", stride);
			int idx_bbox = find_output(det, "bbox", stride);
			int idx_kps = find_output(det, "kps", stride);
			if (idx_cls < 0 || idx_obj < 0 || idx_bbox < 0 ||
			    idx_kps < 0)
				continue;
			OrtValue *vals[4] = {outputs[idx_cls], outputs[idx_obj],
					     outputs[idx_bbox],
					     outputs[idx_kps]};
			const int chans[4] = {1, 1, 4, 10};
			struct hal_view views[4];
			bool ok = true;
			for (int k = 0; k < 4; k++) {
				OrtTensorTypeAndShapeInfo *info = NULL;
				float *data = NULL;
				st = api->GetTensorTypeAndShape(vals[k], &info);
				if (!ort_ok(det, st, "GetTensorTypeAndShape")) {
					ok = false;
					break;
				}
				size_t rank = 0;
				api->GetDimensionsCount(info, &rank);
				int64_t dims[8] = {0};
				if (rank > 8)
					rank = 8;
				api->GetDimensions(info, dims, rank);
				if (!det->logged_io) {
					char dimstr[128] = "";
					for (size_t di = 0; di < rank; di++) {
						char tmp[24];
						snprintf(tmp, sizeof(tmp),
							 "%s%lld", di ? "x" : "",
							 (long long)dims[di]);
						strcat(dimstr, tmp);
					}
					blog(LOG_INFO,
					     "[hal-visor] YuNet出力 %s: %s",
					     det->output_names[k == 0   ? idx_cls
								: k == 1 ? idx_obj
								: k == 2 ? idx_bbox
									 : idx_kps],
					     dimstr);
				}
				api->ReleaseTensorTypeAndShapeInfo(info);
				st = api->GetTensorMutableData(vals[k],
							       (void **)&data);
				if (!ort_ok(det, st, "GetTensorMutableData")) {
					ok = false;
					break;
				}
				if (!view_init(&views[k], data, dims, rank,
					       chans[k], gh, gw)) {
					blog(LOG_ERROR,
					     "[hal-visor] YuNet出力のレイアウトを解釈できません (stride=%d, kind=%d)",
					     stride, k);
					ok = false;
					break;
				}
			}
			if (!ok)
				continue;

			for (int gy = 0; gy < gh; gy++) {
				for (int gx = 0; gx < gw; gx++) {
					float cls = view_at(&views[0], gy, gx, 0);
					float obj = view_at(&views[1], gy, gx, 0);
					if (cls < 0 || obj < 0)
						continue;
					float score = sqrtf(cls * obj);
					if (score < HAL_SCORE_THRESH)
						continue;
					if (ncand >= HAL_MAX_CAND)
						continue;
					struct hal_cand *c = &cands[ncand++];
					float l = view_at(&views[2], gy, gx, 0);
					float t = view_at(&views[2], gy, gx, 1);
					float r = view_at(&views[2], gy, gx, 2);
					float b = view_at(&views[2], gy, gx, 3);
					c->score = score;
					c->x0 = (gx - l) * stride;
					c->y0 = (gy - t) * stride;
					c->x1 = (gx + r) * stride;
					c->y1 = (gy + b) * stride;
					for (int k = 0; k < 5; k++) {
						c->kps[2 * k] =
							(gx + view_at(&views[3],
								      gy, gx,
								      2 * k)) *
							stride;
						c->kps[2 * k + 1] =
							(gy + view_at(&views[3],
								      gy, gx,
								      2 * k + 1)) *
							stride;
					}
					c->suppressed = false;
				}
			}
		}
		det->logged_io = true;

		if (ncand > 0) {
			qsort(cands, (size_t)ncand, sizeof(struct hal_cand),
			      cand_cmp);
			/* greedy NMS: 最良の生存候補を採用 */
			const struct hal_cand *best = NULL;
			for (int i = 0; i < ncand; i++) {
				if (cands[i].suppressed)
					continue;
				best = &cands[i];
				for (int j = i + 1; j < ncand; j++) {
					if (!cands[j].suppressed &&
					    box_iou(best, &cands[j]) >
						    HAL_NMS_IOU)
						cands[j].suppressed = true;
				}
				break;
			}
			if (best) {
				float fx = (float)src_w / (float)in_w;
				float fy = (float)src_h / (float)in_h;
				float ex0 = best->kps[0] * fx;
				float ey0 = best->kps[1] * fy;
				float ex1 = best->kps[2] * fx;
				float ey1 = best->kps[3] * fy;
				float dx = ex1 - ex0, dy = ey1 - ey0;
				out->score = best->score;
				out->eye_mid_x = (ex0 + ex1) * 0.5f;
				out->eye_mid_y = (ey0 + ey1) * 0.5f;
				out->eye_dist = sqrtf(dx * dx + dy * dy);
				out->box_x = best->x0 * fx;
				out->box_y = best->y0 * fy;
				out->box_w = (best->x1 - best->x0) * fx;
				out->box_h = (best->y1 - best->y0) * fy;
				out->found = true;
			}
		}
		bfree(cands);
	}

done:
	if (in_tensor)
		api->ReleaseValue(in_tensor);
	if (outputs) {
		for (size_t i = 0; i < det->num_outputs; i++) {
			if (outputs[i])
				api->ReleaseValue(outputs[i]);
		}
		bfree(outputs);
	}
	return out->found;
}
