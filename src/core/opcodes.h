/* opcodes.h - スタック型VMのバイトコード命令（仕様 §15 フェーズ1）
 *
 * 可変長エンコード。各命令 = 1バイトopcode + 0個以上のオペランド。
 * オペランドの読み方は命令ごとに固定（vm.c の exec を参照）:
 *   I32 = リトルエンディアン int32（4バイト）
 *   U16 = リトルエンディアン uint16（2バイト, ジャンプ先絶対オフセット）
 *   U8  = 1バイト（スロット添字 / ポート番号 / argc）
 *
 * 論理 AND/OR は「短絡評価」(§9)のため専用opcodeを置かず、
 * コンパイラが JZ/JNZ/JMP で正準bool(0/1)を生成する（compiler.c）。
 *
 * ホスト非依存（純C）コア層。
 */
#ifndef OPCODES_H
#define OPCODES_H

typedef enum {
    OP_HALT       = 0,   /* ブロック終端。実行を終える               */

    /* --- スタックへ積む --- */
    OP_PUSH_INT,         /* I32 : 整数リテラルを積む（const解決後もこれ） */
    OP_PUSH_STR,         /* U16 : 文字列プールオフセットをSTR値で積む    */

    /* --- スロット読み（§4） --- */
    OP_LOAD_GVAR,        /* U8 idx : GVAR[idx] を積む  */
    OP_LOAD_VAR,         /* U8 idx : VAR[idx] を積む   */
    OP_LOAD_ARG,         /* U8 idx : ARG[idx] を積む   */
    OP_LOAD_RESULT,      /*        : RESULT を積む      */

    /* --- ポート読み（§3 def_in / def_inout read） --- */
    OP_LOAD_PORT,        /* U8 portidx : 入力getterを呼び結果(int)を積む */

    /* --- スロット書き（§4：… -> GVAR[k] / VAR[k]） --- */
    OP_STORE_GVAR,       /* U8 idx, U8 argc : argc個popし先頭をGVAR[idx]へ */
    OP_STORE_VAR,        /* U8 idx, U8 argc : 同上 VAR[idx]へ              */

    /* --- 文字列スロット（§4 文字列スロット） --- */
    OP_LOAD_SVAR,        /* U8 idx : SVAR[idx] 参照(SV_SREF)を積む  */
    OP_LOAD_SGVAR,       /* U8 idx : SGVAR[idx] 参照を積む          */
    OP_LOAD_SRESULT,     /*        : SRESULT 参照を積む             */
    OP_LOAD_SARG,        /* U8 idx : SARG[idx] 参照を積む（受信専用, v0.3.5） */
    OP_STORE_SSTR,       /* U8 kind, U8 idx : 先頭1値をstrcpy（切詰+ERR_STR_TRUNC, 自己コピー安全） */

    /* --- 出力ポート呼び（§3 def_out / def_inout write, §5 arity） --- */
    OP_CALL_OUT,         /* U8 portidx, U8 argc : argv=&stack[sp-argc]で呼ぶ。
                            関数ポート(def_out)なら戻り値がRESULTへ（§4） */

    /* --- 比較（§9：> < >= <= == !=） pop2 push bool --- */
    OP_GT, OP_LT, OP_GE, OP_LE, OP_EQ, OP_NE,

    /* --- 算術（§9：+ - * / %） pop2 push。/0 %0 は結果0で継続＋ERR_DIVZERO --- */
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,

    /* --- ビット演算（§9：& | ^ << >>） pop2 push（32bit全体に作用） --- */
    OP_BAND, OP_BOR, OP_BXOR, OP_SHL, OP_SHR,

    /* --- 単項（§9） pop1 push --- */
    OP_NOT,    /* 論理否定（bool 0/1） */
    OP_BNOT,   /* ビット反転 ~        */
    OP_NEG,    /* 単項マイナス -      */

    /* --- STATUS（§12） --- */
    OP_CLEAR_ERR, /* popした値のビットを status からクリア（ERR_xxx -> CLEAR_ERR） */

    /* --- 制御フロー（JZ/JMP + 短絡用JNZ） --- */
    OP_JZ,               /* U16 target : popして0ならtargetへ分岐  */
    OP_JNZ,              /* U16 target : popして非0ならtargetへ分岐（短絡OR用） */
    OP_JMP,              /* U16 target : 無条件分岐                */

    /* --- 制御ポート（§7 WAIT, §8 TIMER） --- */
    OP_YIELD,            /* popした ms 中断（WAIT, MAIN専用） */
    OP_ARM_TIMER,        /* popした ms 後に ON TIMER を1発予約 */
    OP_POST_HANDLER,     /* U8 portidx, U8 argc : 先頭argc値を多値イベントにして ON <ハンドラ源> をpost。
                            HANDLER / MYHANDLER 等の名前付きハンドラ共通。非再帰・次tick, v0.3.7+ */

    /* --- スタック調整 --- */
    OP_POP,              /* 先頭を1つ捨てる（arity正規化で「最左を残す」ため） */

    OP__COUNT
} opcode_t;

#endif /* OPCODES_H */
