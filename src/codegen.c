#include "codegen.h"

#include <string.h>

#include "diag.h"
#include "langinfo.h"
#include "module.h"
#include "sema.h"

#include <stdio.h>

// ★ target triple の定義は langinfo.h に集約しました（--version でも使うため）。

// ── 出力バッファ ────────────────────────────────────────────
// IR は「後から前に戻って書き足したい」ことがあるため、
// 用途別のバッファに分けて最後に連結します。
// 例：関数本体の生成中に文字列リテラルを見つけたら globals に追記する。
typedef struct {
    StrBuf header;   // source_filename, target triple, 型定義
    StrBuf globals;  // グローバル変数・文字列定数
    StrBuf decls;    // declare（外部関数宣言）
    StrBuf body;     // 完成した関数定義

    // ── 生成中の関数用（関数ごとにリセットする）──
    //
    // ★ なぜ alloca を別バッファにするのか（第6章）
    //   変数の alloca は AST を歩けば見つかりますが（collect_allocas）、
    //   短絡評価の結果を入れる %and.result.N は
    //   「ソースに現れない、コンパイラが自分で作る領域」です。
    //   生成してみて初めて必要だと分かるので、専用のバッファに溜めておき、
    //   最後に entry ブロックの先頭へまとめて差し込みます（規約 R1）。
    StrBuf allocas;  // entry ブロックに置く alloca
    StrBuf fn;       // 関数本体の命令列

    int tmp_counter;    // 一時値 %tN の連番
    int label_counter;  // ラベルの連番（同名ラベルの衝突を防ぐ）
    bool terminated;    // 現在の基本ブロックが終端命令を出力済みか（規約 R6）

    // 現在のループ（break / continue の飛び先）。第7章
    struct LoopCtx *loop;

    // ── 第27章：エラー処理 ──
    bool fn_raises;          // 生成中の関数が raises を宣言しているか
    Type *fn_ret;            // 生成中の関数の戻り型（失敗時の既定値に使う）
    bool err_slot;           // この関数で %err.slot を alloca 済みか
    bool err_type_emitted;   // %pl.err の型定義を出したか
    char prop_label[32];     // 伝播ブロックのラベル（err.propagate）
    bool prop_used;          // 伝播ブロックが使われたか（使われたときだけ出す）
    struct TryCtxG *try_ctx; // 今いる try（入れ子になるので鎖）

    // ── 第25章：解放（drop）──
    bool drop;              // --drop（解放を挿入するか）
    struct ScopeCtx *scope; // 今いるスコープ（出口で解放するものの一覧）
    StrBuf dropdefs;        // 生成した @drop.* の定義（モジュール末尾に出す）
    struct StrLit *dropfns; // 生成済みの @drop.* （型ごとに 1 つ）
    int drop_counter;       // @drop.list.N の連番

    // 文字列リテラルの共有と declare の重複排除（第9章）
    struct StrLit *strs;
    struct StrLit *decled;
    int str_counter;

    // ── 第13章：モジュール ──
    Node *ast;             // 今生成しているモジュールの AST
    struct StrLit *types;  // 型定義を出済みのクラス（重複排除）
} Emitter;

// 出力済みの文字列リテラル / declare を覚えておくための小さなリスト。
typedef struct StrLit StrLit;
struct StrLit {
    char *bytes;
    int len;
    char *label;
    StrLit *next;
};

// break / continue の飛び先（規約 6.5）。
//
// ★ スタック変数として持つのがポイントです。gen_while の呼び出しがネストすれば、
//   C の呼び出しスタックがそのままループのネストになります。
//   自前でスタック構造を作る必要はありません。
typedef struct LoopCtx LoopCtx;
struct LoopCtx {
    LoopCtx *outer;
    const char *break_label;     // while.end.N
    const char *continue_label;  // while.cond.N
    struct ScopeCtx *scope;      // ループに入ったときのスコープ（第25章）
};

// try 1 つぶんの飛び先（第27章）。
//
// ★ LoopCtx と同じ形です。失敗した呼び出しは、内側の try の振り分けへ飛びます。
//   その try が捕まえない型なら、振り分けの最後で外側の飛び先へ落ちます。
typedef struct TryCtxG TryCtxG;
struct TryCtxG {
    TryCtxG *outer;
    Node *node;              // ND_TRY
    const char *dispatch;    // try.dispatch.N（失敗したときの飛び先）
    struct ScopeCtx *scope;  // try に入ったときのスコープ（解放の巻き戻しに使う）
};

// ── スコープ（第25章）──────────────────────────────────────
//
// ★ LoopCtx と同じで、C の呼び出しスタックにそのまま乗せます。
//   ブロックに入ったら push、出るときに **宣言と逆順**で解放します。
typedef struct DropEnt DropEnt;
struct DropEnt {
    Node *decl;      // ND_VARDECL / ND_PARAM（ir_name と type を持っている）
    DropEnt *next;   // ★ 先頭に足すので、たどると自然に「逆順」になります
};

typedef struct ScopeCtx ScopeCtx;
struct ScopeCtx {
    ScopeCtx *outer;
    DropEnt *ents;
};

// ── 第27章：エラー処理の道具（実体は下のほうにあります）──
static void ensure_err_type(Emitter *e);
static const char *err_slot(Emitter *e);
static const char *fail_label(Emitter *e);
static void emit_fail_br(Emitter *e);
static const char *default_value(Type *t);
static void store_err(Emitter *e, const char *slot, int tag, const char *obj);
static char *load_tag(Emitter *e, const char *slot);
static char *load_payload(Emitter *e, const char *slot);
static void emit_drops_until(Emitter *e, struct ScopeCtx *stop);
static void emit_default_ret(Emitter *e);
static char *deref_rc(Emitter *e, Type *t, char *v);  // 第28章
static char *maybe_retain(Emitter *e, Node *rhs, char *val);

// 新しい一時値の名前を返す（"%t0", "%t1", ...）
//
// ⚠️ 規約 R4：必ず英字始まりの名前にします。
//    %0 のような数値名を自分で使うと、LLVM の暗黙採番と衝突して
//    "instruction expected to be numbered '%N'" という分かりにくい
//    エラーになります。
// ⚠️ 第17章：名前に '.' を入れます。
//    それまでは "%tN" でしたが、利用者が `t0` という変数を書くと
//    IR 上で衝突しました（stage1 の移植中に踏んだ実際のバグ）。
//
//        %t0 = alloca ptr      ← 利用者の変数 t0
//        %t0 = load ptr, ...   ← コンパイラの一時値
//
//    利用者の識別子に '.' は入れられないので、'.' を含む名前にすれば
//    衝突は原理的に起きません（第11章の隠し変数 for.ix.0 と同じ手口）。
static char *new_tmp(Emitter *e) {
    char *buf = xmalloc(24);
    snprintf(buf, 24, "%%t.%d", e->tmp_counter++);
    return buf;
}

// 値（レジスタ）としての LLVM 型。
//
// ★ 「値の型」と「メモリの型」を別の関数にするのが第6章の要点です。
//   1 つの関数で済ませようとすると、呼び出し側ごとに
//   「今はどっちの意味か」を考えることになり、必ず間違えます。
static const char *llvm_type(Type *t) {
    switch (t->kind) {
        case TY_INT: return "i64";
        case TY_FLOAT: return "double";  // IEEE 754 倍精度
        case TY_BOOL: return "i1";   // レジスタ上は 1 ビット
        case TY_NONE: return "void";  // 値がない（第8章）
        case TY_STR: return "ptr";    // 参照型（第9章）
        case TY_LIST: return "ptr";   // PlList へのポインタ（第10章）
        case TY_CLASS: return "ptr";  // インスタンスへのポインタ（第12章）
        case TY_OPT: return "ptr";    // T | None（第15章）。None は null
        case TY_RC: return "ptr";     // rc[T]（第28章）
        case TY_PTR: return "ptr";    // ptr[T]（第30章。生ポインタ）
        case TY_NULL: return "ptr";   // None リテラル
        default: UNREACHABLE();
    }
}

// メモリ（alloca / load / store）としての LLVM 型。
//
// ⚠️ 規約 R5：bool はメモリ上 i8。
//    alloca i1 も合法ですが、実際には 1 バイト確保され残り 7 ビットが未定義に
//    なります。第9章の C ランタイム連携で困るので、i8 に揃えておきます。
static const char *llvm_mem_type(Type *t) {
    switch (t->kind) {
        case TY_INT: return "i64";
        case TY_FLOAT: return "double";
        case TY_BOOL: return "i8";  // メモリ上は 1 バイト
        case TY_STR: return "ptr";  // ポインタをそのまま置く（第9章）
        case TY_LIST: return "ptr"; // 第10章
        case TY_CLASS: return "ptr";  // 第12章
        case TY_OPT: return "ptr";    // 第15章
        case TY_RC: return "ptr";     // 第28章（数え札付きの箱へのポインタ）
        case TY_PTR: return "ptr";    // 第30章
        // ⚠️ TY_NONE はメモリ上の表現を持ちません。
        //    ここに来たら「None の変数を作ろうとしている」= コンパイラのバグ。
        default: UNREACHABLE();
    }
}

// ★ 第8章：変数の IR 名は sema が「記号まで含めた完全な形」で割り当てます。
//    ローカル  : %x, %x.1
//    グローバル: @g.x
//    引数      : %n（%n.arg から alloca にコピーしたもの。規約 R8）
//    codegen 側で名前を組み立てる必要はもうありません（var_ptr は廃止）。

// 二項演算子に対応する LLVM 命令の名前を返す。
//
// ⚠️ int は符号付きなので、必ず 's' の付く命令を使います。
//    sdiv / srem / ashr（udiv / urem / lshr ではない）。
//    間違えると負数で誤った結果になります。
static const char *llvm_binop(Node *n) {
    // ★ float は別命令です。整数の add / sub / mul / sdiv とは
    //   ビット列の意味がまるで違うので、LLVM も別の命令を用意しています。
    //   （fadd などに 's' が付かないのは、符号が指数部と仮数部に
    //     分かれていて「符号付き/なし」の区別が要らないためです）
    if (n->lhs && n->lhs->type && n->lhs->type->kind == TY_FLOAT) {
        switch (n->op) {
            case OP_ADD: return "fadd";
            case OP_SUB: return "fsub";
            case OP_MUL: return "fmul";
            case OP_TRUEDIV: return "fdiv";
            default: UNREACHABLE();  // OP_POW はランタイム呼び出しになる
        }
    }
    switch (n->op) {
        case OP_ADD: return "add";
        case OP_SUB: return "sub";
        case OP_MUL: return "mul";
        // OP_FLOORDIV / OP_MOD / OP_POW はここに来ません。
        // ★ 第9章で「0 除算・負の指数を検査する」ためにランタイム関数
        //   （pl_floordiv / pl_mod / pl_ipow）の呼び出しに変わりました（規約 R10）。
        case OP_BITAND: return "and";
        case OP_BITOR: return "or";
        case OP_BITXOR: return "xor";
        case OP_SHL: return "shl";
        case OP_SHR: return "ashr";  // 算術シフト（符号を保つ）

        // OP_TRUEDIV はここに来ません。
        // ★ 第2章ではこの関数で弾いていましたが、第5章で意味解析パスを
        //   作ったので、本来の担当である sema.c へ移しました。
        //   コード生成器は「検査済みの正しい AST」だけを受け取る、
        //   という役割分担がここで確立します。
        default:
            UNREACHABLE();
    }
}

// 比較演算子に対応する icmp の述語。
//
// ⚠️ 落とし穴：i1 の符号付き比較は逆になる
//    i1 を 2 の補数で解釈すると True(1) は -1 です。
//    icmp slt i1 0, 1 は「0 < -1」を聞くことになり False になります。
//    そのため bool の大小比較は符号なし（ult など）を使います。
//    eq / ne には符号が無いので影響しません。
static const char *icmp_pred(OpKind op, Type *operand_type) {
    bool sign = operand_type->kind == TY_INT;  // int は符号付き
    switch (op) {
        case OP_EQ: return "eq";
        case OP_NE: return "ne";
        case OP_LT: return sign ? "slt" : "ult";
        case OP_LE: return sign ? "sle" : "ule";
        case OP_GT: return sign ? "sgt" : "ugt";
        case OP_GE: return sign ? "sge" : "uge";
        default: UNREACHABLE();
    }
}

// float の比較述語（ordered。NaN が絡むと必ず False）。
static const char *fcmp_pred(OpKind op) {
    switch (op) {
        case OP_EQ: return "oeq";
        // ⚠️ '!=' だけ **unordered**（une）です。
        //    a != b は「a == b ではない」と定義されるので、NaN が絡むと
        //    == が False → != は True でなければなりません。
        //    ここを one（ordered）にすると nan != nan が False になり、
        //    NaN の判定（x != x）が使えなくなります。
        case OP_NE: return "une";
        case OP_LT: return "olt";
        case OP_LE: return "ole";
        case OP_GT: return "ogt";
        case OP_GE: return "oge";
        default: UNREACHABLE();
    }
}

// ── メモリとレジスタの境界（規約 R5）──────────────────────
//
// ★ zext / trunc はこの 2 つの関数の中だけに閉じ込めます。
//   他の場所には 1 つも現れません。

