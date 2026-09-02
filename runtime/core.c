// core.c — ランタイムの「核」（第31章で runtime.c から分離）
//
// ★ ここには **libc に依存しないもの**だけを置きます。
//   OS を書くとき（第32章以降）、リンクできるのはこのファイルだけです。
//
// 🤔 なぜ分けるのか
//   v1 のランタイムは printf / malloc / fopen を直接呼んでいました。
//   ベアメタルにはそのどれもありません。かといって「OS 用の別ランタイム」を
//   もう 1 本書くと、同じ処理が 2 か所に増えて必ずずれます。
//   **libc に触る所だけを 4 つのフック関数に追い出せば、核は 1 本で済みます。**
//
//   ┌─────────────┐        ┌──────────────────┐
//   │  core.c      │──呼ぶ──▶│ pl_hook_alloc     │  hosted.c（PC 上）
//   │（この file） │        │ pl_hook_free      │    → calloc / free / stdout
//   │              │        │ pl_hook_write     │  kernel/（ベアメタル）
//   │              │        │ pl_hook_panic     │    → UART / 停止
//   └─────────────┘        └──────────────────┘
//
// ⚠️ このファイルでは <stdio.h> / <stdlib.h> / <string.h> を include しません。
//    必要な小道具（memcpy 相当）は自分で持ちます。

#include "core.h"

// ★ libc を include しないので、必要な定義は自分で持ちます。
#ifndef NULL
#define NULL ((void *)0)
#endif

// long long の最小値。<limits.h> を引かずに済ませます
// （-9223372036854775808 と直に書くと、まず正の定数を作ってから否定する規則の
//   ため、そのままでは long long に収まりません）。
#define PL_LLONG_MIN (-9223372036854775807LL - 1)

