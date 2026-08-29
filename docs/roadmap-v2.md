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
| [第22章](chapters/ch22-move-check.md) | ムーブ検査（use-after-move） | 新パス `ownck` の新設 | ✅ 完成 |
| [第23章](chapters/ch23-borrow.md) | 借用（保存・返却の禁止） | 借用の寿命規則 | ✅ 完成 |
| [第24章](chapters/ch24-mutability.md) | 可変性 `mut` と借用の衝突 | エイリアス規則 | ✅ 完成 |
| [第25章](chapters/ch25-drop.md) | 解放（drop 挿入） | **初めて free する** | ✅ 完成 |
| [第26章](chapters/ch26-migration.md) | 既存コードの v2 移行 | `lib/` と `selfhost/` を直す | ✅ 完成（第一段） |
| [第27章](chapters/ch27-raises.md) | `raises` / `try` / `except` | エラー処理 | ✅ 完成 |
| [第28章](chapters/ch28-rc.md) | `rc[T]`（共有所有） | 逃げ道の提供 | ✅ 完成 |
| [第29章](chapters/ch29-selfhost-v2.md) | セルフホスト v2 | 不動点の再確立 | 🔨 第一段 完了 |
| [第30章](chapters/ch30-unsafe-ptr.md) | `unsafe` と `ptr[T]` | 低レベル入口 | ✅ 完成 |
| [第31章](chapters/ch31-runtime-split.md) | ランタイム分割と freestanding | libc からの離脱 | ✅ 完成 |
| [第32章](chapters/ch32-baremetal.md) | ベアメタル起動（QEMU） | シリアルに文字を出す | ✅ 完成 |
| [第33章](chapters/ch33-kernel.md) | 最小カーネル | 割り込み（タイマ・UART） | ✅ 完成 |

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

**完了条件**（✅ 2026-08-26 達成）
- `tests/cases/err_move_*.po` が期待どおり落ちる（5 本以上）
- 既存 331 件が緑（警告は出てよい。**出た箇所を ch26 の宿題として記録する**）

**結果**：テストは 344 件（+13）。生成 IR は 1 バイトも変わらず不動点を維持。
`selfhost/` に **196 件**の違反を検出した（`lib/` と `examples/` は 0 件）。
これは第26章の宿題として記録した。既定は警告で、`--deny-move` でエラーに昇格する。
→ [第22章](chapters/ch22-move-check.md)

---

### 第23章 借用（保存・返却の禁止）

**目的**：借用が呼び出しより長生きしないことを保証する（B2）。

- 借用引数をフィールドに保存 → `E-BORROW-3`
- 借用引数を返す → `E-BORROW-4`（`self` 由来は許可。仕様 §4.5）
- 直し方（`own` を付ける）を help に必ず出す

**完了条件**（✅ 2026-08-26 達成）：`err_borrow_*.po` が落ち、既存テストが緑。

**結果**：テストは 354 件（+10）。実装は約 120 行で、**ライフタイム推論は 1 行も要らなかった**。
局所変数への束縛は許し（借用の別名として追跡）、例外は `self` のフィールドの返却だけにした。
`selfhost/` の指摘は 246 件、`lib/` は 2 件（第26章の宿題）。
→ [第23章](chapters/ch23-borrow.md)

---

### 第24章 可変性 `mut`

**目的**：書き換えの許可と、可変借用の排他性（B1 / B3）。

- 読み取り専用の借用への代入・`mut` メソッド呼び出し → `E-MUT-1`
- 1 つの呼び出しで同じ場所を可変＋別の借用に渡す → `E-BORROW-5`
- `--explain-mut`：実引数のうち変更されるものを一覧表示（仕様 §5.3 の補償）

**完了条件**（✅ 2026-08-29 達成）：`err_mut_*.po` が落ち、既存テストが緑。

**結果**：テストは 364 件（+10）。`mut` は「借用の権限」として実装し、局所変数・`own`・
`init` の `self` には要求しない。B1 は設計どおり `place_overlaps` の二重ループ 6 行で済んだ。
`--explain-mut` の形式は `file:line:col: 説明`（D13）。
`selfhost/` の指摘は 400 件、`lib/` は 5 件（第26章の宿題）。
→ [第24章](chapters/ch24-mutability.md)

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

**完了条件**（✅ 2026-08-29 達成）
- `drop_*.po` が解放の順序を `print` で観測でき、期待どおり
- `make drop-asan`（`--drop` の IR を ASan でリンクして実行）で二重解放が無い
  ⚠️ LeakSanitizer は macOS では使えないので、まず「壊れないこと」を確かめる形にした
- `make bootstrap` が緑

