#pragma once

#include <stdbool.h>
#include <stddef.h>

/*
 * YuNet (face_detection_yunet_2023mar.onnx) を ONNX Runtime で実行する薄いラッパ。
 * ステップ3: prepare (縮小, 描画スレッド) と infer (推論, ワーカースレッド) に分割。
 * 失敗時はすべて false/NULL を返し、呼び出し側（フィルター）は従来動作を続ける。
 */

/* 推論入力の最大辺と、入力バッファに必要な float 数 */
#define HAL_FACE_INPUT_MAX_SIDE 320
#define HAL_FACE_INPUT_MAX_FLOATS \
	(HAL_FACE_INPUT_MAX_SIDE * HAL_FACE_INPUT_MAX_SIDE * 3)

struct hal_face_result {
	bool found;
	float score;
	/* 元フレーム座標 (px) */
	float eye_mid_x;
	float eye_mid_y;
	float eye_dist;
	/* 目の線の角度 (rad)。画像座標で右目→左目の方向の atan2(dy, dx) */
	float eye_angle;
	/* 鼻の位置 (px) */
	float nose_x;
	float nose_y;
	float box_x;
	float box_y;
	float box_w;
	float box_h;
};

struct hal_face_detector;

struct hal_face_detector *hal_face_detector_create(const char *model_path);
void hal_face_detector_destroy(struct hal_face_detector *det);

/*
 * この detector の推論入力に必要な float 数を返す。
 * 固定サイズ入力のモデルでは fixed_in_w * fixed_in_h * 3、
 * 動的サイズのモデルでは HAL_FACE_INPUT_MAX_FLOATS。
 * 呼び出し側のフレームバッファはこの容量以上を確保すること。
 */
size_t hal_face_detector_input_capacity(struct hal_face_detector *det);

/*
 * BGRA バイト列を推論入力 (BGR float, 幅 320 目安に縮小) へ変換する。
 * dst の容量は dst_cap (float 数)。必要数が dst_cap を超える場合は
 * 何も書かずに false を返す (バッファ溢れ防止の安全ガード)。
 * 実際に使う入力サイズを *out_w / *out_h に返す。描画スレッドで呼ぶ想定。
 */
bool hal_face_detector_prepare(struct hal_face_detector *det,
			       const unsigned char *bgra, int src_w,
			       int src_h, int linesize, float *dst,
			       size_t dst_cap, int *out_w, int *out_h);

/*
 * prepare 済みの入力に対して推論し、最もスコアの高い顔を1つ返す。
 * src_w / src_h は座標を戻すための元フレームサイズ。
 * ワーカースレッドで呼ぶ想定 (同一 detector に対する同時呼び出しは不可)。
 * 顔が無ければ out->found = false で false を返す。
 */
bool hal_face_detector_infer(struct hal_face_detector *det,
			     const float *input, int in_w, int in_h,
			     int src_w, int src_h,
			     struct hal_face_result *out);

/*
 * bgra: ステージサーフェスをマップした BGRA バイト列 (行ピッチ linesize)
 * prepare + infer を同期で続けて行う互換ラッパ。
 */
bool hal_face_detector_run(struct hal_face_detector *det,
			   const unsigned char *bgra, int src_w, int src_h,
			   int linesize, struct hal_face_result *out);