// メモリから読む：bool なら i8 → i1 に縮める
static char *gen_load(Emitter *e, Type *ty, const char *ptr) {
    char *t = new_tmp(e);
    sb_printf(&e->fn, "  %s = load %s, ptr %s\n", t, llvm_mem_type(ty), ptr);
    if (ty->kind != TY_BOOL) return t;

    char *t2 = new_tmp(e);
    sb_printf(&e->fn, "  %s = trunc i8 %s to i1\n", t2, t);
    return t2;
}

// メモリへ書く：bool なら i1 → i8 に広げる
static void gen_store(Emitter *e, Type *ty, const char *val, const char *ptr) {
    if (ty->kind == TY_BOOL) {
        char *t = new_tmp(e);
        sb_printf(&e->fn, "  %s = zext i1 %s to i8\n", t, val);
        val = t;
    }
    sb_printf(&e->fn, "  store %s %s, ptr %s\n", llvm_mem_type(ty), val, ptr);
}

// ── 基本ブロック（規約 R6 / R7）────────────────────────────
//
// ⚠️ IR にフォールスルーはありません。「次のブロックに続くだけ」でも
//    br label %next が必要です。初心者が最もよくハマる落とし穴です。
//
// ★ 「終端したか」を追跡する変数を 1 つ持つだけで、
//   「br を書き忘れた」も「終端の後に命令を置いた」も起きなくなります。
//   第7章の if / while、第8章の return はこの 3 つの関数の上に載ります。

// ラベルを出力する。直前のブロックが終端していなければ暗黙のジャンプを補う。
static void emit_label(Emitter *e, const char *label) {
    if (!e->terminated) sb_printf(&e->fn, "  br label %%%s\n", label);
    sb_printf(&e->fn, "%s:\n", label);
    e->terminated = false;
}

static void emit_br(Emitter *e, const char *label) {
    sb_printf(&e->fn, "  br label %%%s\n", label);
    e->terminated = true;
}

static void emit_cond_br(Emitter *e, const char *cond, const char *then_l,
                         const char *else_l) {
    sb_printf(&e->fn, "  br i1 %s, label %%%s, label %%%s\n", cond, then_l, else_l);
    e->terminated = true;
}

// 終端済みのブロックの後ろにコードを置く必要が出たら、
// 到達不能ブロックのラベルを作る（規約 R7）。
//
//     while True:
//         break
//         print(1)     ← 到達不能。ラベルが無いと命令を置けない
static void ensure_block(Emitter *e) {
    if (!e->terminated) return;
    char l[24];
    snprintf(l, sizeof(l), "dead.%d", e->label_counter++);
    emit_label(e, l);  // 終端済みなので br は補われない
}

// ── 文字列リテラル（第9章）────────────────────────────────
//
// ★ 同じ内容のリテラルは 1 つにまとめます（線形探索で十分）。

// IR の文字列に 1 バイト出力する。
//
// ⚠️ 安全策として、ASCII 印字可能文字**以外はすべて** \XX にします。
//    「どの文字をエスケープすべきか」を考えなくて済むようにするためです。
//    UTF-8 の日本語も各バイトが \XX になるだけで、そのまま通ります。
static void emit_ir_byte(StrBuf *sb, unsigned char c) {
    if (c >= 0x20 && c < 0x7F && c != '"' && c != '\\')
        sb_printf(sb, "%c", c);
    else
        sb_printf(sb, "\\%02X", c);
}

static char *intern_str(Emitter *e, const char *bytes, int len) {
    for (StrLit *sl = e->strs; sl; sl = sl->next)
        if (sl->len == len && memcmp(sl->bytes, bytes, (size_t)len) == 0)
            return sl->label;

    StrBuf lab;
    sb_init(&lab);
    sb_printf(&lab, "@.str.%d", e->str_counter++);

    // ★ 第15章：str は「長さ + バイト列」になりました（ランタイム参照）。
    //   リテラルも同じ形で出し、値としてはデータ部を指すポインタを使います。
    //
    //     @.str.0 = private unnamed_addr constant { i64, [6 x i8] }
    //                 { i64 5, [6 x i8] c"hello\00" }
    //
    // ⚠️ 配列の長さは「バイト数 + 1」。NUL の分を忘れない。
    StrBuf g;
    sb_init(&g);
    // ★ 第25章：--drop のときは長さのヘッダに「静的」の印を立てます。
    //   リテラルは .rodata にあるので、解放しようとすると落ちるためです
    //   （runtime.c の PL_STR_STATIC を参照）。
    //
    // ⚠️ 印を立てるのは --drop のときだけです。既定の出力は v1 と 1 バイトも
    //    変えない、というのがこの章の約束なので（第29章で移植したら常に立てます）。
    long long hdr = e->drop ? ((long long)len | (1LL << 62)) : (long long)len;
    sb_printf(&g, "%s = private unnamed_addr constant { i64, [%d x i8] } "
                  "{ i64 %lld, [%d x i8] c\"",
              sb_str(&lab), len + 1, hdr, len + 1);
    for (int i = 0; i < len; i++) emit_ir_byte(&g, (unsigned char)bytes[i]);
    sb_printf(&g, "\\00\" }\n");
    sb_printf(&e->globals, "%s", sb_str(&g));

    // 値として使うのはデータ部のアドレス（定数式でそのまま書ける）
    StrBuf ref;
    sb_init(&ref);
    sb_printf(&ref, "getelementptr inbounds ({ i64, [%d x i8] }, ptr %s, i32 0, i32 1)",
              len + 1, sb_str(&lab));

    StrLit *sl = xmalloc(sizeof(StrLit));
    sl->bytes = (char *)bytes;
    sl->len = len;
    sl->label = sb_str(&ref);
    sl->next = e->strs;
    e->strs = sl;
    return sl->label;
}

// ランタイム関数を宣言する（1 回だけ）。
static const char *class_type(Emitter *e, Class *c);  // 第13章
static void declare_extern(Emitter *e, const char *ret, const char *ir_name,
                           const char *param_types);

static void declare_rt(Emitter *e, const char *sig) {
    for (StrLit *d = e->decled; d; d = d->next)
        if (strcmp(d->label, sig) == 0) return;
    sb_printf(&e->decls, "declare %s\n", sig);
    StrLit *d = xmalloc(sizeof(StrLit));
    d->label = (char *)sig;
    d->next = e->decled;
    e->decled = d;
}

// ── 式の生成 ────────────────────────────────────────────────
//
// gen_expr の約束：
//   「式を評価する命令列を body に出力し、
//     結果の値が入っている場所の名前（レジスタ名 or 即値）を返す」
//
// この 1 つの約束が、コード生成器の設計全体を決めます。
// 即値（"42"）とレジスタ（"%t0"）を同じ char * で扱えるので、
// 呼び出し側で場合分けが不要になります。
static char *gen_logical(Emitter *e, Node *n);
static char *gen_cond(Emitter *e, Node *n);      // 第37章：三項演算子
static char *gen_in(Emitter *e, Node *n);        // 第37章：in / not in
static char *gen_slice(Emitter *e, Node *n);     // 第37章：スライス
static bool elem_is_ptr(Type *elem);
static char *elem_to_slot_cmp(Emitter *e, Type *elem, char *v);
static char *gen_call(Emitter *e, Node *n);
static char *gen_list_lit(Emitter *e, Node *n);
static char *gen_index(Emitter *e, Node *n);
static char *gen_method(Emitter *e, Node *n);
static char *gen_field(Emitter *e, Node *n);

static char *gen_expr(Emitter *e, Node *n) {
    switch (n->kind) {
        case ND_FLOAT:
            // ★ 字句解析器が正規化した文字列を**そのまま**出します。
            //   double を経由しないので、C 版と Polonium 版で 1 バイトも
            //   ずれません（tests/selfhost.sh の IR 比較が効きます）。
            return n->sval;

        case ND_INT: {
            // 整数リテラルは命令を出す必要すらありません。
            // LLVM は即値をオペランドに直接書けるので（add i64 42, 1）、
            // 「42」という文字列をそのまま返します。
            char *buf = xmalloc(24);
            snprintf(buf, 24, "%lld", n->ival);
            return buf;
        }

        case ND_BINOP: {
            // ★ 第37章：in / not in。⚠️ **左右をここで評価してはいけません。**
            //   渡す形が要素の型で変わるので、gen_in の中で作ります。
            if (n->op == OP_IN || n->op == OP_NOTIN) return gen_in(e, n);

            // ★ 第15章：is / is not は「null と比べる」だけ。1 命令で済みます。
            if (n->op == OP_IS || n->op == OP_ISNOT) {
                char *v = gen_expr(e, n->lhs);
                char *t = new_tmp(e);
                sb_printf(&e->fn, "  %s = icmp %s ptr %s, null\n", t,
                          n->op == OP_IS ? "eq" : "ne", v);
                return t;
            }

            // ★ 左辺 → 右辺の順に生成する（仕様 4.5：評価順は左から右）
            char *l = gen_expr(e, n->lhs);
            char *r = gen_expr(e, n->rhs);
            char *t = new_tmp(e);

            // ⚠️ オペランドの型は「結果の型」ではありません。
            //    比較の結果は bool ですが、比べているのは左辺の型（int など）です。
            //    第5章までは両者が一致していたので llvm_type(n->type) で
            //    動いていました。比較演算子で初めてこの前提が崩れます。
            Type *ot = n->lhs->type;

            // ── 文字列（第9章）──────────────────────────────
            if (ot->kind == TY_STR) {
                if (n->op == OP_ADD) {
                    declare_rt(e, "ptr @pl_str_concat(ptr, ptr)");
                    sb_printf(&e->fn, "  %s = call ptr @pl_str_concat(ptr %s, ptr %s)\n",
                              t, l, r);
                    return t;
                }
                // ⚠️ 比較は「内容」で行う（言語仕様 4.3）。ポインタ比較ではない。
                //   pl_str_cmp が strcmp の符号を返すので、0 と比べる述語を
                //   変えるだけで 6 種類すべてに対応できます。
                declare_rt(e, "i64 @pl_str_cmp(ptr, ptr)");
                char *c = new_tmp(e);
                sb_printf(&e->fn, "  %s = call i64 @pl_str_cmp(ptr %s, ptr %s)\n", c,
                          l, r);
                sb_printf(&e->fn, "  %s = icmp %s i64 %s, 0\n", t,
                          icmp_pred(n->op, ty_int), c);
                return t;
            }

            // ── 検査つきの算術（規約 R10。第9章）──────────────
            //
            // ★ 0 除算は SIGFPE でプロセスが死にます。何が起きたか分からない
            //   より、メッセージを出して死ぬほうが親切です。分岐を IR に出さず、
            //   ランタイム関数に押し込むのが R10 の実践です。
            if (n->op == OP_FLOORDIV || n->op == OP_MOD || n->op == OP_POW) {
                const char *fn = n->op == OP_FLOORDIV ? "pl_floordiv"
                                 : n->op == OP_MOD    ? "pl_mod"
                                                      : "pl_ipow";
                StrBuf sig;
                sb_init(&sig);
                sb_printf(&sig, "i64 @%s(i64, i64)", fn);
                declare_rt(e, sb_str(&sig));
                sb_printf(&e->fn, "  %s = call i64 @%s(i64 %s, i64 %s)\n", t, fn, l, r);
                return t;
            }

            // ★ float の比較は fcmp です。しかも述語に 'o'（ordered）を
            //   付けます。NaN が絡むと「どちらでもない」が正しい答えなので、
            //   ordered を選ぶと NaN との比較はすべて False になります
            //   （unordered の 'u' を選ぶと逆にすべて True になり、
            //     NaN != NaN が成り立たなくなります）。
            if (is_compare(n->op) && ot->kind == TY_FLOAT)
                sb_printf(&e->fn, "  %s = fcmp %s double %s, %s\n", t,
                          fcmp_pred(n->op), l, r);
            else if (is_compare(n->op))
                sb_printf(&e->fn, "  %s = icmp %s %s %s, %s\n", t,
                          icmp_pred(n->op, ot), llvm_type(ot), l, r);
            else
                sb_printf(&e->fn, "  %s = %s %s %s, %s\n", t, llvm_binop(n),
                          llvm_type(ot), l, r);
            return t;
        }

        case ND_BOOL: {
            // True / False は i1 の即値。LLVM は "true" / "false" と書けます。
            return n->ival ? "true" : "false";
        }

        case ND_NONE:
            // ★ 第15章：None は「null というポインタ即値」。命令は出ません。
            return "null";

        case ND_STR:
            // ★ リテラルは .rodata の定数。ラベルをそのまま ptr として使えます
            //   （opaque pointer なので getelementptr は不要）。
            return intern_str(e, n->sval, n->slen);

        case ND_LOGICAL:
            return gen_logical(e, n);

        case ND_COND:
            return gen_cond(e, n);

        case ND_CALL:
            return gen_call(e, n);

        case ND_LIST:
            return gen_list_lit(e, n);

        case ND_INDEX:
            return gen_index(e, n);

        case ND_SLICE:
            return gen_slice(e, n);

        case ND_METHOD:
            return gen_method(e, n);

        case ND_FIELD:
            return gen_field(e, n);

        case ND_VAR: {
            // 変数の読み出し（規約 R2）。bool なら i8 → i1 の変換も入る。
            // ★ n->name ではなく sema が割り当てた n->ir_name を使う（第7章）
            char *v = gen_load(e, n->type, n->ir_name);

            // ★ 第25章：ここで所有権が移ったなら、スロットに null を書きます。
            //   これが drop フラグの代わりです（設計 §6.3 の見直し。決定 D17）。
            if (e->drop && n->moved_out && !n->is_global)
                sb_printf(&e->fn, "  store ptr null, ptr %s\n", n->ir_name);
            return v;
        }

        case ND_UNARY: {
            char *v = gen_expr(e, n->lhs);

            // +x は何もしない（値をそのまま返す）
            if (n->op == OP_POS) return v;

            char *t = new_tmp(e);
            if (n->op == OP_NEG && n->type && n->type->kind == TY_FLOAT) {
                // ★ float には専用の否定命令 fneg があります（符号ビットを
                //   反転するだけ。0.0 - x とは -0.0 の扱いが違います）。
                sb_printf(&e->fn, "  %s = fneg double %s\n", t, v);
            } else if (n->op == OP_NEG) {
                // ⚠️ LLVM に整数の neg 命令はありません。0 からの減算で表現します。
                sb_printf(&e->fn, "  %s = sub i64 0, %s\n", t, v);
            } else if (n->op == OP_BITNOT) {
                // ~x は全ビット反転 = x XOR -1（-1 は全ビット 1）
                sb_printf(&e->fn, "  %s = xor i64 %s, -1\n", t, v);
            } else if (n->op == OP_NOT) {
                // not x は x XOR true（~x と同じ発想。幅が 1 ビットになっただけ）
                sb_printf(&e->fn, "  %s = xor i1 %s, true\n", t, v);
            } else {
                UNREACHABLE();
            }
            return t;
        }

        default:
            UNREACHABLE();
    }
}

