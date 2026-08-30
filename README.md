# Polonium

**Python の書きやすさのまま、Rust の安全性を手に入れる**ことを目指すプログラミング言語と、
その処理系（C 言語 + LLVM の自作コンパイラ）です。最終的に **OS を書けること**を目標にしています。

```python
# examples/fizzbuzz.po
def main() -> int:
    for i in range(1, 16):
        if i % 15 == 0:
            print("FizzBuzz")
        elif i % 3 == 0:
            print("Fizz")
        elif i % 5 == 0:
            print("Buzz")
        else:
            print(str(i))
    return 0
```

| | |
|---|---|
| 拡張子 | `.po` |
| コンパイラ | `poloniumc`（C 実装 → **セルフホスト済み**） |
| バックエンド | LLVM IR を直接出力（テキスト） |
| 型付け | 静的・型注釈必須・実行時型情報なし |
| 現在地 | v2（安全性・エラー処理・共有所有）実装済み／**RISC-V のベアメタルで動作**・388 テスト |

---

## 使ってみる（Linux / macOS / Windows）

### 必要なもの

**clang だけです。** このコンパイラは LLVM IR のテキストを出力し、
アセンブルとリンクを clang に任せる作りなので、clang があればどの OS でも同じように動きます。

| OS | 入れるもの |
|---|---|
| **Linux** | `sudo apt install clang llvm make`（Debian / Ubuntu）<br>`sudo dnf install clang llvm make`（Fedora） |
| **macOS** | `xcode-select --install`（Apple clang で足ります） |
| **Windows** | [MSYS2](https://www.msys2.org/) を入れて、MINGW64 シェルで<br>`pacman -S mingw-w64-x86_64-clang mingw-w64-x86_64-lld make diffutils grep coreutils` |

> **⚠️ Windows は MSYS2（または WSL）の上で使ってください。**
> テストとビルドが bash と make に依存しているためです。
> WSL を使う場合は「Linux」の手順がそのまま使えます。

### A. 配布物をダウンロードして使う（ビルド不要）

[Releases](https://github.com/shota-iwamoto/polonium/releases) から
OS に合う `.tar.gz` を取って展開します。

| ファイル | 対象 |
|---|---|
| `polonium-linux-x86_64.tar.gz` | Linux（x86_64） |
| `polonium-macos-universal.tar.gz` | macOS（**Intel / Apple Silicon 両対応**） |
| `polonium-windows-x86_64.tar.gz` | Windows（MSYS2 / MINGW64） |

```bash
tar xzf polonium-linux-x86_64.tar.gz
cd polonium-*

printf 'def main() -> int:\n    print("hello")\n    return 0\n' > hello.po
./bin/poloniumc hello.po -o hello
./hello                      # → hello
```

中身はこの 3 つだけです。**展開した場所がどこでも動きます**
（コンパイラが実行ファイルからの相対で標準ライブラリを探すため）。

```
bin/poloniumc
lib/polonium/runtime.a
lib/polonium/lib/*.po        ← 標準ライブラリ（文字列・入出力・数学・線形代数ほか）
```

### B. ソースからビルドする

```bash
git clone https://github.com/shota-iwamoto/polonium.git
cd polonium
make                         # → build/poloniumc

./build/poloniumc examples/wordcount.po -o wc
./wc examples/sample.txt
```

### C. インストールする（PATH に置く）

```bash
sudo make install            # 既定は /usr/local
make install PREFIX=$HOME/.local   # 自分の環境だけに入れるなら

poloniumc hello.po -o hello  # どこからでも呼べる
poloniumc --version          # → poloniumc 0.2.0 (stage0)
```

### よく使うコマンド

```bash
make test                  # 全テスト + 解放の検査 + セルフホスト比較
make bootstrap             # 3 段ビルドと不動点の検証（stage2 == stage3）
make dist                  # 配布用のディレクトリを build/dist に作る
make qemu-test             # ベアメタル（RISC-V）の検証（第32〜33章）
make info                  # 使っている clang・triple などの現在値
```

環境変数で差し替えられます。

| 変数 | 用途 |
|---|---|
| `PLC_CLANG` | 使う clang（`clang-18` など名前が違うとき） |
| `PLC_RUNTIME_O` / `PLC_LIB_DIR` | ランタイムと標準ライブラリの場所 |
| `make CC=gcc` | コンパイラ本体のビルドに使う C コンパイラ |

---

## ドキュメント

**言語の使い方を知りたい方は [docs/tutorial.md](docs/tutorial.md)（Polonium 入門）から。**
30 分で読み切れます。

| | |
|---|---|
| [docs/README.md](docs/README.md) | ドキュメントの入口（全体の地図） |
| [docs/tutorial.md](docs/tutorial.md) | **入門** — 言語の使い方 |
| [docs/reference/cli.md](docs/reference/cli.md) | `poloniumc` のオプション |
| [docs/spec/](docs/spec/) | **言語仕様** — 構文・型・安全性・標準ライブラリ |
| [docs/design/](docs/design/) | **処理系の設計** — パス構成・IR 規約・所有権検査 |
| [docs/roadmap.md](docs/roadmap.md) | 到達点と、これから入れるもの |
| [old/](old/) | 処理系を作っていく過程の記録（旧資料） |

---

## 安全性の考え方

Rust の保証（use-after-free / 二重解放 / データ競合 / null の排除）を入れつつ、
**Rust の記法は持ち込みません**。書かせるのは `own` / `mut` / `raises` の 3 つだけです。

```python
def total(xs: list[int]) -> int:        # 引数は既定で「借用」。&Vec<i64> とは書かない
    s: int = 0
    for x in xs:
        s = s + x
    return s

def store(self, name: own str) -> None:  # 保存するときだけ own を書く
    self.name = name

def read_config(path: str) -> Config raises IOError:   # 失敗は型で宣言する
    ...
```

- ライフタイム注釈（`'a`）は**ありません** — 借用は呼び出しより長生きしない、という規則で代用します
- 例外はアンワインドしません — `try` / `except` は戻り値検査に落ちるので、**カーネルでも使えます**

---

## ディレクトリ

```
src/        C 版コンパイラ（stage0）
selfhost/   Polonium 版コンパイラ（stage1 以降）
runtime/    C 製ランタイム（core = libc 非依存／hosted = PC 用）
lib/        Polonium 製の標準ライブラリ（数値計算を含む。libm は使わない）
kernel/     ベアメタル（RISC-V）のカーネル（第32〜33章）
tests/      テストケースとテストランナー
docs/       言語仕様・処理系の設計・入門
old/        処理系を作っていく過程の記録（旧資料）
```

## ベアメタルで動かす（RISC-V）

```bash
brew install llvm riscv64-elf-binutils qemu   # 必要な道具
make qemu                                     # QEMU で起動（Ctrl-A X で終了）
make qemu-test                                # 出力を自動で検証
```

```
=================================
 Polonium kernel on RISC-V (virt)
=================================
1 から 10 までの合計: 55
tick 1
tick 2
tick 3
3 回割り込みが来ました
```

**カーネル本体（`kernel/kernel.po`）に `unsafe` は 2 か所だけ**です。
`print` も `for` も `list[str]` も、PC 上とまったく同じように書けます。

**言語名が変わるときは** [docs/design/naming.md](docs/design/naming.md) の手順に従ってください
（書き換えるのは `Makefile` / `src/langinfo.h` / `selfhost/langinfo.po` の 3 か所です）。
