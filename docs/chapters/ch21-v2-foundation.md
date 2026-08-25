# 第21章 v2 の土台（キーワードと `own` / `mut` の構文）

> **この章のゴール**
> `own` / `mut` を引数に書けるようになる。**ただし意味はまだ与えない。**
>
> ```bash
> $ cat t.po
> def take(xs: own list[int]) -> int:      # 所有権を受け取る
>     return len(xs)
>
> def fill(xs: mut list[int], n: int) -> None:   # 書き換えるために借りる
>     xs.append(n)
>
> class Counter:
>     n: int
>     def init(self) -> None:
>         self.n = 0
>     def bump(mut self) -> None:          # self を書き換える
>         self.n = self.n + 1
>
> def main() -> int:
>     xs: list[int] = [1, 2]
>     fill(xs, 3)
>     print(take(xs))
>     return 0
> $ ./build/poloniumc t.po -o t && ./t
> 3
> ```

**この章では、書けるようになるだけで、何も検査されません。**
`own` と書いても所有権は移らず、`mut` と書かなくても書き換えられます。
動作は v1 とまったく同じです。

**🤔 なぜ意味のない構文を先に入れるのか**

所有権の検査（第22〜24章）は、**構文・AST・セルフホストの 3 か所すべてに**変更が要ります。
検査の実装と同時にやると、落ちたときに「解析が悪いのか、構文の読み方が悪いのか」が
切り分けられません。**構文だけを先に通して緑にしておけば、次章からは解析だけを疑えます。**

---

## 目次