// ── 短絡評価（規約 6.6）────────────────────────────────────
//
// ★ この章で初めて基本ブロックを分岐させます。
//
//   a and b  … a が偽なら b を評価せずに偽
//   a or  b  … a が真なら b を評価せずに真
//
//   「評価しない」を実現するには命令を飛び越える必要があるので、分岐が要ります。
//
// 🤔 なぜ phi を使わないのか（規約 R3）
//   教科書的には合流点で phi を使いますが、phi は「どのブロックから来たか」を
//   書く必要があり、生成側が前のブロックのラベルを覚えていなければなりません。
//   ネストすると管理が急激に面倒になります。
//   「alloca に置いて最後に読む」方式ならその面倒がゼロで、
//   mem2reg がこの alloca を phi に変換してくれます。
static char *gen_logical(Emitter *e, Node *n) {
    // ⚠️ 番号は最初に 1 回だけ確保する。
    //    使うたびに e->label_counter++ すると同じ and の中で番号がずれます。
    int id = e->label_counter++;
    const char *kind = n->op == OP_AND ? "and" : "or";

    char rhs_l[32], end_l[32], res[40];
    snprintf(rhs_l, sizeof(rhs_l), "%s.rhs.%d", kind, id);
    snprintf(end_l, sizeof(end_l), "%s.end.%d", kind, id);
    snprintf(res, sizeof(res), "%%%s.result.%d", kind, id);

    // 結果を入れる箱。★ alloca は entry ブロックへ（規約 R1）
    sb_printf(&e->allocas, "  %s = alloca i8\n", res);

    // ① 左辺を評価し、その値をいったん結果として置く
    char *l = gen_expr(e, n->lhs);
    gen_store(e, ty_bool, l, res);

    // ② 右辺を評価すべきか分岐する（and と or で真偽が逆）
    if (n->op == OP_AND)
        emit_cond_br(e, l, rhs_l, end_l);
    else
        emit_cond_br(e, l, end_l, rhs_l);

    // ③ 右辺（飛ばされることがあるブロック）
    emit_label(e, rhs_l);
    char *r = gen_expr(e, n->rhs);
    gen_store(e, ty_bool, r, res);
    emit_br(e, end_l);

    // ④ 合流点
    emit_label(e, end_l);
    return gen_load(e, ty_bool, res);
}

// xs[a:b] / s[a:b]（第37章）
//
// ★ 省略された端は「先頭（0）」と「末尾（長さ）」に置き換えてから、
//   ランタイムの 1 つの関数に渡します。範囲の丸めもランタイム側の仕事です
//   （規約 R10：分岐を IR に出さない）。
static char *gen_slice(Emitter *e, Node *n) {
    bool is_str = n->lhs->type->kind == TY_STR;
    char *obj = gen_expr(e, n->lhs);

    char *lo = n->rhs ? gen_expr(e, n->rhs) : "0";

    char *hi;
    if (n->els) {
        hi = gen_expr(e, n->els);
    } else {
        // 終端の省略は「長さ」
        declare_rt(e, is_str ? "i64 @pl_str_len(ptr)" : "i64 @pl_list_len(ptr)");
        hi = new_tmp(e);
        sb_printf(&e->fn, "  %s = call i64 @%s(ptr %s)\n", hi,
                  is_str ? "pl_str_len" : "pl_list_len", obj);
    }

    declare_rt(e, is_str ? "ptr @pl_str_slice(ptr, i64, i64)"
                         : "ptr @pl_list_slice(ptr, i64, i64)");
    char *t = new_tmp(e);
    sb_printf(&e->fn, "  %s = call ptr @%s(ptr %s, i64 %s, i64 %s)\n", t,
              is_str ? "pl_str_slice" : "pl_list_slice", obj, lo, hi);
    return t;
}

// x in xs / sub in s（第37章）
//
// ★ どちらも **位置を返すランタイム関数**（見つからなければ -1）に落として、
//   その結果を 0 と比べるだけにします。IR に分岐が 1 つも出ません（規約 R10）。
static char *gen_in(Emitter *e, Node *n) {
    Type *rt = n->rhs->type;
    char *pos = new_tmp(e);

    if (rt->kind == TY_STR) {
        declare_rt(e, "i64 @pl_str_find(ptr, ptr)");
        char *hay = gen_expr(e, n->rhs);
        char *nee = gen_expr(e, n->lhs);
        sb_printf(&e->fn, "  %s = call i64 @pl_str_find(ptr %s, ptr %s)\n",
                  pos, hay, nee);
    } else {
        // list[T]。要素の型で呼び分けます
        Type *el = rt->elem;
        const char *fn_name;
        const char *aty;
        if (el->kind == TY_FLOAT) {
            fn_name = "pl_list_index_f64";
            aty = "double";
        } else if (el->kind == TY_STR) {
            fn_name = "pl_list_index_str";
            aty = "ptr";
        } else if (elem_is_ptr(el)) {
            fn_name = "pl_list_index_ptr";
            aty = "ptr";
        } else {
            fn_name = "pl_list_index_i64";
            aty = "i64";
        }
        StrBuf sig;
        sb_init(&sig);
        sb_printf(&sig, "i64 @%s(ptr, %s)", fn_name, aty);
        declare_rt(e, sb_str(&sig));

        char *lst = gen_expr(e, n->rhs);
        char *v = elem_to_slot_cmp(e, el, gen_expr(e, n->lhs));
        sb_printf(&e->fn, "  %s = call i64 @%s(ptr %s, %s %s)\n", pos, fn_name,
                  lst, aty, v);
    }

    // 見つかった（>= 0）か。not in なら逆にする
    char *t = new_tmp(e);
    sb_printf(&e->fn, "  %s = icmp %s i64 %s, 0\n", t,
              n->op == OP_IN ? "sge" : "slt", pos);
    return t;
}

// 三項演算子 a if c else b（第37章）
//
// ★ 作りは and / or の短絡評価とまったく同じです。**箱を 1 つ用意して、
//   選ばれた側だけがそこに書く**（規約 R3：phi を使わない）。
//   選ばれなかった側は **評価もされません**（Python と同じ）。
static char *gen_cond(Emitter *e, Node *n) {
    int id = e->label_counter++;
    char then_l[32], else_l[32], end_l[32], res[40];
    snprintf(then_l, sizeof(then_l), "cond.then.%d", id);
    snprintf(else_l, sizeof(else_l), "cond.else.%d", id);
    snprintf(end_l, sizeof(end_l), "cond.end.%d", id);
    snprintf(res, sizeof(res), "%%cond.result.%d", id);

    sb_printf(&e->allocas, "  %s = alloca %s\n", res, llvm_mem_type(n->type));

    char *c = gen_expr(e, n->lhs);
    emit_cond_br(e, c, then_l, else_l);

    emit_label(e, then_l);
    gen_store(e, n->type, gen_expr(e, n->rhs), res);
    emit_br(e, end_l);

    emit_label(e, else_l);
    gen_store(e, n->type, gen_expr(e, n->els), res);
    emit_br(e, end_l);

    emit_label(e, end_l);
    return gen_load(e, n->type, res);
}

// ── list[T] の生成（第10章）────────────────────────────────
//
// ★ 要素はすべて 8 バイト。i64 で持つか、ポインタで持つかの 2 通りだけです。
static bool elem_is_ptr(Type *elem) {
    // ★ 第12章：クラスも参照（ポインタ）なので、ここに 1 語足すだけで
    //   list[Token] が動きます。第10章の設計がそのまま効いています。
    // ★ 第15章：T | None もポインタ（None は null）。
    // ★ 第28章：rc[T] もポインタ（数え札付きの箱を指す）。
    return elem->kind == TY_STR || elem->kind == TY_LIST ||
           elem->kind == TY_CLASS || elem->kind == TY_OPT ||
           elem->kind == TY_RC || elem->kind == TY_NULL;
}

// 要素の値を「ランタイムに渡す形」にする（bool は i64 に広げる。規約 R5）
// 探索関数に渡す形にする。
// ⚠️ elem_to_slot と違い、**float は double のまま**渡します
//   （数値として比べたいので、ビットに崩しません）。
static char *elem_to_slot_cmp(Emitter *e, Type *elem, char *v) {
    if (elem->kind != TY_BOOL) return v;
    char *t = new_tmp(e);
    sb_printf(&e->fn, "  %s = zext i1 %s to i64\n", t, v);
    return t;
}

static char *elem_to_slot(Emitter *e, Type *elem, char *v) {
    // ★ float はビットパターンのまま i64 のスロットに入れます。
    //   list の中身は「ポインタ 1 個か i64 1 個」という第10章の作りを
    //   変えずに済みます（double も 8 バイトなので過不足なく入ります）。
    //   ⚠️ 数値としての変換（sitofp）ではありません。bitcast です。
    if (elem->kind == TY_FLOAT) {
        char *t = new_tmp(e);
        sb_printf(&e->fn, "  %s = bitcast double %s to i64\n", t, v);
        return t;
    }
    if (elem->kind != TY_BOOL) return v;
    char *t = new_tmp(e);
    sb_printf(&e->fn, "  %s = zext i1 %s to i64\n", t, v);
    return t;
}

// ランタイムから受け取った値を「Polonium の値」に戻す（bool は i1 に縮める）
static char *slot_to_elem(Emitter *e, Type *elem, char *v) {
    if (elem->kind == TY_FLOAT) {
        char *t = new_tmp(e);
        sb_printf(&e->fn, "  %s = bitcast i64 %s to double\n", t, v);
        return t;
    }
    if (elem->kind != TY_BOOL) return v;
    char *t = new_tmp(e);
    sb_printf(&e->fn, "  %s = trunc i64 %s to i1\n", t, v);
    return t;
}

static const char *slot_ty(Type *elem) { return elem_is_ptr(elem) ? "ptr" : "i64"; }

// [1, 2, 3] は「空リストを作って append を繰り返す」に脱糖する。
// ★ 第5章の複合代入、第7章の elif と同じ「脱糖」の手です。
static char *gen_list_lit(Emitter *e, Node *n) {
    Type *elem = n->type->elem;

    declare_rt(e, "ptr @pl_list_new()");
    char *l = new_tmp(e);
    sb_printf(&e->fn, "  %s = call ptr @pl_list_new()\n", l);

    const char *sty = slot_ty(elem);
    const char *push = elem_is_ptr(elem) ? "pl_list_push_ptr" : "pl_list_push_i64";
    StrBuf sig;
    sb_init(&sig);
    sb_printf(&sig, "void @%s(ptr, %s)", push, sty);
    declare_rt(e, sb_str(&sig));

    for (Node *el = n->body; el; el = el->next) {
        char *v = elem_to_slot(e, elem, gen_expr(e, el));
        sb_printf(&e->fn, "  call void @%s(ptr %s, %s %s)\n", push, l, sty, v);
    }
    return l;
}

static char *gen_index(Emitter *e, Node *n) {
    Type *ot = n->lhs->type;
    char *obj = gen_expr(e, n->lhs);
    char *idx = gen_expr(e, n->rhs);

    // str の添字は 1 文字の str を返す（型システム 5.8）
    if (ot->kind == TY_STR) {
        declare_rt(e, "ptr @pl_str_index(ptr, i64)");
        char *t = new_tmp(e);
        sb_printf(&e->fn, "  %s = call ptr @pl_str_index(ptr %s, i64 %s)\n", t, obj,
                  idx);
        return t;
    }

    Type *elem = ot->elem;
    const char *sty = slot_ty(elem);
    const char *get = elem_is_ptr(elem) ? "pl_list_get_ptr" : "pl_list_get_i64";
    StrBuf sig;
    sb_init(&sig);
    sb_printf(&sig, "%s @%s(ptr, i64)", sty, get);
    declare_rt(e, sb_str(&sig));

    char *t = new_tmp(e);
    sb_printf(&e->fn, "  %s = call %s @%s(ptr %s, i64 %s)\n", t, sty, get, obj, idx);
    return slot_to_elem(e, elem, t);
}

