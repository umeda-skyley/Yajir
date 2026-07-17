# src/host — プラットフォーム別ホスト実装

コア（`src/core/`）はプラットフォーム非依存の純C。実機/PCの違いはすべてこの `host/` に閉じています。
コアとは `script_register_*` などの API でのみ接続し、コアは host の存在を知りません（仕様 §11）。

```
src/host/
├── common/        … 全プラットフォーム共通のホスト補助
│   └── host_diag  … ロードエラーの文字列化 + did-you-mean（綴り間違い推定）
├── pc/            … PC（Windows/MSVC）ホスト = 開発・デバッグ用モック
│   ├── host_mock  … STDOUT=コンソール, LED1=擬似GPIO, NOW=GetTickCount, DELAY
│   └── main.c     … エントリ。キーボードを擬似割り込み（UART1/BTN）に見立てる
└── stm32_l476/    … STM32 Nucleo-L476RG 実機ホスト（Tera Term で貼り付け実行）
    ├── yajir_glue   … LED1(PA5)/NOW/STDOUT(USART2)/DELAY ＋ 源 BTN(PC13)/UART1
    └── yajir_loader … UART受信 → @run でロード＆実行（常駐ループ）
```

## ホストが公開するポート（PC / 実機で共通）

| 名前 | 種別 | PC | 実機 |
|---|---|---|---|
| `LED1` | inout(int) | 擬似GPIO（状態変化をコンソールへ） | PA5（LD2・緑） |
| `NOW` | in(int) | `GetTickCount64` 起点差 | `HAL_GetTick` |
| `STDOUT` | out | コンソール | USART2 |
| `DELAY` | out | ビジーループ | `HAL_Delay` |
| `BTN` | 源 | TAB キー／3秒ごと自動 | PC13 の EXTI |
| `UART1` | 源 | 印字キー | USART2 受信1文字 |
| `MYHANDLER` | 源 | `m` キー → `host_fire_myhandler()` | 登録のみ（発火元はユーザが書く） |

**顔ぶれは両者で完全に一致**させてある。だから `scripts/` のサンプルは PC で試したものが
そのまま実機で動く。`MYHANDLER` は「C側から `script_post_msg_v()` で自作イベントを上げる」
実例枠で、PC は擬似発火まで用意し、実機は登録だけして雛形をコメントで示している。

新しいポートを片方だけに足したくなったら、まず「もう一方にも同じ名前が置けるか」を考えること。
置けないなら、それはサンプルスクリプトから使えないポートになる。

## 新しいボードを足すには（ポーティングガイド）

`stm32_l476/` を雛形にして `host/<board>/` を作り、下記を用意すれば動きます。コアは `src/core/`
のまま**一切触りません**。ホストがやることは「C資源をコアの register API に束ねる」ことだけです。

接続は一方向——**ホスト → コア**（`script_*` を呼ぶ）。コアからホストの関数を名指しで呼ぶことは
ありません。ホストが渡した**関数ポインタ**を通じてのみコールバックが起きます（仕様 §11）。

### 1. コアが呼び出すライフサイクル（メインループ側で必ず呼ぶ）

| 関数 | いつ | 役割 |
|---|---|---|
| `script_init(arena, size)` | 起動時1回 | 固定RAM（`arena` ＝ `char[sizeof(script_vm_t)+α]`）をVMに割り当て。ヒープ不使用 |
| （ここでポート束縛。下の 2 節） | init 直後 | `script_register_*` を並べる |
| `script_load(src, len)` | スクリプト受信時 | テキストをコンパイルして常駐。**0=ok / 非0=エラー**（`script_last_error()` で詳細） |
| `script_tick()` | ループで毎回 | タイマ満期＋イベント＋周期＋MAIN を1ステップ進める。**協調的**（長居しない） |

```c
static char arena[sizeof(script_vm_t) + 128];   /* 固定確保。sizeof は script_config.h で決まる */
script_init(arena, sizeof(arena));
host_register_all();                            /* ↓ 2節。ここでポートを束ねる */
if (script_load(src, len) != 0) { /* script_last_error() を host_diag で表示（4節） */ }
for (;;) { poll_your_irq_sources(); script_tick(); }   /* while(1) 常駐 */
```

