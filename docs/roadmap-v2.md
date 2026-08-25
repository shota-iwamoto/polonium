# ロードマップ v2 — 安全性と OS へ（第21章〜）

> v1（第1〜20章）で「Python の文法・静的型・セルフホスト」は完成しました。
> v2 は **Rust の安全性**を入れ、その先で **OS を書ける**ところまで進みます。
>
> 仕様は [spec/safety-spec.md](spec/safety-spec.md)、設計は
> [design/ownership.md](design/ownership.md) / [design/error-handling.md](design/error-handling.md) /
> [design/os-support.md](design/os-support.md)。**この 4 つが実装の唯一の正解**です。

---

## 全体像

```
  フェーズ A  安全性の土台        ch21 ─ ch26   所有権・借用・可変性・解放
  フェーズ B  エラー処理          ch27 ─ ch28   raises / try / except、rc[T]
  フェーズ C  セルフホスト v2      ch29          Polonium 版コンパイラを v2 仕様に
  フェーズ D  OS へ               ch30 ─ ch33   unsafe・freestanding・ベアメタル・カーネル
```

| 章 | タイトル | 主眼 | 状態 |
|---|---|---|---|
| [第21章](chapters/ch21-v2-foundation.md) | v2 の土台（キーワードと `own` / `mut` の構文） | 構文だけ通す。検査はしない | ✅ 完成 |
| [第22章] | ムーブ検査（use-after-move） | 新パス `ownck` の新設 | 未着手 |
| [第23章] | 借用（保存・返却の禁止） | 借用の寿命規則 | 未着手 |
| [第24章] | 可変性 `mut` と借用の衝突 | エイリアス規則 | 未着手 |
| [第25章] | 解放（drop 挿入） | **初めて free する** | 未着手 |
| [第26章] | 既存コードの v2 移行 | `lib/` と `selfhost/` を直す | 未着手 |
| [第27章] | `raises` / `try` / `except` | エラー処理 | 未着手 |
| [第28章] | `rc[T]`（共有所有） | 逃げ道の提供 | 未着手 |
| [第29章] | セルフホスト v2 | 不動点の再確立 | 未着手 |
| [第30章] | `unsafe` と `ptr[T]` | 低レベル入口 | 未着手 |
| [第31章] | ランタイム分割と freestanding | libc からの離脱 | 未着手 |
| [第32章] | ベアメタル起動（QEMU） | 画面に文字を出す | 未着手 |
| [第33章] | 最小カーネル | 割り込み・ページング | 未着手 |

---

## フェーズ A — 安全性の土台

### 第21章 v2 の土台

**目的**：新しいキーワードを字句・構文・AST に通す。**意味は与えない**。

- 追加キーワード：`own`, `mut`, `raises`, `try`, `except`, `unsafe`, `pragma`, `del`, `with`
- `ND_PARAM` に `ParamMode`（`PM_BORROW` / `PM_MUT` / `PM_OWN`）
- `mut self` を構文として受ける
- 既存コードで新キーワードを識別子に使っている箇所を洗い出して直す

**完了条件**（✅ 2026-08-26 達成）
- `def f(x: own list[int], y: mut list[int]) -> None:` が**構文解析を通り**、`--dump-ast` に mode が出る
- `make test` 全件・`make bootstrap` が緑（**動作は v1 のまま**）

**結果**：キーワードは 5 語追加で済んだ（`del` / `try` / `except` / `with` は v1 で予約済み）。
既存識別子との衝突はゼロ。テストは 331 件（+8）、IR は 1 バイトも変わらず不動点を維持。
→ [第21章](chapters/ch21-v2-foundation.md)

---

### 第22章 ムーブ検査

**目的**：`src/ownck.c` を新設し、use-after-move を検出する。

- 場所（Place）の表現と重なり判定（[ownership.md §3](design/ownership.md)）
- 3 状態の格子と、構造化制御フロー上のデータフロー解析（§4）
- **最初は警告**として出す（`--deny-move` で エラーに昇格）

**完了条件**
- `tests/cases/err_move_*.po` が期待どおり落ちる（5 本以上）
- 既存 323 件が緑（警告は出てよい。**出た箇所を ch26 の宿題として記録する**）

---

### 第23章 借用（保存・返却の禁止）

**目的**：借用が呼び出しより長生きしないことを保証する（B2）。

- 借用引数をフィールドに保存 → `E-BORROW-3`
- 借用引数を返す → `E-BORROW-4`（`self` 由来は許可。仕様 §4.5）
- 直し方（`own` を付ける）を help に必ず出す

**完了条件**：`err_borrow_*.po` が落ち、既存テストが緑。

---

### 第24章 可変性 `mut`

**目的**：書き換えの許可と、可変借用の排他性（B1 / B3）。