static void gen_index_store(Emitter *e, Node *target, char *val) {
    Type *elem = target->lhs->type->elem;
    char *obj = gen_expr(e, target->lhs);
    char *idx = gen_expr(e, target->rhs);

    const char *sty = slot_ty(elem);
    const char *set = elem_is_ptr(elem) ? "pl_list_set_ptr" : "pl_list_set_i64";
    StrBuf sig;
    sb_init(&sig);
    sb_printf(&sig, "void @%s(ptr, i64, %s)", set, sty);
    declare_rt(e, sb_str(&sig));

    char *v = elem_to_slot(e, elem, val);
    sb_printf(&e->fn, "  call void @%s(ptr %s, i64 %s, %s %s)\n", set, obj, idx, sty,
              v);
}

static char *gen_new(Emitter *e, Node *n);

// 引数を評価して "型 値, 型 値" と "型, 型"（declare 用）を同時に作る
static void gen_args(Emitter *e, Node *args, StrBuf *vals, StrBuf *types,
                     bool first) {
    for (Node *a = args; a; a = a->next) {
        char *v = gen_expr(e, a);
        sb_printf(vals, "%s%s %s", first ? "" : ", ", llvm_type(a->type), v);
        sb_printf(types, "%s%s", first ? "" : ", ", llvm_type(a->type));
        first = false;
    }
}

// 呼び出しを 1 行出す（戻り値が None なら値を返さない）
static char *emit_call(Emitter *e, Node *n, const char *args) {
    // ── 第27章：失敗しうる呼び出し ──
    //
    //   ① エラースロットのタグを 0 にする
    //   ② スロットのアドレスを最後の引数として渡す
    //   ③ 戻ってきたらタグを見て、0 でなければ「失敗の飛び先」へ跳ぶ
    StrBuf full;
    sb_init(&full);
    sb_printf(&full, "%s", args);
    const char *slot = NULL;
    if (n->can_fail) {
        slot = err_slot(e);
        char *tp = new_tmp(e);
        sb_printf(&e->fn, "  %s = getelementptr %%pl.err, ptr %s, i32 0, i32 0\n", tp,
                  slot);
        sb_printf(&e->fn, "  store i64 0, ptr %s\n", tp);
        sb_printf(&full, "%sptr %s", args[0] ? ", " : "", slot);
    }

    char *t = NULL;
    if (n->type->kind == TY_NONE) {
        sb_printf(&e->fn, "  call void @%s(%s)\n", n->ir_name, sb_str(&full));
    } else {
        t = new_tmp(e);
        sb_printf(&e->fn, "  %s = call %s @%s(%s)\n", t, llvm_type(n->type),
                  n->ir_name, sb_str(&full));
    }

    if (n->can_fail) {
        int id = e->label_counter++;
        char ok_l[32];
        snprintf(ok_l, sizeof(ok_l), "call.ok.%d", id);
        char *tag = load_tag(e, slot);
        char *bad = new_tmp(e);
        sb_printf(&e->fn, "  %s = icmp ne i64 %s, 0\n", bad, tag);

        // ⚠️ 解放が有効なら、失敗の経路でも「抜けるスコープ」を解放します。
        //   分岐の辺には命令を置けないので、専用のブロックを 1 つ挟みます。
        if (e->drop) {
            char fail_l[32];
            snprintf(fail_l, sizeof(fail_l), "call.fail.%d", id);
            emit_cond_br(e, bad, fail_l, ok_l);
            emit_label(e, fail_l);
            emit_drops_until(e, e->try_ctx ? e->try_ctx->scope : NULL);
            emit_fail_br(e);
        } else {
            const char *fl = fail_label(e);
            if (fl) {
                emit_cond_br(e, bad, fl, ok_l);
            } else {
                // 到達しない経路（sema が保証）。専用ブロックに落とす。
                char un_l[32];
                snprintf(un_l, sizeof(un_l), "call.unreach.%d", id);
                emit_cond_br(e, bad, un_l, ok_l);
                emit_label(e, un_l);
                sb_printf(&e->fn, "  unreachable\n");
                e->terminated = true;
            }
        }
        emit_label(e, ok_l);
    }
    return t;
}

static char *gen_method(Emitter *e, Node *n) {
    // ★ 第13章：'.' の左がモジュールだった場合。sema が記録を残している。
    if (n->mod_name) {
        if (n->cls) return gen_new(e, n);  // lexer.Token(1, "x")

        StrBuf args, types;
        sb_init(&args);
        sb_init(&types);
        gen_args(e, n->args, &args, &types, true);
        if (n->is_extern) {
            if (n->can_fail) sb_printf(&types, "%sptr", sb_str(&types)[0] ? ", " : "");
            declare_extern(e, llvm_type(n->type), n->ir_name, sb_str(&types));
        }
        return emit_call(e, n, sb_str(&args));
    }

    // クラスのメソッド（第12章）。self を第 1 引数に渡すだけ。
    // ★ 呼ぶ関数名は sema が修飾済み（n->ir_name = "lexer.Token.show"）。
    // ★ 第28章：rc[T] の中身のメソッドも同じように呼べます（自動デリファレンス）。
    if (n->lhs->type->kind == TY_CLASS || n->lhs->type->kind == TY_RC) {
        char *obj = deref_rc(e, n->lhs->type, gen_expr(e, n->lhs));

        StrBuf args, types;
        sb_init(&args);
        sb_init(&types);
        sb_printf(&args, "ptr %s", obj);
        sb_printf(&types, "ptr");
        gen_args(e, n->args, &args, &types, false);

        // ★ 第13章：import したクラスのメソッドは、このモジュールには
        //   定義がないので declare する（sema が is_extern を立てている）。
        if (n->is_extern) {
            if (n->can_fail) sb_printf(&types, ", ptr");
            declare_extern(e, llvm_type(n->type), n->ir_name, sb_str(&types));
        }

        return emit_call(e, n, sb_str(&args));
    }

    // ── list のメソッド（第10章 append／第37章でその他）──
    //
    // ★ ランタイム側は **すべて i64 のスロット**で扱います。list の中身が
    //   「ポインタ 1 個か i64 1 個」という第10章の作りのままなので、
    //   pop / insert / remove は 1 つの関数で全部の要素型に効きます。
    //   要素の型を意識するのは、値を出し入れするときの変換だけです。
    Type *elem = n->lhs->type->elem;
    char *obj = gen_expr(e, n->lhs);

    if (strcmp(n->name, "pop") == 0 || strcmp(n->name, "remove") == 0) {
        const char *fn = strcmp(n->name, "pop") == 0 ? "pl_list_pop"
                                                     : "pl_list_remove_at";
        char *t = new_tmp(e);
        if (strcmp(n->name, "pop") == 0) {
            declare_rt(e, "i64 @pl_list_pop(ptr)");
            sb_printf(&e->fn, "  %s = call i64 @%s(ptr %s)\n", t, fn, obj);
        } else {
            declare_rt(e, "i64 @pl_list_remove_at(ptr, i64)");
            char *i = gen_expr(e, n->args);
            sb_printf(&e->fn, "  %s = call i64 @%s(ptr %s, i64 %s)\n", t, fn,
                      obj, i);
        }
        // i64 のスロットから要素の型へ戻す
        if (elem_is_ptr(elem)) {
            char *pt = new_tmp(e);
            sb_printf(&e->fn, "  %s = inttoptr i64 %s to ptr\n", pt, t);
            return pt;
        }
        return slot_to_elem(e, elem, t);
    }

    if (strcmp(n->name, "insert") == 0) {
        declare_rt(e, "void @pl_list_insert(ptr, i64, i64)");
        char *i = gen_expr(e, n->args);
        char *v = elem_to_slot(e, elem,
                               maybe_retain(e, n->args->next,
                                            gen_expr(e, n->args->next)));
        char *iv = v;
        if (elem_is_ptr(elem)) {
            iv = new_tmp(e);
            sb_printf(&e->fn, "  %s = ptrtoint ptr %s to i64\n", iv, v);
        }
        sb_printf(&e->fn,
                  "  call void @pl_list_insert(ptr %s, i64 %s, i64 %s)\n", obj,
                  i, iv);
        return NULL;
    }

    if (strcmp(n->name, "index") == 0) {
        Type *el = elem;
        const char *fn_name;
        const char *aty;
        if (el->kind == TY_FLOAT)      { fn_name = "pl_list_index_f64"; aty = "double"; }
        else if (el->kind == TY_STR)   { fn_name = "pl_list_index_str"; aty = "ptr"; }
        else if (elem_is_ptr(el))      { fn_name = "pl_list_index_ptr"; aty = "ptr"; }
        else                           { fn_name = "pl_list_index_i64"; aty = "i64"; }
        StrBuf sig;
        sb_init(&sig);
        sb_printf(&sig, "i64 @%s(ptr, %s)", fn_name, aty);
        declare_rt(e, sb_str(&sig));
        char *v = elem_to_slot_cmp(e, el, gen_expr(e, n->args));
        char *t = new_tmp(e);
        sb_printf(&e->fn, "  %s = call i64 @%s(ptr %s, %s %s)\n", t, fn_name,
                  obj, aty, v);
        return t;
    }

    if (strcmp(n->name, "reverse") == 0) {
        declare_rt(e, "void @pl_list_reverse(ptr)");
        sb_printf(&e->fn, "  call void @pl_list_reverse(ptr %s)\n", obj);
        return NULL;
    }
    if (strcmp(n->name, "clear") == 0) {
        declare_rt(e, "void @pl_list_clear(ptr)");
        sb_printf(&e->fn, "  call void @pl_list_clear(ptr %s)\n", obj);
        return NULL;
    }
    if (strcmp(n->name, "copy") == 0) {
        declare_rt(e, "ptr @pl_list_copy(ptr)");
        char *t = new_tmp(e);
        sb_printf(&e->fn, "  %s = call ptr @pl_list_copy(ptr %s)\n", t, obj);
        return t;
    }
    if (strcmp(n->name, "extend") == 0) {
        declare_rt(e, "void @pl_list_extend(ptr, ptr)");
        char *o = gen_expr(e, n->args);
        sb_printf(&e->fn, "  call void @pl_list_extend(ptr %s, ptr %s)\n", obj, o);
        return NULL;
    }

    const char *sty = slot_ty(elem);
    const char *push = elem_is_ptr(elem) ? "pl_list_push_ptr" : "pl_list_push_i64";
    StrBuf sig;
    sb_init(&sig);
    sb_printf(&sig, "void @%s(ptr, %s)", push, sty);
    declare_rt(e, sb_str(&sig));

    char *v = elem_to_slot(e, elem, maybe_retain(e, n->args, gen_expr(e, n->args)));
    sb_printf(&e->fn, "  call void @%s(ptr %s, %s %s)\n", push, obj, sty, v);
    return NULL;
}

// ── class の生成（第12章）──────────────────────────────────
//
// ★ 使う道具は getelementptr ひとつだけです。
//   「オブジェクトの何番目のフィールドか」を渡すと、
//   バイト数への変換（パディング込み）は LLVM がやってくれます。

// フィールドのアドレスを求める。読み出しにも代入にも使います。
// rc[T] の参照を 1 つ増やす（第28章）。
//
// ★ 増やすのは「場所から読んだ参照を、別の場所に置く」ときだけです。
//   rc(...) や関数の戻り値は**新しい参照**なので、そのまま置けます。
// ⚠️ retain と release は対です。解放を挿さない（--drop 無し）ときは、
//    どちらも出しません（数が合わなくなるより、何もしないほうが安全）。
static char *maybe_retain(Emitter *e, Node *rhs, char *val) {
    if (!e->drop || !rhs || !rhs->type || rhs->type->kind != TY_RC) return val;
    if (rhs->kind != ND_VAR && rhs->kind != ND_FIELD && rhs->kind != ND_INDEX)
        return val;
    declare_rt(e, "ptr @pl_rc_retain(ptr)");
    char *t = new_tmp(e);
    sb_printf(&e->fn, "  %s = call ptr @pl_rc_retain(ptr %s)\n", t, val);
    return t;
}

// rc[T] なら中身を取り出す（第28章の自動デリファレンス）
static char *deref_rc(Emitter *e, Type *t, char *v) {
    if (!t || t->kind != TY_RC) return v;
    declare_rt(e, "ptr @pl_rc_get(ptr)");
    char *g = new_tmp(e);
    sb_printf(&e->fn, "  %s = call ptr @pl_rc_get(ptr %s)\n", g, v);
    return g;
}