### 2. ホストが供給する関数（＝ボード依存の実体）

コアは「何もない」状態で起動し、ホストが `script_register_*` で C 資源を束ねて初めてポートになります。

#### (a) 必須 — これが無いと `script_load` が失敗する

| 供給するもの | 束ね方 | 無いと |
|---|---|---|
| **単調クロック** `int32_t (*)(void)`（ms） | `script_register_now(get_tick)` | ロード時 **`ERR_NO_CLOCK`**。タイマ/周期/`WAIT`/遅延post が全部これに乗るため必須（v0.4.8） |

`get_tick()` は「起動からの経過ミリ秒」を返す関数。STM32 は `HAL_GetTick()`、PC は
`GetTickCount64()` の起点差。**これ1本だけがコア必須**です。

#### (b) ほぼ必須 — 出力

| 供給するもの | 束ね方 | 備考 |
|---|---|---|
| **出力シンク** `void (*)(const char *s)` | `script_register_stdout(puts_fn)` | 1文字列をUART/コンソールへ出すだけの関数を渡す。ループ/int→10進/改行の整形はコアが持つ（v0.4.8）。任意（未登録なら `-> STDOUT` はコンパイル時 `ERR_UNKNOWN_PORT`） |

`puts_fn` の中身は「渡された C 文字列をそのまま送信」だけ（例: `HAL_UART_Transmit` / `fputs`）。
整形はコア側なので、ボードごとに書くのは送信の1行だけです。

#### (c) 任意 — 周辺I/O（センサ・GPIO・アクチュエータ）

| 種別 | シグネチャ | 束ね方 | 例 |
|---|---|---|---|
| 入力 `in` | `int32_t (*)(void)` | `script_register_in(name, get, SCRIPT_T_INT\|_STR)` | ADC読み・スイッチ状態 |
| 出力 `out` | `void (*)(int argc, const script_value_t *argv)` | `script_register_out(name, set)` | LED消灯・ブザー・`DELAY` |
| 入出力 `inout` | 上記2つ | `script_register_inout(name, get, set, type)` | GPIO（書いて現在値をtee）・変換ポート |
| 定数 `const` | 値 | `script_register_const(name, value)` | しきい値・ピン番号 |

- `out`/`inout` の thunk は `argv[i]` を `script_val_is_str()` で型判定し、`argv[i].i`（int）または
  `script_resolve_str(argv[i])`（str）で取り出す。戻り値を返すなら `script_set_result(v)`（int）／
  `script_set_sresult(s)`（str）。**産出型はコアの概念**なので、詳しくは既存グルーを見倣うのが早い。
- 文字列Utility（`FORMATTER` 等）や数値Utility（`RAND` 等）、`STATUS`/`ERR_*` はコア組込み。
  ホストは登録不要（`script_init` が用意する）。

#### (d) 任意 — 非同期イベント源（ISR/割り込みからVMへ橋渡し）

イベント源（`ON <名前>` の相手）は `script_register_handler(name)` で名前だけ登録し、実際の発火は
ISR/タスクから post 関数を呼ぶ。**post 系は ISR から呼べる**（積んで即 return・次tickで処理, §10）:

| 用途 | 関数 |
|---|---|
| 単値（受信バイト等） | `script_post_msg(name, value)` |
| 多値をアトミックに（int/str混在） | `script_post_msg_v(name, argc, argv)`（`SCRIPT_ARG_INT`/`SCRIPT_ARG_STR` で組む） |

```c
void HAL_GPIO_EXTI_Callback(uint16_t pin){ if (pin==B1_Pin) script_post_msg("BTN", 0); }
```

#### (e) 任意 — その他フック

| 供給するもの | 束ね方 | 備考 |
|---|---|---|
| エントロピー種 | `script_srand(seed)` | ADCノイズ/UID/tick 等で `RAND` の初期種を撹拌（省略時は固定種＝再現的） |
| did-you-mean 候補収集 | `host_diag_reset()` ＋ register ごとに `host_diag_note(name)` | 綴り間違い提案が効く（`common/` にあるので全ボード共通） |