- [21.1 何を足すか](#211-何を足すか)
- [21.2 キーワードを予約する](#212-キーワードを予約する)
- [21.3 受け取り方をどこに持たせるか](#213-受け取り方をどこに持たせるか)
- [21.4 own / mut を読む](#214-own--mut-を読む)
- [21.5 なぜ「型の前」なのか](#215-なぜ型の前なのか)
- [21.6 --dump-ast に出す](#216---dump-ast-に出す)
- [21.7 Polonium 版に移植する](#217-polonium-版に移植する)
- [21.8 動作確認](#218-動作確認)
- [21.9 まとめと次章の予告](#219-まとめと次章の予告)

---

## 21.1 何を足すか

| # | 足すもの | 触るファイル |
|---|---|---|
| ① | キーワード 5 語（`own` / `mut` / `raises` / `unsafe` / `pragma`） | `src/lexer.c`, `selfhost/lexer.po` |
| ② | `ParamMode`（受け取り方）と `Node.mode` | `src/ast.h`, `selfhost/ast.po` |
| ③ | `x: own T` / `x: mut T` / `mut self` の解析 | `src/parser.c`, `selfhost/parser.po` |
| ④ | `--dump-ast` への出力 | `src/ast.c`, `selfhost/ast.po` |

仕様は [safety-spec.md §4・§11](../spec/safety-spec.md)、設計は
[design/ownership.md §2](../design/ownership.md) です。**先にそちらを読んでください。**

---

## 21.2 キーワードを予約する

✍️ `src/lexer.c` の `KEYWORDS` に 5 語足します。

```c
    // 言語仕様 v2 で使う語（第21章〜。所有権とエラー処理）
    // ★ del / try / except / with は v1 の時点で予約済みなので下の表にあります。
    "own", "mut", "raises", "unsafe", "pragma",
```

**★ 第1章の「使わない語も予約する」という判断が、ここで効きます。**
`try` / `except` / `with` / `del` は v1 の時点で予約してあったので、**今回の追加は 5 語で済みました**。
もし予約していなければ、`try` を変数名に使った既存コードが第27章で壊れていたはずです。

**✅ 確認**：予約語は変数名に使えません。

```bash
$ echo 'def main() -> int:
    own: int = 1
    return own' > t.po && ./build/poloniumc --check t.po
error: 'own' は予約語です
```

**⚠️ 既存コードとの衝突を先に調べること。**

```bash
$ grep -rnw "own\|mut\|raises\|unsafe\|pragma" src selfhost lib tests examples
（キーワード表以外に出てこないことを確認する）
```

---

## 21.3 受け取り方をどこに持たせるか

引数の受け取り方は 3 通りです。

```c
typedef enum {
    PM_BORROW,  // 既定。読むだけ借りる
    PM_MUT,     // mut。書き換えるために借りる
    PM_OWN,     // own。所有権を受け取る
} ParamMode;
```

これを **`Type` ではなく `Node`（`ND_PARAM`）に持たせます**。

**🤔 なぜ型に持たせないのか**

`Type` はシングルトンで共有されています（`src/types.h`。`int` 型のオブジェクトは
プログラム全体で 1 個）。ここに所有の情報を足すと、`int` が場所ごとに別物になり、
**既存の「型の同一性はポインタ比較」という前提が壊れます**。

受け取り方は「型の性質」ではなく「**その引数の性質**」です。
`list[int]` という型が変わるのではなく、`xs` という引数の扱いが変わるだけ。
だから引数のノードに持たせるのが正しい置き場所です。

✍️ `src/ast.h` の `Node` に 1 行足します。

```c
    // 仮引数の受け取り方（ND_PARAM。第21章）。
    // ★ 既定は PM_BORROW なので、new_node の calloc がそのまま初期値になります。
    ParamMode mode;
```

**★ 既定値 0 = `PM_BORROW` になるように列挙の順序を決めてあります。**
`new_node` は `calloc` でゼロ埋めするので、**初期化のコードを 1 行も書かずに済みます**。
「既定が 0 になる並び」は、こういう場面で効きます。

---

## 21.4 `own` / `mut` を読む

✍️ `src/parser.c` の `param()` を書き換えます。

```
param ::= IDENT ":" [ "own" | "mut" ] type
        | [ "mut" ] "self"
```

読む順序はこうです。

```
  ① 名前の前に mut / own があるか？   → あれば覚えておく（lead）
  ② 名前を読む
  ③ self で、次が ':' でない？        → メソッドの self。lead が mut なら PM_MUT
  ④ ここで lead が残っていたら        → 「型の前に書きます」と案内して終了
  ⑤ ':' を読む
  ⑥ own / mut があれば mode に入れる
  ⑦ 型注釈を読む
```

**⚠️ ④ を忘れると `mut xs: list[int]` が黙って通ってしまいます。**
「読めてしまうが意味が無い書き方」は、エラーにして**直し方を示す**のが仕事です。

```
error: ここには書けません
  --> t.po:1:7
   |
 1 | def f(mut xs: list[int]) -> int:
   |       ^^^
   |
   = ヒント: 'mut' は型の前に書きます（例: xs: mut list[int]）
```

`own self` も同じく弾きます（self の所有権を奪うと、呼び出し元のオブジェクトが消えるため）。

```
   = ヒント: self の所有権は奪えません（'mut self' なら書けます）
```

---

## 21.5 なぜ「型の前」なのか

Rust は `&mut self`、C++ は `const T&` のように、**修飾を型の側に書きます**。
Polonium も同じ側に置きました。

```python
def fill(xs: mut list[int]) -> None:     # ✅ 採用
def fill(mut xs: list[int]) -> None:     # ❌ 却下
```

**🤔 判断の理由**

Python の引数は「**名前: 型**」という並びです。修飾を名前の前に置くと、
その並びの外側に別の要素が割り込み、**型注釈が「名前の一部」に見えてしまいます**。
型の前に置けば `xs` は「**可変で借りた `list[int]`**」だと、型注釈の中で読み切れます。

例外は `self` です。`self` には型注釈がないので（クラスが決まっているため書かせない）、
修飾を置く場所が名前の前しかありません。**例外は 1 つだけ**なので、
「型の前。ただし self だけは名前の前」と覚えられます。

---

## 21.6 `--dump-ast` に出す

```c
const char *param_mode_prefix(ParamMode mode) {
    if (mode == PM_OWN) return "own ";
    if (mode == PM_MUT) return "mut ";
    return "";
}
```

```c
        case ND_PARAM:
            printf("(param %s%s\n", param_mode_prefix(n->mode), n->name);
```

**★ 既定（借用）は何も出しません。** 既存の出力を変えないためです。
出力を変えると、第17章から積み上げてきた **AST 比較（`make selfhost-test`）が
全件落ちて、本当の差分が埋もれます**。

```
$ ./build/poloniumc --dump-ast t.po | grep param
    (param xs
    (param own xs
    (param mut xs
      (param mut self
```

---

## 21.7 Polonium 版に移植する

✍️ 同じ変更を `selfhost/` にも入れます。

| C 版 | Polonium 版 |
|---|---|
| `KEYWORDS` に 5 語 | `selfhost/lexer.po` の `self.keywords` |
| `ParamMode` | `selfhost/ast.po` の `PM_BORROW` / `PM_MUT` / `PM_OWN`（**C 版と同じ値**） |
| `Node.mode` | `selfhost/ast.po` の `Node` に `mode: int` と `self.mode = PM_BORROW` |
| `param_mode_prefix` | 同名の関数 |
| `param()` | `selfhost/parser.po` の `Parser.param` |

**⚠️ Polonium には enum が無いので、グローバル定数で代用します（`token.po` と同じ手）。
値は C 版と一致させること。** 値がずれても動いてしまい、`--dump-ast` の比較で
初めて気づくことになります。

**⚠️ 診断の文字列は 1 バイトも変えないこと。** `make selfhost-test` は
エラーメッセージも突き合わせます。C 版で `diag_fmt("'%s' は型の前に…", ...)` と
書いた部分は、Polonium 版では文字列連結になります。

```python
            self.fail_hint(lead,
                           "'" + lead.text + "' は型の前に書きます（例: " +
                           name_tok.text + ": " + lead.text + " list[int]）",
                           "ここには書けません")
```

---

## 21.8 動作確認

✍️ テストを 8 本足します。

| ファイル | 確認すること |
|---|---|
| `own_syntax_basic.po` | `own` / `mut` 付きの引数が動く（意味は変わらない） |
| `own_syntax_mut_self.po` | `mut self` のメソッドが動く |
| `own_syntax_str_field.po` | `own str` をフィールドに保存できる |
| `tok_own_mut.po` | `own` / `mut` が KEYWORD として字句解析される |
| `err_own_before_name.po` | `mut xs: list[int]` が落ちる |
| `err_own_self.po` | `own self` が落ちる |
| `err_own_as_var.po` | `own` を変数名に使うと落ちる |
| `err_raises_as_var.po` | `raises` も予約済み |

**✅ 確認**

```bash
$ make test
全 331 件パス
トークン列一致 370 件 / AST 一致 328 件 / 型検査 一致 187 件 / IR 一致 187 件

$ make bootstrap
★ 不動点に到達しました（stage2 == stage3）

$ make bootstrap-test
全 331 件パス
```

**★ 「動作が v1 とまったく同じ」ことこそが、この章の合格条件です。**
IR が 1 バイトも変わらないので、不動点もそのまま維持されます。

---

## 21.9 まとめと次章の予告

この章でやったこと：

- キーワードを 5 語予約した（`own` / `mut` / `raises` / `unsafe` / `pragma`）
- 受け取り方（`ParamMode`）を**型ではなく引数**に持たせた
- `x: own T` / `x: mut T` / `mut self` を解析し、`--dump-ast` に出した
- **意味は与えず、動作は v1 のまま**にした

次章（第22章）で、いよいよ**検査**が始まります。
新しいパス `src/ownck.c` を作り、`Place` とデータフロー解析で
「移動済みの値を使っていないか」を見ます（[design/ownership.md §3〜4](../design/ownership.md)）。

**⚠️ 第22章は既存コードに警告を出します。** その警告は消さずに記録し、
第26章でまとめて直します。**「動くものを壊さない」ために、検査は段階的に入れます。**