// ── 自前の小道具（libc の代わり）────────────────────────────
//
// ★ freestanding でも clang は memcpy / memset の呼び出しを生成することが
//   あります。ベアメタル向けにビルドするときだけ、自分で用意します。
static void *pl_memcpy(void *dst, const void *src, long long n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    for (long long i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

static long long pl_cstr_len(const char *s) {
    long long n = 0;
    while (s[n]) n++;
    return n;
}

#ifdef PL_FREESTANDING
// ★ clang は「明らかにコピー」と見たループを memcpy の呼び出しに変えることが
//   あります。libc の無い世界では、その相手も自分で用意しておきます。
void *memcpy(void *dst, const void *src, unsigned long n) {
    return pl_memcpy(dst, src, (long long)n);
}
void *memset(void *dst, int c, unsigned long n) {
    unsigned char *d = dst;
    for (unsigned long i = 0; i < n; i++) d[i] = (unsigned char)c;
    return dst;
}
#endif

// 整数を 10 進の文字列にする（snprintf の代わり）。返すのは書いた長さ。
static long long pl_itoa(long long v, char *out) {
    char tmp[24];
    int n = 0;
    int neg = v < 0;
    unsigned long long u = neg ? (unsigned long long)(-(v + 1)) + 1ULL
                               : (unsigned long long)v;
    if (u == 0) tmp[n++] = '0';
    while (u > 0) {
        tmp[n++] = (char)('0' + (u % 10));
        u /= 10;
    }
    long long len = 0;
    if (neg) out[len++] = '-';
    while (n > 0) out[len++] = tmp[--n];
    out[len] = '\0';
    return len;
}


// ── エラー ─────────────────────────────────────────────────

// 回復不能なエラー。stderr に出して終了コード 1 で死ぬ。
// 例外機構（try / except）は v1 では採用しません（言語仕様 8 節）。
_Noreturn void pl_panic(const char *msg) {
    // ★ 「どう死ぬか」は環境ごとに違うので、フックに任せます。
    pl_hook_panic(msg);
    for (;;) {  // フックは戻ってこない約束（戻ってきたらここで止まる）
    }
}

// ── メモリ ─────────────────────────────────────────────────

// ★ calloc でゼロ初期化し、失敗したら即終了する。
//   即終了にすることで、生成する IR に NULL チェックを入れずに済みます。
void *pl_alloc(long long size) {
    void *p = pl_hook_alloc(size);
    if (!p) pl_panic("out of memory");
    return p;
}

// ── None（null ポインタ）の検査（第12章）────────────────────
//
// ★ クラス型のフィールドは calloc により NULL から始まります
//   （既定値を作ろうとすると無限再帰するため。docs/chapters/ch12-class.md 12.6）。
//   init で入れ忘れたまま参照すると segfault しますが、
//   ここを通しておけば「何が起きたか分かるメッセージ」に変わります。
//
// ⚠️ 本来の解決策は型システム側（T | None と narrowing）です。第15章で塞ぎます。
//
// ★ 第45章：検査そのものは codegen が IR に展開するようになりました
//   （gen_not_none）。フィールド参照はこの言語で最も回数の多い操作で、
//   ここを呼び出しのままにしておくと -O2 でも 1 回ずつ call を払います。
//   ランタイムは別にリンクされるのでインライン化されません。

// None だったときだけ呼ばれる出口。
// ⚠️ **戻ってきません**。呼び出し側の IR は直後に unreachable を置きます。
void pl_none_fail(void) {
    pl_panic("field access on None (uninitialized reference field?)");
}

// ★ 展開後の codegen はもう呼びませんが、ランタイム内から使います。
void *pl_check_not_none(void *p) {
    if (!p) pl_none_fail();
    return p;
}

// ── 出力（print のオーバーロード）──────────────────────────

// ── double を 10 進の文字列にする（第34章）────────────────────
//
// ⚠️ **libc は使えません。** このランタイムはベアメタルでも動く必要があり、
//    snprintf("%g") に頼れません（runtime/README.md）。そこで最小限の
//    変換を自前で持ちます。
//
// 出力の規則（言語仕様 7 節。**この 1 か所が唯一の定義**です）:
//   - NaN → "nan"、無限大 → "inf" / "-inf"
//   - 小数部は最大 6 桁。末尾の 0 は落とすが、最低 1 桁は必ず残す（1.0）
//   - |v| >= 1e18 または 0 < |v| < 1e-6 は指数表記（1.5e-9）
//
// ★ 6 桁に決め打つのは「短く読める」ことを優先した割り切りです。
//   17 桁の往復保証（double → 文字列 → double が元に戻る）は**しません**。
static long long pl_utoa(unsigned long long u, char *out) {
    char tmp[24];
    int n = 0;
    if (u == 0) tmp[n++] = '0';
    while (u > 0) {
        tmp[n++] = (char)('0' + (u % 10));
        u /= 10;
    }
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    return n;
}

static long long pl_ftoa(double v, char *out) {
    long long n = 0;

    // NaN は自分自身と等しくない、という性質で見分けます
    if (v != v) {
        out[0] = 'n'; out[1] = 'a'; out[2] = 'n';
        return 3;
    }
    // 無限大は ∞ - ∞ = NaN になる、という性質で見分けます
    if (v - v != v - v) {
        if (v < 0) out[n++] = '-';
        out[n++] = 'i'; out[n++] = 'n'; out[n++] = 'f';
        return n;
    }
    if (v < 0) {
        out[n++] = '-';
        v = -v;
    }

    // 大きすぎ・小さすぎるときは [1, 10) に正規化して指数を覚える
    int exp10 = 0;
    if (v != 0.0 && (v >= 1e18 || v < 1e-6)) {
        while (v >= 10.0) { v /= 10.0; exp10++; }
        while (v < 1.0)   { v *= 10.0; exp10--; }
    }

    // 整数部と、6 桁に丸めた小数部に分ける
    unsigned long long ip = (unsigned long long)v;
    double frac = v - (double)ip;
    unsigned long long f = (unsigned long long)(frac * 1000000.0 + 0.5);
    if (f >= 1000000ULL) {   // 繰り上がり（0.9999996 → 1.0）
        f = 0;
        ip++;
    }

    n += pl_utoa(ip, out + n);
    out[n++] = '.';

    // 小数部は必ず 6 桁書いてから、末尾の 0 を落とす
    char fb[8];
    long long fn = pl_utoa(f, fb);
    for (long long i = 0; i < 6 - fn; i++) out[n++] = '0';
    for (long long i = 0; i < fn; i++) out[n++] = fb[i];
    while (out[n - 1] == '0' && out[n - 2] != '.') n--;

    if (exp10 != 0) {
        out[n++] = 'e';
        if (exp10 < 0) {
            out[n++] = '-';
            exp10 = -exp10;
        }
        n += pl_utoa((unsigned long long)exp10, out + n);
    }
    return n;
}

void pl_print_float(double v) {
    char buf[64];
    long long n = pl_ftoa(v, buf);
    buf[n++] = '\n';
    pl_hook_write(buf, n);
}

void pl_print_int(long long v) {
    char buf[26];
    long long n = pl_itoa(v, buf);
    buf[n++] = '\n';
    pl_hook_write(buf, n);
}

// stdout にそのまま書く（改行を足さない）。第19章。
// ★ print は改行を足すので、IR の出力には使えません。
void pl_print_raw(const char *s) { pl_hook_write(s, pl_cstr_len(s)); }


void pl_print_str(const char *s) {
    pl_hook_write(s, pl_cstr_len(s));
    pl_hook_write("\n", 1);
}

void pl_print_bool(long long v) {
    if (v) pl_hook_write("True\n", 5);
    else pl_hook_write("False\n", 6);
}

// ── 文字列 ─────────────────────────────────────────────────
//
// ★ 第15章：str の表現に「長さ」を持たせました。
//
//     [ i64 長さ ][ バイト列 ... ][ '\0' ]
//                  ^ str の値が指すのはここ
//
//   ⚠️ なぜ変えたか（ch15 15.7 節）
//     それまでの str は「ただの NUL 終端文字列」でした。すると
//     len(s) も s[i] も毎回 strlen する＝ O(n) になり、
//     字句解析器のように 1 文字ずつ回るコードが O(n^2) になります。
//     4000 行のソースを読むだけで数秒かかる計算で、セルフホストできません。
//
//   ★ 値が指すのは「データの先頭」のままなので、C から見ると今までどおり
//     NUL 終端の char * です（extern に渡してもそのまま使えます）。
//     長さは p[-1] の位置（8 バイト手前）にあります。

// 文字列リテラルの印（第25章）。
//
// ★ なぜ必要か
//   `s: str = "abc"` の "abc" は **プログラムに埋め込まれた定数**（.rodata）で、
//   ヒープではありません。解放しようとすると落ちます。
//   実行時にポインタだけを見て「ヒープか定数か」を判定する移植性のある方法は
//   無いので、**長さのヘッダに 1 ビットの印**を付けて区別します。
//
//   ヘッダ（8 バイト手前）:  [ 静的ビット | 長さ ]
//
// ⚠️ 印を付けるのは codegen（--drop のとき）です。ランタイム側で作る文字列は
//    すべてヒープなので、印は付きません。
#define PL_STR_STATIC (1LL << 62)

// 長さ len のバイト列を置ける str を確保する（NUL の分も含めて確保）。
char *pl_str_alloc(long long len) {
    char *base = pl_alloc(8 + len + 1);
    *(long long *)base = len;
    return base + 8;
}

// C 文字列から str を作る（argv など、ヘッダを持たない文字列から作るとき）
char *pl_str_from_cstr(const char *s) {
    long long n = (long long)pl_cstr_len(s);
    char *p = pl_str_alloc(n);
    pl_memcpy(p, s, (long long)n + 1);
    return p;
}

// ★ O(1) になりました（第15章）。
//
// ⚠️ 第25章：ヘッダの最上位ビットの 1 つを「静的な文字列」の印に使うので、
//    長さを読むときは必ず落とします（下の PL_STR_STATIC を参照）。
long long pl_str_len(const char *s) {
    return ((const long long *)s)[-1] & ~PL_STR_STATIC;
}

// 文字列の繰り返し（第39章。"ab" * 3）
char *pl_str_repeat(const char *s, long long n) {
    if (n < 0) n = 0;
    long long m = pl_str_len(s);
    char *out = pl_str_alloc(m * n);
    for (long long k = 0; k < n; k++) pl_memcpy(out + k * m, s, m);
    out[m * n] = '\0';
    return out;
}

char *pl_str_concat(const char *a, const char *b) {
    long long la = pl_str_len(a);
    long long lb = pl_str_len(b);
    char *p = pl_str_alloc(la + lb);
    pl_memcpy(p, a, (long long)la);
    pl_memcpy(p + la, b, (long long)lb);
    p[la + lb] = '\0';
    return p;
}

// strcmp の符号をそのまま返す。
// ★ これ 1 つで == != < <= > >= の 6 種類すべてに使えます
//   （生成側は結果を 0 と比べる述語を変えるだけ）。
long long pl_str_cmp(const char *a, const char *b) {
    // ⚠️ libc の strcmp は使えません（core は libc に触らない）。
    //    符号だけ分かればよいので、自分で比べます。
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    while (*x && *x == *y) {
        x++;
        y++;
    }
    if (*x == *y) return 0;
    return *x < *y ? -1 : 1;
}

char *pl_str_from_int(long long v) {
    char buf[26];
    long long n = pl_itoa(v, buf);
    char *p = pl_str_alloc(n);
    pl_memcpy(p, buf, n + 1);
    return p;
}

char *pl_str_from_float(double v) {
    char buf[64];
    long long n = pl_ftoa(v, buf);
    buf[n] = '\0';
    char *p = pl_str_alloc(n);
    pl_memcpy(p, buf, n + 1);
    return p;
}

// int ↔ float の変換（言語仕様 7 節）。
//
// ★ 命令 1 つ（sitofp / fptosi）で済みますが、**組み込み関数の仕組みに
//   そのまま乗せる**ためにランタイム関数にしています。コード生成器に
//   float 専用の分岐を増やさずに済みます。
// ⚠️ float → int は **0 方向への切り捨て**です（-1.7 → -1）。
//    Python の int() と同じで、round() ではありません。
double pl_float_from_int(long long v) { return (double)v; }
// float → int（0 方向へ切り捨て）
//
// ★ 第47章：**範囲の外を黙って通しません。** C の (long long) キャストは
//   範囲外だと未定義動作で、実際には int の最小値が返っていました
//   （int(1.0e30) が -9223372036854775808）。NaN も同じです。
//   ⚠️ 境界は正確です。9223372036854775808.0（＝ int の最大値 + 1）は
//     double でちょうど表せるので、これ以上を弾けば足ります。
//     最小値のほうは -9223372036854775808.0 が表せるので、そのものは通します。
long long pl_int_from_float(double v) {
    if (v != v) pl_panic("int(): not a number (NaN)");
    if (v >= 9223372036854775808.0 || v < -9223372036854775808.0)
        pl_panic("int(): out of range");
    return (long long)v;
}

char *pl_str_from_bool(long long v) {
    return pl_str_from_cstr(v ? "True" : "False");
}

// 文字列を整数にする。パースできなければ実行時エラー（言語仕様 7 節）。
long long pl_str_to_int(const char *s) {
    // ⚠️ libc の strtoll は使えないので、自分で読みます（第31章）。
    //    ★ v1 と同じ規則：符号 1 個 + 数字 1 個以上、余りがあれば panic。
    const char *p = s;
    int neg = 0;
    if (*p == '-' || *p == '+') {
        neg = *p == '-';
        p++;
    }
    if (*p < '0' || *p > '9') pl_panic("int(): not a number");

    // ★ 第47章：**桁があふれたら panic します。**
    //   int("99999999999999999999") が黙って 7766279631452241919 に
    //   なっていました。
    //   ⚠️ 負の側は 1 つ広い（-9223372036854775808 まで）ので、
    //     符号を見て上限を変えます。累算は符号なしで行い、
    //     1 桁進めるたびに上限と比べます。
    unsigned long long limit = neg ? 9223372036854775808ULL
                                   : 9223372036854775807ULL;
    unsigned long long v = 0;
    while (*p >= '0' && *p <= '9') {
        unsigned long long d = (unsigned long long)(*p - '0');
        if (v > (limit - d) / 10ULL)
            pl_panic("int(): out of range");
        v = v * 10ULL + d;
        p++;
    }
    if (*p != '\0') pl_panic("int(): not a number");
    // ⚠️ -9223372036854775808 は long long の正の側に無いので、
    //   符号なしのまま否定してから変換します。
    if (neg) return (long long)(0ULL - v);
    return (long long)v;
}

long long pl_ord(const char *s) {
    if (s[0] == '\0') pl_panic("ord(): empty string");
    return (long long)(unsigned char)s[0];
}

char *pl_chr(long long v) {
    if (v < 0 || v > 255) pl_panic("chr(): out of range");
    char *p = pl_str_alloc(1);
    p[0] = (char)v;
    p[1] = '\0';
    return p;
}

// ── 検査つきの算術（規約 R10）──────────────────────────────
//
// ★ 0 除算は SIGFPE でプロセスが死にます。何が起きたか分からないより、
//   メッセージを出して死ぬほうがずっと親切です。
//   分岐を IR に出さず、ランタイム関数に押し込むのが R10 の実践です。

// ★ 第46章：'//' は **切り捨て（truncate）ではなく切り下げ（floor）** です。
//
//   C の '/' は 0 の方向へ丸めるので -7 / 2 == -3 になります。Polonium は
//   Python と同じく負の無限大の方向へ丸め、-7 // 2 == -4 とします。
//   「Python の書きやすさは絶対」（ロードマップ §0）に従った判断です。
//
//   ⚠️ 商だけ直して余りを直さないと a == (a // b) * b + a % b が崩れます。
//     この等式が成り立つことは tests/cases/floordiv_sign.po で固定しています。
//
//   ⚠️ 検査は 1 つ増えますが、どのみち 0 除算のためにこの関数を通るので
//     命令数の増分だけです（呼び出しは元から 1 回）。
long long pl_floordiv(long long a, long long b) {
    if (b == 0) pl_panic("division by zero");
    // ★ PL_LLONG_MIN / -1 は C では未定義動作で、実際には SIGFPE で落ちます。
    //   何が起きたか分からないまま死ぬより、名前を付けて死にます（規約 R10）。
    if (b == -1 && a == PL_LLONG_MIN) pl_panic("integer overflow in //");
    long long q = a / b;
    // 符号が食い違っていて、割り切れていないときだけ 1 つ下げる。
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}

// 余りは **除数と同じ符号** になります（Python と同じ）。
//   -7 % 2 == 1 / 7 % -2 == -1
// ── 桁あふれ（第47章）────────────────────────────────────
//
// ★ codegen が出す `llvm.sadd/ssub/smul.with.overflow` の失敗側から呼ばれます。
//   ⚠️ **戻りません。** IR は直後に unreachable を置きます。
//   ⚠️ 宣言には noreturn と cold の両方が付きます（第46章）。付けないと
//     この呼び出しの費用が、囲む関数のインライン化の見積りに入ります。
//
// 引数は演算の種類です。文字列を渡すと演算のたびに大域定数が増えるので、
// 番号にしてメッセージはこちら側に持ちます。
void pl_overflow_fail(long long op) {
    if (op == 0) pl_panic("integer overflow in +");
    if (op == 1) pl_panic("integer overflow in -");
    if (op == 2) pl_panic("integer overflow in *");
    pl_panic("integer overflow in unary -");
}

long long pl_mod(long long a, long long b) {
    if (b == 0) pl_panic("division by zero");
    // ★ こちらの答えは 0 で確定していますが、a % b の計算自体が
    //   PL_LLONG_MIN % -1 で落ちるので、割る前に返します。
    if (b == -1) return 0;
    long long r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) r += b;
    return r;
}

// 掛け算があふれたかを見る。あふれたら 1 を返す。
//
// ★ ハードウェアの桁あふれフラグを使う組み込み（__builtin_mul_overflow）は
//   使いません。ベアメタル（RISC-V）でライブラリ呼び出しに化けないことを
//   保証したいので、割り戻して確かめる形にしてあります。
// ⚠️ -1 を先に外すのは、p / b が b == -1 で未定義動作になるためです。
static int pl_mul_ovf(long long a, long long b, long long *out) {
    if (a == 0 || b == 0) { *out = 0; return 0; }
    if (a == -1) { if (b == PL_LLONG_MIN) return 1; *out = -b; return 0; }
    if (b == -1) { if (a == PL_LLONG_MIN) return 1; *out = -a; return 0; }
    // 折り返しは符号なしで行う（符号付きの桁あふれは未定義動作のため）
    long long p = (long long)((unsigned long long)a * (unsigned long long)b);
    if (p / b != a) return 1;
    *out = p;
    return 0;
}

// 繰り返し二乗法。ループがあるので当然ランタイム側（R10）。
// 負の指数は int で表せないので実行時エラーにします（第2章から先送りしていた宿題）。
//
// ★ 第47章：**あふれたら panic します。** 2 ** 64 が黙って 0 を返していました。
//   ⚠️ 二乗は「次の周がある」ときだけ行います。最後の周でも二乗していた
//     元の形だと、答えは正しいのに途中の二乗だけがあふれて
//     **誤検出**になります（2 ** 62 など）。
long long pl_ipow(long long base, long long exp) {
    if (exp < 0) pl_panic("negative exponent");
    long long r = 1;
    while (exp > 0) {
        if (exp & 1) {
            if (pl_mul_ovf(r, base, &r)) pl_panic("integer overflow in **");
        }
        exp >>= 1;
        if (exp > 0) {
            if (pl_mul_ovf(base, base, &base)) pl_panic("integer overflow in **");
        }
    }
    return r;
}

// float のべき乗（第44章）
//
// ★ **libc の pow は使えません**（ベアメタルで動く必要があるため）。
//   lib/math.po と同じ手で自前に持ちます:
//     指数が整数なら **繰り返し二乗法**（負の底も扱える／誤差も小さい）
//     そうでなければ exp(y·log(x))
//
// ⚠️ math.po の exp / log と **同じアルゴリズム**にしてあります。
//   片方だけ直すと `2.0 ** 0.5` と `math.pow(2.0, 0.5)` がずれます。
static double pl_exp_(double x) {
    if (x != x) return x;
    if (x > 710.0) return 1.0e308 * 10.0;
    if (x < -746.0) return 0.0;

    const double LN2 = 0.6931471805599453;
    double q = x / LN2;
    long long k = (long long)(q >= 0 ? q + 0.5 : q - 0.5);
    double r = x - (double)k * LN2;

    double term = 1.0, s = 1.0;
    for (int n = 1; n <= 14; n++) {
        term = term * r / (double)n;
        s += term;
    }
    // 2^k を掛ける
    double out = s;
    if (k >= 0) for (long long i = 0; i < k; i++) out *= 2.0;
    else        for (long long i = 0; i < -k; i++) out /= 2.0;
    return out;
}

static double pl_log_(double x) {
    double inf = 1.0e308 * 10.0;
    if (x != x || x < 0.0) return inf - inf;   // NaN
    if (x == 0.0) return -inf;
    if (x > 1.0e308) return x;

    const double LN2 = 0.6931471805599453;
    double m = x;
    long long k = 0;
    while (m >= 2.0) { m /= 2.0; k++; }
    while (m < 1.0)  { m *= 2.0; k--; }

    double z = (m - 1.0) / (m + 1.0);
    double z2 = z * z, term = z, s = z;
    for (int n = 3; n <= 33; n += 2) {
        term *= z2;
        s += term / (double)n;
    }
    return 2.0 * s + (double)k * LN2;
}

double pl_fpow(double x, double y) {
    if (y == 0.0) return 1.0;
    if (x != x || y != y) return x != x ? x : y;   // NaN
    if (x == 1.0) return 1.0;

    // 指数が整数なら繰り返し二乗法（負の底もここで扱える）
    double ty = y < 0 ? -y : y;
    if (ty <= 1024.0 && (double)(long long)ty == ty) {
        long long n = (long long)ty;
        double base = x, r = 1.0;
        while (n > 0) {
            if (n & 1) r *= base;
            base *= base;
            n >>= 1;
        }
        return y < 0.0 ? 1.0 / r : r;
    }

    if (x < 0.0) {
        double inf = 1.0e308 * 10.0;
        return inf - inf;      // 負の数の非整数乗は実数にならない → NaN
    }
    if (x == 0.0) return y > 0.0 ? 0.0 : 1.0e308 * 10.0;
    return pl_exp_(y * pl_log_(x));
}

// ── プロセス ───────────────────────────────────────────────


// ── ファイル入出力（第14章）────────────────────────────────
//
// ★ ここは「C でしか書けないもの」です。Polonium で書けるものは lib/*.po に置きます
//   （docs/chapters/ch14-stdlib.md 14.1 節）。
//
// ⚠️ 失敗したら panic で落とします。エラー値を返して利用者に検査させる形は
//    `T | None` がまだ無いので書けません（第15章）。




// ── コマンドライン引数と外部コマンド（第14章）──────────────
//
// ★ この 2 つが無いと、Polonium 製コンパイラは「コマンド」になれません
//   （docs/design/self-hosting.md 3.4）。






// ── list[T]（第10章）────────────────────────────────────────
//
// ★ 要素はすべて 8 バイトに統一します（int/bool は i64、str/list はポインタ）。
//   要素サイズが型ごとに違うと getelementptr のオフセット計算が型ごとに
//   変わりますが、8 バイト固定なら「long long の配列」と「void* の配列」の
//   2 種類だけで済みます。bool で 7 バイト無駄になりますが、
//   実装の単純さと引き換えにするなら安い代償です。

typedef struct {
    void *data;  // 要素の配列（8 バイト × cap）
    long long len;
    long long cap;
} PlList;

PlList *pl_list_new(void) {
    PlList *l = pl_alloc((long long)sizeof(PlList));
    l->cap = 4;
    l->data = pl_alloc(l->cap * 8);
    l->len = 0;
    return l;
}

long long pl_list_len(PlList *l) { return l->len; }

// ⚠️ realloc を使わないのは「一度渡したポインタは永久に有効」という
//    方針（メモリモデル 3 節）と噛み合わないためです。
//    memcpy して古い領域を捨てるほうが、方針と一貫します。
//
// ★ 倍々に増やすので、n 回の append の総コストは O(n) です。
static void pl_list_grow(PlList *l) {
    if (l->len < l->cap) return;
    long long ncap = l->cap * 2;
    void *nd = pl_alloc(ncap * 8);
    pl_memcpy(nd, l->data, (long long)l->len * 8);
    l->data = nd;
    l->cap = ncap;
}

// 範囲検査（規約 R10）。
//
// ⚠️ 負の添字は **末尾からの位置**です（第39章。Python と同じ）。
//   xs[-1] が最後の要素。呼ぶ側で正規化してから渡します。
//
// ★ 第45章：**ここはもう「検査の置き場所」ではありません。**
//   以前は「検査をここに置くので、生成する IR に分岐が 1 つも出ない」と
//   書いていましたが、その呼び出しこそが最大のボトルネックでした。
//   いまは codegen が検査ごと IR に展開します（gen_index_addr）。
//   **IR には分岐が出ます。** そのぶん LLVM が検査をループの外へ
//   持ち上げたり、範囲が自明なときに消したりできます。

// 範囲外だったときだけ呼ばれる出口。
// ⚠️ **戻ってきません**。呼び出し側の IR は直後に unreachable を置きます。
void pl_index_fail(long long i, long long len) {
    char buf[80];
    char *w = buf;
    const char *m = "index out of range: ";
    while (*m) *w++ = *m++;
    w += pl_itoa(i, w);
    *w++ = ' ';
    *w++ = '(';
    w += pl_itoa(len, w);
    *w++ = ')';
    *w = '\0';
    pl_panic(buf);
}

// ★ 展開後の codegen はもう呼びませんが、ランタイム内から使います。
static void pl_list_check(PlList *l, long long i) {
    if (i < 0 || i >= l->len) pl_index_fail(i, l->len);
}

// 部分文字列の位置（第37章：'in' 演算子）。無ければ -1
//
// ⚠️ 素朴な走査（O(n·m)）です。KMP のような高速化はしていません。
//   'in' の用途では入力が短いことがほとんどで、コードの短さを優先しました。
// ★ 空文字列はどこにでも含まれるので 0 を返します（Python と同じ）。
long long pl_str_find(const char *hay, const char *needle) {
    long long n = pl_str_len(hay);
    long long m = pl_str_len(needle);
    if (m == 0) return 0;
    if (m > n) return -1;
    for (long long i = 0; i + m <= n; i++) {
        long long k = 0;
        while (k < m && hay[i + k] == needle[k]) k++;
        if (k == m) return i;
    }
    return -1;
}

// 部分文字列（第37章）。**新しい文字列を作って返します**
//
// ⚠️ 範囲は Python と同じ規則で丸めます。**範囲外でも落ちません**
//   （添字と違い、スライスは「はみ出したぶんは無い」と読むのが自然なため）。
//     負の値 → 0、長さを超える → 長さ、開始 > 終端 → 空
char *pl_str_slice(const char *s, long long lo, long long hi) {
    long long n = pl_str_len(s);
    // ★ 第39章：負の端は末尾から数えます（s[:-1] で最後の 1 文字を落とす）
    if (lo < 0) lo += n;
    if (hi < 0) hi += n;
    if (lo < 0) lo = 0;
    if (hi > n) hi = n;
    if (lo > hi) lo = hi;
    long long m = hi - lo;
    char *out = pl_str_alloc(m);
    pl_memcpy(out, s + lo, m);
    out[m] = '\0';
    return out;
}

// ── 探索（第37章：'in' 演算子と list のメソッド）─────────────
//
// ★ 見つかった位置を返し、無ければ -1。'in' はこの結果を >= 0 と比べます。
//   ⚠️ 位置を返す形にしておくと index() にもそのまま使えます。

long long pl_list_index_i64(PlList *l, long long v) {
    for (long long i = 0; i < l->len; i++)
        if (((long long *)l->data)[i] == v) return i;
    return -1;
}

// ⚠️ float は **ビットではなく数値として**比べます。ビットで比べると
//    0.0 と -0.0 が別物になり、NaN が自分自身と一致してしまいます。
long long pl_list_index_f64(PlList *l, double v) {
    for (long long i = 0; i < l->len; i++) {
        double x;
        long long bits = ((long long *)l->data)[i];
        pl_memcpy(&x, &bits, 8);
        if (x == v) return i;
    }
    return -1;
}

// ポインタの同一性で比べます（クラス・list の 'in' はこちら。== と同じ意味）
long long pl_list_index_ptr(PlList *l, void *v) {
    for (long long i = 0; i < l->len; i++)
        if (((void **)l->data)[i] == v) return i;
    return -1;
}

// str は **中身**で比べます（== と同じ）
long long pl_list_index_str(PlList *l, const char *v) {
    for (long long i = 0; i < l->len; i++) {
        const char *x = ((const char **)l->data)[i];
        if (x == v) return i;
        if (x && v && pl_str_cmp(x, v) == 0) return i;
    }
    return -1;
}

void pl_list_push_i64(PlList *l, long long v);   // 下で定義

// ── list を文字列にする（第44章）──────────────────────────────
//
// ★ `print(xs)` / `str(xs)` のためのものです。要素の型ごとに 1 本ずつ
//   用意します。**要素の型はコンパイル時に決まっている**ので、
//   どれを呼ぶかは codegen が選べます。
//
// ⚠️ 形は Python に寄せます: [1, 2, 3] / ["a", "b"] / [True, False]
//   文字列だけ引用符で囲むのは、空文字や空白を含む要素が見えるようにするためです。
//
// ⚠️ **入れ子（list[list[int]]）は対象外です。** 要素をさらに文字列に
//   する手立てが要るためで、sema が先に弾きます。

// 組み立て用の可変長バッファ（この節の中だけで使う）
typedef struct {
    char *p;
    long long len;
    long long cap;
} SBuf;

static void sb_need(SBuf *b, long long n) {
    if (b->len + n <= b->cap) return;
    long long ncap = b->cap ? b->cap * 2 : 64;
    while (ncap < b->len + n) ncap *= 2;
    char *np = pl_alloc(ncap);
    pl_memcpy(np, b->p, b->len);
    b->p = np;
    b->cap = ncap;
}

static void sb_put(SBuf *b, const char *s, long long n) {
    sb_need(b, n);
    pl_memcpy(b->p + b->len, s, n);
    b->len += n;
}

static void sb_putc(SBuf *b, char c) { sb_put(b, &c, 1); }

static char *sb_finish(SBuf *b) {
    char *out = pl_str_alloc(b->len);
    pl_memcpy(out, b->p, b->len);
    out[b->len] = '\0';
    return out;
}

// 要素の種類。codegen が数で渡します
//   0 = int / 1 = float / 2 = str / 3 = bool
char *pl_list_str(PlList *l, long long kind) {
    SBuf b = {0};
    sb_putc(&b, '[');
    for (long long i = 0; i < l->len; i++) {
        if (i) sb_put(&b, ", ", 2);
        long long raw = ((long long *)l->data)[i];
        char tmp[64];
        if (kind == 0) {
            sb_put(&b, tmp, pl_itoa(raw, tmp));
        } else if (kind == 1) {
            double d;
            pl_memcpy(&d, &raw, 8);
            sb_put(&b, tmp, pl_ftoa(d, tmp));
        } else if (kind == 2) {
            const char *s = (const char *)raw;
            sb_putc(&b, '"');
            if (s) sb_put(&b, s, pl_str_len(s));
            sb_putc(&b, '"');
        } else {
            const char *t = raw ? "True" : "False";
            sb_put(&b, t, pl_cstr_len(t));
        }
    }
    sb_putc(&b, ']');
    return sb_finish(&b);
}

void pl_print_list(PlList *l, long long kind) {
    char *s = pl_list_str(l, kind);
    pl_print_str(s);
}

// ── ハッシュ（第42章）────────────────────────────────────────
//
// ★ FNV-1a。短い鍵に強く、実装が数行で済みます。
//   ⚠️ **暗号用ではありません。** 敵が鍵を選べる場面（外部入力を鍵にする
//     サーバなど）では、衝突を狙われて線形探索に落とされます。
//   ⚠️ 返す値は **非負**にします（剰余で添字にするため）。
long long pl_hash_str(const char *s) {
    unsigned long long h = 14695981039346656037ULL;
    long long n = pl_str_len(s);
    for (long long i = 0; i < n; i++) {
        h ^= (unsigned char)s[i];
        h *= 1099511628211ULL;
    }
    return (long long)(h & 0x7fffffffffffffffULL);
}

// 整数は「散らす」だけ（そのままだと下位ビットに偏りが出る）
long long pl_hash_i64(long long v) {
    unsigned long long h = (unsigned long long)v;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return (long long)(h & 0x7fffffffffffffffULL);
}

// float はビット列を整数として散らす
// ⚠️ 0.0 と -0.0 はビットが違うので別の値になります。鍵にするときは注意。
long long pl_hash_f64(double v) {
    long long bits;
    pl_memcpy(&bits, &v, 8);
    return pl_hash_i64(bits);
}

// 組み込み関数（第39章）
long long pl_iabs(long long v) { return v < 0 ? -v : v; }
double pl_fabs(double v) { return v < 0.0 ? -v : v; }

// ── 負の添字の正規化（第39章）────────────────────────────────
//
// ★ Python と同じく、負の添字は **末尾から**数えます（-1 が最後）。
//   ⚠️ 正規化だけで、範囲の検査はしません（後段の pl_list_check / pl_str_index
//     がそのまま担当します）。-100 のような値は負のまま渡り、そこで落ちます。
long long pl_norm_index(long long i, long long len) {
    if (i < 0) return i + len;
    return i;
}

// list[int] の総和（第39章）
// ⚠️ **int のリストだけ**です。float の総和は要素の型で命令が変わるので、
//   linalg.vsum を使ってください（sema が型を見て弾きます）。
long long pl_list_sum(PlList *l) {
    long long s = 0;
    for (long long i = 0; i < l->len; i++) s += ((long long *)l->data)[i];
    return s;
}

// list の連結（新しい list を作る）
PlList *pl_list_concat(PlList *a, PlList *b) {
    PlList *out = pl_list_new();
    for (long long i = 0; i < a->len; i++)
        pl_list_push_i64(out, ((long long *)a->data)[i]);
    for (long long i = 0; i < b->len; i++)
        pl_list_push_i64(out, ((long long *)b->data)[i]);
    return out;
}

// list の繰り返し（負や 0 なら空）
PlList *pl_list_repeat(PlList *a, long long n) {
    PlList *out = pl_list_new();
    for (long long k = 0; k < n; k++)
        for (long long i = 0; i < a->len; i++)
            pl_list_push_i64(out, ((long long *)a->data)[i]);
    return out;
}


// list のスライス（第37章）。**新しい list を作ります**（借用ではありません）
//
// ⚠️ 要素をそのまま写すので、参照型なら「同じものを指す 2 つのリスト」に
//   なります。所有権の観点では借用と同じ扱いが要るため、複製した中身の
//   解放は行いません（仕様 §6 の一時値と同じ扱い）。
PlList *pl_list_slice(PlList *l, long long lo, long long hi) {
    // ★ 第39章：負の端は末尾から数えます
    if (lo < 0) lo += l->len;
    if (hi < 0) hi += l->len;
    if (lo < 0) lo = 0;
    if (hi > l->len) hi = l->len;
    if (lo > hi) lo = hi;
    PlList *out = pl_list_new();
    for (long long i = lo; i < hi; i++)
        pl_list_push_i64(out, ((long long *)l->data)[i]);
    return out;
}

// ── list のメソッド（第37章）────────────────────────────────

// 末尾を取り出す（空なら panic）
long long pl_list_pop(PlList *l) {
    if (l->len == 0) pl_panic("pop from empty list");
    return ((long long *)l->data)[--l->len];
}

// i の位置に差し込む（後ろへずらす）。i == len なら末尾に足すのと同じ
void pl_list_insert(PlList *l, long long i, long long v) {
    if (i < 0 || i > l->len) {
        char buf[80];
        char *w = buf;
        const char *m = "insert index out of range: ";
        while (*m) *w++ = *m++;
        w += pl_itoa(i, w);
        *w = '\0';
        pl_panic(buf);
    }
    pl_list_grow(l);
    for (long long k = l->len; k > i; k--)
        ((long long *)l->data)[k] = ((long long *)l->data)[k - 1];
    ((long long *)l->data)[i] = v;
    l->len++;
}

// i の位置を取り除いて、その値を返す
long long pl_list_remove_at(PlList *l, long long i) {
    pl_list_check(l, i);
    long long v = ((long long *)l->data)[i];
    for (long long k = i; k + 1 < l->len; k++)
        ((long long *)l->data)[k] = ((long long *)l->data)[k + 1];
    l->len--;
    return v;
}

void pl_list_reverse(PlList *l) {
    for (long long i = 0, j = l->len - 1; i < j; i++, j--) {
        long long t = ((long long *)l->data)[i];
        ((long long *)l->data)[i] = ((long long *)l->data)[j];
        ((long long *)l->data)[j] = t;
    }
}

void pl_list_clear(PlList *l) { l->len = 0; }

// 中身を丸ごと写した新しい list
PlList *pl_list_copy(PlList *l) { return pl_list_slice(l, 0, l->len); }

// 別の list の中身を末尾に足す
void pl_list_extend(PlList *l, PlList *o) {
    for (long long i = 0; i < o->len; i++)
        pl_list_push_i64(l, ((long long *)o->data)[i]);
}

void pl_list_push_i64(PlList *l, long long v) {
    pl_list_grow(l);
    ((long long *)l->data)[l->len++] = v;
}

void pl_list_push_ptr(PlList *l, void *v) {
    pl_list_grow(l);
    ((void **)l->data)[l->len++] = v;
}

long long pl_list_get_i64(PlList *l, long long i) {
    pl_list_check(l, i);
    return ((long long *)l->data)[i];
}

void *pl_list_get_ptr(PlList *l, long long i) {
    pl_list_check(l, i);
    return ((void **)l->data)[i];
}

void pl_list_set_i64(PlList *l, long long i, long long v) {
    pl_list_check(l, i);
    ((long long *)l->data)[i] = v;
}

void pl_list_set_ptr(PlList *l, long long i, void *v) {
    pl_list_check(l, i);
    ((void **)l->data)[i] = v;
}

// list[str] を sep でつないだ str を作る（第15章）。
//
// ★ Polonium 側で out = out + xs[i] と書くと O(n^2) になります
//   （1 回ごとに全部コピーするため）。文字列を「組み立てる」のは
//   セルフホストの IR 出力で毎回やることなので、ここだけは C で用意します。
//   利用者は list[str] に溜めて最後に join する、という形で O(n) になります。
char *pl_str_join(PlList *xs, const char *sep) {
    long long n = xs->len;
    long long sep_len = pl_str_len(sep);

    long long total = n > 0 ? sep_len * (n - 1) : 0;
    for (long long i = 0; i < n; i++)
        total += pl_str_len(((char **)xs->data)[i]);

    char *p = pl_str_alloc(total);
    long long at = 0;
    for (long long i = 0; i < n; i++) {
        if (i > 0) {
            pl_memcpy(p + at, sep, (long long)sep_len);
            at += sep_len;
        }
        const char *e = ((char **)xs->data)[i];
        long long el = pl_str_len(e);
        pl_memcpy(p + at, e, (long long)el);
        at += el;
    }
    p[total] = '\0';
    return p;
}


// 文字列の i 番目のバイトを int で返す（第15章）。
//
// ★ pl_str_index と違い、確保しません。字句解析器のように 1 文字ずつ回る
//   コードでは、1 文字ごとの 2 バイト確保が効いてきます（ch15 15.7 で実測）。
// ⚠️ 言語には足していません。lib/strings.po から extern で呼ぶだけです
//   （第14章で引いた境界線のとおり）。
long long pl_byte_at(const char *s, long long i) {
    long long n = pl_str_len(s);
    if (i < 0 || i >= n) {
        char buf[80];
        char *w = buf;
        const char *m = "index out of range: ";
        while (*m) *w++ = *m++;
        w += pl_itoa(i, w);
        *w++ = ' ';
        *w++ = '(';
        w += pl_itoa(n, w);
        *w++ = ')';
        *w = '\0';
        pl_panic(buf);
    }
    return (long long)(unsigned char)s[i];
}

// 文字列の添字：1 文字の str を返す（char 型は作らない。型システム 5.8）
char *pl_str_index(const char *s, long long i) {
    long long n = pl_str_len(s);
    if (i < 0 || i >= n) {
        char buf[80];
        char *w = buf;
        const char *m = "index out of range: ";
        while (*m) *w++ = *m++;
        w += pl_itoa(i, w);
        *w++ = ' ';
        *w++ = '(';
        w += pl_itoa(n, w);
        *w++ = ')';
        *w = '\0';
        pl_panic(buf);
    }
    char *p = pl_str_alloc(1);
    p[0] = s[i];
    p[1] = '\0';
    return p;
}

// ── 複製（第26章）───────────────────────────────────────────
//
// ★ 借りたものを保存したいときの逃げ道です（決定 D8）。
//   `copy(s)` は **ヒープに新しい文字列**を作るので、呼び出し側が所有できます。
//   リテラルを複製しても「静的」の印は付きません（複製はヒープにあるため）。
char *pl_str_copy(const char *s) {
    if (!s) return NULL;
    long long n = pl_str_len(s);
    char *p = pl_str_alloc(n);
    pl_memcpy(p, s, (long long)n + 1);
    return p;
}

// ── 共有所有 rc[T]（第28章）─────────────────────────────────
//
// ★ 所有者を 1 つに決められないデータのための逃げ道です（仕様 v2 §7）。
//
//   ┌──────────┬──────────┬──────────────┐
//   │ strong   │ borrow   │ 中身へのポインタ │
//   │ i64      │ i64      │ ptr           │
//   └──────────┴──────────┴──────────────┘
//
// ⚠️ 設計（ownership.md §7）では「中身を埋め込む」形にしていましたが、
//    Polonium の所有型はすべてポインタなので、**ポインタを 1 本持つ**ほうが
//    型ごとのレイアウト計算が要らず、どの型でも同じ形になります。
typedef struct {
    long long strong;
    long long borrow;
    void *value;
} PlRc;

void *pl_rc_new(void *value) {
    PlRc *r = pl_alloc((long long)sizeof(PlRc));
    r->strong = 1;
    r->borrow = 0;
    r->value = value;
    return r;
}

void *pl_rc_get(void *p) {
    if (!p) pl_panic("rc: None の中身は読めません");
    return ((PlRc *)p)->value;
}

void *pl_rc_retain(void *p) {
    if (p) ((PlRc *)p)->strong++;
    return p;
}

// カウントを 1 減らし、0 になったら中身を解放する。
// ★ 中身の解放のしかたは型ごとに違うので、関数ポインタで受け取ります
//   （pl_drop_list と同じ形）。
void pl_rc_release(void *p, void (*value_drop)(void *)) {
    if (!p) return;
    PlRc *r = p;
    r->strong--;
    if (r->strong > 0) return;
    if (r->borrow > 0) pl_panic("rc: 借用したまま解放されました");
    if (value_drop) value_drop(r->value);
    pl_hook_free(r);
}

// 借用の数え札（仕様 §7.2。検査は**実行時**）
void *pl_rc_borrow(void *p) {
    if (!p) pl_panic("rc: None は借用できません");
    ((PlRc *)p)->borrow++;
    return ((PlRc *)p)->value;
}

void *pl_rc_borrow_mut(void *p) {
    if (!p) pl_panic("rc: None は借用できません");
    PlRc *r = p;
    if (r->borrow != 0)
        pl_panic("rc: 既に借用されているので、可変で借りられません");
    r->borrow++;
    return r->value;
}

void pl_rc_unborrow(void *p) {
    if (p) ((PlRc *)p)->borrow--;
}

// ── 解放（第25章）───────────────────────────────────────────
//
// ★ v1 は「解放しない」設計でした（docs/design/memory-model.md）。
//   所有権（第22〜24章）で「誰が所有者か」が静的に決まったので、
//   ここで初めて pl_hook_free() を入れます。
//
// ⚠️ どれも「NULL を渡してよい」ようにしてあります。
//    T | None のフィールドや、まだ入っていない値をそのまま渡せるからです。

void pl_drop_str(char *s) {
    if (!s) return;
    // ★ 文字列リテラル（.rodata）は解放しない
    if (((long long *)s)[-1] & PL_STR_STATIC) return;
    pl_hook_free(s - 8);  // 確保したのはヘッダの先頭（pl_str_alloc を参照）
}

// リストを解放する。
//
// ★ elem_drop は「要素 1 つを解放する関数」。要素がコピー型（int / bool）なら
//   NULL を渡します。所有型なら codegen が適切な関数を渡します
//   （str なら pl_drop_str、クラスなら生成した @drop.C）。
void pl_drop_list(PlList *l, void (*elem_drop)(void *)) {
    if (!l) return;
    if (elem_drop) {
        void **items = (void **)l->data;
        for (long long i = 0; i < l->len; i++) elem_drop(items[i]);
    }
    pl_hook_free(l->data);
    pl_hook_free(l);
}

// クラスのインスタンスそのものを解放する。
// ★ フィールドの解放と drop メソッドの呼び出しは、codegen が生成する
//   @drop.C の中で先に済ませてあります。
void pl_drop_obj(void *p) {
    if (!p) return;
    pl_hook_free(p);
}
