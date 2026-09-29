#include <obs-module.h>
#include <graphics/image-file.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("hal-visor", "en-US")

struct hal_visor_data {
    obs_source_t *source;
    gs_image_file_t image;
    char *image_path;
    int pos_x;
    int pos_y;
    int scale;
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

    // 画像パスが変更された場合のみ再読み込み
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

static void hal_visor_destroy(void *data)
{
    struct hal_visor_data *filter = data;

    obs_enter_graphics();
    gs_image_file_free(&filter->image);
    obs_leave_graphics();

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

static void hal_visor_video_render(void *data, gs_effect_t *effect)
{
    struct hal_visor_data *filter = data;

    // 1. 元のカメラ映像を通常レンダリング
    if (!obs_source_process_filter_begin(filter->source, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING))
        return;
    obs_source_process_filter_end(filter->source, effect, 0, 0);

    // 2. バイザー画像が読み込まれていれば上に重ねて描画
    if (filter->image.loaded) {
        gs_effect_t *draw_effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
        gs_technique_t *tech = gs_effect_get_technique(draw_effect, "Draw");
        gs_eparam_t *image_param = gs_effect_get_param_by_name(draw_effect, "image");
        gs_effect_set_texture(image_param, filter->image.texture);

        gs_blend_state_push();
        gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

        gs_matrix_push();
        
        // 指定座標へ移動
        gs_matrix_translate3f((float)filter->pos_x, (float)filter->pos_y, 0.0f);
        
        // 拡大縮小（%から倍率に変換）
        float s = (float)filter->scale / 100.0f;
        gs_matrix_scale3f(s, s, 1.0f);

        // 画像の中心を基準点にする
        float half_w = (float)filter->image.cx * 0.5f;
        float half_h = (float)filter->image.cy * 0.5f;
        gs_matrix_translate3f(-half_w, -half_h, 0.0f);

        // 描画実行
        size_t passes = gs_technique_begin(tech);
        for (size_t i = 0; i < passes; i++) {
            gs_technique_begin_pass(tech, i);
            gs_draw_sprite(filter->image.texture, 0, filter->image.cx, filter->image.cy);
            gs_technique_end_pass(tech);
        }
        gs_technique_end(tech);

        gs_matrix_pop();
        gs_blend_state_pop();
    }
}

// 設定プロパティUIの定義
static obs_properties_t *hal_visor_properties(void *data)
{
    UNUSED_PARAMETER(data);
    obs_properties_t *props = obs_properties_create();

    obs_properties_add_path(props, "image_path", "バイザー画像 (PNG)", OBS_PATH_FILE,
                            "PNG Image (*.png);;All Files (*.*)", NULL);

    obs_properties_add_int_slider(props, "pos_x", "位置 X", 0, 1920, 1);
    obs_properties_add_int_slider(props, "pos_y", "位置 Y", 0, 1080, 1);
    obs_properties_add_int_slider(props, "scale", "サイズ (%)", 10, 300, 1);

    return props;
}

// デフォルト設定値
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
    return true;
}