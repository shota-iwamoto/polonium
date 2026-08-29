# 第22章 ムーブ検査（use-after-move）

> **この章のゴール**
> 移動済みの値を使っていることを、コンパイラが指摘できるようになる。
>
> ```bash
> $ cat t.po
> def main() -> int:
>     xs: list[int] = [1, 2]
>     ys: list[int] = xs
>     return len(xs) + len(ys)
> $ ./build/poloniumc t.po -o t
> warning[E-MOVE-1]: 移動済みの値 'xs' を使っています
>   --> t.po:4:16
>    |
>  4 |     return len(xs) + len(ys)
>    |                ^^ ここで使われています
>    |
> note: 'xs' はここで移動しました
>   --> t.po:3:21
>    |
>  3 |     ys: list[int] = xs
>    |                     ^^
>    |
>    = ヒント: 移動した後も使うなら、値を作り直して代入してください（例: xs = [...]）
> ```

前章（第21章）で `own` / `mut` を**書けるように**しました。この章で初めて、
書いたことに**意味**が付きます。仕様 v2 の 8 つの保証のうち、
**S1（use-after-move が無い）** を担当する章です（[safety-spec.md §1](../../docs/spec/safety-spec.md)）。

**⚠️ この章の診断は既定では「警告」です。** `selfhost/` はまだ v1 の
参照セマンティクス前提で書かれているので、エラーにすると自分自身を
ビルドできなくなります。書き換えは第26章の仕事です。
**警告を消すために仕様を緩めてはいけません。**

---

## 目次