static char *gen_field_ptr(Emitter *e, Node *n) {
    Type *ot = n->lhs->type;
    Class *c = ot->kind == TY_RC ? ot->elem->cls : ot->cls;
    char *obj = deref_rc(e, ot, gen_expr(e, n->lhs));

    // ⚠️ クラス型のフィールドは NULL から始まります（12.6 節）。
    //    NULL 参照を segfault ではなく親切なメッセージに変えるため、
    //    ランタイムに 1 回問い合わせます（規約 R10：分岐は IR に出さない）。
    declare_rt(e, "ptr @pl_check_not_none(ptr)");
    char *ok = new_tmp(e);
    sb_printf(&e->fn, "  %s = call ptr @pl_check_not_none(ptr %s)\n", ok, obj);

    // ⚠️ 第 1 インデックスは常に 0（「Token の配列の何個目か」）。
    //    ここを 1 にすると隣のオブジェクトがある場所を読みます。
    char *t = new_tmp(e);
    sb_printf(&e->fn, "  %s = getelementptr %%%s.type, ptr %s, i32 0, i32 %d\n", t,
              class_type(e, c), ok, n->field->index);
    return t;
}

// 他モジュールのグローバル変数は、使う側の .ll に external で宣言する。
//   @g.lexer.MAX_KIND = external global i64
static void declare_extern_global(Emitter *e, Node *n) {
    StrBuf sig;
    sb_init(&sig);
    sb_printf(&sig, "%s = external global %s", n->ir_name, llvm_mem_type(n->type));

    for (StrLit *d = e->decled; d; d = d->next)
        if (strcmp(d->label, sb_str(&sig)) == 0) return;

    sb_printf(&e->globals, "%s\n", sb_str(&sig));
    StrLit *d = xmalloc(sizeof(StrLit));
    d->label = sb_str(&sig);
    d->next = e->decled;
    e->decled = d;
}

static char *gen_field(Emitter *e, Node *n) {
    // ★ 第13章：'.' の左がモジュールなら、これはグローバル変数の読み出し。
    //   ND_FIELD のままだが、sema が ir_name を入れているので変数と同じ扱い。
    if (n->mod_name) {
        if (n->is_extern) declare_extern_global(e, n);
        return gen_load(e, n->type, n->ir_name);
    }

    // ★ 読み書きは第6章の gen_load / gen_store をそのまま使います。
    //   bool フィールドの i8 ↔ i1 変換（規約 R5）は、何も書かずに手に入ります。
    return gen_load(e, n->type, gen_field_ptr(e, n));
}

// インスタンス生成 Token(1, "x")。
//
//   ① ヒープに確保する（pl_alloc は calloc なので必ずゼロ初期化される）
//   ② 参照型フィールドに既定値を入れる（12.6 節）
//   ③ init があれば呼ぶ
static char *gen_new(Emitter *e, Node *n) {
    Class *c = n->cls;

    declare_rt(e, "ptr @pl_alloc(i64)");
    char *obj = new_tmp(e);
    sb_printf(&e->fn, "  %s = call ptr @pl_alloc(i64 %d)\n", obj, c->size);

    // ★ ゼロ初期化では足りない型に、有効な値を入れておきます。
    //   str → ""、list[T] → 空のリスト。
    //   これで「init を書き忘れたら壊れる」がほぼ無くなります。
    for (Field *f = c->fields; f; f = f->next) {
        if (f->type->kind != TY_STR && f->type->kind != TY_LIST) continue;

        char *val;
        if (f->type->kind == TY_STR) {
            val = intern_str(e, "", 0);
        } else {
            declare_rt(e, "ptr @pl_list_new()");
            val = new_tmp(e);
            sb_printf(&e->fn, "  %s = call ptr @pl_list_new()\n", val);
        }
        char *p = new_tmp(e);
        sb_printf(&e->fn, "  %s = getelementptr %%%s.type, ptr %s, i32 0, i32 %d\n",
                  p, class_type(e, c), obj, f->index);
        sb_printf(&e->fn, "  store ptr %s, ptr %s\n", val, p);
    }

    if (!c->has_init) return obj;

    // init は「self を第 1 引数に取るふつうの関数」（規約 R8 がそのまま働く）
    StrBuf args, ptypes;
    sb_init(&args);
    sb_init(&ptypes);
    sb_printf(&args, "ptr %s", obj);
    sb_printf(&ptypes, "ptr");
    for (Node *a = n->args; a; a = a->next) {
        char *v = gen_expr(e, a);
        sb_printf(&args, ", %s %s", llvm_type(a->type), v);
        sb_printf(&ptypes, ", %s", llvm_type(a->type));
    }

    StrBuf init;
    sb_init(&init);
    sb_printf(&init, "%s.init", c->ir_name);

    // ★ 第13章：別モジュールのクラスなら declare が要る
    if (n->is_extern) declare_extern(e, "void", sb_str(&init), sb_str(&ptypes));

    sb_printf(&e->fn, "  call void @%s(%s)\n", sb_str(&init), sb_str(&args));
    return obj;
}

// クラスの型定義を出す：%lexer.Token.type = type { i64, ptr }
//
// ★ フィールドの LLVM 型は「メモリ上の型」（bool は i8。規約 R5）。
// ⚠️ 第13章：LLVM の型定義はモジュールローカルです。import したクラスを
//    使うモジュールにも、同じ定義を書き直す必要があります。レイアウトは
//    コンパイラのプロセス内で 1 回だけ計算した Class * を共有しているので、
//    2 つの .ll が食い違うことはありません（13.7 節）。
static void gen_class_type(Emitter *e, Class *c) {
    for (StrLit *t = e->types; t; t = t->next)
        if (strcmp(t->label, c->ir_name) == 0) return;  // 出済み

    StrLit *t = xmalloc(sizeof(StrLit));
    t->label = c->ir_name;
    t->next = e->types;
    e->types = t;

    sb_printf(&e->header, "%%%s.type = type { ", c->ir_name);
    bool first = true;
    for (Field *f = c->fields; f; f = f->next) {
        sb_printf(&e->header, "%s%s", first ? "" : ", ", llvm_mem_type(f->type));
        first = false;
    }
    // ⚠️ フィールドが 0 個でも空の構造体は書けます（サイズ 0）。
    //    pl_alloc(0) は calloc(1, 0) になり、有効なポインタが返ります。
    sb_printf(&e->header, " }\n");
}

// クラスの型名を返す（まだ出していなければ定義も出す）。
//
// ★ 「使ったものだけ出す」ので、import したクラスの型定義も自動で付いてきます。
static const char *class_type(Emitter *e, Class *c) {
    gen_class_type(e, c);
    return c->ir_name;
}

// 別モジュールの関数を declare する（引数の型は呼び出しから作る）。
//
// ★ 第9章の declare_rt と同じ仕組み（使ったものだけ宣言する）。
static void declare_extern(Emitter *e, const char *ret, const char *ir_name,
                           const char *param_types) {
    StrBuf sig;
    sb_init(&sig);
    sb_printf(&sig, "%s @%s(%s)", ret, ir_name, param_types);
    declare_rt(e, sb_str(&sig));
}



// ── エラー処理の生成（第27章）───────────────────────────────
//
// 設計は docs/design/error-handling.md。**アンワインドはしません。**
// 失敗しうる関数は、末尾に「エラー出力ポインタ」を 1 本余分に取ります。
//
//   define ptr @read(ptr %path, ptr %err.out)
//
//   %pl.err = type { i64, ptr }     ; { タグ, エラーオブジェクト }
//   タグ 0 は「エラー無し」に予約。

// %pl.err の型定義を 1 回だけ出す
static void ensure_err_type(Emitter *e) {
    if (e->err_type_emitted) return;
    e->err_type_emitted = true;
    sb_printf(&e->header, "%%pl.err = type { i64, ptr }\n");
}

// この関数のエラースロット（呼び出しの結果を受け取る場所）を用意する
static const char *err_slot(Emitter *e) {
    ensure_err_type(e);
    if (!e->err_slot) {
        e->err_slot = true;
        sb_printf(&e->allocas, "  %%err.slot = alloca %%pl.err\n");
    }
    return "%err.slot";
}

// 失敗して戻るときの ret（値は使われない。設計 §2 の表）
static void emit_default_ret(Emitter *e) {
    const char *dv = default_value(e->fn_ret);
    if (!dv) sb_printf(&e->fn, "  ret void\n");
    else sb_printf(&e->fn, "  ret %s %s\n", llvm_type(e->fn_ret), dv);
    e->terminated = true;
}

// 型ごとの「使われない戻り値」（設計 §2 の表）
static const char *default_value(Type *t) {
    switch (t->kind) {
        case TY_NONE: return NULL;  // void（値を返さない）
        case TY_INT: return "0";
        case TY_BOOL: return "false";
        default: return "null";  // str / list / class / T | None
    }
}

// 失敗したときの飛び先（内側の try があればその振り分け、無ければ伝播）。
//
// ⚠️ どちらでもない場合（この関数は raises を宣言していない）は **到達しません**。
//    sema が「捕まえるか宣言するか」を強制しているからです（E-RAISE-1）。
//    その場合は NULL を返し、呼ぶ側が unreachable を出します。
static const char *fail_label(Emitter *e) {
    if (e->try_ctx) return e->try_ctx->dispatch;
    if (!e->fn_raises) return NULL;
    e->prop_used = true;
    return e->prop_label;
}

// 失敗の経路へ飛ぶ（飛び先が無ければ unreachable）
static void emit_fail_br(Emitter *e) {
    const char *l = fail_label(e);
    if (l) {
        emit_br(e, l);
    } else {
        sb_printf(&e->fn, "  unreachable\n");
        e->terminated = true;
    }
}

// エラー（タグと値）をスロットへ書く
static void store_err(Emitter *e, const char *slot, int tag, const char *obj) {
    ensure_err_type(e);
    char *tp = new_tmp(e);
    sb_printf(&e->fn, "  %s = getelementptr %%pl.err, ptr %s, i32 0, i32 0\n", tp, slot);
    sb_printf(&e->fn, "  store i64 %d, ptr %s\n", tag, tp);
    char *pp = new_tmp(e);
    sb_printf(&e->fn, "  %s = getelementptr %%pl.err, ptr %s, i32 0, i32 1\n", pp, slot);
    sb_printf(&e->fn, "  store ptr %s, ptr %s\n", obj, pp);
}

// スロットからタグを読む
static char *load_tag(Emitter *e, const char *slot) {
    char *tp = new_tmp(e);
    sb_printf(&e->fn, "  %s = getelementptr %%pl.err, ptr %s, i32 0, i32 0\n", tp, slot);
    char *tv = new_tmp(e);
    sb_printf(&e->fn, "  %s = load i64, ptr %s\n", tv, tp);
    return tv;
}

// スロットからエラーオブジェクトを読む
static char *load_payload(Emitter *e, const char *slot) {
    char *pp = new_tmp(e);
    sb_printf(&e->fn, "  %s = getelementptr %%pl.err, ptr %s, i32 0, i32 1\n", pp, slot);
    char *pv = new_tmp(e);
    sb_printf(&e->fn, "  %s = load ptr, ptr %s\n", pv, pp);
    return pv;
}

// ── 解放（drop）の生成（第25章）─────────────────────────────
//
// 設計は docs/design/ownership.md §6。仕様は safety-spec.md §6。
//
// ★ この章のいちばん大きな判断：**drop フラグを持たない。**
//
//   設計 §6.3 は Rust に倣って「MaybeMoved の場所は alloca i1 のフラグを持つ」
//   としていました。しかし Polonium の所有型（str / list / class）は
//   **すべてポインタ**なので、**移動したときにスロットへ null を書けば**
//   同じことができます。解放関数はどれも null を受け取れるので、
//   フラグの alloca も分岐も要りません（決定 D17）。
//
//     xs: list[int] = [1, 2]      %xs = alloca ptr        …… 所有している
//     ys: list[int] = xs          store ptr null, ptr %xs …… 移動した印
//     （スコープ終端）              %v = load ptr, ptr %xs
//                                 call void @drop.list.0(ptr %v)  …… null なら何もしない

static const char *drop_fn_for(Emitter *e, Type *t);
static const char *gen_rc_drop(Emitter *e, Type *t);

// 生成済みの @drop.* を覚えておく（型名を鍵にする）
static const char *drop_fn_cached(Emitter *e, const char *key) {
    for (StrLit *d = e->dropfns; d; d = d->next)
        if (strcmp(d->bytes, key) == 0) return d->label;
    return NULL;
}

static void drop_fn_remember(Emitter *e, const char *key, const char *name) {
    StrLit *d = xmalloc(sizeof(StrLit));
    d->bytes = (char *)key;
    d->label = (char *)name;
    d->next = e->dropfns;
    e->dropfns = d;
}

