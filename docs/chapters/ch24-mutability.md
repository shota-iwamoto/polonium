# 第24章 可変性 `mut` と借用の衝突

> **この章のゴール**
> 「読み取り専用で借りたものを書き換えていないか」と
> 「同じ値を可変借用と共有借用で同時に貸していないか」を検出する。
>
> ```bash
> $ cat t.po
> class Counter:
>     n: int
>     def init(self) -> None:
>         self.n = 0
>     def bump(self) -> None:
>         self.n = self.n + 1
> $ ./build/poloniumc t.po -o t
> warning[E-MUT-1]: 読み取り専用の借用 'self.n' を書き換えています
>   --> t.po:6:9
>    |
>  6 |         self.n = self.n + 1
>    |         ^^^^^^^^^^^^^^^^^^^ この代入で書き換えています
>    |
> note: 'self' は読み取り専用で借りています
>   --> t.po:5:14
>    |
>  5 |     def bump(self) -> None:
>    |              ^^^^
>    |
>    = ヒント: メソッドの宣言を 'def bump(mut self, ...)' にしてください
> ```

第22章は「移動」、第23章は「借用の寿命」でした。この章は「**借用の権限**」です。
仕様 v2 の保証 **S5（データ競合の芽が無い）** を担当します。

**★ この章で、安全性の検査（ownck）が仕様どおり全部そろいます。**
残る S3 / S4（二重解放とリーク）は「検査」ではなく「解放の挿入」なので、第25章です。

---

## 目次