- [22.1 何を足すか](#221-何を足すか)
- [22.2 4 本目のパスを足す](#222-4-本目のパスを足す)
- [22.3 追いかけるのは「値」ではなく「場所」](#223-追いかけるのは値ではなく場所)
- [22.4 コピー型と所有型](#224-コピー型と所有型)
- [22.5 3 つの状態と合流](#225-3-つの状態と合流)
- [22.6 CFG を作らずにデータフロー解析をする](#226-cfg-を作らずにデータフロー解析をする)
- [22.7 移動が起きる 5 か所](#227-移動が起きる-5-か所)
- [22.8 つまずき：脱糖が作った隠し変数](#228-つまずき脱糖が作った隠し変数)
- [22.9 診断コードを持たせる](#229-診断コードを持たせる)
- [22.10 既定は警告、`--deny-move` でエラー](#2210-既定は警告---deny-move-でエラー)
- [22.11 動作確認](#2211-動作確認)
- [22.12 第26章への宿題](#2212-第26章への宿題)
- [22.13 まとめと次章の予告](#2213-まとめと次章の予告)

---

## 22.1 何を足すか

| # | 足すもの | 触るファイル |
|---|---|---|
| ① | 所有権検査のパス本体 | `src/ownck.h` / `src/ownck.c`（新規） |
| ② | パスの起動と `--deny-move` | `src/main.c` |
| ③ | 診断コード（`error[E-MOVE-1]:`） | `src/diag.h` / `src/diag.c` |
| ④ | テストの期待値 `# WARN:` / `# FLAGS:` / `# STAGE0-ONLY:` | `tests/run_tests.sh` |
| ⑤ | テスト 13 本 | `tests/cases/err_move_*.po` ほか |

仕様は [safety-spec.md §3](../../docs/spec/safety-spec.md)、設計は
[design/ownership.md §3〜4](../../docs/design/ownership.md) です。**先にそちらを読んでください。**

**★ `selfhost/` には手を入れません。** Polonium 版への移植は第29章です。
C 版で解析の形が固まる前に移植すると、同じ設計変更を 2 回ずつやることになります。

---

## 22.2 4 本目のパスを足す

```
   ① lexer  → ② parser → ③ sema → ④ ownck（新設）→ ⑤ codegen
                                    ~~~~~~~~~~~~~
                                    移動済みの値を使っていないか
```

**🤔 なぜ sema に混ぜないのか**

sema は「型が合うか」を見るパスで、**実行順序を考えません**。
所有権の検査は逆に「**どの文が先に走るか**」が本質です。

```python
ys: list[int] = xs     # ← この文が先に走るから
print(len(xs))         # ← この文が問題になる
```

順序を見ない解析と、順序が本質の解析を 1 つの関数に混ぜると、両方が読めなくなります。
分けておけば、**パスを呼ばないだけで**この章の変更を丸ごと無効化できます。

✍️ `src/main.c`：

```c
    // ── ③ 意味解析・型検査（全モジュールまとめて）──
    sema_program(mods, entry);

    if (opt.stage == STAGE_CHECK) return 0;

    // ── ④ 所有権の検査（第22章）──
    ownck_program(mods, opt.deny_move);
```

**⚠️ `--check` では ownck を走らせません。**
`--check` は「③ 型検査まで」という意味の option で、その出力は
`tests/selfhost.sh` が **C 版と Polonium 版で 1 バイト単位に突き合わせて**います。
Polonium 版にはまだ ownck が無いので、ここで警告を出すと全ケースが不一致になります。
第29章で移植したら、この分岐も外します。

---

## 22.3 追いかけるのは「値」ではなく「場所」

解析の対象は変数だけではありません。

| 書き方 | Place |
|---|---|
| `x` | `Local(x)` |
| `self.name` | `Field(Local(self), name)` |
| `xs[i]` | `Index(Local(xs))` ← **添字は区別しない** |
| `mod.g` | `Global(mod.g)` |

```c
struct Place {
    PlaceKind kind;    // LOCAL / FIELD / INDEX / GLOBAL
    Place *base;       // FIELD / INDEX の親
    const char *key;   // 同一性の判定に使う名前
    const char *disp;  // 診断に出す見た目（"self.name"）
};
```

**★ 名前ではなく IR 名（`%x` / `%x.1`）で識別します。** 兄弟スコープの同名変数は
別の場所だからです（第7章で sema が振り分けた名前が、ここで効きます）。

**重なり判定**は「片方がもう片方の接頭辞か」だけです。

```
  Local(a)                  と  Field(Local(a), x)   → 重なる（a を丸ごと移動した）
  Field(Local(a), x)        と  Field(Local(a), y)   → 重ならない（別のフィールド）
```

```c
static bool place_overlaps(Place *a, Place *b) {
    return place_prefix_of(a, b) || place_prefix_of(b, a);
}
```

この 3 行が、「`p.a` を移動しても `p.b` はまだ使える」という
（Rust でいう partial move の）ふるまいそのものです。

**⚠️ 添字は区別しません。** `xs[0]` と `xs[1]` は同じ場所です。
どの要素かは実行時にしか分からないためです（設計 ownership.md §3）。

---

## 22.4 コピー型と所有型

```c
bool ty_is_owned(Type *t) {
    switch (t->kind) {
        case TY_STR:
        case TY_LIST:
        case TY_CLASS: return true;
        case TY_OPT: return ty_is_owned(t->elem);  // Token | None も所有型
        default: return false;                     // int / bool / None
    }
}
```

**🤔 なぜ `Type` に「所有型フラグ」を足さないのか**

`Type` はシングルトンで共有されています（`src/types.h`）。`int` の型オブジェクトは
プログラム全体で 1 個だけです。そこにフラグを足すと**場所ごとに別の `int` 型**が
必要になり、既存のポインタ比較（型の同一性）が壊れます。
所有は「型の性質」なので、**データを増やさず関数で導く**のが正解です。

---

## 22.5 3 つの状態と合流

```
      Valid              使える
        │
   MaybeMoved            分岐によっては移動済み
        │
      Moved              移動済み
```

合流は**保守的な結合**です。`Valid ⊔ Moved = MaybeMoved`。

```c
static OwnState st_join(OwnState a, OwnState b) {
    return a == b ? a : ST_MAYBE;
}
```

★ 表に載せるのは**移動された場所だけ**です。載っていない場所は `Valid`。
そうしないと、関数に入るたびに全変数を並べる必要があります。

```c
typedef struct {
    Ent *ents;  // 移動された場所たち
    bool dead;  // この経路は return / break / continue で終わっている
} Flow;
```

`dead` は「その経路は合流に参加しない」という印です（設計 §4.2）。
`return` の後ろの状態を合流させてしまうと、
**通らない経路のせいで警告が出る**という最悪の誤検出になります。

---

## 22.6 CFG を作らずにデータフロー解析をする

**制御フローグラフ（CFG）は作りません。** Polonium には `goto` が無く、
制御構文が構造化されているので、**AST を再帰でたどるだけ**で正しい解析ができます。

| 構文 | 扱い |
|---|---|
| 逐次 | 上から順に状態を更新 |
| `if` / `else` | 分岐前の状態を複製 → 各枝を解析 → 合流で結合 |
| `while` | **不動点反復** |
| `break` / `continue` | ループの出口／入口の状態に積む |
| `return` | `dead` にして以降の合流から外す |

`if` はそのまま読めます。

```c
        case ND_IF: {
            use_expr(o, f, n->lhs);
            Flow then_f = flow_copy(f);
            stmt(o, &then_f, n->body);

            Flow else_f = flow_copy(f);
            if (n->els) stmt(o, &else_f, n->els);

            *f = then_f;
            flow_join(f, &else_f);
            return;
        }
```

### while — 不動点反復

ループは 1 回たどるだけでは足りません。**2 周目に入ったときの状態**を知る必要があります。

```python
    while i < 3:
        print(take(name))   # 1 周目は有効。2 周目は？
        i = i + 1
```

そこで「入口の状態」が変化しなくなるまで繰り返します。

```c
    Flow entry = flow_copy(f);
    for (;; round++) {
        while_once(o, n, &entry, &back, &exit);   // 条件 → 本体 → 増分
        Flow next = flow_copy(&entry);
        flow_join(&next, &back);                  // 逆辺を合流
        if (flow_eq(&next, &entry)) break;        // 変化しなくなった＝不動点
        entry = next;
        if (round >= 3) internal_error(...);      // 収束しなければコンパイラのバグ
    }
```

**🤔 なぜ 2 周で収束するのか**

格子の高さが 2（`Valid` → `MaybeMoved` → `Moved`）で、状態は**単調にしか下がらない**ためです。
3 周目に変化することはありません。実装ではそれを assert し、
崩れたら**利用者のミスではなくコンパイラのバグ**として落とします。

**⚠️ 反復中は診断を出しません。** そのまま報告すると、同じ警告が周回のたびに出ます。
不動点が求まってから、**収束した入口でもう一度だけ**解析して報告します。

```c
    o->quiet++;
    ... 不動点を求める ...
    o->quiet--;
    while_once(o, n, &entry, &back, &exit);  // ← ここで初めて診断が出る
    *f = exit;
```

---

## 22.7 移動が起きる 5 か所

仕様 v2 §3.1 の表がそのまま実装になります。

| 場所 | 例 | 実装 |
|---|---|---|
| 代入 | `ys = xs` | `ND_ASSIGN` / `ND_VARDECL` の右辺を `move_expr` |
| `own` 引数 | `take(xs)` | 実引数を `ParamMode` で振り分け |
| `return` | `return xs` | `ND_RETURN` の値を `move_expr` |
| フィールドへの代入 | `self.items = xs` | 上の代入と同じ道 |
| `append` | `xss.append(a)` | メソッドの特別扱い |

**関数の実引数は、第21章で `ND_PARAM` に入れた `mode` で決まります。**

```c
static void args_by_mode(Own *o, Flow *f, Node *args, Node *params) {
    Node *pm = params;
    for (Node *a = args; a; a = a->next) {
        if (pm && pm->mode == PM_OWN) move_expr(o, f, a);
        else use_expr(o, f, a);          // 既定（借用）と mut は移動しない
        if (pm) pm = pm->next;
    }
}
```

**★ 呼び出し先の定義（`ND_FUNC`）は IR 名で引きます。**
sema の `FuncSig` は受け取り方を持っていない（第21章で「型ではなく引数に持たせる」と
決めたため）ので、ownck は自分用の表を 1 つ作ります。
`obj.m(args)` は `m(obj, args)` と同じ扱いで、`self` は必ず借用です（`own self` は書けません）。

**⚠️ 添字からは move out できません。**

```c
    if (p->kind == PL_INDEX) {
        use_expr(o, f, n);   // 借用として扱う
        return;
    }
```

`xs[0]` と `xs[1]` を区別できないので、要素を 1 つだけ持ち出すことは許しません
（Rust の `Vec` と同じ制限）。所有権ごと取り出す `xs.pop()` は第25章です。

---

## 22.8 つまずき：脱糖が作った隠し変数

最初の実装で、**`for` を書いただけで警告が出ました。**

```python
    for x in names:     # ← names を移動した、と言われる
        print(x)
    print(len(names))   # ← 移動済みの値を使っています（誤検出）
```

原因は第11章の脱糖です。`for` は while に書き換えられており、
その途中に**隠し変数への代入**が現れます。

```python
    for.it.0 = names            # ← ここが「代入＝移動」に見えた
    for.ix.0: int = 0
    while for.ix.0 < len(for.it.0):
        x = for.it.0[for.ix.0]
```

複合代入（`t.f += 1`）の `aug.obj.0 = t` も同じです。

**★ これらは利用者が書いた代入ではありません。**「対象を 1 回だけ評価する」ための
別名なので、**借用として扱う**のが正しい判断です。仕様 v2 §3.1 も
「`for` の要素は移動しない」と決めています。

```c
// 名前に '.' が入るのは脱糖で作った変数だけなので、これで見分けられます
static bool is_hidden_var(const char *name) {
    return name && strchr(name, '.') != NULL;
}
```

**🤔 なぜ脱糖の側を直さないのか**：`for` を while に落とす判断（第11章）は、
sema と codegen が `for` を知らずに済むという大きな利点があります。
それを崩すより、**脱糖の産物だと分かる印**（名前の `.`）を解析側が読むほうが安く済みます。
⚠️ ただし「隠し変数は借用」という規則は、第25章（drop 挿入）でもう一度出てきます。
そのときは「隠し変数は解放しない」と読み替えることになります。

---

## 22.9 診断コードを持たせる

仕様書は `E-MOVE-1` のような**コード**で規則を指しています（safety-spec.md §1）。
診断にもそれを出せるようにします。

✍️ `src/diag.h`：

```c
typedef struct {
    const char *severity;  // "error" / "warning"
    const char *code;      // 診断コード（"E-MOVE-1"）。NULL なら出力しない
    const char *message;
    ...
```

✍️ `src/diag.c`：

```c
    if (d->code) fprintf(stderr, "%s[%s]: %s\n", sev, d->code, d->message);
    else fprintf(stderr, "%s: %s\n", sev, d->message);
```

**★ 既存の診断は `code` を NULL のままにしておくので、出力は 1 文字も変わりません。**
`Diag` を `= {0}` で初期化する約束（第3章）が、こういうときに効きます。

移動した場所と使った場所の**2 か所**を指せるのは、第3章で `Diag` に
`related` を用意しておいたからです。新しく作るものはありません。

さらに、**同じ位置を 2 回指すとき**は言い回しを変えます。

```
note: 'name' は前の繰り返しで、ここで移動しています
```

ループの中で移動している場合、移動した場所と使った場所は同じ行です。
そこに「分岐によっては」と書いても読み手には意味が通りません。

---

## 22.10 既定は警告、`--deny-move` でエラー

```bash
$ ./build/poloniumc t.po -o t                # warning（コンパイルは成功する）
$ ./build/poloniumc --deny-move t.po -o t    # error（1 件目で終了）
```

**★ 既定を警告にした理由**は 1 つだけです。この章の時点で `selfhost/` に
**196 件**の違反があり、エラーにすると `make bootstrap` が通らなくなるからです。
「検査を入れる」と「既存コードを直す」を同じ章でやると、
**どちらが原因で落ちているのか分からなくなります。**

警告は先頭 20 件で打ち切り、残りは件数だけ知らせます。

```
warning: 移動済みの値の使用が他に 176 件あります（表示したのは先頭 20 件です）
```

（全部見たいときは `-DOWNCK_MAX_REPORT=100000` でビルドしてください。）

### テストの回し方

`tests/run_tests.sh` に期待値を 3 つ足します。

| 書き方 | 意味 |
|---|---|
| `# WARN: メッセージ` | コンパイルは**成功**し、stderr にその文字列を含む |
| `# FLAGS: --deny-move` | コンパイラに渡す追加オプション |
| `# STAGE0-ONLY: 理由` | C 版でだけ実行する（`make bootstrap-test` では飛ばす） |

**★ `--deny-move` は「落ちるべきテスト」だけでなく「通るべきテスト」にも効きます。**

```python
# FLAGS: --deny-move
# EXIT: 3
def main() -> int:
    xs: list[int] = [1, 2]
    n: int = take(xs)
    xs = [7]              # ✅ 代入し直せばまた使える
    return n + len(xs)
```

`--deny-move` を付けておけば、**違反が 1 件でもあればコンパイルが失敗する**ので、
「警告が出ていないこと」を EXIT だけで検証できます。新しい仕組みは要りません。

**⚠️ `# STAGE0-ONLY:` が要る理由**：Polonium 版コンパイラ（stage2）には
まだ ownck も `--deny-move` も無いので、`make bootstrap-test` でこれらのケースを
そのまま回すと必ず落ちます。第29章で移植したら、この印を外します。

---

## 22.11 動作確認

✍️ テストを 13 本足します。

| ファイル | 確認すること |
|---|---|
| `err_move_assign.po` | 代入は移動する |
| `err_move_own_arg.po` | `own` 引数に渡すと移動する |
| `err_move_return.po` | `return` も移動する場所 |
| `err_move_field.po` | フィールドへの代入は移動する |
| `err_move_append.po` | `append` した要素はリストのものになる |
| `err_move_branch.po` | 片方の枝だけの移動は `MaybeMoved` |
| `err_move_loop.po` | ループの 2 周目を不動点反復で捕まえる |
| `warn_move_default.po` | **既定は警告**（実行はできる） |
| `own_move_reassign.po` | 代入し直せば復活する |
| `own_move_borrow_ok.po` | 既定の引数（借用）と `mut` は移動しない |
| `own_move_for_borrow.po` | `for` の対象も要素も借用 |
| `own_move_field_disjoint.po` | `p.a` を移動しても `p.b` は無事 |
| `own_move_mod/main.po` | `mod.f(args)` は `self` を取らない（引数をずらさない） |

**✅ 確認**

```bash
$ make test
全 344 件パス
トークン列一致 384 件 / AST 一致 342 件 / 型検査 一致 200 件 / IR 一致 200 件

$ make bootstrap
★ 不動点に到達しました（stage2 == stage3）

$ make bootstrap-test
全 331 件パス
（13 件スキップ）
```

**★ IR は 1 バイトも変わりません。** この章は「見て報告する」だけで、
生成物には一切触れていないからです。だから不動点もそのまま維持されます。
コード生成が変わるのは第25章（drop の挿入）です。

---

## 22.12 第26章への宿題

`selfhost/` に **196 件**の違反が見つかりました。**これは失敗ではなく、成果です。**
コンパイラ自身が、仕様 v2 にとって最初の（そして最大の）テストケースになっています。

| ファイル | 件数 |
|---|---|
| `selfhost/sema.po` | 114 |
| `selfhost/parser.po` | 37 |
| `selfhost/codegen.po` | 19 |
| `selfhost/module.po` | 15 |
| `selfhost/main.po` | 5 |
| `selfhost/ast.po` | 4 |
| `selfhost/lexer.po` | 2 |

内訳は「移動済み」171 件・「移動済みかもしれない」25 件。
**`lib/` と `examples/` は 0 件**でした（標準ライブラリは既に v2 で通ります）。

いちばん多いのは、この形です。

```python
def type_opt(elem: Type) -> Type:
    cached: Type | None = elem.opt    # ← elem.opt をフィールドから「移動」した
    if cached is not None:
        return cached
    t: Type = Type(TY_OPT)
    t.elem = elem                     # ← elem を丸ごと使う（部分的に移動済み）
```

「テーブルから引いて使い回す」「木の節点を親子で持ち合う」という、
v1 の参照セマンティクスに素直に乗ったコードです。
**直し方は第26章で検討します。** 選択肢は 3 つあります。

1. 借用で足りるように書き換える（`self` からのフィールド借用は仕様 §4.5 で許されている）
2. 添字（`int`）で参照する（設計 ownership.md §8 の方針）
3. `rc[T]` を使う（第28章。**最後の手段**）

**⚠️ 警告を消すために仕様を緩めることはしません。** 直せない形が残ったら、
そのときこそ仕様を見直す根拠になります（設計 §8）。

### この章で残した宿題（ownck 自身）

| 残したもの | いつ |
|---|---|
| リストリテラル `[a, b]` の要素の移動 | 第25章 |
| `xs[i]` からの move out（`pop()` の導入とセットで） | 第25章 |
| 借用引数そのものを移動してしまう形（`ys = xs` で `xs` が借用） | 第23章 |
| 借用の保存・返却の禁止（B2）、可変借用の衝突（B1 / B3） | 第23〜24章 |
| `--check` でも ownck を走らせる | 第29章（Polonium 版に移植したら） |
| 解析結果（`MaybeMoved` の場所）を codegen に渡す | 第25章（drop フラグ） |

---

## 22.13 まとめと次章の予告

この章でやったこと：

- 4 本目のパス `ownck` を新設した（**sema とは別のパス**にした）
- 追いかける単位を「値」ではなく **場所（Place）** にした
- 3 状態の格子と、**CFG を作らない**構造化データフロー解析を書いた
- `while` を**不動点反復**で解き、2 周で収束することを assert した
- 診断にコード（`E-MOVE-1`）を持たせ、移動と使用の 2 か所を指した
- **既定は警告**にして、既存のビルドを止めなかった
- `selfhost/` の 196 件を、消すのではなく**記録**した

次章（第23章）は**借用**です。仕様 §4.4 で
「借用は呼び出しより長生きしない」と決めたおかげで、
Rust のライフタイム推論は**まるごと不要**になります。
必要なのは「借用をフィールドに保存していないか」「借用を返していないか」の
2 つだけです（[design/ownership.md §5](../../docs/design/ownership.md)）。

**★ この章で作った `Place` と `Flow` は、そのまま第23〜25章で使います。**
場所の重なり判定は借用の衝突検査（B1）に、`MaybeMoved` は drop フラグ（§6.3）に
そのまま繋がります。
