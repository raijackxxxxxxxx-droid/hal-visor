# HaL-VISOR

顔出し配信で目元を隠すための OBS Studio 用ビデオフィルターです。
カメラ映像から顔を検出し、目の位置に合わせてバイザー画像を自動で追従表示します。顔の傾きや向き（左右・上下）に合わせてバイザーの位置・大きさ・傾きが変わり、向きに応じて弱い透視変形もかかります。

サングラスをかけなくても目元を隠せるので、配信中でもコメントや画面が素の目で読みやすく…なることを目指したプラグインです。

## 動作環境

- Windows（x64）
- OBS Studio 31.x（OBS 31.1.1 向けにビルドしています）

## インストール

ビルド成果物（GitHub Actions の Artifacts、または配布 zip）の中身を、OBS Studio のインストールフォルダに配置します。

| ファイル | 配置先 |
| --- | --- |
| `hal-visor.dll` | `C:\Program Files\obs-studio\obs-plugins\64bit\` |
| `onnxruntime.dll` | `C:\Program Files\obs-studio\obs-plugins\64bit\`（`hal-visor.dll` と同じフォルダ） |
| `face_detection_yunet_2023mar.onnx` | `C:\Program Files\obs-studio\data\obs-plugins\hal-visor\` |

配置したら OBS Studio を再起動してください。カメラ映像のソースにフィルター **「HaL Visor」** が追加できるようになります。

> 古いバージョン（`plugintemplate-for-obs.dll`）を使っていた場合は、古い DLL と `data\obs-plugins\plugintemplate-for-obs\` フォルダを削除してから入れてください。フィルターの設定（シーン側の保存情報）はそのまま引き継がれます。

## 使い方

1. カメラ映像のソースを右クリックし、「フィルタ」から **HaL Visor** を追加します。
2. **バイザー画像** に、目元を隠すための画像（PNG など、透過画像がおすすめ）を選びます。
3. カメラを正面から見た状態で、フィルター設定の **「正面を登録」** ボタンを押します。これが変形計算の基準になります。
4. **幅の倍率** で目の間隔に対するバイザーの横幅を、**変形の強さ** で顔の向きへの追従変形の効き具合を調整します。

## 設定項目

| 項目 | 説明 |
| --- | --- |
| バイザー画像 | 目元に重ねる画像ファイル |
| 位置 X / 位置 Y / サイズ (%) | 顔追従がオフのとき（または顔が一度も検出されていないとき）の固定表示位置と大きさ |
| 顔追従 | オンにすると検出した顔に合わせてバイザーが動きます（既定: オン） |
| 幅の倍率 | 目の間隔の何倍の横幅でバイザーを表示するか（1.0〜6.0、既定: 3.0） |
| 変形の強さ | 正面登録を基準に、顔の向きに合わせてバイザーを変形させる強さ（0〜200%、既定: 100%。0 で変形なし） |
| 正面を登録 | 正面を向いた状態で押すと、その顔の配置を基準として記録します |

## 備考

- 顔検出は ONNX Runtime と YuNet モデルで **すべてローカルで処理** します。映像が外部に送信されることはありません。
- 顔を見失った場合、バイザーは最後に検出した位置にそのまま留まります（目元が露出しないための仕様です）。
- **顔追従** をオフにすると、位置 X / Y・サイズで指定した固定位置のオーバーレイとして使えます。
- 検出や正面登録の様子は OBS のログ（ヘルプ → ログファイル）に出力されます。うまく動かないときはログの `[hal-visor]` 行を確認してください。

## ライセンス・クレジット

- HaL-VISOR 本体: GNU General Public License v2.0（同梱の [LICENSE](LICENSE) を参照）
- 顔検出モデル: [YuNet](https://github.com/opencv/opencv_zoo/tree/main/models/face_detection_yunet)（OpenCV Zoo、Apache License 2.0）
- 推論ランタイム: [ONNX Runtime](https://github.com/microsoft/onnxruntime)（MIT License）