- [24.1 何を足すか](#241-何を足すか)
- [24.2 `mut` は誰に要るのか](#242-mut-は誰に要るのか)
- [24.3 書き換えが起きる 4 か所](#243-書き換えが起きる-4-か所)
- [24.4 `init` を例外にした](#244-init-を例外にした)
- [24.5 B1：二重ループだけで済む理由](#245-b1二重ループだけで済む理由)
- [24.6 メソッドは `m(obj, args)` と同じ](#246-メソッドは-mobj-args-と同じ)
- [24.7 `--explain-mut` — 書かせない代わりに、聞けば答える](#247---explain-mut--書かせない代わりに聞けば答える)
- [24.8 動作確認](#248-動作確認)
- [24.9 第26章の宿題（更新）](#249-第26章の宿題更新)
- [24.10 まとめと次章の予告](#2410-まとめと次章の予告)

---

## 24.1 何を足すか

| # | 足すもの | 触るファイル |
|---|---|---|
| ① | `BorrowRoot.is_mut` と `check_mut`（B3 → `E-MUT-1`） | `src/ownck.c` |
| ② | `ArgRef` と衝突検査（B1 → `E-BORROW-5`） | `src/ownck.c` |
| ③ | `--deny-mut` / `--explain-mut` と `OwnckOptions` | `src/main.c`, `src/ownck.h` |
| ④ | テストの期待値 `# EXPLAIN-MUT:` | `tests/run_tests.sh` |
| ⑤ | テスト 10 本 | `tests/cases/err_mut_*.po` ほか |

**★ option が 4 つになったので、`OwnckOptions` にまとめました。**

```c
typedef struct {
    bool deny_move;    // --deny-move
    bool deny_borrow;  // --deny-borrow
    bool deny_mut;     // --deny-mut
    bool explain_mut;  // --explain-mut
} OwnckOptions;
```

引数を 5 個並べる関数は、呼ぶ側で順番を間違えます。
**「増えたらまとめる」は、増えた直後にやるのがいちばん安い。**

---

## 24.2 `mut` は誰に要るのか

**`mut` は「借用の権限」です。** 自分のものを書き換えるのに許可は要りません。

| 書き換える対象 | `mut` が要るか | 根拠 |
|---|---|---|
| 局所変数の再代入（`i = i + 1`） | ❌ 要らない | 別名を作らないので安全（仕様 §5.1） |
| `own` で受け取った引数 | ❌ 要らない | 所有権が自分にある |
| ローカルで作ったオブジェクト | ❌ 要らない | 同上 |
| グローバル変数 | ❌ 要らない | 借用ではない（設計 §5.3） |
| **借用した引数**（既定） | ✅ **要る** | 貸し手が知らないうちに変わるため |
| **`self`**（`init` を除く） | ✅ **要る** | 同上 |

実装は第23章の `BorrowRoot` に 1 ビット足すだけです。

```c
struct BorrowRoot {
    ...
    bool is_mut;      // mut で借りているか（書き換えてよいか）
};
```

```c
static void check_mut(Own *o, Node *at, Node *target, WriteKind kind) {
    Place *p = place_of(target);
    if (!p) return;
    BorrowRoot *br = borrow_root_of(o, p);
    if (!br) return;         // 借りものではない＝自分のもの。書き換え自由
    if (br->is_mut) return;  // mut で借りている
    ... E-MUT-1 ...
}
```

**★ 「借りものでなければ何も言わない」** という形が大事です。
`mut` を Rust の `let mut` のように**規律**として要求し始めると、
局所変数にまで注釈が要る言語になります（決定 D3 に反します）。

---

## 24.3 書き換えが起きる 4 か所

```c
typedef enum {
    WR_ASSIGN,  // p.f = v / xs[i] = v
    WR_METHOD,  // mut self のメソッドを呼んだ
    WR_APPEND,  // append した
    WR_ARG,     // mut 引数に渡した
} WriteKind;
```

**⚠️ 4 番目（`mut` 引数への又貸し）を忘れがちです。**

```python
def fill(xs: mut list[int], v: int) -> None:
    xs.append(v)

def relay(xs: list[int]) -> None:
    fill(xs, 1)        # ❌ E-MUT-1：読み取り専用で借りたものを可変で又貸ししている
```

`relay` の中には代入も `append` も出てきません。**渡した先で書き換わります。**
「書き換えの構文」ではなく「**可変借用として渡したかどうか**」で見るのが正解です。

種類の違いは**言い回しだけ**に使います。

| WriteKind | 主ラベル |
|---|---|
| `WR_ASSIGN` | この代入で書き換えています |
| `WR_METHOD` | このメソッドは self を書き換えます |
| `WR_APPEND` | append はリストを書き換えます |
| `WR_ARG` | この引数は 'mut' で受け取られます |

---

## 24.4 `init` を例外にした

実装して最初に出た警告がこれでした。

```python
    def init(self) -> None:
        self.n = 0        # ← 'self' は読み取り専用の借用です？
```

**すべてのコンストラクタが `mut self` を要求される**ことになります。
仕様 §4.2 の例も `def init(self, name: own str)` と書いていて、そうはなっていません。

**🤔 `init` の `self` は本当に借用か**

`init` が動いている間、そのオブジェクトは**まだ誰にも貸していません**。
`Token(1, "x")` という式が作った、その場の値です。貸し手がいないのだから、
「貸し手が知らないうちに変わる」という危険がそもそも起きません。

**★ 決定：`init` の `self` は可変として扱う（`mut self` と書かなくてよい）。**

```c
        b->is_mut = pm->mode == PM_MUT ||
                    (b->is_self && strcmp(fn->name, "init") == 0);
```

**⚠️ 例外は「1 つで済むか」を必ず確かめること。**
第21章で `mut self` を例外にしたときと同じ判断です（例外が 2 つ 3 つと増えるなら、
それは規則のほうが間違っています）。ここでは `init` の 1 つで済みました。

---

## 24.5 B1：二重ループだけで済む理由

```python
def swap_first(xs: mut list[int], ys: list[int]) -> None: ...

swap_first(a, a)   # ❌ E-BORROW-5
```

設計（[ownership.md §5.1](../design/ownership.md)）にはこう書いてありました。

```c
for (each arg_i with PM_MUT)
    for (each arg_j, j != i)
        if (place_overlaps(place_of(arg_i), place_of(arg_j)))
            error_E_BORROW_5();
```

**実装もそのままです。** 借用の寿命が「呼び出しの間」に固定されているので
（仕様 §4.4）、**比べる範囲が 1 つの呼び出しの中に閉じています**。
Rust の借用検査が難しいのは、借用が関数の外へ出ていくからです。出ていかないなら、
検査は「その場で並べて見比べる」だけになります。

```c
    for (ArgRef *a = args; a; a = a->next)
        for (ArgRef *b = a->next; b; b = b->next) {
            if (!a->is_mut && !b->is_mut) continue;
            Place *pa = place_of(a->expr), *pb = place_of(b->expr);
            if (!pa || !pb || !place_overlaps(pa, pb)) continue;
            report_alias(o, a->is_mut ? a : b, a->is_mut ? b : a, pa);
        }
```

**★ `place_overlaps` は第22章で書いたものをそのまま使っています。**
`p.a` と `p.b` は重ならないので、`fill(p.a, p.b)` は通ります。
場所を接頭辞で比べる、というたった 3 行の判定が、3 つの章で働いています。

---

## 24.6 メソッドは `m(obj, args)` と同じ

```python
    b.push_len(b.items)    # ❌ self（可変）と引数（共有）が重なっている
```

設計 §5.2 のとおり、**`self` を第 0 引数として並べれば**、あとは同じ検査で済みます。

```c
    if (self_param && n->kind == ND_METHOD && n->lhs)
        tail = tail->next =
            arg_ref(n->lhs, NULL, self_param->mode == PM_MUT, WR_METHOD);
```

`xs.append(v)` も同じ形にしてあります（受け手が可変、引数が共有）。
おかげで `xs.append(xs)` のような自己参照も、**追加のコードなしで**引っかかります。

---

## 24.7 `--explain-mut` — 書かせない代わりに、聞けば答える

仕様 §5.3 は「呼び出し側には何も書かせない」と決めました。

```python
c.bump()        # Rust の (&mut c).bump() に相当。記法上は Python のまま
clear(xs)       # clear(mut xs) とは書かない
```

**🤔 「変更されるのが呼び出し側から見えない」問題**

見えないのは事実です。仕様はその代わりに「診断と道具で補う」と約束しました。
その道具がこれです。

```bash
$ ./build/poloniumc --explain-mut t.po
t.po:17:5: 'xs' が変更されます（list.append）
t.po:21:5: 'c' が変更されます（Counter.bump の 'mut self'）
t.po:23:10: 'xs' が変更されます（fill の引数 'xs: mut list[int]'）
```

**★ 出力形式の決定（D13）：`file:line:col: 説明` の 1 行 1 件。**

- **エディタから飛べる**（`file:line:col:` は多くのエディタと `grep -n` の共通形式）
- **`grep` で読める**（「この関数を変更する呼び出しはどこか」を機械的に絞れる）
- 専用の構造化形式（JSON など）にはしない。**読む人が最初のユーザー**で、
  機械可読が必要になったらそのとき決める（第7章の「まず動かす」と同じ）

テストも `--dump-tokens` を `# TOKENS:` で検証するのと同じ形にしました。

```python
# EXPLAIN-MUT: 'c' が変更されます（Counter.bump の 'mut self'）
```

**⚠️ 「表示するだけ」の option もテストすること。** 表示は仕様の一部です。

---

## 24.8 動作確認

✍️ テストを 10 本足します。

| ファイル | 確認すること |
|---|---|
| `err_mut_self.po` | `self` を書き換えるメソッドには `mut self` が要る |
| `err_mut_append.po` | `append` は書き換え（仕様 §5.2 の例そのもの） |
| `err_mut_arg.po` | 読み取り専用の借用を `mut` 引数へ又貸しできない |
| `err_mut_index.po` | 添字への代入も書き換え |
| `err_mut_method.po` | `mut self` のメソッドは共有借用には呼べない |
| `err_borrow_alias_arg.po` | `swap_first(a, a)` が落ちる（B1） |
| `err_borrow_alias_self.po` | `b.push_len(b.items)` が落ちる（self も並べる） |
| `own_mut_ok.po` | `mut` を書けば通る。局所変数と `init` には要らない |
| `own_mut_disjoint.po` | `fill(p.a, p.b)` は衝突しない |
| `explain_mut.po` | `--explain-mut` の出力 |

**✅ 確認**

```bash
$ make test
全 364 件パス
トークン列一致 404 件 / AST 一致 362 件 / 型検査 一致 220 件 / IR 一致 220 件

$ make bootstrap
★ 不動点に到達しました（stage2 == stage3）

$ make bootstrap-test
全 331 件パス
（33 件スキップ）
```

**★ この章も生成 IR は 1 バイトも変わりません。** 変わるのは第25章からです。

---

## 24.9 第26章の宿題（更新）

| 検査 | `selfhost/` | `lib/` |
|---|---|---|
| `E-MOVE-1`（移動済みの使用） | 138 | 0 |
| `E-BORROW-3`（借用の保存） | 76 | 1 |
| `E-BORROW-4`（借用の返却） | 32 | 1 |
| `E-MUT-1`（読み取り専用への書き換え） | **154** | 3 |
| `E-BORROW-5`（借用の衝突） | 0 | 0 |
| **合計** | **400** | **5** |

**⚠️ `E-BORROW-5` が 0 件なのは、良い知らせではありません。**
`selfhost/` にはまだ `mut` が 1 つも無いので、**可変借用が存在しない**からです。
第26章で `mut` を付け始めると、初めてこの検査が働きます。
**「0 件だから安全」ではなく「0 件になる理由」を確かめること。**

`E-MUT-1` の 154 件は、ほとんどが「メソッドが `self` を書き換えている」形です。
第26章の作業は、大部分が **`mut self` を機械的に付けて回る**ことになります。
`--explain-mut` は、その後で「どの呼び出しが何を変更するのか」を
読み直すために使えます。

### この章で残した宿題

| 残したもの | いつ |
|---|---|
| 解放（drop）の挿入と drop フラグ | 第25章 |
| `copy(x)` / `del x` / `xs.pop()` | 第25章 |
| 局所コンテナの要素の move out | 第25章 |
| `lib/` と `selfhost/` の移行（405 件） | 第26章 |

---

## 24.10 まとめと次章の予告

この章でやったこと：

- `mut` を「**借用の権限**」として実装した（自分のものには要求しない）
- 書き換えの 4 か所（代入・`append`・`mut self` 呼び出し・`mut` 引数への又貸し）を見た
- `init` の `self` を例外にした（貸し手がいないので危険が無い）
- B1 を**二重ループ 6 行**で実装した（借用が呼び出しの外へ出ないため）
- `--explain-mut` で「呼び出し側に書かせない」ことの代償を埋めた
- **安全性の検査（S1・S2・S5）がこれで全部そろった**

次章（第25章）は **解放（drop）の挿入**です。**v1 で諦めていた `free` を実装します。**
ここが v2 の山場で、**初めて生成される IR が変わる章**でもあります。

- スコープの出口すべて（通常終端・`return`・`break` / `continue`）に drop を挿入する
- `MaybeMoved` の場所には drop フラグ（`alloca i1`）を持たせる
  — **第22章の解析結果を、初めて codegen が使います**
- `drop` メソッド（デストラクタ）、`del x`、`copy(x)`、`xs.pop()`
- [design/memory-model.md](../design/memory-model.md) を「解放しない」から書き換える

**★ 第22〜24章で「誰が所有者か」が静的に決まりました。**
所有者が決まったからこそ、「いつ解放してよいか」も決まります。
順番を逆にはできません。
