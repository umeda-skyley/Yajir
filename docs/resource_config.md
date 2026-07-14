# Yajir リソース設定ガイド（`script_config.h` と RAM の効き方）

ポーティング時に触る固定値（`src/core/script_config.h` の `#define`）の早見表です。
「どれを増やすと RAM が増えるのか」「何が RAM 消費を支配するのか」「減らすと何ができなくなるのか」を、
実測値に基づいてまとめています。（対象バージョン: Yajir v0.4.6）

> すべての `#define` は `#ifndef` で囲まれているので、ビルド側から上書きできます
> （`-DYJ_CONFIG_HEADER="\"board_config.h\""` で自前ヘッダを差し込む、など）。

---

## 大原則：RAM は2箇所だけ

Yajir はヒープを使いません。RAM は次の2箇所にしか存在しません。

- **(a) アリーナ** ＝ `sizeof(script_vm_t)` の一塊。ホストが `script_init(arena, size)` に渡すバッファ。
  固定スロット・ポート表・イベントキュー・コールスタック・バイトコード・文字列プールが**全部この中に同居**します。
- **(b) コンパイラ内の小さな static 配列**（別名表・DFS作業用・`compiler.c` の `.bss`）＝**アリーナの外**。
  ロード（コンパイル）中だけ使う一時領域です。

**重要**: どの `#define` も「コードサイズ」はほとんど変えません。効くのは
**「アリーナを増やす／(b) の static を増やす／単なる上限値で RAM 不変」** のどれかです。

---

## 実測：既定構成のアリーナ内訳

既定の `script_config.h` で **`sizeof(script_vm_t) ≈ 13.0 KB`**。大きい順の内訳（実測）:

| 領域 | 既定 | 決める式 |
|---|---:|---|
| ポート表 | 3584 B (28%) | `MAX_PORTS × sizeof(port_t)`（port_t ≈ `MAX_NAME` + 40） |
| イベントキュー | 2752 B (21%) | `QUEUE_LEN × (ARG×8 + SARG_COUNT×SARG_LEN + 12)` |
| コールスタック | 1936 B (15%) | `CALL_NEST × (ARG×8 + SARG_COUNT×SARG_LEN + VAR×8 + SVAR_COUNT×SSTR_LEN + 4)` |
| バイトコード | 2048 B (16%) | `CODE_SIZE`（1:1） |
| 文字列プール | 1024 B (8%) | `STRPOOL_SIZE`（1:1） |
| 文字列スロット | 640 B | `(SGVAR_COUNT + SVAR_COUNT + 2) × SSTR_LEN` |
| その他 | ~1000 B | ブロック表 / 固定intスロット / SARG / タイマ / ループ 等 |

構造体1個あたりの実測サイズ（既定）: `port_t=64` `event_t=172` `call_frame_t=484` `block_t=20` `timer_slot_t=12` `loop_frame_t=8` `value_t=8`。

---

## ① 何を増やすとアリーナが増えるか（大物5つ）

| `#define` | RAM への効き | 減らすと失うもの |
|---|---|---|
| `CFG_MAX_PORTS` | `×64 B/本`（最大の項） | 登録できる**総ポート数**（組込み〜29＋ホストの `def_out/in/inout`＋スクリプトの `def_handler`/`def_port` の合計）。溢れは登録失敗／`ERR_TOO_MANY_PORTS` |
| `CFG_EVENT_QUEUE_LEN` | `×172 B/段`（Q） | 同 tick でさばける**イベント/self-post 世代**の在庫。バーストで溢れると `ERR_QUEUE_OVF` で新着ドロップ |
| `CFG_CALL_NEST` | `×484 B/段` | スクリプト内ポートの**ネスト呼び出し段数**（超過はロード時 `ERR_NEST_TOO_DEEP`）。v0.4.6 でフレームが重い（VAR/SVAR 退避）ので**削減効果が大きい** |
| `CFG_CODE_SIZE` | 1:1 | 1スクリプトの**最大バイトコード長**（`CODE_USED` で使用量が読める） |
| `CFG_STRPOOL_SIZE` | 1:1 | ソースに直書きした**文字列リテラルの総量**（`"..."` の合計バイト・重複は畳まない。`STR_USED` で読める） |

---

## ② RAM 消費を支配する乗数（複数の箱に同時に効く＝レバレッジ大）

文字列の「長さ」と「本数」は、**基本スロット＋コールスタック＋イベントキューの複数箇所に同時に掛かる**ため、
1つ動かすと効果が大きいのが特徴です。