**結果**：テストは 371 件（+7）。解放は **`--drop` で opt-in**（D16）にしたので、
既定の IR は 1 バイトも変わらず不動点も維持。drop フラグは持たず、
**スロットに `null` を書く**方式にした（D17。設計 §6.3 を改訂）。
`del` / `copy` / `pop` と式の途中の一時値は次章以降に回した。
→ [第25章](chapters/ch25-drop.md)

---

### 第26章 既存コードの v2 移行

**目的**：ch22〜25 で出た警告を、`lib/` と `selfhost/` から消す。

- 典型パターンごとの直し方は [ownership.md §8](design/ownership.md)
- 直せない箇所が出たら、**仕様（safety-spec.md）を見直す**。コンパイラ自身が仕様の妥当性テスト
- 最後に警告を**エラーへ昇格**する（既定で `--deny-move` 相当）

**完了条件**：警告ゼロで `make test` / `make bootstrap` / `make bootstrap-test` が緑。

**結果**（2026-08-29）：`lib/` と `examples/` は **0 件**（`--drop` 付きで ASan も通過）。
`selfhost/` は 454 → **228 件**（`mut` の移行 154 件が完了）。
`own` を機械的に足すと **229 → 494 件に増えた**ため撤回した——所有権は呼び出し側へ伝播し、
共有グラフでは着地しない。**残りは `rc[T]`（第28章）を入れてから第29章で片付ける。**
仕様も 2 つ直した（D18：フィールドの読み出しは借用／D19：借用はスコープを超えない）。
→ [第26章](chapters/ch26-migration.md)

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

**進捗（2026-08-29）**：**第一段（構文と意味づけの移植）完了。**
`raises` / `try` / `except` / `raise` / `rc[T]` を `selfhost/` に移植し、
C 版と **1 バイト違わない IR** を出せるようになりました（`# STAGE1-SKIP:` は 0 件）。
不動点も維持。残りは 4 段のうちの ②〜④：

| 段 | 内容 | 状態 |
|---|---|---|
| ① | 構文と意味づけの移植（`raises` / `rc[T]`） | ✅ 完了 |
| ② | `selfhost/` 自身を `rc[T]` で移行（228 件） | ⬜ |
| ③ | `ownck` と解放（`--drop`）の移植 | ⬜ |
| ④ | `--deny-*` と `--drop` を既定 on にする | ⬜ |

→ [第29章](chapters/ch29-selfhost-v2.md)

---

## フェーズ D — OS へ

### 第30章 `unsafe` と `ptr[T]` ✅
`unsafe:` ブロック、`ptr[T]`、低レベルの 6 操作（`ptr_at` / `addr_of` /
`peek8` / `peek64` / `poke8` / `poke64`。読み書きは volatile）。
**完了条件**（✅ 達成）：`unsafe` の外でポインタを触ると `E-UNSAFE-1` で落ちる。
→ [第30章](chapters/ch30-unsafe-ptr.md)

### 第31章 ランタイム分割と freestanding ✅
`runtime/core.c` と `runtime/hosted.c` に分割、フック **4 本**（alloc / free / write / panic）、
`pragma target` / `pragma no_runtime`、`-c` / `--target=`。
**完了条件**（✅ 達成）：`-c --target=riscv64-unknown-elf` で ELF のオブジェクトが出る。
**★ プロファイル（hosted / freestanding）は要らなかった**——フックにしたので
切り替えは `no_runtime` 1 つで足りた。
→ [第31章](chapters/ch31-runtime-split.md)

### 第32章 ベアメタル起動 ✅
**ターゲットは RISC-V**（Q6 の決着）。`kernel/`（boot.s / link.ld / hooks.c / kernel.po）、
`make kernel` / `make qemu` / `make qemu-test`。
**完了条件**（✅ 達成）：QEMU のシリアルに Polonium で書いた文字列が出る。
`@naked` / `@section` は要らなかった（起動コードはアセンブリ 1 ファイルで足りた）。
→ [第32章](chapters/ch32-baremetal.md)

### 第33章 最小カーネル ✅
`asm` / `asm_in` / `asm_out`、タイマ割り込み（CLINT）、UART 受信、簡易ヒープ。
GDT / IDT は RISC-V には無い（x86 の都合）。ページングは次の宿題。
**完了条件**（✅ 達成）：タイマ割り込みを受けてカウントし、キー入力をエコーする。
→ [第33章](chapters/ch33-kernel.md)

---

## 進め方の約束（v1 から継続）

1. **仕様 → 設計 → 実装 → テスト → dev-log** の順。コードから書き始めない
2. 1 章 = 1 機能。**章末で必ず `make test` と `make bootstrap` が緑**
3. C 版（`src/`）を先に、Polonium 版（`selfhost/`）を後に
4. 迷った判断は [dev-log.md](dev-log.md) に理由ごと残す
5. 仕様を変えたら、**コードより先に** `docs/spec/` を直す
