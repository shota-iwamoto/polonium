// main.c — コマンドライン処理と各パスの起動
//
//   poloniumc [options] <input.po>
//
// パイプライン（第13章から）：
//   load_modules（import をたどって読み込み・構文解析）
//     → sema_program（全モジォールをまとめて検査）
//     → ownck_program（所有権の検査。第22章）
//     → codegen（モジュールごとに .ll）
//     → clang（.ll を全部渡してリンク）
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ast.h"
#include "codegen.h"
#include "lexer.h"
#include "module.h"
#include "ownck.h"
#include "parser.h"
#include "langinfo.h"
#include "sema.h"
#include "types.h"
#include "util.h"

// ビルド時に Makefile が -DPLC_RUNTIME_O=... で渡してきます（第9章）。
#ifndef PLC_RUNTIME_O
#define PLC_RUNTIME_O "build/runtime.a"
#endif

// 生成物をリンクするのに使う clang（第31章）。
// ★ clang-18 のように名前が違う環境があるので、ビルド時に埋め込み、
//   環境変数 PLC_CLANG でも上書きできるようにします。
#ifndef PLC_CLANG
#define PLC_CLANG "clang"
#endif

// 実際に使う clang を返す（環境変数 → ビルド時の埋め込み）。
static const char *clang_cmd(void) {
    const char *env = getenv("PLC_CLANG");
    if (env && env[0]) return env;
    return PLC_CLANG;
}

static void usage(int status) {
    FILE *out = status == 0 ? stdout : stderr;
    fprintf(out,
            PLC_LANG_NAME " コンパイラ (stage0)\n"
            "\n"
            "使い方: " PLC_LANG_CC " [オプション] <入力" PLC_LANG_EXT ">\n"
            "\n"
            "オプション:\n"
            "  -o <file>       出力する実行ファイル名（既定: a.out）\n"
            "  -S              LLVM IR を標準出力に書いて終了\n"
            "  --dump-tokens   トークン列を表示して終了（字句解析のデバッグ用）\n"
            "  --dump-ast      AST を S 式で表示して終了（構文解析のデバッグ用）\n"
            "  --keep-ll       実行ファイル生成後も .ll を残す\n"
            "  --check         型検査までで止める（エラーが無ければ何も出さない）\n"
            "  --deny-move     移動済みの値の使用を警告ではなくエラーにする\n"
            "  --deny-borrow   借用した値の保存・返却を警告ではなくエラーにする\n"
            "  --deny-mut      読み取り専用の借用への書き換えを警告ではなくエラーにする\n"
            "  --explain-mut   呼び出しで変更される実引数を一覧表示して終了\n"
            "  --drop          スコープの出口に解放（drop）を挿入する\n"
            "  -c              リンクせずオブジェクト（.o）を出す\n"
            "  --target=<t>    生成する IR の target triple を指定する\n"
            "                  （例: --target=riscv64-unknown-elf）\n"
            "  -O0|-O1|-O2|-O3 clang に渡す最適化レベル（既定: -O0）\n"
            "  -h, --help      この使い方を表示\n");
    exit(status);
}

// 実行するステージ
typedef enum {
    STAGE_ALL,          // 実行ファイルまで作る
    STAGE_DUMP_TOKENS,  // 字句解析まで
    STAGE_DUMP_AST,     // 構文解析まで
    STAGE_EMIT_IR,      // コード生成まで（-S）
    STAGE_CHECK,        // 意味解析まで（--check。第18章）
    STAGE_EXPLAIN_MUT,  // 所有権検査まで。変更される実引数を並べる（--explain-mut。第24章）
} Stage;

typedef struct {
    const char *input;
    const char *output;
    const char *opt_level;
    Stage stage;
    int keep_ll;
    int deny_move;    // --deny-move（第22章）
    int deny_borrow;  // --deny-borrow（第23章）
    int deny_mut;     // --deny-mut（第24章）
    int drop;         // --drop（第25章。解放を挿入する）
    const char *target;  // --target=<triple>（第31章。ベアメタル向け）
    int emit_obj;        // -c（リンクせずオブジェクトを出す。第31章）
} Options;

