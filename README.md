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

## ビルドと実行

```bash
make                       # コンパイラをビルド（build/poloniumc）
./build/poloniumc examples/wordcount.po -o wc && ./wc examples/sample.txt

make test                  # 全テスト + セルフホスト比較
make bootstrap             # 3 段ビルドと不動点の検証（stage2 == stage3）
make info                  # 言語名・拡張子などの現在値
```

必要なもの：clang、LLVM ツール（`opt` / `lli`）、make。

---

## ドキュメント

**すべての判断は `docs/` にあります。コードより先にドキュメントを読んでください。**

| | |
|---|---|
| [docs/tasks.md](docs/tasks.md) | **作業ボード** — いまの状態と次の一手 |
| [docs/README.md](docs/README.md) | ドキュメントの入口（全体の地図） |
| [docs/spec/language-spec.md](docs/spec/language-spec.md) | 言語仕様 v1 |
| [docs/spec/safety-spec.md](docs/spec/safety-spec.md) | **言語仕様 v2 — 安全性モデル**（所有権・借用・エラー処理） |
| [docs/roadmap-v2.md](docs/roadmap-v2.md) | 第21〜33章のロードマップ（安全性 → セルフホスト v2 → OS） |
| [docs/dev-log.md](docs/dev-log.md) | 開発ログ（判断の理由とつまずき） |

---

## v2 でやろうとしていること

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
lib/        Polonium 製の標準ライブラリ
kernel/     ベアメタル（RISC-V）のカーネル（第32〜33章）
tests/      テストケースとテストランナー
docs/       仕様・設計・章・開発ログ
```

## ベアメタルで動かす（第32〜33章）

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
