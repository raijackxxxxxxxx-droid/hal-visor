#include <obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("hal-visor", "en-US")

// フィルタの名前（OBSのフィルタ一覧に表示される名前）
static const char *hal_visor_get_name(void *unused)
{
    UNUSED_PARAMETER(unused);
    return "HaL Visor (Alpha)";
}

// フィルタ破棄時のメモリ解放
static void hal_visor_destroy(void *data)
{
    bfree(data);
}

// フィルタ追加時に呼ばれる初期化
static void *hal_visor_create(obs_data_t *settings, obs_source_t *source)
{
    UNUSED_PARAMETER(settings);
    UNUSED_PARAMETER(source);
    
    // 最小限のデータを確保
    void *data = bzalloc(sizeof(int));
    return data;
}

// 映像描画処理（まずは映像を素通しするだけ）
static void hal_visor_video_render(void *data, gs_effect_t *effect)
{
    UNUSED_PARAMETER(data);
    UNUSED_PARAMETER(effect);

    // フィルタをスキップして元の映像をそのまま通す
    obs_source_skip_video_filter((obs_source_t*)data);
}

// OBSに教えるフィルタの基本設計図
struct obs_source_info hal_visor_filter_info = {
    .id = "hal_visor_filter",
    .type = OBS_SOURCE_TYPE_FILTER,
    .output_flags = OBS_SOURCE_VIDEO,
    .get_name = hal_visor_get_name,
    .create = hal_visor_create,
    .destroy = hal_visor_destroy,
    .video_render = hal_visor_video_render,
};

// OBS起動時にプラグインを読み込む
bool obs_module_load(void)
{
    obs_register_source(&hal_visor_filter_info);
    return true;
}