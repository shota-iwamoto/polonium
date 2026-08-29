# 標準ライブラリ

`import` するだけで使えます。**中身はすべて Polonium で書かれています**（`lib/*.po`）。
コンパイラは一切特別扱いしていません — ユーザーが書けるものだけで出来ています。

```python
import strings

def main() -> int:
    print(strings.join(strings.split("a,b,c", ","), "-"))   # → a-b-c
    return 0
```

**必ず `モジュール名.` を付けて呼びます**（`from ... import ...` はありません）。

| モジュール | 内容 |
|---|---|
| [`strings`](#strings) | 文字列の操作 |
| [`io`](#io) | ファイル・標準出力・標準エラー・標準入力 |
| [`sys`](#sys) | コマンドライン引数・環境変数・外部コマンド |
| [`dict`](#dict) | 文字列を鍵とするハッシュ表 |

---

## strings

`str` は**不変**なので、どの関数も**新しい文字列を返します**。

### 取り出す

| 関数 | 説明 |
|---|---|
| `byte_at(s: str, i: int) -> int` | `i` バイト目のバイト値。範囲外は実行時エラー |
| `substr(s: str, start: int, count: int) -> str` | `start` から `count` バイト |

### 探す

| 関数 | 説明 |
|---|---|
| `find(s: str, sub: str) -> int` | `sub` が最初に現れる位置。**無ければ `-1`** |
| `contains(s: str, sub: str) -> bool` | 含むか |
| `startswith(s: str, prefix: str) -> bool` | で始まるか |
| `endswith(s: str, suffix: str) -> bool` | で終わるか |
| `matches_at(s: str, sub: str, at: int) -> bool` | 位置 `at` に `sub` があるか |

### 分ける・つなぐ

| 関数 | 説明 |
|---|---|
| `split(s: str, sep: str) -> list[str]` | `sep` で分ける。`sep` が空なら 1 要素 |
| `join(xs: list[str], sep: str) -> str` | `sep` を挟んで連結 |

### 整える

| 関数 | 説明 |
|---|---|
| `strip(s: str) -> str` | 前後の空白を落とす |
| `lpad(s: str, width: int) -> str` | 左に空白を詰めて `width` にする |
| `rpad(s: str, width: int) -> str` | 右に空白を詰める |
| `repeat(s: str, times: int) -> str` | `times` 回くり返す |
| `replace(s: own str, old: str, new: str) -> str` | 全て置換。**`s` の所有権を取ります**（`own`） |

### 判定

| 関数 | 説明 |
|---|---|
| `is_space(c: str) -> bool` | 空白文字か（先頭 1 バイトを見る） |
| `is_space_byte(b: int) -> bool` | バイト値が空白か |

> **⚠️ すべてバイト単位です。** `str` は UTF-8 のバイト列で、
> 文字（コードポイント）単位の操作はありません。`len("あ")` は 3 です。

---

## io

| 関数 | 説明 |
|---|---|
| `read_file(path: str) -> str` | 全部読む。開けなければ実行時エラー |
| `write_file(path: str, text: str) -> None` | 上書きで書く |
| `exists(path: str) -> bool` | あるか |
| `remove(path: str) -> None` | 消す |
| `print_raw(s: str) -> None` | 標準出力へ**改行を足さずに**書く |
| `eprint(s: str) -> None` | 標準エラーへ書く |
| `read_line() -> str \| None` | 標準入力から 1 行。**改行は含まない**。読めなければ `None` |
| `read_all() -> str` | 標準入力を最後まで。何も無ければ空文字列 |

```python
import io

def main() -> int:
    io.write_file("memo.txt", "hello\n")
    print(io.read_file("memo.txt"))
    io.remove("memo.txt")
    return 0
```

### 標準入力

`read_line` は 1 行ずつ読み、**EOF で `None` を返します**。これが繰り返しの終わり方です。

```python
import io

def main() -> int:
    n: int = 0
    line: str | None = io.read_line()
    while line is not None:
        n = n + 1
        print(str(n) + ": " + line)
        line = io.read_line()
    return 0
```

```
$ printf 'hello\nworld\n' | ./count
1: hello
2: world
```

| | |
|---|---|
| 改行 | `read_line` の戻り値に**含まれません** |
| CRLF | 行末の `\r` は落とします（Windows で作った入力でも同じ結果） |
| 最終行 | 改行で終わっていなくても 1 行として読めます |
| 空の入力 | `read_line` は `None`、`read_all` は `""` |

> **⚠️ ベアメタルでは使えません。** 標準入力は `runtime/hosted.c`（PC 用）にしかなく、
> カーネル側（`runtime/core.c` の 4 フック）には含めていません。
> 入力の無い環境に「使わないのに実装させる」ことを避けるためです。

---

## sys

| 関数 | 説明 |
|---|---|
| `argv() -> list[str]` | コマンドライン引数。**`argv()[0]` はプログラム名** |
| `argc() -> int` | その個数 |
| `getenv(name: str) -> str` | 環境変数。**無ければ空文字列**（`None` ではない） |
| `run(cmd: str) -> int` | シェルでコマンドを実行し、終了コードを返す |

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

---

## dict

**鍵は `str`、値は `int`** の固定です。ジェネリクスが無いためで、
これは[将来入れる予定](../design/future-features.md#1-ジェネリクス)です。

| メソッド | 説明 |
|---|---|
| `Dict()` | 空の表を作る |
| `has(k: str) -> bool` | 鍵があるか |
| `get(k: str) -> int` | 値。**無ければ実行時エラー** |
| `get_or(k: str, default: int) -> int` | 値。無ければ `default` |
| `set(mut self, k: own str, v: int) -> None` | 入れる（あれば上書き）。**鍵の所有権を取ります** |
| `len() -> int` | 個数 |
| `keys() -> list[str]` | 鍵の一覧 |

```python
import dict

def main() -> int:
    d: dict.Dict = dict.Dict()
    d.set("apple", 3)
    print(str(d.get_or("apple", 0)))     # → 3
    print(str(d.get_or("none", -1)))     # → -1
    return 0
```

### 値以外のものを入れたいとき

値が `int` に固定されているので、**`list` の添字を値に入れる**のが定石です。

```python
import dict

class Symbol:
    name: str
    value: int
    def init(self, name: own str, value: int) -> None:
        self.name = name
        self.value = value

def main() -> int:
    table: list[Symbol] = []
    syms: dict.Dict = dict.Dict()

    table.append(Symbol("x", 10))
    syms.set("x", len(table) - 1)        # 添字を覚えておく

    i: int = syms.get_or("x", -1)
    print(str(table[i].value))           # → 10
    return 0
```

### 鍵の所有権に注意

`set` は鍵の**所有権を取ります**（`k: own str`）。借りている文字列を渡すときは
`copy` が要ります。

```python
def count(words: list[str]) -> None:
    d: dict.Dict = dict.Dict()
    for w in words:                       # w は借用
        d.set(copy(w), d.get_or(w, 0) + 1)
```

`copy` を書かないと `E-BORROW-1` の警告が出ます。詳しくは
[safety-spec.md](safety-spec.md) を参照してください。
