# src/host — プラットフォーム別ホスト実装

コア（`src/core/`）はプラットフォーム非依存の純C。実機/PCの違いはすべてこの `host/` に閉じています。
コアとは `script_register_*` などの API でのみ接続し、コアは host の存在を知りません（仕様 §11）。

```
src/host/
├── common/        … 全プラットフォーム共通のホスト補助
│   └── host_diag  … ロードエラーの文字列化 + did-you-mean（綴り間違い推定）
├── pc/            … PC（Windows/MSVC）ホスト = 開発・デバッグ用モック
│   ├── host_mock  … STDOUT=コンソール, LED1/BUZZER=擬似GPIO, NOW=GetTickCount
│   └── main.c     … エントリ。キーボードを擬似割り込み（UART1/BTN）に見立てる
└── stm32_l476/    … STM32 Nucleo-L476RG 実機ホスト（Tera Term で貼り付け実行）
    ├── yajir_glue   … LED1(PA5)/SW1・BTN(PC13)/STDOUT(USART2)/DELAY/NOW
    └── yajir_loader … UART受信 → @run でロード＆実行（常駐ループ）
```

## 新しいボードを足すには

`stm32_l476/` を雛形にして `host/<board>/` を作り、次の2つを用意すれば動きます。

1. **ボードグルー**: `host_register_all()`（ポート束縛）と `get_tick()`、出力関数。
   `common/host_diag.h` の `host_diag_reset()/host_diag_note()` を register と一緒に呼べば
   did-you-mean がそのまま効きます。
2. **ローダ/メインループ**: スクリプトの入手経路（UART貼り付け、ファイル、フラッシュ常駐 等）と
   `script_init → host_register_all → script_load`、そして `while(1){ … script_tick(); }`。

`common/` はどのボードからもそのまま使えます（OS非依存・純C）。

## ビルド

- **PC**: リポジトリ直下の `build.bat`（`build.bat main` で `build\script.exe`）。
  使用ソースは `CORE` + `PCHOST`（= `common/host_diag.c` + `pc/host_mock.c` + `pc/main.c`）。
- **STM32**: CubeIDE プロジェクトに `core/` `common/` `stm32_l476/` を追加。
  詳細は [`stm32_l476/README.md`](stm32_l476/README.md)。

> 注意: `script_config.h` の `CFG_MAX_PORTS` は「組込みポート + そのボードが register する数」を
> 賄える値にすること（既定 48）。超過すると register が**黙って失敗**します。