static Options parse_args(int argc, char **argv) {
    Options o = {0};
    o.output = "a.out";
    o.opt_level = "-O0";
    o.stage = STAGE_ALL;

    for (int i = 1; i < argc; i++) {
        char *a = argv[i];

        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) usage(0);

        if (strcmp(a, "-o") == 0) {
            if (i + 1 >= argc) error("-o の後に出力ファイル名が必要です");
            o.output = argv[++i];
            continue;
        }
        if (strcmp(a, "-S") == 0) { o.stage = STAGE_EMIT_IR; continue; }
        if (strcmp(a, "--dump-tokens") == 0) { o.stage = STAGE_DUMP_TOKENS; continue; }
        if (strcmp(a, "--dump-ast") == 0) { o.stage = STAGE_DUMP_AST; continue; }
        if (strcmp(a, "--keep-ll") == 0) { o.keep_ll = 1; continue; }
        // ★ 第18章：型検査までで止める。stage1（Polonium 版）と
        //   「エラーが出るか / 出ないか」を突き合わせるために使います。
        if (strcmp(a, "--check") == 0) { o.stage = STAGE_CHECK; continue; }
        // ★ 第22章：所有権の検査（ownck）の結果をエラーに昇格させる。
        if (strcmp(a, "--deny-move") == 0) { o.deny_move = 1; continue; }
        if (strcmp(a, "--deny-borrow") == 0) { o.deny_borrow = 1; continue; }
        if (strcmp(a, "--deny-mut") == 0) { o.deny_mut = 1; continue; }
        // ★ 第25章：解放（drop）の挿入。既定では入れません（決定 D16）。
        if (strcmp(a, "--drop") == 0) { o.drop = 1; continue; }
        // ★ 第31章：ベアメタル向け。リンクは自分でやるので -c で止める。
        if (strcmp(a, "-c") == 0) { o.emit_obj = 1; continue; }
        if (strncmp(a, "--target=", 9) == 0) { o.target = a + 9; continue; }
        // ★ 第24章：呼び出し側に mut を書かせない代わりの道具（仕様 §5.3）。
        if (strcmp(a, "--explain-mut") == 0) { o.stage = STAGE_EXPLAIN_MUT; continue; }

        if (strcmp(a, "-O0") == 0 || strcmp(a, "-O1") == 0 ||
            strcmp(a, "-O2") == 0 || strcmp(a, "-O3") == 0) {
            o.opt_level = a;
            continue;
        }

        if (a[0] == '-' && a[1] != '\0') error("不明なオプション: %s", a);

        if (o.input) error("入力ファイルが複数指定されています: %s と %s", o.input, a);
        o.input = a;
    }

    if (!o.input) usage(1);
    return o;
}

// ── ランタイムと標準ライブラリの探し方 ─────────────────────
//
// ★ 探す順番は 3 つ（第31章で「配って使える」ようにするため足しました）。
//   ① 環境変数（PLC_RUNTIME_O / PLC_LIB_DIR）
//   ② ビルド時に埋め込んだ絶対パス（ソースの木の中で使うとき）
//   ③ **実行ファイルからの相対**（インストールしたとき / 配布物を展開したとき）
//
//      <prefix>/bin/poloniumc
//      <prefix>/lib/polonium/runtime.a
//      <prefix>/lib/polonium/lib/*.po
//
// ⚠️ ③ が無いと、ビルドした場所を動かした瞬間に動かなくなります。
//    「ダウンロードして展開したら動く」ためには、この規則が要ります。
static bool file_exists(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    fclose(fp);
    return true;
}

// argv[0] から実行ファイルのあるディレクトリを求める（無理なら NULL）
static char *exe_dir(const char *argv0) {
    if (!argv0 || !argv0[0]) return NULL;
    const char *slash = NULL;
    for (const char *c = argv0; *c; c++)
        if (*c == '/' || *c == '\\') slash = c;
    if (!slash) return NULL;  // PATH 経由で起動された（相対を諦める）
    return xstrndup(argv0, (size_t)(slash - argv0));
}

static char *g_exe_dir;  // main が最初に埋める

// <exe>/../lib/polonium/<name> を組み立てる
static char *installed_path(const char *name) {
    if (!g_exe_dir) return NULL;
    StrBuf sb;
    sb_init(&sb);
    sb_printf(&sb, "%s/../lib/polonium/%s", g_exe_dir, name);
    return sb_str(&sb);
}

