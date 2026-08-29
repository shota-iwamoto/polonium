# Polonium 入門

**Polonium を書く人のための手引きです。** コンパイラの作り方（このリポジトリの本編）ではなく、
**言語そのものの使い方**を扱います。

Python を書いたことがあれば、最初の 30 分で読み切れる分量にしてあります。
Python との違いに絞って説明するので、同じところは説明しません。

> **一行でいうと** — Python の見た目のまま、型が静的に決まり、機械語にコンパイルされる言語です。
> さらに、Rust の所有権に近い仕組みで「解放し忘れ・二重解放」を検査します。

| | |
|---|---|
| 拡張子 | `.po` |
| コンパイラ | `poloniumc` |
| 入口 | `def main() -> int:` |
| 実行時 | GC なし・例外なし・リフレクションなし |

---

## 1. 5 分で動かす

インストールは [README](../README.md#使ってみるlinux--macos--windows) を見てください。**clang があれば動きます。**

```python
# hello.po
def main() -> int:
    print("hello, polonium")
    return 0
```

```bash
$ poloniumc hello.po -o hello
$ ./hello
hello, polonium
```

**`main` は必須です。** Python のようにトップレベルへ処理を書くことはできません。
`main` の戻り値の**下位 8 ビット**がプロセスの終了コードになります。

```bash
$ poloniumc --version
poloniumc 0.1.0 (stage0)
target: x86_64-apple-macosx26.0.0
```

---

## 2. まず知っておく 7 つの違い

Python のつもりで書くと最初に必ず引っかかる場所です。
**どれもコンパイル時に止まります**（実行してから気づくことはありません）。

| # | Python では | Polonium では | 出るエラー |
|---|---|---|---|
| 1 | `x = 1` | **`x: int = 1`**（宣言に型注釈が要る） | `未定義の名前 'x' に代入しています` |
| 2 | `7 / 2` → `3.5` | **`7 // 2`** を使う | `整数の除算に '/' は使えません` |
| 3 | `if xs:` | **`if len(xs) > 0:`** | `if の条件には bool が必要です` |
| 4 | `1 < 2 < 3` | **`1 < 2 and 2 < 3`** | `比較演算子を連鎖させることはできません` |
| 5 | トップレベルに `print(...)` | **`main` の中に書く** | `トップレベルに実行文は書けません` |
| 6 | `"ab" * 2` | **なし**（`strings.repeat`） | `型 'str' と 'int' に演算子 '*' は適用できません` |
| 7 | `-7 // 2` → `-4` | **`-3`**（0 方向に丸める） | — |

そのほか **無いもの**：継承・例外（後述の `raises` が代わり）・クロージャ・`lambda`・
ジェネリクス・タプル・集合・内包表記・スライス・変数のシャドーイング・`float`。

> **⚠️ `float` について** — 言語仕様書には載っていますが、**まだ実装されていません**。
> `1.5` と書くと構文エラーになります。数値は当面 `int`（64 ビット）だけです。

---

## 3. 型と値

### 3.1 プリミティブ

| 型 | 例 | 備考 |
|---|---|---|
| `int` | `42`, `-7` | 64 ビット符号付き。あふれたら 2 の補数で回り込む |
| `bool` | `True`, `False` | 大文字始まり（Python と同じ） |
| `str` | `"abc"` | **不変**。NUL 終端の UTF-8 バイト列 |
| `None` | `None` | 値を持たない型。戻り値の無い関数の戻り型 |

### 3.2 まとまった型

| 型 | 例 |
|---|---|
| `list[T]` | `xs: list[int] = [1, 2, 3]` |
| `dict.Dict` | 標準ライブラリのクラス（§7） |
| `class` | 自分で定義する（§6） |
| `T \| None` | `p: Node \| None = None`（§6.3） |
| `rc[T]` | 共有したいときの逃げ道（§9.4） |

`list` は**入れ子にできます**。

```python
def main() -> int:
    grid: list[list[int]] = [[1, 2], [3]]
    print(len(grid[0]))       # → 2
    return 0
```

### 3.3 型注釈が要る場所

**宣言・引数・戻り値**の 3 か所です。それ以外（再代入など）には書きません。

```python
def add(a: int, b: int) -> int:      # 引数と戻り値
    total: int = a + b               # 宣言
    total = total + 0                # 再代入には書かない
    return total
```

### 3.4 暗黙の変換はしない

`int` と `str` は自動で混ざりません。**明示的に変換します。**

```python
n: int = 5
print("答え: " + str(n))     # ○
print("答え: " + n)          # × 型 'str' と 'int' に演算子 '+' は適用できません
```

変換に使う組み込み関数はこれだけです。

| 関数 | 説明 |
|---|---|
| `str(int)` / `str(bool)` | 文字列にする |
| `int(str)` | 整数にする（パースできなければ実行時エラー） |
| `ord(str)` | 先頭 1 バイトのコード |
| `chr(int)` | 1 バイトの文字列を作る |
| `len(str)` / `len(list[T])` | 長さ |
| `print(...)` | `int` / `str` / `bool` を受け取る |
| `panic(str)` | メッセージを stderr に出して異常終了 |
| `exit(int)` | 終了コードを指定して終了 |
| `copy(str)` | 文字列を複製する（§9.3） |

---

## 4. 制御構造

Python と同じ見た目です。**条件は必ず `bool`** である点だけ違います。

```python
def classify(n: int) -> str:
    if n < 0:
        return "負"
    elif n == 0:
        return "ゼロ"
    else:
        return "正"

def main() -> int:
    for i in range(0, 6, 2):          # 0, 2, 4
        print(str(i) + ": " + classify(i - 2))

    xs: list[str] = ["a", "b"]
    for s in xs:                      # リストを直接まわす
        print(s)

    i: int = 0
    while True:
        i = i + 1
        if i < 3:
            continue
        break                         # break / continue は使える
    print(i)                          # → 3
    return 0
```

`range` は `range(n)` / `range(a, b)` / `range(a, b, step)` の 3 通りです。
インデントは**スペースのみ**（タブはエラー）。

---

## 5. 関数

```python
def greet(name: str) -> str:
    return "hello, " + name

def show(n: int) -> None:        # 値を返さないなら -> None
    print(str(n))
```

**オーバーロードはできません**（`print` だけコンパイラが特別扱いしています）。
デフォルト引数・キーワード引数・可変長引数もありません。

---

## 6. クラス

### 6.1 定義

`__init__` ではなく **`init`** です。ダンダーは導入していません。

```python
class Point:
    x: int                      # フィールドは先に宣言する
    y: int

    def init(self, x: int, y: int) -> None:
        self.x = x
        self.y = y

    def norm2(self) -> int:
        return self.x * self.x + self.y * self.y

def main() -> int:
    p: Point = Point(3, 4)      # Point(...) で生成（new は無い）
    print(p.norm2())            # → 25
    return 0
```

**継承はありません。** フィールドは宣言したものだけで、後から生やせません（静的レイアウトのため）。

### 6.2 参照セマンティクス

`class` と `list` は**参照**です。代入しても複製されません。

```python
a: Point = Point(1, 2)
b: Point = a
b.x = 99
print(a.x)                      # → 99（同じものを指している）
```

### 6.3 `T | None` と絞り込み

「無いかもしれない」は `T | None` で表します。**中身を触る前に必ず確認が要ります。**

```python
class Node:
    n: int
    def init(self, n: int) -> None:
        self.n = n

def find(ok: bool) -> Node | None:
    if ok:
        return Node(7)
    return None

def main() -> int:
    p: Node | None = find(True)
    print(p.n)                  # × 'Node | None' のまま触ることはできません
    if p is not None:
        print(p.n)              # ○ ここでは Node に絞り込まれている
    return 0
```

これが **None 安全**です。`if p is not None:` の中では、コンパイラが `p` を `Node` として扱います。

---

## 7. 標準ライブラリ

`import` するだけで使えます。**中身はすべて Polonium で書かれています**（`lib/*.po`）。
コンパイラは特別扱いしていません。

### strings

```python
import strings

def main() -> int:
    for w in strings.split("a,b,c", ","):
        print(w)
    print(strings.join(["x", "y"], "-"))     # → x-y
    print(strings.strip("  hi  "))           # → hi
    print(str(strings.find("hello", "ll")))  # → 2
    return 0
```

ほかに `substr` / `contains` / `startswith` / `endswith` / `replace` /
`repeat` / `lpad` / `rpad` / `byte_at` / `matches_at` / `is_space` があります。

### io

`read_file` / `write_file` / `remove` / `exists` / `print_raw`（改行なし）/ `eprint`（stderr）。

```python
import io

def main() -> int:
    io.write_file("memo.txt", "hello\n")
    print(io.read_file("memo.txt"))
    io.remove("memo.txt")
    return 0
```

### sys

`argv()` / `argc()` / `getenv(name)` / `run(cmd)`。

```python
import sys

def main() -> int:
    args: list[str] = sys.argv()
    if len(args) < 2:
        print("使い方: prog <名前>")
        return 1
    print("hello, " + args[1])
    return 0
```

### dict

**鍵は `str`、値は `int`** の固定です（ジェネリクスが無いため）。

```python
import dict

def main() -> int:
    d: dict.Dict = dict.Dict()
    d.set("apple", 3)
    print(str(d.get_or("apple", 0)))    # → 3
    print(str(d.get_or("none", -1)))    # → -1
    for k in d.keys():
        print(k)
    return 0
```

値に**リストの添字**を入れるのが、ジェネリクスの無い言語での定石です。

```python
import dict

class Symbol:
    name: str
    value: int
    def init(self, name: own str, value: int) -> None:   # own は §9.2
        self.name = name
        self.value = value

def main() -> int:
    table: list[Symbol] = []
    syms: dict.Dict = dict.Dict()

    table.append(Symbol("x", 10))
    syms.set("x", len(table) - 1)       # 添字を覚えておく

    i: int = syms.get_or("x", -1)
    print(str(table[i].value))          # → 10
    return 0
```

---

## 8. モジュール

ファイルがそのままモジュールです。`import foo` は
**同じディレクトリの `foo.po` → 標準ライブラリ**の順に探します。

```python
# geometry.po
class Point:
    x: int
    y: int
    def init(self, x: int, y: int) -> None:
        self.x = x
        self.y = y

def origin() -> Point:
    return Point(0, 0)
```

```python
# main.po
import geometry

def main() -> int:
    p: geometry.Point = geometry.origin()
    print(str(p.x))
    return 0
```

**必ず `モジュール名.` を付けて呼びます**（`from ... import ...` はありません）。
コンパイルするのは入口のファイルだけで、`import` は自動でたどられます。

```bash
$ poloniumc main.po -o main
```

---

## 9. 安全性 — Polonium の中心

ここからが Python にも C にも無い部分です。**GC を使わずに、解放し忘れと二重解放を防ぎます。**

> **⚠️ いまはすべて「警告」です。** 既定ではコンパイルは通ります。
> エラーにしたいときは `--deny-move` / `--deny-borrow` / `--deny-mut` を付けます。
> 段階的に直していけるように、こうしてあります。

### 9.1 引数は既定で「借用」

関数に値を渡しても、**所有権は移りません**。呼び出しの後も自由に使えます。

```python
def total(xs: list[int]) -> int:     # 借りるだけ
    s: int = 0
    for x in xs:
        s = s + x
    return s

def main() -> int:
    xs: list[int] = [1, 2, 3]
    print(str(total(xs)))
    print(str(len(xs)))              # ○ まだ使える
    return 0
```

**呼び出し側には何も書きません。** `&` も `.clone()` も要りません。

### 9.2 `own` — 所有権を受け取る

「この関数が値を引き取る」と言いたいときだけ `own` を書きます。
渡した側は**それ以降その変数を使えません**。

```python
def take(xs: own list[int]) -> int:
    return len(xs)

def main() -> int:
    xs: list[int] = [1, 2, 3]
    print(str(take(xs)))             # ここで xs は take のものになった
    print(str(len(xs)))              # ⚠️ 移動済みの値 'xs' を使っています
    return 0
```

```
warning[E-MOVE-1]: 移動済みの値 'xs' を使っています
  --> mv.po:7:15
   |
 7 |     print(len(xs))
   |               ^^ ここで使われています
   |
note: 'xs' はここで移動しました
  --> mv.po:6:16
   |
 6 |     print(take(xs))
   |                ^^
```

### 9.3 借用したものは保存できない

借りた値を**自分より長生きする場所**（フィールド・グローバル・戻り値）に置くことはできません。
貸してくれた相手が先に消えると、壊れたポインタが残るためです。

直し方は 2 つです。

| やりたいこと | 書き方 |
|---|---|
| 呼び出し側から所有権をもらう | 引数を `own` にする |
| 中身だけ複製して持つ | `copy(x)`（いまは `str` のみ） |

```python
import dict

def count(words: list[str]) -> None:
    d: dict.Dict = dict.Dict()
    for w in words:
        d.set(copy(w), d.get_or(w, 0) + 1)   # 鍵は Dict が所有するので複製
```

### 9.4 `mut` — 借りたものを書き換える

借用は既定で**読み取り専用**です。書き換えるなら `mut` を書きます。

```python
def fill(xs: mut list[int], n: int) -> None:
    xs.append(n)

class Counter:
    n: int
    def init(self) -> None:
        self.n = 0
    def bump(mut self) -> None:      # self を書き換えるメソッドにも mut
        self.n = self.n + 1
    def get(self) -> int:            # 読むだけなら要らない
        return self.n
```

**呼び出し側は何も変わりません。**

```python
c: Counter = Counter()
c.bump()                             # mut と書く必要はない
```

どの引数が書き換えられるのかを確かめたいときは `--explain-mut` を使います。

> 局所変数の再代入（`i = i + 1`）と `init` の中の `self.x = ...` に `mut` は要りません。

### 9.5 `rc[T]` — 共有したいとき

「1 つの値を複数の場所が持ち続ける」構造は、所有権だけでは書けません。
そのための逃げ道が `rc[T]`（参照カウント）です。**最後の 1 つが手放したときに解放されます。**

```python
class Node:
    name: str
    def init(self, name: own str) -> None:
        self.name = name

def keep(dst: mut list[rc[Node]], n: rc[Node]) -> None:
    dst.append(n)

def main() -> int:
    a: rc[Node] = rc(Node("a"))
    b: rc[Node] = a                  # 共有。a はまだ使える
    box: list[rc[Node]] = []
    keep(box, a)
    print(a.name)                    # → a（`.` は自動で中身に届く）
    print(b.name)                    # → a
    return 0
```

`rc` は**最後の手段**です。まず借用で書けないか試してください。

### 9.6 解放

スコープを出るときに解放が入ります。クラスに `drop` を書けば、そのときに呼ばれます。

```python
class File:
    name: str
    def init(self, name: own str) -> None:
        self.name = name
    def drop(self) -> None:
        print("閉じる: " + self.name)
```

**解放の挿入は `--drop` を付けたときだけ**行われます（既定は付きません）。

---

## 10. エラー処理

例外はありません。`raises` / `try` / `except` は**見た目だけ例外で、実体は戻り値の検査**です。
スタックの巻き戻し（アンワインド）は起きません。

```python
class IOError:
    message: str
    def init(self, message: own str) -> None:
        self.message = message

def read(path: str) -> int raises IOError:     # 起こしうるエラーを宣言する
    if len(path) == 0:
        raise IOError("空のパス")
    return len(path)

def main() -> int:
    try:
        print("ok:" + str(read("abcde")))
    except IOError as e:
        print("caught: " + e.message)

    try:
        print("ok:" + str(read("")))
    except IOError as e2:
        print("caught: " + e2.message)
    return 0
```

```
ok:5
caught: 空のパス
```

3 つの規則があります。

1. **宣言しないと投げられない** — `raises` に書いていない型は `raise` できません。
2. **握りつぶせない** — `raises` する関数を呼ぶ側は、`try` で受けるか自分も `raises` を付けるか、どちらかが必要です。
3. **エラー型は `class`** — `int` や `str` は使えません。

複数書くときは `raises A | B` とします。

**`panic` との使い分け** — 呼び出し側が対処できるものは `raises`、
プログラムの間違い（配列の範囲外など、直すべきバグ）は `panic` です。

---

## 11. コンパイラの使い方

```bash
poloniumc [オプション] <入力.po>
```

| オプション | 説明 |
|---|---|
| `-o <file>` | 出力する実行ファイル名（既定 `a.out`） |
| `--check` | 型検査までで止める。問題が無ければ何も出さない |
| `-O0`〜`-O3` | clang に渡す最適化レベル（既定 `-O0`） |
| `--version` | 版番号と target triple |
| `-S` | LLVM IR を標準出力に出して終了 |

安全性の検査：

| オプション | 説明 |
|---|---|
| `--deny-move` | 移動済みの値の使用を**エラーにする** |
| `--deny-borrow` | 借用の保存・返却を**エラーにする** |
| `--deny-mut` | 読み取り専用の借用への書き換えを**エラーにする** |
| `--drop` | スコープの出口に解放を挿入する |
| `--explain-mut` | 呼び出しで書き換えられる実引数を一覧表示 |

新しく書くコードでは、最初から 3 つの `--deny-*` を付けることをおすすめします。

```bash
poloniumc --deny-move --deny-borrow --deny-mut app.po -o app
```

コンパイラ自身をいじる人向けのオプション（`--dump-tokens` / `--dump-ast` /
`--keep-ll` / `--target=` / `-c`）は `poloniumc --help` を見てください。

---

## 12. まとめの例

ここまでの要素だけで書ける、語数カウントです。

```python
# wordcount.po
import io
import strings
import sys
import dict

def main() -> int:
    args: list[str] = sys.argv()
    if len(args) < 2:
        print("使い方: wordcount <ファイル>")
        return 1

    text: str = io.read_file(args[1])
    counts: dict.Dict = dict.Dict()

    for line in strings.split(text, "\n"):
        for word in strings.split(strings.strip(line), " "):
            if len(word) > 0:
                counts.set(copy(word), counts.get_or(word, 0) + 1)

    for k in counts.keys():
        print(k + ": " + str(counts.get(k)))
    return 0
```

```bash
$ poloniumc examples/wordcount.po -o wc
$ ./wc examples/sample.txt
```

---

## 13. 次に読むもの

| 目的 | ドキュメント |
|---|---|
| 言語の正確な定義を知りたい | [spec/language-spec.md](spec/language-spec.md) |
| 所有権・エラー処理の正確な規則 | [spec/safety-spec.md](spec/safety-spec.md) |
| 文法（EBNF） | [spec/grammar.md](spec/grammar.md) |
| **コンパイラの作り方を学びたい** | [docs/README.md](README.md) — 第1章から第33章 |
| 用語がわからない | [reference/glossary.md](reference/glossary.md) |

---

## 付録: いま無いもの

正直に書いておきます。**これらは未実装です。**

| 無いもの | 代わりに |
|---|---|
| `float` | `int` のみ（仕様書にはあるが未実装） |
| ジェネリクス | `dict.Dict` は `str → int` 固定。添字を値に入れる定石を使う |
| 継承・インタフェース | 合成（フィールドに持つ） |
| クロージャ・`lambda` | トップレベルの関数 |
| スライス `xs[a:b]` | `for` と添字 |
| タプル・集合・内包表記 | `list` と `for` |
| 例外 | `raises` / `try` / `except`（§10） |
| 文字列の書式化（f-string） | `+` と `str(...)` |
| 標準入力の読み取り | `io.read_file` |

言語の現在地と今後は [roadmap-v2.md](roadmap-v2.md) と [tasks.md](tasks.md) にあります。