// クラス C を解放する関数 @drop.<C> を生成する。
//
//   ① drop メソッドがあれば先に呼ぶ（デストラクタ。仕様 §6.2）
//   ② 所有型のフィールドを宣言順に解放する
//   ③ インスタンス自身を解放する
//
// ⚠️ 自分自身を含むクラス（連結リストなど）では再帰します。
//    長いリストではスタックを使い切る可能性があります（第28章で見直します）。
static const char *gen_class_drop(Emitter *e, Class *c) {
    StrBuf key;
    sb_init(&key);
    sb_printf(&key, "class:%s", c->ir_name);
    const char *hit = drop_fn_cached(e, sb_str(&key));
    if (hit) return hit;

    StrBuf name;
    sb_init(&name);
    sb_printf(&name, "@drop.%s", c->ir_name);
    // ★ 先に登録します。フィールドが自分自身の型でも無限再帰しないため。
    drop_fn_remember(e, sb_str(&key), sb_str(&name));

    // ユーザー定義の drop メソッド（デストラクタ）を探す
    Node *dtor = NULL;
    if (c->node)
        for (Node *m = c->node->body; m; m = m->next)
            if (m->kind == ND_FUNC && strcmp(m->name, "drop") == 0) dtor = m;

    // ★ フィールドの解放関数を先に作ります（本体を書き始める前に）。
    //   drop_fn_for が e->dropdefs に書き足すので、書きかけの本体と混ざらないように。
    const char *ftype = class_type(e, c);
    const char **fdrops = xmalloc(sizeof(char *) * (size_t)(c->nfields + 1));
    int nf = 0;
    for (Field *f = c->fields; f; f = f->next) fdrops[nf++] = drop_fn_for(e, f->type);

    StrBuf b;
    sb_init(&b);
    sb_printf(&b, "\ndefine internal void %s(ptr %%p) {\nentry:\n", sb_str(&name));
    sb_printf(&b, "  %%isnull = icmp eq ptr %%p, null\n");
    sb_printf(&b, "  br i1 %%isnull, label %%done, label %%body\nbody:\n");

    if (dtor) {
        // ⚠️ 別モジュールのクラスなら declare が要ります。自分のモジュールで
        //    定義しているクラスに declare を出すと「再定義」で落ちます。
        bool local = false;
        for (Node *d = e->ast->body; d; d = d->next)
            if (d->kind == ND_CLASS && d->cls == c) local = true;
        if (!local) declare_extern(e, "void", dtor->ir_name, "ptr");
        sb_printf(&b, "  call void @%s(ptr %%p)\n", dtor->ir_name);
    }

    int i = 0;
    for (Field *f = c->fields; f; f = f->next, i++) {
        if (!fdrops[i]) continue;  // コピー型のフィールドは何もしない
        sb_printf(&b, "  %%f%d = getelementptr %%%s.type, ptr %%p, i32 0, i32 %d\n", i,
                  ftype, f->index);
        sb_printf(&b, "  %%v%d = load ptr, ptr %%f%d\n", i, i);
        sb_printf(&b, "  call void %s(ptr %%v%d)\n", fdrops[i], i);
    }

    declare_rt(e, "void @pl_drop_obj(ptr)");
    sb_printf(&b, "  call void @pl_drop_obj(ptr %%p)\n");
    sb_printf(&b, "  br label %%done\ndone:\n  ret void\n}\n");
    sb_printf(&e->dropdefs, "%s", sb_str(&b));

    return sb_str(&name);
}

// list[T] を解放する関数を生成する。
//
// ★ pl_drop_list は「要素を解放する関数」を受け取ります（要素がコピー型なら null）。
//   引数が 2 つあるので、そのままでは「ptr を 1 つ取る解放関数」の形に合いません。
//   包む関数を 1 つ作れば、あとはどの型でも同じ形で扱えます。
static const char *gen_list_drop(Emitter *e, Type *t) {
    StrBuf key;
    sb_init(&key);
    sb_printf(&key, "list:%s", type_name(t));
    const char *hit = drop_fn_cached(e, sb_str(&key));
    if (hit) return hit;

    StrBuf name;
    sb_init(&name);
    sb_printf(&name, "@drop.list.%d", e->drop_counter++);
    drop_fn_remember(e, sb_str(&key), sb_str(&name));

    const char *elem = drop_fn_for(e, t->elem);

    declare_rt(e, "void @pl_drop_list(ptr, ptr)");
    sb_printf(&e->dropdefs,
              "\ndefine internal void %s(ptr %%l) {\nentry:\n"
              "  call void @pl_drop_list(ptr %%l, ptr %s)\n"
              "  ret void\n}\n",
              sb_str(&name), elem ? elem : "null");
    return sb_str(&name);
}

// rc[T] を手放す関数を生成する（カウントを 1 減らす。第28章）
static const char *gen_rc_drop(Emitter *e, Type *t) {
    StrBuf key;
    sb_init(&key);
    sb_printf(&key, "rc:%s", type_name(t));
    const char *hit = drop_fn_cached(e, sb_str(&key));
    if (hit) return hit;

    StrBuf name;
    sb_init(&name);
    sb_printf(&name, "@drop.rc.%d", e->drop_counter++);
    drop_fn_remember(e, sb_str(&key), sb_str(&name));

    const char *inner = drop_fn_for(e, t->elem);
    declare_rt(e, "void @pl_rc_release(ptr, ptr)");
    sb_printf(&e->dropdefs,
              "\ndefine internal void %s(ptr %%p) {\nentry:\n"
              "  call void @pl_rc_release(ptr %%p, ptr %s)\n"
              "  ret void\n}\n",
              sb_str(&name), inner ? inner : "null");
    return sb_str(&name);
}

// 型 t の値 1 つを解放する関数の名前。コピー型なら NULL。
static const char *drop_fn_for(Emitter *e, Type *t) {
    if (!t) return NULL;
    switch (t->kind) {
        case TY_STR:
            declare_rt(e, "void @pl_drop_str(ptr)");
            return "@pl_drop_str";
        case TY_LIST: return gen_list_drop(e, t);
        case TY_RC: return gen_rc_drop(e, t);
        case TY_CLASS: return gen_class_drop(e, t->cls);
        // T | None は中身と同じ扱い（解放関数はどれも null を受け取れる）
        case TY_OPT: return drop_fn_for(e, t->elem);
        default: return NULL;  // int / bool / None
    }
}

// 変数 1 つを解放する（スロットを読んで、解放関数に渡すだけ）
static void emit_drop_slot(Emitter *e, Node *decl) {
    const char *fn = drop_fn_for(e, decl->type);
    if (!fn) return;
    char *v = new_tmp(e);
    sb_printf(&e->fn, "  %s = load ptr, ptr %s\n", v, decl->ir_name);
    sb_printf(&e->fn, "  call void %s(ptr %s)\n", fn, v);
}

// 一時的な値を解放する（式文の結果など）
static void emit_drop_value(Emitter *e, Type *t, const char *val) {
    const char *fn = drop_fn_for(e, t);
    if (!fn) return;
    sb_printf(&e->fn, "  call void %s(ptr %s)\n", fn, val);
}

// この変数はスコープ終端で解放する対象か。
//
// ⚠️ 借りものを束縛している変数（`t = xs[i]` や for のループ変数）は
//    **所有していない**ので解放しません。ownck が印を付けています（第23章）。
static bool is_droppable(Node *decl) {
    return decl->type && !decl->is_global && !decl->binds_borrow &&
           (decl->type->kind == TY_STR || decl->type->kind == TY_LIST ||
            decl->type->kind == TY_CLASS || decl->type->kind == TY_OPT ||
            decl->type->kind == TY_RC);  // 第28章：rc[T] はカウントを減らす
}

static void scope_add(Emitter *e, Node *decl) {
    if (!e->drop || !e->scope || !is_droppable(decl)) return;
    DropEnt *d = xmalloc(sizeof(DropEnt));
    d->decl = decl;
    d->next = e->scope->ents;  // 先頭に足す＝たどると宣言の逆順
    e->scope->ents = d;
}

// スコープ 1 つぶんの解放を出す（宣言と逆順。設計 §6.1）
static void emit_scope_drops(Emitter *e, ScopeCtx *sc) {
    for (DropEnt *d = sc->ents; d; d = d->next) emit_drop_slot(e, d->decl);
}

// 今のスコープから stop（含まない）まで、抜けるスコープすべてを解放する。
//   return  … stop = NULL（関数の外まで抜ける）
//   break   … stop = ループの外側のスコープ
static void emit_drops_until(Emitter *e, struct ScopeCtx *stop) {
    if (!e->drop) return;
    for (ScopeCtx *sc = e->scope; sc && sc != stop; sc = sc->outer)
        emit_scope_drops(e, sc);
}

// ── 制御構文の生成（規約 6.3 / 6.4 / 6.5）──────────────────

static char *gen_stmt(Emitter *e, Node *n);

static void gen_if(Emitter *e, Node *n) {
    int id = e->label_counter++;  // ★ 番号は最初に 1 回だけ確保する

    char then_l[32], else_l[32], end_l[32];
    snprintf(then_l, sizeof(then_l), "if.then.%d", id);
    snprintf(else_l, sizeof(else_l), "if.else.%d", id);
    snprintf(end_l, sizeof(end_l), "if.end.%d", id);

    char *cond = gen_expr(e, n->lhs);
    // else が無ければ else ブロックを作らず、直接 end へ分岐する
    emit_cond_br(e, cond, then_l, n->els ? else_l : end_l);

    emit_label(e, then_l);
    gen_stmt(e, n->body);
    // ⚠️ then 節が break / continue で終わっていたら、そこは既に終端済み。
    //    もう 1 つ br を出すと「1 ブロックに終端命令が 2 つ」になり LLVM が怒ります。
    if (!e->terminated) emit_br(e, end_l);

    if (n->els) {
        emit_label(e, else_l);
        gen_stmt(e, n->els);
        if (!e->terminated) emit_br(e, end_l);
    }

    emit_label(e, end_l);
}

static void gen_while(Emitter *e, Node *n) {
    int id = e->label_counter++;

    char cond_l[32], body_l[32], end_l[32];
    snprintf(cond_l, sizeof(cond_l), "while.cond.%d", id);
    snprintf(body_l, sizeof(body_l), "while.body.%d", id);
    snprintf(end_l, sizeof(end_l), "while.end.%d", id);

    // ⚠️ 条件ブロックに「入る」ための br が必要（規約 6.4）。
    //    条件を独立したブロックにしないと 1 回目の判定が飛ばされ、
    //    do-while になってしまいます。
    emit_br(e, cond_l);

    emit_label(e, cond_l);
    char *cond = gen_expr(e, n->lhs);  // ★ 条件は反復のたびに評価される
    emit_cond_br(e, cond, body_l, end_l);

    // ★ 第11章：増分があるなら continue の飛び先は「増分ブロック」。
    //   無ければ従来どおり「条件ブロック」（第7章のまま）。
    //
    // ⚠️ ここを間違えると、for の中の continue が増分を飛ばして無限ループになります。
    char incr_l[32];
    const char *cont_l = cond_l;
    if (n->incr) {
        snprintf(incr_l, sizeof(incr_l), "for.incr.%d", id);
        cont_l = incr_l;
    }

    // break / continue の飛び先を積む
    LoopCtx ctx = {.outer = e->loop,
                   .break_label = end_l,
                   .continue_label = cont_l,
                   .scope = e->scope};
    e->loop = &ctx;

    emit_label(e, body_l);
    gen_stmt(e, n->body);

    if (n->incr) {
        // ★ emit_label が「終端していなければ br を補う」ので、
        //   本体から増分ブロックへは自動的に繋がります（第6章の関数）。
        emit_label(e, incr_l);
        gen_stmt(e, n->incr);
    }
    if (!e->terminated) emit_br(e, cond_l);  // ループバック

    e->loop = ctx.outer;  // ★ 対で戻す

    emit_label(e, end_l);
}

// print(e) の暫定実装。C の printf を借ります。
//
// ★ 第1章で用意した globals / decls バッファが、ここで初めて使われます。
// 組み込み関数の呼び出し（第9章）。
//
// ★ sema が選んだ候補（n->builtin）に従って、対応するランタイム関数を呼ぶだけ。
//   「どの実装を呼ぶか」の判断は sema 側で終わっています。
static char *gen_builtin_call(Emitter *e, Node *n) {
    const Builtin *b = n->builtin;
    // ⚠️ 引数の型は表からではなく実引数から取ります。
    //    list[T] にはシングルトンが無いので type_from_kind では引けません（第10章）。
    Type *at = n->args->type;
    Type *rt = type_from_kind(b->ret);

    // ⚠️ bool は Polonium のレジスタ上では i1 ですが、C 側は long long で
    //    受け取ります。境界で i64 に広げます（規約 R5 と同じ考え方）。
    const char *argty = at->kind == TY_BOOL ? "i64" : llvm_type(at);

    // declare を 1 回だけ出す
    StrBuf sig;
    sb_init(&sig);
    sb_printf(&sig, "%s @%s(%s)", llvm_type(rt), b->impl, argty);
    declare_rt(e, sb_str(&sig));

    char *v = gen_expr(e, n->args);
    if (at->kind == TY_BOOL) {
        char *z = new_tmp(e);
        sb_printf(&e->fn, "  %s = zext i1 %s to i64\n", z, v);
        v = z;
    }

    if (rt->kind == TY_NONE) {
        sb_printf(&e->fn, "  call void @%s(%s %s)\n", b->impl, argty, v);
        return NULL;
    }
    char *t = new_tmp(e);
    sb_printf(&e->fn, "  %s = call %s @%s(%s %s)\n", t, llvm_type(rt), b->impl,
              argty, v);
    return t;
}