- 読み取り専用の借用への代入・`mut` メソッド呼び出し → `E-MUT-1`
- 1 つの呼び出しで同じ場所を可変＋別の借用に渡す → `E-BORROW-5`
- `--explain-mut`：実引数のうち変更されるものを一覧表示（仕様 §5.3 の補償）

**完了条件**：`err_mut_*.po` が落ち、既存テストが緑。

---

### 第25章 解放（drop 挿入）

**目的**：**v1 で諦めていた解放を実装する**。ここが v2 の山場です。

- スコープ出口・早期 return・break/continue・エラー伝播の全経路に drop を挿入
- クラスごとの `@drop.C` 生成、`drop` メソッド（デストラクタ）
- `MaybeMoved` のための drop フラグ（`alloca i1`）
- 一時値の drop（文の終わり）
- `del x`（早期解放）
- `runtime/` に `pl_drop_*` を追加（**core 側**。[os-support.md §4](design/os-support.md)）
- [design/memory-model.md](design/memory-model.md) を改訂（「解放しない」→「所有者が解放する」）

**完了条件**
- `drop_*.po` が解放の順序を `print` で観測でき、期待どおり
- `make asan`（Leak 検査を有効化）で既存テストがリーク無しで通る
- `make bootstrap` が緑

---

### 第26章 既存コードの v2 移行

**目的**：ch22〜25 で出た警告を、`lib/` と `selfhost/` から消す。

- 典型パターンごとの直し方は [ownership.md §8](design/ownership.md)
- 直せない箇所が出たら、**仕様（safety-spec.md）を見直す**。コンパイラ自身が仕様の妥当性テスト
- 最後に警告を**エラーへ昇格**する（既定で `--deny-move` 相当）

**完了条件**：警告ゼロで `make test` / `make bootstrap` / `make bootstrap-test` が緑。

---

## フェーズ B — エラー処理

### 第27章 `raises` / `try` / `except`

**目的**：握りつぶせないエラー処理（[error-handling.md](design/error-handling.md)）。

- エラー出力引数の ABI、伝播分岐、`try` のディスパッチ
- エラー型 ID の決定規則（stage0 と stage1 で一致させる）
- 検査 R1〜R6
- `lib/io.po` を `raises IOError` に直す → `selfhost/` の呼び出し側を直す

**完了条件**：`raise_*.po` / `err_raise_*.po` が期待どおり、`make bootstrap` が緑。

### 第28章 `rc[T]`

**目的**：所有権で書けない構造のための逃げ道。

- ヒープ表現（strong / borrow カウント）、`retain` / `release`
- `with x.borrow() as v:` 構文（`with` はここで導入）
- 実行時借用違反で panic

**完了条件**：双方向リストのテストが動き、リーク検査が通る。

---

## フェーズ C — セルフホスト v2

### 第29章 セルフホスト v2

**目的**：Polonium 版コンパイラを **v2 の機能を持つ**コンパイラにする。

- `selfhost/ownck.po` を追加（C 版 `src/ownck.c` の移植）
- `selfhost/` 側にも `raises` / drop 生成を実装
- **stage2 == stage3 の不動点を再確立**

**完了条件**：`make bootstrap` と `make bootstrap-test` が緑。
**★ ここで「Rust の安全性を持つ言語が、自分自身をその安全性の下でコンパイルできる」状態になります。**

---

## フェーズ D — OS へ

### 第30章 `unsafe` と `ptr[T]`
`unsafe:` ブロック、`ptr[T]`、`volatile_load/store`、`transmute`、`extern` の拡張。
**完了条件**：`unsafe` の外でポインタを触ると落ちる。

### 第31章 ランタイム分割と freestanding
`runtime/core.c` と `runtime/hosted.c` に分割、フック 3 本（alloc / free / panic）、
`pragma profile freestanding`、`pragma target`、`-c`（オブジェクト出力）。
**完了条件**：`-nostdlib` でリンクできる `.o` が出る。

### 第32章 ベアメタル起動
`@naked` / `asm` / `@section` / `@static`、リンカスクリプト、QEMU テストランナー。
**完了条件**：QEMU のシリアルに Polonium で書いた文字列が出る。

### 第33章 最小カーネル
GDT / IDT / 割り込み・タイマ・キーボード・ページング・簡易ヒープ（`pl_hook_alloc` の実装）。
**完了条件**：キー入力をエコーする常駐カーネルが動く。

---

## 進め方の約束（v1 から継続）

1. **仕様 → 設計 → 実装 → テスト → dev-log** の順。コードから書き始めない
2. 1 章 = 1 機能。**章末で必ず `make test` と `make bootstrap` が緑**
3. C 版（`src/`）を先に、Polonium 版（`selfhost/`）を後に
4. 迷った判断は [dev-log.md](dev-log.md) に理由ごと残す
5. 仕様を変えたら、**コードより先に** `docs/spec/` を直す
