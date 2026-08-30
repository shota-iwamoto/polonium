// hosted.c — PC の上で動かすときのランタイム（第31章で分離）
//
// ★ 役割は 2 つです。
//   ① core.c が求める 4 つのフックを libc で実装する
//   ② ファイル入出力など、**OS があるからこそ使える機能**を提供する
//
// ⚠️ ベアメタル（第32章〜）ではこのファイルをリンクしません。
//    代わりにカーネルが同じ 4 つのフックを実装します。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ⚠️ WIFEXITED / WEXITSTATUS は POSIX の <sys/wait.h> にあります。
//    macOS では <stdlib.h> が連れてきますが、Linux では明示しないと通りません
//    （CI の Linux ジョブが最初に見つけた移植性の穴です）。
//    Windows にはこのヘッダが無いので、system() の戻り値をそのまま使います。
#ifndef _WIN32
#include <sys/wait.h>
#else
// ⚠️ Windows の標準出力は既定で「テキストモード」で、\n を \r\n に書き換えます。
//    それでは **書いたバイトと出るバイトが違う**ことになり、
//    「どの OS でも同じ結果」という約束が崩れます（CI の Windows ジョブが
//    出力の不一致で見つけました）。binary モードに切り替えて、
//    print が書いた通りのバイトを出します。
#include <fcntl.h>
#include <io.h>
#endif

#include "core.h"

// core.c が使う関数のうち、ここでも使うもの
char *pl_str_alloc(long long len);
char *pl_str_from_cstr(const char *s);
long long pl_str_len(const char *s);
_Noreturn void pl_panic(const char *msg);
void *pl_alloc(long long size);

// ★ リストの中身（PlList）は core.c の中だけの秘密です。
//   ここでは「ポインタとして受け渡す」だけなので、不完全型で足ります。
typedef struct PlList PlList;
PlList *pl_list_new(void);
void pl_list_push_ptr(PlList *l, void *v);

// ── ① フックの実装（libc で）────────────────────────────────

void *pl_hook_alloc(long long size) { return calloc(1, (size_t)size); }

void pl_hook_free(void *p) { free(p); }

void pl_hook_write(const char *s, long long len) {
#ifdef _WIN32
    static int mode_set = 0;
    if (!mode_set) {
        mode_set = 1;
        _setmode(_fileno(stdout), _O_BINARY);
        _setmode(_fileno(stderr), _O_BINARY);
    }
#endif
    fwrite(s, 1, (size_t)len, stdout);
}

void pl_hook_panic(const char *msg) {
    fprintf(stderr, "runtime error: %s\n", msg);
    exit(1);
}

// ── ② OS があるからこそ使える機能 ───────────────────────────

static long long g_argc;
static char **g_argv;

// stderr にそのまま書く（改行は付けない）。第18章。
//
// ★ コンパイラは診断を stderr に書きます。print は stdout なので、
//   セルフホストの診断にはこれが要ります（移植で見つかった穴）。
void pl_eprint(const char *s) {
#ifdef _WIN32
    _setmode(_fileno(stderr), _O_BINARY);  // 上と同じ理由（第31章）
#endif
    fputs(s, stderr);
}

_Noreturn void pl_exit(long long code) { exit((int)code); }

char *pl_read_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        char buf[512];
        snprintf(buf, sizeof(buf), "cannot open file: %s", path);
        pl_panic(buf);
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *buf = pl_str_alloc((long long)size);
    size_t got = fread(buf, 1, (size_t)size, fp);
    buf[got] = '\0';
    // ⚠️ テキストモードの差などで読めたバイト数が減ることがあるので、
    //    実際に読めた長さで書き直します（長さは 8 バイト手前）。
    ((long long *)buf)[-1] = (long long)got;
    fclose(fp);
    return buf;
}

// ── 標準入力（第35章）────────────────────────────────────────
//
// ⚠️ **core.c ではなく、ここ（hosted.c）に置きます。**
//    ベアメタルには標準入力がありません。core.c に置くと、カーネル側に
//    「使わないのに実装しなければならないフック」を強いることになります
//    （docs/design/os-support.md の 4 フックを増やさない、という判断）。