// 関数呼び出し。
//
// ⚠️ void の呼び出しに結果を代入してはいけません。
//      %t0 = call void @f()   ✗
//      call void @f()         ✅
// ── 第30章：低レベルの生成 ──────────────────────────────────
//
// ★ どれも命令 1〜2 個です。ランタイム関数は要りません（OS では呼べないので）。
//   ⚠️ 読み書きは volatile にします。MMIO（装置のレジスタ）は
//     「同じ番地を読んでも値が変わる」ので、最適化で消されると困ります。
static char *gen_lowlevel(Emitter *e, Node *n) {
    Node *a0 = n->args;

    // ── 第33章：インラインアセンブリ ──
    //
    // ★ sideeffect を付けます。「値を返さないから消してよい」と
    //   最適化に判断されると、csrw も wfi も消えてしまうためです。
    if (strncmp(n->name, "asm", 3) == 0) {
        // ⚠️ 利用者は C と同じ %0 で書きますが、LLVM IR のインライン
        //    アセンブリでは $0 が「1 番目のオペランド」です。ここで直します
        //    （RISC-V では % は %hi(...) のような再配置指定に使われるため、
        //     そのままだとアセンブラが別物として読んでしまいます）。
        StrBuf asmbuf;
        sb_init(&asmbuf);
        for (const char *c = a0->sval; *c; c++) {
            if (*c == '%' && c[1] >= '0' && c[1] <= '9') {
                sb_printf(&asmbuf, "$%c", c[1]);
                c++;
            } else if (*c == '$') {
                sb_printf(&asmbuf, "$$");
            } else {
                sb_printf(&asmbuf, "%c", *c);
            }
        }
        const char *text = sb_str(&asmbuf);
        if (strcmp(n->name, "asm") == 0) {
            sb_printf(&e->fn, "  call void asm sideeffect \"%s\", \"\"()\n", text);
            return NULL;
        }
        if (strcmp(n->name, "asm_in") == 0) {
            char *v = gen_expr(e, a0->next);
            sb_printf(&e->fn,
                      "  call void asm sideeffect \"%s\", \"r\"(i64 %s)\n", text, v);
            return NULL;
        }
        char *t = new_tmp(e);
        sb_printf(&e->fn, "  %s = call i64 asm sideeffect \"%s\", \"=r\"()\n", t, text);
        return t;
    }

    Node *a1 = a0 ? a0->next : NULL;
    Node *a2 = a1 ? a1->next : NULL;

    if (strcmp(n->name, "ptr_at") == 0) {
        char *v = gen_expr(e, a0);
        char *t = new_tmp(e);
        sb_printf(&e->fn, "  %s = inttoptr i64 %s to ptr\n", t, v);
        return t;
    }
    if (strcmp(n->name, "addr_of") == 0) {
        char *v = gen_expr(e, a0);
        char *t = new_tmp(e);
        sb_printf(&e->fn, "  %s = ptrtoint ptr %s to i64\n", t, v);
        return t;
    }

    bool is8 = strstr(n->name, "8") != NULL;
    const char *ity = is8 ? "i8" : "i64";
    char *p = gen_expr(e, a0);
    char *off = gen_expr(e, a1);
    char *addr = new_tmp(e);
    sb_printf(&e->fn, "  %s = getelementptr %s, ptr %s, i64 %s\n", addr, ity, p, off);

    if (strncmp(n->name, "peek", 4) == 0) {
        char *v = new_tmp(e);
        sb_printf(&e->fn, "  %s = load volatile %s, ptr %s\n", v, ity, addr);
        if (!is8) return v;
        char *z = new_tmp(e);
        sb_printf(&e->fn, "  %s = zext i8 %s to i64\n", z, v);
        return z;
    }

    // poke8 / poke64
    char *val = gen_expr(e, a2);
    if (is8) {
        char *tr = new_tmp(e);
        sb_printf(&e->fn, "  %s = trunc i64 %s to i8\n", tr, val);
        val = tr;
    }
    sb_printf(&e->fn, "  store volatile %s %s, ptr %s\n", ity, val, addr);
    return NULL;
}

static char *gen_call(Emitter *e, Node *n) {
    // ★ 第30章：低レベルの組み込み（sema が ir_name を付けない目印）
    if (!n->ir_name && !n->cls && !n->builtin && is_lowlevel_name(n->name))
        return gen_lowlevel(e, n);

    if (n->builtin) return gen_builtin_call(e, n);
    if (n->cls) return gen_new(e, n);  // ★ 第12章：インスタンス生成

    // ★ 第28章：rc(x) — 数え札を付けてくるむ（sema が ir_name を付けない目印）
    if (!n->ir_name && strcmp(n->name, "rc") == 0) {
        char *v = gen_expr(e, n->args);
        declare_rt(e, "ptr @pl_rc_new(ptr)");
        char *t = new_tmp(e);
        sb_printf(&e->fn, "  %s = call ptr @pl_rc_new(ptr %s)\n", t, v);
        return t;
    }

    // 引数を左から順に評価する（言語仕様 4.5）
    StrBuf args, types;
    sb_init(&args);
    sb_init(&types);
    gen_args(e, n->args, &args, &types, true);

    // ★ 第13章：呼ぶ名前は sema が修飾済み（n->ir_name = "lexer.make"）
    if (n->is_extern) {
        // ★ 第27章：失敗しうる関数は、エラー出力の ptr が 1 本増えています
        if (n->can_fail) sb_printf(&types, "%sptr", sb_str(&types)[0] ? ", " : "");
        declare_extern(e, llvm_type(n->type), n->ir_name, sb_str(&types));
    }
    return emit_call(e, n, sb_str(&args));
}

// ── 文の生成 ────────────────────────────────────────────────
//
// 文は「値を返さない」ので、gen_expr とは別の関数にします。
// ただし式文だけは値を持つので、その値を返します
// （プログラムの値＝最後の式文の値、という暫定仕様のため）。
static char *gen_stmt(Emitter *e, Node *n) {
    // 終端済みブロックの後ろに来たら、到達不能ブロックを開く（規約 R7）
    ensure_block(e);

    switch (n->kind) {
        case ND_BLOCK: {
            // ★ 第25章：ブロック＝スコープ。抜けるときに宣言の逆順で解放します。
            ScopeCtx sc = {e->scope, NULL};
            e->scope = &sc;

            char *last = NULL;
            for (Node *st = n->body; st; st = st->next) {
                char *v = gen_stmt(e, st);
                if (v) last = v;
            }
            // 終端済み（return / break で抜けた）なら、そちらで解放済み
            if (e->drop && !e->terminated) emit_scope_drops(e, &sc);

            e->scope = sc.outer;
            return last;
        }

        // ★ 第30章：unsafe: は「検査のための印」なので、生成は中身そのまま
        case ND_UNSAFE: return gen_stmt(e, n->body);

        case ND_IF: gen_if(e, n); return NULL;
        case ND_WHILE: gen_while(e, n); return NULL;

        // ── 第27章：try / except ──
        //
        //   try の本体で失敗したら「振り分け」へ飛び、タグを見て except を選びます。
        //   どの except にも当たらなければ、外側の飛び先（外の try か伝播）へ。
        case ND_TRY: {
            int id = e->label_counter++;
            char disp_l[32], end_l[32];
            snprintf(disp_l, sizeof(disp_l), "try.dispatch.%d", id);
            snprintf(end_l, sizeof(end_l), "try.end.%d", id);

            TryCtxG ctx = {e->try_ctx, n, NULL, e->scope};
            ctx.dispatch = xstrndup(disp_l, strlen(disp_l));
            e->try_ctx = &ctx;
            gen_stmt(e, n->body);
            e->try_ctx = ctx.outer;
            if (!e->terminated) emit_br(e, end_l);

            // ── 振り分け ──
            emit_label(e, disp_l);
            const char *slot = err_slot(e);
            char *tag = load_tag(e, slot);

            int k = 0;
            for (Node *ex = n->els; ex; ex = ex->next, k++) {
                char hit_l[40], next_l[40];
                snprintf(hit_l, sizeof(hit_l), "except.%d.%d", id, k);
                snprintf(next_l, sizeof(next_l), "try.next.%d.%d", id, k);
                char *c = new_tmp(e);
                sb_printf(&e->fn, "  %s = icmp eq i64 %s, %d\n", c, tag, ex->err_tag);
                emit_cond_br(e, c, hit_l, next_l);
                emit_label(e, next_l);
            }
            // どの except にも当たらなかった → 外側へ渡す（無ければ到達しない）
            emit_fail_br(e);

            // ── 各 except の本体 ──
            k = 0;
            for (Node *ex = n->els; ex; ex = ex->next, k++) {
                char hit_l[40];
                snprintf(hit_l, sizeof(hit_l), "except.%d.%d", id, k);
                emit_label(e, hit_l);
                if (ex->ir_name) {
                    // as e : エラーオブジェクトを局所変数に入れる
                    char *pv = load_payload(e, slot);
                    sb_printf(&e->fn, "  store ptr %s, ptr %s\n", pv, ex->ir_name);
                }
                gen_stmt(e, ex->body);
                if (!e->terminated) emit_br(e, end_l);
            }

            emit_label(e, end_l);
            return NULL;
        }

        // ── 第27章：raise ──
        case ND_RAISE: {
            char *obj = gen_expr(e, n->lhs);

            // ★ 同じ関数の中の try が捕まえるなら、そこへ飛びます（Python と同じ）。
            //   捕まえる try が無ければ、呼び出し元へ伝播します。
            TryCtxG *catcher = NULL;
            for (TryCtxG *t = e->try_ctx; t && !catcher; t = t->outer)
                for (Node *ex = t->node->els; ex; ex = ex->next)
                    if (ex->err_tag == n->err_tag) {
                        catcher = t;
                        break;
                    }

            if (catcher) {
                store_err(e, err_slot(e), n->err_tag, obj);
                if (e->drop) emit_drops_until(e, catcher->scope);
                emit_br(e, catcher->dispatch);
            } else {
                store_err(e, "%err.out", n->err_tag, obj);
                if (e->drop) emit_drops_until(e, NULL);
                emit_default_ret(e);
            }
            return NULL;
        }
        case ND_RETURN: {
            if (!n->lhs) {
                emit_drops_until(e, NULL);  // 第25章：抜けるスコープを全部解放
                sb_printf(&e->fn, "  ret void\n");  // 規約 R9
            } else {
                // ★ 戻り値を先に評価します。移動した変数はスロットが null に
                //   なっているので、この後の解放は何もしません（設計 §6.1）。
                //   ⚠️ rc[T] を返すときは参照を 1 つ増やします（呼び出し側のぶん）。
                char *v = maybe_retain(e, n->lhs, gen_expr(e, n->lhs));
                emit_drops_until(e, NULL);
                sb_printf(&e->fn, "  ret %s %s\n", llvm_type(n->lhs->type), v);
            }
            e->terminated = true;
            return NULL;
        }
        case ND_PASS: return NULL;  // 本当に何も出さない

        // 飛び先は sema が保証している（ループの外なら検査で弾かれる）
        // ★ 第25章：ループから抜ける経路でも、抜けるスコープぶんだけ解放します。
        case ND_BREAK:
            emit_drops_until(e, e->loop->scope);
            emit_br(e, e->loop->break_label);
            return NULL;
        case ND_CONTINUE:
            emit_drops_until(e, e->loop->scope);
            emit_br(e, e->loop->continue_label);
            return NULL;

        case ND_VARDECL: {
            // alloca は entry ブロックに出済み（規約 R1）。ここでは store だけ。
            char *val = maybe_retain(e, n->rhs, gen_expr(e, n->rhs));
            gen_store(e, n->type, val, n->ir_name);
            scope_add(e, n);  // 第25章：このスコープの解放対象に加える
            return NULL;
        }

        case ND_ASSIGN: {
            char *val = maybe_retain(e, n->rhs, gen_expr(e, n->rhs));

            // ★ 第25章：入れ替える前に、古い値を解放します。
            //   `s = s + "!"` のように、上書きは v1 では黙って捨てていました。
            //   ⚠️ 借りものを束縛している変数（ownck が印を付けた）は所有者では
            //      ないので触りません。
            if (e->drop && !n->binds_borrow) {
                // ⚠️ グローバル（@g.x）は「プログラムが終わるまで生きる場所」
                //    なので、ここでは触りません（解放するのは所有者だけ）。
                if (n->lhs->kind == ND_VAR && n->lhs->ir_name[0] == '%')
                    emit_drop_value(e, n->type, gen_load(e, n->type, n->lhs->ir_name));
                // ⚠️ 対象を 2 回評価しないこと。古い値を読むために
                //    gen_field_ptr をもう一度通すので、対象が単純な変数の
                //    ときだけに限ります（xs[f()].g = v で f が 2 回走るのを防ぐ）。
                else if (n->lhs->kind == ND_FIELD && !n->lhs->mod_name &&
                         n->lhs->lhs->kind == ND_VAR)
                    emit_drop_value(e, n->type,
                                    gen_load(e, n->type, gen_field_ptr(e, n->lhs)));
            }
            // 添字への代入 xs[i] = v（第10章）
            if (n->lhs->kind == ND_INDEX) {
                gen_index_store(e, n->lhs, val);
                return NULL;
            }
            // フィールドへの代入 t.kind = v（第12章）
            if (n->lhs->kind == ND_FIELD) {
                // 第13章：lexer.counter = v は「他モジュールのグローバル」への代入
                if (n->lhs->mod_name) {
                    if (n->lhs->is_extern) declare_extern_global(e, n->lhs);
                    gen_store(e, n->type, val, n->lhs->ir_name);
                    return NULL;
                }
                gen_store(e, n->type, val, gen_field_ptr(e, n->lhs));
                return NULL;
            }
            gen_store(e, n->type, val, n->lhs->ir_name);
            return NULL;
        }

        default: {
            char *v = gen_expr(e, n);  // 式文
            // ★ 第25章：捨てられる一時値（呼び出しの戻り値）を解放します。
            //   ⚠️ 借用を返す関数（仕様 §4.5）の戻り値は所有していません。
            if (e->drop && !n->binds_borrow && n->type) emit_drop_value(e, n->type, v);
            return v;
        }
    }
}