// module.c が「標準ライブラリの場所」を聞きに来る（第31章）
const char *plc_installed_lib_dir(void) {
    char *p = installed_path("lib");
    if (!p) return NULL;
    StrBuf probe;
    sb_init(&probe);
    sb_printf(&probe, "%s/strings" PLC_LANG_EXT, p);
    return file_exists(sb_str(&probe)) ? p : NULL;
}

static const char *runtime_o(void) {
    const char *env = getenv("PLC_RUNTIME_O");
    if (env && env[0]) return env;
    if (file_exists(PLC_RUNTIME_O)) return PLC_RUNTIME_O;
    char *p = installed_path("runtime.a");
    if (p && file_exists(p)) return p;
    return PLC_RUNTIME_O;  // 見つからないときは、埋め込んだ値でエラーを出させる
}

// 出力ファイル名とモジュール名から .ll のパスを作る。
//   a.out + main  → a.out.main.ll
//
// ★ 第13章：モジュールごとに 1 本出すので、名前にモジュール名を挟みます。
static char *ll_path_for(const char *output, const char *mod_name) {
    StrBuf sb;
    sb_init(&sb);
    sb_printf(&sb, "%s.%s.ll", output, mod_name);
    return sb_str(&sb);
}

int main(int argc, char **argv) {
    g_exe_dir = exe_dir(argv[0]);  // ★ 配布物でも動くように（上の runtime_o を参照）
    Options opt = parse_args(argc, argv);

    // プリミティブ型のシングルトンを用意する（types.h 参照）
    types_init();

    // ── ①② 字句解析・構文解析だけを見たいとき（入口ファイルのみ）──
    //
    // ⚠️ --dump-tokens / --dump-ast は import をたどりません。
    //    「1 ファイルの中身を確かめる」道具だからです。
    if (opt.stage == STAGE_DUMP_TOKENS || opt.stage == STAGE_DUMP_AST) {
        char *src = read_file(opt.input);
        TokenVec toks = tokenize(opt.input, src);
        if (opt.stage == STAGE_DUMP_TOKENS) {
            dump_tokens(toks);
            return 0;
        }
        // ⚠️ --dump-ast は sema の前に出します。
        //    構文解析だけを独立して確認したいためです（型エラーがあっても木は見たい）。
        dump_ast(parse(toks));
        return 0;
    }

    // ── ⓪ 読み込み：import をたどって依存順に並べる（第13章）──
    Module *entry = NULL;
    Module *mods = load_modules(opt.input, &entry);

    // ── ③ 意味解析・型検査（全モジュールまとめて）──
    sema_program(mods, entry);

    // --check : ここで終わり（エラーがあれば sema が既に終了している）
    //
    // ⚠️ --check は「③ 型検査まで」です。所有権の検査（④）は走りません。
    //    stage1（Polonium 版）にはまだ ownck が無く、--check の出力を
    //    突き合わせて比較しているためです（tests/selfhost.sh）。
    //    第29章で移植したら、ここも ownck を通すように変えます。
    if (opt.stage == STAGE_CHECK) return 0;

    // ── 第31章：target triple を決める ──
    //
    //   ① --target=... が最優先
    //   ② ソースの pragma target "..."
    //   ③ ビルド時に埋め込んだ、この機械のもの（codegen の既定）
    const char *triple = opt.target;
    bool no_runtime = false;
    for (Node *d = entry->ast->body; d; d = d->next) {
        if (d->kind != ND_PRAGMA) continue;
        if (!triple && strcmp(d->name, "target") == 0) triple = d->sval;
        // ★ 第32章：ベアメタルでは C の main も argv も無い。
        //   「@main のラッパを出さない」がこの pragma の意味です。
        if (strcmp(d->name, "no_runtime") == 0) no_runtime = true;
    }

    // ── ④ 所有権の検査（第22章）──
    //
    // ★ 既定は警告です。移動済みの値を使っていても、生成される IR は
    //   v1 のまま変わりません（解放の挿入は第25章）。
    OwnckOptions own = {0};
    own.deny_move = opt.deny_move;
    own.deny_borrow = opt.deny_borrow;
    own.deny_mut = opt.deny_mut;
    own.explain_mut = opt.stage == STAGE_EXPLAIN_MUT;
    ownck_program(mods, &own);

    if (opt.stage == STAGE_EXPLAIN_MUT) return 0;

    // 入口モジュールの main の IR 名（@main のラッパが呼ぶ相手）
    StrBuf main_ir;
    sb_init(&main_ir);
    sb_printf(&main_ir, "%s.main", entry->name);

    // ── ④ コード生成（モジュールごとに 1 本の .ll）──
    for (Module *m = mods; m; m = m->next) {
        const char *entry_main = (m == entry && !no_runtime) ? sb_str(&main_ir) : NULL;
        char *ir = codegen(m, entry_main, opt.drop != 0, triple);

        if (opt.stage == STAGE_EMIT_IR) {
            // -S : IR を出して終了。複数モジュールなら区切りを入れて並べる。
            if (mods->next) printf("; ── module: %s ──\n", m->name);
            fputs(ir, stdout);
            continue;
        }

        m->ll_path = ll_path_for(opt.output, m->name);
        write_file(m->ll_path, ir);
    }
    if (opt.stage == STAGE_EMIT_IR) return 0;

    // ── 第31章：-c ならオブジェクトを出して終わり ──
    //
    // ★ ベアメタル（第32章）では、リンクはこちらの仕事ではありません。
    //   リンカスクリプトを渡すのも、起動アセンブリを混ぜるのも利用者側です。
    //
    // ⚠️ いまは 1 モジュールだけ対応します。import を含むカーネルは、
    //    モジュールごとに .o を作って自分でリンクしてください。
    if (opt.emit_obj) {
        if (mods->next)
            error("-c は 1 モジュールのファイルにだけ使えます"
                  "（import があるときは、モジュールごとに分けてください）");

        StrBuf oc;
        sb_init(&oc);
        // ★ RISC-V などは Apple の clang が対応していないことがあるので、
        //   PLC_CLANG で使う clang を差し替えられるようにします。
        const char *cc = clang_cmd();
        // ⚠️ 引用は "…" にします。Windows の system() は cmd.exe を通すので、
        //    '…' は引用符として扱われません（POSIX の sh は "…" も理解します）。
        sb_printf(&oc, "%s %s -Wno-override-module -c \"%s\" -o \"%s\"", cc,
                  opt.opt_level, entry->ll_path, opt.output);
        if (triple) sb_printf(&oc, " --target=%s", triple);
        const char *extra = getenv("PLC_CFLAGS");
        if (extra && extra[0]) sb_printf(&oc, " %s", extra);

        int orc = system(sb_str(&oc));
        if (orc != 0) {
            fprintf(stderr, "error: オブジェクトの生成に失敗しました\n  %s\n",
                    sb_str(&oc));
            return 1;
        }
        if (!opt.keep_ll) unlink(entry->ll_path);
        return 0;
    }

    // ── ⑤ clang に丸投げして実行ファイルを作る ──
    //
    // ★ Windows では拡張子が無いと実行できないので、`.` を含まない
    //   出力名には .exe を足します（gcc / clang と同じふるまい）。
    const char *out_path = opt.output;
#ifdef _WIN32
    if (!strchr(opt.output, '.')) {
        StrBuf w;
        sb_init(&w);
        sb_printf(&w, "%s.exe", opt.output);
        out_path = sb_str(&w);
    }
#endif
    //
    // ★ 第13章：.ll を全部並べて渡します。モジュール修飾のおかげで、
    //   別ファイルの同名関数があっても duplicate symbol になりません。
    StrBuf cmd;
    sb_init(&cmd);
    sb_printf(&cmd, "%s %s", clang_cmd(), opt.opt_level);
    for (Module *m = mods; m; m = m->next) sb_printf(&cmd, " \"%s\"", m->ll_path);
    // ★ 第9章：ランタイム（runtime/runtime.c をコンパイルしたもの）をリンクする。
    sb_printf(&cmd, " \"%s\" -o \"%s\"", runtime_o(), out_path);

    int rc = system(sb_str(&cmd));
    if (rc != 0) {
        // ここに来たら、生成した IR に問題があるということ。
        // .ll を残して調査できるようにする。
        fprintf(stderr,
                "error: clang の実行に失敗しました（生成した IR に問題があります）\n"
                "  生成された IR を残しました:\n");
        for (Module *m = mods; m; m = m->next)
            fprintf(stderr, "    %s\n", m->ll_path);
        return 1;
    }

    if (!opt.keep_ll)
        for (Module *m = mods; m; m = m->next) unlink(m->ll_path);
    return 0;
}