// 1 行読む。改行は**含めません**。EOF で 1 行も読めなければ NULL
// （Polonium 側では str | None の None になります）。
char *pl_read_line(void) {
    long long cap = 128;
    long long n = 0;
    char *buf = (char *)malloc((size_t)cap);
    if (!buf) pl_panic("out of memory");

    int c = fgetc(stdin);
    if (c == EOF) {
        free(buf);
        return NULL;
    }
    // ⚠️ 改行の直前の '\r' を落とします。Windows で作ったファイルを
    //    読んだときに、末尾に見えない文字が残らないようにするためです。
    while (c != EOF && c != '\n') {
        if (n + 1 >= cap) {
            cap *= 2;
            char *g = (char *)realloc(buf, (size_t)cap);
            if (!g) pl_panic("out of memory");
            buf = g;
        }
        buf[n++] = (char)c;
        c = fgetc(stdin);
    }
    if (n > 0 && buf[n - 1] == '\r') n--;

    char *out = pl_str_alloc(n);
    memcpy(out, buf, (size_t)n);
    out[n] = '\0';
    free(buf);
    return out;
}

// input(prompt) — プロンプトを出して 1 行読む（第44章）
//
// ★ Python の input() に合わせます。
//   ⚠️ **EOF では panic します。** Python も EOFError を投げます。
//     「読めなかった」を静かに空文字列にすると、ループが止まらなくなります。
//     読めないかもしれない場面では io.read_line()（None が返る）を使ってください。
char *pl_input(const char *prompt) {
    if (prompt && prompt[0]) {
        fputs(prompt, stdout);
        fflush(stdout);
    }
    char *line = pl_read_line();
    if (!line) pl_panic("input: 入力がありません（EOF）");
    return line;
}

// 標準入力を最後まで読む。何も無ければ空文字列。
char *pl_read_all(void) {
    long long cap = 4096;
    long long n = 0;
    char *buf = (char *)malloc((size_t)cap);
    if (!buf) pl_panic("out of memory");

    while (1) {
        if (n == cap) {
            cap *= 2;
            char *g = (char *)realloc(buf, (size_t)cap);
            if (!g) pl_panic("out of memory");
            buf = g;
        }
        size_t got = fread(buf + n, 1, (size_t)(cap - n), stdin);
        if (got == 0) break;
        n += (long long)got;
    }

    char *out = pl_str_alloc(n);
    memcpy(out, buf, (size_t)n);
    out[n] = '\0';
    free(buf);
    return out;
}

void pl_write_file(const char *path, const char *text) {
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        char buf[512];
        snprintf(buf, sizeof(buf), "cannot write file: %s", path);
        pl_panic(buf);
    }
    fputs(text, fp);
    fclose(fp);
}

// ⚠️ bool ではなく int を返します。extern の境界を bool は越えられません
//    （14.2 節。C の _Bool と i1 の ABI が環境依存のため）。
long long pl_file_exists(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    fclose(fp);
    return 1;
}

// 生成される C の main が、いちばん最初に呼びます。
void pl_set_args(long long argc, char **argv) {
    g_argc = argc;
    g_argv = argv;
}

// ⚠️ system() が返すのは「終了コード」ではなく wait(2) の状態値です。
//    そのまま返すと exit 3 が 768（3 << 8）に見えて驚くので、
//    ここで終了コードに直します。境界の食い違いはランタイムで吸収します。
// ファイルを削除する（失敗しても何もしない）。第20章。
// ★ コンパイラは中間ファイル（.ll）を片付ける必要があります。
void pl_remove(const char *path) { remove(path); }

// 環境変数を読む（無ければ空文字列）。第18章。
//
// ★ stage1 が標準ライブラリの場所を知るために使います。C 版はビルド時に
//   埋め込みますが（-DPLC_LIB_DIR）、Polonium にプリプロセッサは無いので
//   実行時に読みます。C 版も同じ環境変数を見るようにして、挙動を揃えます。
char *pl_getenv(const char *name) {
    const char *v = getenv(name);
    if (!v) return pl_str_from_cstr("");
    return pl_str_from_cstr(v);
}

long long pl_system(const char *cmd) {
    int st = system(cmd);
    if (st == -1) return -1;
#ifdef _WIN32
    // Windows の system() は終了コードをそのまま返します
    return (long long)st;
#else
    if (WIFEXITED(st)) return (long long)WEXITSTATUS(st);
    if (WIFSIGNALED(st)) return (long long)(128 + WTERMSIG(st));
    return (long long)st;
#endif
}

// argv を list[str] にして返す（第14章）。
//
// ★ PlList を使うので、list の実装より後ろに置いています。
//   argv の文字列はプロセスの寿命のあいだ有効なので、複製せずそのまま指します。
PlList *pl_argv(void) {
    PlList *l = pl_list_new();
    // ⚠️ argv の文字列は C のものなので長さヘッダがありません。
    //    Polonium の str として渡すには作り直す必要があります（第15章）。
    for (long long i = 0; i < g_argc; i++)
        pl_list_push_ptr(l, pl_str_from_cstr(g_argv[i]));
    return l;
}
