# 作業ボード

> **「いま何が終わっていて、次に何をするか」だけを書く場所**です。
> 判断の理由は [dev-log.md](dev-log.md)、決めごとは [spec/](spec/) と [design/](design/) にあります。
> 作業を始めるときは、まずこのファイルを読んでください。

最終更新：2026-08-26（第21章 完了）

---

## 1. いまの状態

| 項目 | 状態 |
|---|---|
| 言語名 / 拡張子 / コマンド | **Polonium** / `.po` / `poloniumc` |
| C 版コンパイラ（stage0） | ✅ 動作（`src/`、約 7,700 行） |
| Polonium 版コンパイラ（stage1〜） | ✅ 動作（`selfhost/`、約 6,600 行） |
| セルフホスト | ✅ 不動点（stage2 == stage3） |
| テスト | ✅ 331 件パス（`make test`） |
| 安全性（v2） | 🔨 進行中 — [第21章](chapters/ch21-v2-foundation.md) 完了（構文のみ。検査はまだ無い） |
| OS 対応 | ⬜ 未着手（要件のみ確定） |

```bash
make            # ビルド
make test       # 全テスト + セルフホスト比較
make bootstrap  # 3 段ビルドと不動点の検証
make info       # 言語名・拡張子などの現在値
```

---

## 2. 次の一手 — 第22章「ムーブ検査」

**目的**：新パス `src/ownck.c` を作り、**移動済みの値を使っていないか**を検出する。
設計は [design/ownership.md §3〜4](design/ownership.md)。**最初は警告**として出す。

- [ ] `src/ownck.h` / `src/ownck.c` を新設し、`src/main.c` から sema の後に呼ぶ
- [ ] `Place`（`Local` / `Field` / `Index` / `Global`）と重なり判定 `place_overlaps` を実装
- [ ] `ty_is_owned(Type*)`（`str` / `list` / class が所有型、`int` / `bool` はコピー型）
- [ ] 移動が起きる場所を実装（代入・`own` 引数・`return`・フィールド代入・`append`）
- [ ] 3 状態（`Valid` / `MaybeMoved` / `Moved`）の格子と結合（`⊔`）
- [ ] 構造化制御フロー上の解析（`if` は分岐して結合、`while` は不動点反復、`break`/`continue`/`return` の経路）
- [ ] `while` の反復が 2 周で収束することを assert する
- [ ] 診断 `E-MOVE-1`（移動した場所と使った場所の 2 か所を指す）
- [ ] `--deny-move` でエラー昇格。**既定は警告**（既存コードを止めない）
- [ ] `tests/cases/err_move_*.po` を 5 本以上追加
- [ ] 既存 331 件が緑。**警告が出た箇所は第26章の宿題として記録する**
- [ ] `selfhost/ownck.po` への移植は**第29章**（C 版が固まってから）
- [ ] `docs/chapters/ch22-*.md` / `dev-log.md` / このファイルを更新

**⚠️ 注意**：この章で初めて「既存コードに警告が出る」状態になります。
**警告を消すために仕様を緩めないこと。** 記録して第26章で直します。

**先に決めること**：Q1（`copy(x)` を組み込み関数にするか、`x.copy()` にするか）。

---

## 3. 章ごとのチェックリスト（毎章これを使う）

- [ ] 仕様（`docs/spec/`）にこの章で入れる機能が書いてある。書いていなければ**先に書く**
- [ ] 設計（`docs/design/`）に「どう実装するか」がある
- [ ] C 版（`src/`）を実装
- [ ] テストを追加（**通るべきもの**と**落ちるべきもの**の両方）
- [ ] `make test` 全件パス
- [ ] Polonium 版（`selfhost/`）に移植
- [ ] `make selfhost-test`（トークン／AST／診断／IR の一致）
- [ ] `make bootstrap`（不動点）と `make bootstrap-test`
- [ ] `docs/chapters/chNN-*.md` を書く（読む → 手を動かす → テストが通る）
- [ ] `docs/dev-log.md` に判断・つまずきを記録
- [ ] `docs/roadmap-v2.md` の進捗表を更新し、このファイルの「次の一手」を書き換える

---

## 4. 決まっていること（この先ぶれさせない）

| # | 決定 | 決めた日 | 根拠 |
|---|---|---|---|
| D1 | 安全性は**所有権＋借用**（GC も参照カウント既定も採らない） | 2026-08-26 | OS 開発が最終目標。[safety-spec.md](spec/safety-spec.md) |
| D2 | **ライフタイム注釈は導入しない**。借用は呼び出しより長生きしない | 2026-08-26 | Python の書きやすさを優先。[safety-spec.md §4.4](spec/safety-spec.md) |
| D3 | 借用・`&`・`let mut`・`clone()` は**書かせない**。書かせるのは `own` / `mut` / `raises` の 3 つだけ | 2026-08-26 | 同上 |
| D4 | エラー処理は **`try`/`except` の見た目 + 戻り値検査**（アンワインドしない） | 2026-08-26 | OS で使えること。[error-handling.md](design/error-handling.md) |
| D5 | OS 向け機能は**仕様を先に固め、実装は ch30 以降** | 2026-08-26 | 設計の手戻り防止。[os-support.md](design/os-support.md) |
| D6 | 言語名に依存する文字列は 3 か所に集約（`Makefile` / `src/langinfo.h` / `selfhost/langinfo.po`） | 2026-08-26 | 改名の可能性。[naming.md](design/naming.md) |
| D7 | 内部接頭辞は名前非依存（`PLC_` / `pl_`） | 2026-08-26 | 同上 |

---

## 5. 未決（着手前に決めること）

| # | 論点 | いつ決めるか |
|---|---|---|
| Q1 | `copy(x)` を組み込み関数にするか、メソッド `x.copy()` にするか | **ch22（次章。着手前に決める）** |
| Q2 | `for x in xs:` で要素を `own` として取り出す構文（`for own x in xs:`？） | ch23 |
| Q3 | `del x` の対象を局所変数だけに限るか、フィールドも許すか | ch25 |
| Q4 | `rc[T]` の `with` 構文を借用以外にも使うか（ファイルなど） | ch28 |
| Q5 | ジェネリクスを入れずに `Result` 相当をどこまで一般化できるか | ch27 |
| Q6 | ベアメタルのターゲットを x86_64 と RISC-V のどちらから始めるか | ch32 |

---

## 6. 完了したこと（新しい順）

| 日付 | 内容 |
|---|---|
| 2026-08-26 | **Mython → Polonium に改名**（`.my` → `.po`、`mythonc` → `poloniumc`）。内部接頭辞を名前非依存化（`MYTHON_` → `PLC_`、`my_` → `pl_`）。`langinfo` を新設し名前の定義を 3 か所に集約。v2 の仕様・設計・ロードマップを作成 |
| 2026-08-26 | **第21章 完了** — キーワード 5 語（`own` / `mut` / `raises` / `unsafe` / `pragma`）を予約し、`x: own T` / `x: mut T` / `mut self` を構文として解析。`ParamMode` は型ではなく引数（`ND_PARAM`）に持たせた。**意味づけは無し・IR は不変**。テスト 331 件（+8）、不動点維持 |
| 2026-08-26 | v1 完成（第20章）。セルフホスト不動点・323 テスト |