// ── alloca の収集（規約 R1）────────────────────────────────
//
// ★ すべてのローカル変数は entry ブロックで alloca します。
//
//   関数本体の途中に alloca を書いても動きますが、ループの中にあると
//   反復のたびにスタックを消費します。entry にまとめるのが LLVM の作法で、
//   mem2reg が最適化しやすい形でもあります。
//
//   本体を生成する「前」に AST を歩いて、宣言されている変数を全部集めます。
//   第7章で if / while のブロックが入っても、再帰で辿れば同じように動きます。
static void collect_allocas(Emitter *e, Node *n) {
    if (!n) return;

    // ⚠️ グローバル変数は alloca しない（@g.x をそのまま読み書きする）
    if (n->kind == ND_VARDECL && !n->is_global)
        sb_printf(&e->allocas, "  %s = alloca %s\n", n->ir_name,
                  llvm_mem_type(n->type));

    // ★ 第27章：except ... as e で束縛する変数も、ふつうの局所変数と同じ箱が要ります。
    if (n->kind == ND_EXCEPT && n->ir_name)
        sb_printf(&e->allocas, "  %s = alloca %s\n", n->ir_name,
                  llvm_mem_type(n->type));

    // ⚠️ except の並びは next で繋がっています（if の else と違って複数あります）。
    //    2 番目以降はここでたどります（先頭は下の collect_allocas(n->els) が拾う）。
    if (n->kind == ND_TRY && n->els)
        for (Node *ex = n->els->next; ex; ex = ex->next) collect_allocas(e, ex);  // ★ bool は i8（規約 R5）

    // 子と兄弟をたどる。
    // ★ 第5章で「再帰なので第7章でブロックが入っても勝手に見つかる」と
    //   書いたとおりになりました。else 節の分だけ 1 行足せば済みます。
    collect_allocas(e, n->lhs);
    collect_allocas(e, n->rhs);
    collect_allocas(e, n->els);
    for (Node *s = n->body; s; s = s->next) collect_allocas(e, s);
}

// ── 関数の生成 ──────────────────────────────────────────────

// 1 つの関数を出力する。
//
// ★ 第8章：第1章から「暗黙の main」だったものが、ここで普通の関数になりました。
static void gen_func(Emitter *e, Node *n) {
    // 関数ごとに状態をリセットする（第1章から決めてあった規約）
    e->tmp_counter = 0;
    e->label_counter = 0;
    e->terminated = false;
    e->loop = NULL;

    // ── 第27章：エラー処理の状態 ──
    e->fn_raises = n->raises != NULL;
    e->fn_ret = n->type;
    e->err_slot = false;
    e->try_ctx = NULL;
    e->prop_used = false;
    snprintf(e->prop_label, sizeof(e->prop_label), "err.propagate");
    if (e->fn_raises) ensure_err_type(e);
    sb_init(&e->allocas);
    sb_init(&e->fn);

    // ★ 第13章：関数もメソッドも、sema がモジュール修飾済みの名前を入れています
    //   （@lexer.make / @lexer.Token.show）。main も例外ではありません。
    //   「main だけ @pl_main」という第1章からの特別扱いは、
    //   モジュール修飾がその役目を引き取ったので無くなりました。
    const char *ir_name = n->ir_name;

    // ① 引数を alloca にコピーする（規約 R8）。
    //
    // 🤔 なぜコピーするのか
    //   %n.arg は SSA レジスタなので代入できません。Polonium では引数に代入
    //   できる（a = a + 1）ので、ローカル変数と同じ「箱」にしてしまいます。
    //   mem2reg がこの余分なコピーを消してくれます。
    for (Node *pm = n->params; pm; pm = pm->next) {
        sb_printf(&e->allocas, "  %s = alloca %s\n", pm->ir_name,
                  llvm_mem_type(pm->type));
        StrBuf arg;
        sb_init(&arg);
        sb_printf(&arg, "%%%s.arg", pm->name);
        gen_store(e, pm->type, sb_str(&arg), pm->ir_name);
    }

    // ② ローカル変数の alloca（第5章のまま）
    collect_allocas(e, n->body);

    // ★ 第25章：引数のスコープ。own で受け取った引数は、この関数が所有者なので
    //   出口で解放します（借用の引数には ownck が「借りもの」の印を付けています）。
    ScopeCtx params = {NULL, NULL};
    e->scope = &params;
    for (Node *pm = n->params; pm; pm = pm->next) scope_add(e, pm);

    // ③ 本体
    gen_stmt(e, n->body);

    // ── 第27章：伝播ブロック（呼び出し元へエラーをそのまま返す）──
    //
    // ★ 使われたときだけ出します（使わないブロックがあると LLVM が警告します）。
    if (e->prop_used) {
        emit_label(e, e->prop_label);
        if (e->drop) emit_drops_until(e, NULL);
        ensure_err_type(e);
        char *ev = new_tmp(e);
        sb_printf(&e->fn, "  %s = load %%pl.err, ptr %%err.slot\n", ev);
        sb_printf(&e->fn, "  store %%pl.err %s, ptr %%err.out\n", ev);
        emit_default_ret(e);
    }

    // ④ 終端されていなければ終端する（規約 R6）
    if (!e->terminated) {
        if (e->drop) emit_scope_drops(e, &params);
        if (n->type->kind == TY_NONE) {
            sb_printf(&e->fn, "  ret void\n");  // 規約 R9
        } else {
            // 全経路 return は sema が保証済み。ここに来るのは
            // 「if/else の両方が return して合流点が到達不能」の場合。
            sb_printf(&e->fn, "  unreachable\n");
        }
    }
    e->scope = NULL;

    // ⑤ 組み立て
    sb_printf(&e->body, "\ndefine %s @%s(", llvm_type(n->type), ir_name);
    bool first = true;
    for (Node *pm = n->params; pm; pm = pm->next) {
        sb_printf(&e->body, "%s%s %%%s.arg", first ? "" : ", ",
                  llvm_type(pm->type), pm->name);
        first = false;
    }
    // ★ 第27章：失敗しうる関数は、エラー出力ポインタを 1 本余分に取ります
    if (e->fn_raises) sb_printf(&e->body, "%sptr %%err.out", first ? "" : ", ");
    sb_printf(&e->body, ") {\nentry:\n");
    sb_printf(&e->body, "%s", sb_str(&e->allocas));
    sb_printf(&e->body, "%s", sb_str(&e->fn));
    sb_printf(&e->body, "}\n");
}

// グローバル変数を出力する（言語仕様 6.2）
static void gen_global(Emitter *e, Node *n) {
    // 初期化式がリテラルであることは sema が保証している
    if (n->type->kind == TY_STR) {
        char *lab = intern_str(e, n->rhs->sval, n->rhs->slen);
        sb_printf(&e->globals, "%s = global ptr %s\n", n->ir_name, lab);
        return;
    }
    // ★ float は値ではなく、字句解析器が正規化した文字列を出します
    //   （src/lexer.c の read_number 参照）。
    if (n->type->kind == TY_FLOAT) {
        sb_printf(&e->globals, "%s = global double %s\n", n->ir_name,
                  n->rhs->sval);
        return;
    }
    sb_printf(&e->globals, "%s = global %s %lld\n", n->ir_name,
              llvm_mem_type(n->type), n->rhs->ival);
}

// C の main を出力する。
//
// Polonium の main は int（= i64）を返しますが、C の main は i32 を返します。
// そこで「ユーザーの main を @<モジュール>.main として出し、@main は
// それを呼んで trunc するラッパにする」方式をとります。
// （ir-conventions.md 第7節の方式 A）
//
// 🤔 なぜラッパ方式か：main を他の関数と同じ規則で生成できるので、
//    コード生成器に「main だけ特別」という分岐が入りません。
//
// ⚠️ 第13章：これを出すのは入口モジュールだけです。
//    全モジュールが出すと、リンク時に @main が重複します。
static void gen_c_main(Emitter *e, const char *main_ir_name) {
    e->tmp_counter = 0;

    // ★ 第14章：argc / argv を受け取り、ランタイムに預けます。
    //   sys.argv() はこれを list[str] にして返します。
    //   ラッパ方式にしておいたので、利用者の main も AST も無変更です。
    declare_rt(e, "void @pl_set_args(i64, ptr)");

    sb_printf(&e->body, "\n");
    sb_printf(&e->body, "define i32 @main(i32 %%argc, ptr %%argv) {\n");
    sb_printf(&e->body, "entry:\n");

    char *a = new_tmp(e);
    sb_printf(&e->body, "  %s = sext i32 %%argc to i64\n", a);
    sb_printf(&e->body, "  call void @pl_set_args(i64 %s, ptr %%argv)\n", a);

    char *t0 = new_tmp(e);
    sb_printf(&e->body, "  %s = call i64 @%s()\n", t0, main_ir_name);

    char *t1 = new_tmp(e);
    sb_printf(&e->body, "  %s = trunc i64 %s to i32\n", t1, t0);

    // ★ 第31章：下位 8 ビットに切り詰めます（言語仕様 6.1）。
    //   POSIX は終了コードを勝手に & 0xFF しますが、Windows は 32 ビットを
    //   そのまま返すので、-3 が 0xFFFFFFFD になって「異常終了」に見えます。
    //   ここで揃えておけば、**どの OS でも同じ終了コード**になります
    //   （CI の Windows ジョブが見つけた差）。
    char *t2 = new_tmp(e);
    sb_printf(&e->body, "  %s = and i32 %s, 255\n", t2, t1);

    sb_printf(&e->body, "  ret i32 %s\n", t2);
    sb_printf(&e->body, "}\n");
}

// ── 入口 ───────────────────────────────────────────────────

// モジュール 1 つぶんの IR を作る。
//
// ★ 第13章：「1 ファイル = 1 モジュール = 1 つの .ll」（13.2 節）。
//   import したモジュールのものは、使ったぶんだけ declare / 型定義の複製が
//   自動で付いてきます（class_type / declare_extern が「出済みか」を見るため）。
char *codegen(Module *mod, const char *main_ir_name, bool drop,
              const char *triple) {
    Node *ast = mod->ast;

    Emitter e = {0};
    e.ast = ast;
    e.drop = drop;  // 第25章：解放を挿入するか
    sb_init(&e.header);
    sb_init(&e.globals);
    sb_init(&e.decls);
    sb_init(&e.body);
    sb_init(&e.dropdefs);

    // ① ヘッダ
    sb_printf(&e.header, "; Generated by " PLC_LANG_CC "\n");
    sb_printf(&e.header, "source_filename = \"%s\"\n", mod->path);

    // ⚠️ 規約 R11：target triple は必ず出力する。
    //    書かないと clang が -Woverride-module 警告を出します。
    // ★ 第31章：triple は外から渡します（--target / pragma target）。
    //   NULL ならビルド時に埋め込んだ既定値（＝この機械のもの）。
    if (!triple) triple = PLC_TARGET_TRIPLE;
    if (triple[0]) sb_printf(&e.header, "target triple = \"%s\"\n", triple);

    // ② クラスの型定義（★ 使う側より先に、モジュールの先頭に出す）
    for (Node *d = ast->body; d; d = d->next) {
        if (d->kind == ND_CLASS) gen_class_type(&e, d->cls);
    }

    // ⑤ グローバル変数と関数定義
    for (Node *d = ast->body; d; d = d->next) {
        if (d->kind == ND_VARDECL) gen_global(&e, d);
        // ★ 第31章：pragma は生成に何も出しません（main.c が読むだけ）
    }
    for (Node *d = ast->body; d; d = d->next) {
        // ★ 第14章：extern は宣言だけを出す（定義は C 側にある）
        if (d->kind == ND_FUNC && !d->body) {
            StrBuf types;
            sb_init(&types);
            bool first = true;
            for (Node *pm = d->params; pm; pm = pm->next) {
                sb_printf(&types, "%s%s", first ? "" : ", ", llvm_type(pm->type));
                first = false;
            }
            declare_extern(&e, llvm_type(d->type), d->name, sb_str(&types));
            continue;
        }
        if (d->kind == ND_FUNC) gen_func(&e, d);
        // メソッドも、ふつうの関数とまったく同じ関数で出します。
        // 違うのは名前（@lexer.Token.show）と、第 1 引数が self であることだけ。
        if (d->kind == ND_CLASS)
            for (Node *m = d->body; m; m = m->next)
                if (m->kind == ND_FUNC) gen_func(&e, m);
    }

    // ⚠️ C の main を出すのは入口モジュールだけ（重複定義になるため）
    if (main_ir_name) gen_c_main(&e, main_ir_name);

    // バッファを規定の順に連結する
    //
    // ★ 第25章：生成した @drop.* は最後に置きます（関数定義と同じ扱い）。
    StrBuf out;
    sb_init(&out);
    sb_printf(&out, "%s", sb_str(&e.header));
    sb_printf(&out, "%s", sb_str(&e.globals));
    sb_printf(&out, "%s", sb_str(&e.decls));
    sb_printf(&out, "%s", sb_str(&e.body));
    sb_printf(&out, "%s", sb_str(&e.dropdefs));
    return sb_str(&out);
}