> 慣用として、register を薄いラッパ（`reg_out`/`reg_inout`/`reg_handler`）でくるみ、その中で
> `host_diag_note()` も一緒に呼ぶ。既存グルーの冒頭がその形（`NOW`/`STDOUT` はコア昇格したので
> 候補は `host_diag` の builtin 表側が持つ＝ホストからの note は不要, v0.4.8）。

#### (f) 任意 — config / ISRガードマクロの上書きインクルード

コアを一切編集せずにボード固有の値やクリティカルセクション実装を差し込む2つの継ぎ目。
どちらも**コンパイル定義（`-D`）で指定**するだけで、`script_config.h` が自動で取り込みます。

| 差し込み先 | 用途 | 指定方法 |
|---|---|---|
| `YJ_CONFIG_HEADER` | `CFG_GVAR_COUNT` などの `CFG_*` 定数をボード向けに上書き | `-DYJ_CONFIG_HEADER=\"yajir_config_<board>.h\"` |
| `YJ_PORT_HEADER` | `YJ_ENTER_CRITICAL`/`YJ_EXIT_CRITICAL`（ISRガードマクロ）を再定義 | `-DYJ_PORT_HEADER=\"yajir_port_<board>.h\"` |

- **config 上書き**: 新ボード用ヘッダに欲しい `CFG_*` だけを `#define` すればよい（未指定分は
  `script_config.h` 側の既定値がそのまま効く）。RAM 事情に合わせて `CFG_GVAR_COUNT` 等を絞る/広げる
  用途（詳細は [`docs/resource_config.md`](../../docs/resource_config.md)）。
- **ISRガードマクロ上書き**: 既定の `YJ_ENTER_CRITICAL`/`YJ_EXIT_CRITICAL` は空マクロ（単一生産者
  ホストはコスト0）。ISR＋self-post など**複数生産者**からイベントを post するボードは、
  `YJ_PORT_HEADER` 経由でヘッダごと差し込むか、ビルド前に直接 `#define` して割り込み禁止/復帰を
  実装すること（例は `stm32_l476/yajir_port_stm32.h` を参照。CMSIS/PRIMASK退避の実例）。
- 検討タイミングの目安: 新ボードを足す際、まず「(a)〜(e) の register だけで済むか」を確認し、
  RAM 上限やマルチ生産者な割り込み構成がある場合にのみ、この2つの上書きを検討する。

### 3. ローダ／メインループ（スクリプトの入手経路）

`script_load` に渡すテキストの入手は完全にボード裁量: UART貼り付け（STM32 の `@run`）、ファイル読み
（PC）、フラッシュ常駐、ネットワーク受信など。**入手経路が違うだけで、init→register→load→tick の
骨格は同じ**です。

### 4. ロードエラーの表示

`script_load` の非0戻り値は「事実＋判断材料」だけ（行・コード・トークン）。**文字列化・言語・
表示は組み込む人の領域**（(B)方針）。`common/host_diag.c` の `script_strerror()`／`host_suggest_name()`
がそのまま使えます（全ボード共通・純C）。`ERR_NO_CLOCK` なら「`script_register_now` の呼び忘れ」を
すぐ指せます。

### まとめ：最小構成

**クロック1本（必須）＋出力シンク1本（ほぼ必須）＋ init/load/tick を呼ぶループ**。これだけで
`scripts/` の多くが動きます。周辺I/Oとイベント源は「そのボードで繋ぎたいものだけ」足していけば
よく、コア（`src/core/`）は最後まで触りません。

## ビルド

- **STM32**: CubeIDE プロジェクトに `core/` `common/` `stm32_l476/` を追加。
  詳細は [`stm32_l476/README.md`](stm32_l476/README.md)。

> 注意: `script_config.h` の `CFG_MAX_PORTS` は「組込みポート + そのボードが register する数」を
> 賄える値にすること（既定 48）。超過すると register が**黙って失敗**します。