| `#define` | かかる箱（＝乗数） | 減らすと失うもの |
|---|---|---|
| `CFG_SSTR_LEN` | 基本スロット `(SGVAR+SVAR+2)` ＋ `save_svar × CALL_NEST` | **文字列スロットの最大長**。式 ≈ `SSTR_LEN × (SGVAR + SVAR + 2 + SVAR×CALL_NEST)` |
| `CFG_SARG_LEN` | `SARG` ＋ `save_sarg × CALL_NEST` ＋ `event.sstr × QUEUE_LEN` | **受信文字列の最大長**。式 ≈ `SARG_LEN × SARG_COUNT × (1 + CALL_NEST + QUEUE_LEN)` |
| `CFG_SARG_COUNT` | 同上（`× SARG_LEN`） | 1メッセージに載せられる**文字列位置の数**（`(int,int,str)` の str 位置） |
| `CFG_ARG_COUNT` | `ARG` ＋ `save_arg×CALL_NEST` ＋ `event.pos×QUEUE_LEN`（`×8`） | 1メッセージ/呼び出しの**引数位置数** |
| `CFG_SVAR_COUNT` | `SVAR` ＋ `save_svar×CALL_NEST`（`× SSTR_LEN`） | **文字列作業スロット**の本数 |
| `CFG_VAR_COUNT` | `VAR` ＋ `save_var×CALL_NEST`（`×8`） | **int 作業スロット**の本数 |
| `CFG_GVAR_COUNT` / `CFG_SGVAR_COUNT` | 基本スロットのみ（`×8` / `× SSTR_LEN`） | 大域 int / str スロットの本数 |
| `CFG_MAX_NAME` | ポート表を丸ごと乗算（`× MAX_PORTS`）＋エラー `tok` 長 | ポート/別名の**名前の最大長**（超過は切り詰め） |

> **文字列長を動かすと、基本スロット・コールスタック・キューの3箇所が一斉に変わります。**
> 「post キューを増やしても、スクリプト内ポートの SVAR 退避でも、同じ `SSTR_LEN`/`SARG_LEN` が効く」——
> ここが Yajir の RAM 見積もりで一番レバレッジのあるポイントです。

---

## ③ アリーナの小物

| `#define` | 効き | 減らすと |
|---|---|---|
| `CFG_MAX_BLOCKS` | `×20 B` | `INIT`/`MAIN`/`ON`/`PORT` の**総ブロック数** |
| `CFG_STACK_DEPTH` | `×8 B` | 式の複雑さ・チェイン段数（深い式で足りないと実行時 `EXEC_ERROR`） |
| `CFG_TIMER_SLOTS` | `×12 B` | 同時に張れる**ワンショットタイマ本数**（満杯は `ERR_TIMER_FULL`） |
| `CFG_LOOP_NEST` | `×8 B` | `REPEAT` の**ネスト段数**（超過は `ERR_NEST_TOO_DEEP`） |

---

## ④ コンパイル時のみの static（アリーナ外の `.bss`・ロード中だけ）

| `#define` | 効き | 減らすと |
|---|---|---|
| `CFG_MAX_ALIAS` | `×32 B`（アリーナ外） | `def_alias`（大域）＋1ブロックの `def_local`（局所）の**同時数** |
| `CFG_MAX_SCRIPT_PORTS` | 小配列 数本（アリーナ外・≤32） | `def_port` の**最大数**（再帰判定 DFS のビットセット幅） |

---

## ⑤ RAM は変えない（単なる上限値・挙動フラグ）

| `#define` | 意味 |
|---|---|
| `CFG_INSTR_BUDGET` | 1 tick の命令数上限（暴走ガード）。大きくすると重い `REPEAT`/呼び出しを許すだけ＝**RAM 不変** |
| `CFG_NEST_LIMIT` | `IFYES` ネスト上限（配列なし・パーサ再帰の番人）＝**RAM 不変** |
| `TS_PERIODIC_CATCHUP` / `CFG_LINE_DELIM(_STRICT)` | 周期満期ポリシー / 行末文字の**挙動フラグ** |
| `CFG_MAX_RESOURCES` | **現状コード内で未使用**（将来ノブの名残・消してよい） |

---

## 極小チップ向けの絞り方（優先順）

1. **大物3つ**（`CFG_MAX_PORTS` / `CFG_EVENT_QUEUE_LEN` / `CFG_CALL_NEST`）を実需まで下げる。ポート表が最大の項、キューとコールスタックが続きます。
2. **2つの文字列長**（`CFG_SSTR_LEN` / `CFG_SARG_LEN`）を締める。乗数なので効果が大きい。
3. `CFG_CALL_NEST` は v0.4.6 でフレームが重い（1段 = 484 B）。**実際の最大呼び出し深度はロード時 DFS で検算済**なので、スクリプトが 1〜2 段しか潜らないなら `2` まで下げて良い（超過はロード時に安全に弾かれる）。

**例**: 既定 13.0 KB から `QUEUE_LEN 16→8`・`SARG_COUNT 4→2`・`CALL_NEST 4→2` にすると、
キュー −1.4 KB / コールスタック −1.0 KB 前後で **≈ 9〜10 KB** に収まります。

> **キューエントリは一律固定サイズ**です。文字列を持たないイベント（BTN/TIMER/周期）も `event_t` の満額を占めます。
> 「文字列イベントだけプールから取る可変方式」は簡易アロケータが要るため将来ノブ扱いです。

---

## 実測してから決める

余裕を見て決めた既定値（プール 1 K・コード 2 K 等）は「まず困らない安全側」の値です。
無駄を削るなら **代表スクリプトを流して実使用量を測る → 少し余裕を足す** のが確実です。

- `CODE_USED`（入力ポート）: 使用中のバイトコード量（= `CFG_CODE_SIZE` の実需）
- `STR_USED`（入力ポート）: 使用中の文字列プール量（= `CFG_STRPOOL_SIZE` の実需）
- アリーナ全体は `sizeof(script_vm_t)`。STM32 ローダは起動バナーに版番号とアリーナサイズを表示します。

```
CODE_USED -> STDOUT      # 例: このスクリプトのバイトコード使用量
STR_USED  -> STDOUT      # 例: 文字列リテラルの使用量
```
