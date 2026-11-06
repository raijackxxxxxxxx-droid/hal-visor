#pragma once

#include <stdbool.h>

/*
 * YuNet (face_detection_yunet_2023mar.onnx) を ONNX Runtime で実行する薄いラッパ。
 * ステップ2（配管確認）用: 同期実行のみ。失敗時はすべて false/NULL を返し、
 * 呼び出し側（フィルター）は従来動作を続ける。
 */

struct hal_face_result {
	bool found;
	float score;
	/* 元フレーム座標 (px) */
	float eye_mid_x;
	float eye_mid_y;
	float eye_dist;
	float box_x;
	float box_y;
	float box_w;
	float box_h;
};

struct hal_face_detector;

struct hal_face_detector *hal_face_detector_create(const char *model_path);
void hal_face_detector_destroy(struct hal_face_detector *det);

/*
 * bgra: ステージサーフェスをマップした BGRA バイト列 (行ピッチ linesize)
 * 内部で幅 320 目安に縮小して推論し、最もスコアの高い顔を1つ返す。
 * 顔が無ければ false。
 */
bool hal_face_detector_run(struct hal_face_detector *det,
			   const unsigned char *bgra, int src_w, int src_h,
			   int linesize, struct hal_face_result *out);
