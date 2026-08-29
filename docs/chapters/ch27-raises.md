# 第27章 `raises` / `try` / `except`

> **この章のゴール**
> 失敗を型で宣言し、**握りつぶせなくする**。ただし**アンワインドはしない**。
>
> ```bash
> $ cat t.po
> class IOError:
>     message: str
>     def init(self, message: own str) -> None:
>         self.message = message
>
> def read(path: str) -> int raises IOError:
>     if len(path) == 0:
>         raise IOError("空のパス")
>     return len(path)
>
> def main() -> int:
>     try:
>         print(str(read("")))
>     except IOError as e:
>         print("caught: " + e.message)
>     return 0
> $ ./build/poloniumc t.po -o t && ./t
> caught: 空のパス
> ```

仕様 v2 の保証 **S8（エラーの握りつぶしが無い）** を担当する章です。
見た目は Python の例外ですが、**実体は戻り値の検査**です
（[design/error-handling.md](../design/error-handling.md)）。

**★ だからカーネルでも使えます。** アンワインド機構（landing pad・DWARF テーブル・
`_Unwind_RaiseException`）は libc とランタイムに強く依存していて、OS を書くときに
自分で用意できません。**最終目標から逆算して、実装方式を決めています。**

---

## 目次

- [27.1 何を足すか](#271-何を足すか)
- [27.2 仕様に穴があった：`raise` が無い](#272-仕様に穴があったraise-が無い)
- [27.3 ABI：エラーは「出力引数」で返す](#273-abiエラーは出力引数で返す)
- [27.4 呼び出しの展開](#274-呼び出しの展開)
- [27.5 `try` / `except` の振り分け](#275-try--except-の振り分け)
- [27.6 握りつぶせないことを保証する](#276-握りつぶせないことを保証する)
- [27.7 つまずき：`|` は文脈で意味が変わる](#277-つまずきは文脈で意味が変わる)
- [27.8 解放（第25章）との噛み合わせ](#278-解放第25章との噛み合わせ)
- [27.9 動作確認](#279-動作確認)
- [27.10 まとめと次章の予告](#2710-まとめと次章の予告)

---

## 27.1 何を足すか

| # | 足すもの | 触るファイル |
|---|---|---|
| ① | `raise` キーワードと `ND_TRY` / `ND_EXCEPT` / `ND_RAISE` | `src/lexer.c`, `src/ast.h`, `src/ast.c` |
| ② | `raises` 節・`try` / `except` / `raise` の構文解析 | `src/parser.c` |
| ③ | エラー型の ID 割り当てと 4 つの検査（R1 / R2 / R4 / R5） | `src/sema.c` |
| ④ | エラー出力引数・分岐の挿入・振り分け | `src/codegen.c` |
| ⑤ | 所有権解析の対応（`try` の合流、`raise` は `return` と同じ移動） | `src/ownck.c` |
| ⑥ | テスト 10 本 | `tests/cases/raises_*.po` ほか |

---

## 27.2 仕様に穴があった：`raise` が無い

仕様 §8 を読み直して、実装を始める前に気づきました。

```python
def read_config(path: str) -> Config raises IOError:
    text: str = io.read_file(path)      # io.read_file も raises IOError
    return parse(text)                   # ← 失敗すればここで自動的に返る
```

**⚠️ 伝播する側しか書かれていません。** これでは**誰もエラーを起こせません**。
一番下（`io.read_file`）が何をするのかが定義されていない、という穴です。

**★ `raise` 文を足し、仕様（§8.1.1 と §11 の文法）に書きました。**

```python
    if len(path) == 0:
        raise IOError("空のパスです")
```

**🤔 なぜ気づけなかったのか**：仕様を書いたとき、頭にあったのは
「Rust の `?` に相当するものをどう書かせるか」でした。`Result` を**作る**側は
Rust なら `Err(...)` を返すだけなので、**構文を足す必要があるという発想が無かった**のです。
**実装は仕様のレビューとして働きます。**

---

## 27.3 ABI：エラーは「出力引数」で返す

```python
def read(path: str) -> int raises IOError
```

```llvm
%pl.err = type { i64, ptr }              ; { タグ, エラーオブジェクト }

define i64 @t.read(ptr %path.arg, ptr %err.out)
```

| 状況 | `%err.out` の中身 | 戻り値 |
|---|---|---|
| 成功 | `tag = 0` | 本来の値 |
| 失敗 | `tag = <型 ID>`, `payload = エラー` | 型ごとの既定値（**使われない**） |

**🤔 なぜ複合戻り値（`{i64, ptr}` を返す）にしないのか**

既存の codegen は「1 つの値を返す」前提です。複合戻り値にすると、
呼び出し・戻り値・`main` のラッパまで全部が影響を受けます。
**出力引数なら、増えるのは引数 1 本だけ**で、既存の生成規則がそのまま使えます。

エラー型の ID は**プログラム全体で一意な整数**で、`0` は「エラー無し」に予約します。
割り当ての規則は**仕様として固定**してあります（設計 §4）。

> モジュールを依存順に、モジュール内は**出現順**に、`1` から連番。

**⚠️ stage0 と stage1 で番号が食い違うと IR がバイト単位で一致しません。**
「どちらの実装でも同じ結果になる規則」を先に決めておくのが、セルフホストの作法です。

---

## 27.4 呼び出しの展開

失敗しうる呼び出しは、**呼んだ直後にタグを見る**だけです。

```llvm
  %t.0 = getelementptr %pl.err, ptr %err.slot, i32 0, i32 0
  store i64 0, ptr %t.0                       ; ① タグを 0 に
  %t.1 = call i64 @t.read(ptr %t.2, ptr %err.slot)   ; ② スロットを渡す
  %t.3 = getelementptr %pl.err, ptr %err.slot, i32 0, i32 0
  %t.4 = load i64, ptr %t.3
  %t.5 = icmp ne i64 %t.4, 0                  ; ③ 失敗したか
  br i1 %t.5, label %try.dispatch.0, label %call.ok.1
call.ok.1:
  ...
```

**★ 飛び先は 2 通りだけです。**

| いる場所 | 失敗したときの飛び先 |
|---|---|
| `try` の中 | その `try` の振り分け（`try.dispatch.N`） |
| `try` の外（`raises` を宣言した関数） | 伝播ブロック（`err.propagate`） |
| `try` の外（宣言していない関数） | **到達しない**（sema が禁止済み → `unreachable`） |

伝播ブロックは関数ごとに 1 つで、**使われたときだけ**出します。

```llvm
err.propagate:
  %t.9 = load %pl.err, ptr %err.slot
  store %pl.err %t.9, ptr %err.out           ; そのまま呼び出し元へ
  ret i64 0                                   ; 値は使われない
```

**★ エラースロットは関数に 1 つで足ります。** 呼んだ直後に必ず見るので、
2 つの失敗が同時に生きていることはありません。

---

## 27.5 `try` / `except` の振り分け

```llvm
try.dispatch.0:
  %tag = load i64, ptr %err.slot
  %c0 = icmp eq i64 %tag, 1
  br i1 %c0, label %except.0.0, label %try.next.0.0     ; IOError か
try.next.0.0:
  %c1 = icmp eq i64 %tag, 2
  br i1 %c1, label %except.0.1, label %try.next.0.1     ; ParseError か
try.next.0.1:
  br label %err.propagate        ; ★ どれでもなければ外側へ渡す
```

**★ 「外側へ渡す」があるので、入れ子の `try` が自然に動きます。**
内側が捕まえない型は、内側の振り分けの最後から外側の振り分けへ落ちていきます。
`except ... as e` は、payload をふつうの局所変数に入れるだけです。

`raise` も同じ仕組みに乗ります。**同じ関数の中の `try` が捕まえるなら、
呼び出し元へ戻らずにその振り分けへ飛びます**（Python と同じ挙動）。

---

## 27.6 握りつぶせないことを保証する

検査は sema にあります。中心は 1 つだけです。

```c
static void check_can_fail(Sema *s, Node *n, FuncSig *f, const char *shown) {
    if (f->nraises == 0) return;
    n->can_fail = true;                       // ★ codegen はこれを見て分岐を挿す
    for (int i = 0; i < f->nraises; i++) {
        Class *ec = f->raises[i];
        if (try_catches(s, ec)) continue;     // try で捕まえる
        if (func_declares(s->cur_func, ec)) continue;  // 自分も raises に書いている
        ... E-RAISE-1 / E-RAISE-2 ...
    }
}
```

**受け止め方は 2 つだけ**（捕まえるか、宣言するか）なので、検査もこの 2 行です。

```
error[E-RAISE-1]: 失敗しうる呼び出し 'read' を処理していません
    = ヒント: try で捕まえるか、この関数に 'raises IOError' を足してください
```

| コード | いつ | 直し方 |
|---|---|---|
| `E-RAISE-1` | 宣言していない関数で失敗しうる呼び出しをした | `try` か `raises` |
| `E-RAISE-2` | 呼び出し先のエラーが自分の `raises` に無い | `raises` に足す |
| `E-RAISE-4` | `main` に `raises` がある | 中で処理する |
| `E-RAISE-5` | `try` の中に失敗しうる呼び出しが無い（**警告**） | `try` を消す |

**★ `E-RAISE-3`（except の網羅性）は要りませんでした。**
「捕まえていない型は、宣言していなければ呼び出しの時点でエラー」なので、
**網羅していないことは自動的に別のエラーとして現れます**。
検査を 1 つ減らせるうえに、**エラーの位置が「網羅していない try」ではなく
「処理されない呼び出し」になる**ぶん、読み手にとって正確です。

---

## 27.7 つまずき：`|` は文脈で意味が変わる

```python
def load(path: str) -> int raises IOError | ParseError:
```

最初の実装は、`raises` の型を既存の `type_ref()` で読んでいました。結果：

```
error: '| None' の形で書いてください
    = ヒント: 型の '|' の後ろに書けるのは None だけです（共用体型はありません）
```

`type_ref()` は第15章で `T | None` を読むように作ってあります。
**同じ `|` でも、型注釈の中と `raises` の中では意味が違います。**

```c
// raises に書けるのは「エラー型の名前」だけ（第27章）。
// ⚠️ type_ref は使えません。`raises A | B` の '|' を
//    「T | None」の '|' と読んでしまうからです。
static Node *raises_type(Parser *p) { ... }
```

**★ 第15章で「型の `|` と式の `|` は、読む関数が別だから迷わない」と書きました。**
その理屈がそのまま今回にも当てはまります——**文脈ごとに読む関数を分ける**。
20 行の小さな関数を足すだけで済みました。

---

## 27.8 解放（第25章）との噛み合わせ

失敗して抜ける経路でも、**スコープの所有値は解放しなければなりません**。

```python
def work(fail: bool) -> int raises IOError:
    r: Res = Res("in-work")
    if fail:
        raise IOError("失敗")     # ← ここで r を解放してから戻る
    return 1
```

```
$ ./build/poloniumc --drop t.po -o t && ./t
drop in-work          ← 失敗の経路でもちゃんと解放される
caught: 失敗
```

分岐の「辺」には命令を置けないので、**失敗用のブロックを 1 つ挟んで**そこで解放します。

```c
        if (e->drop) {
            emit_cond_br(e, bad, fail_l, ok_l);
            emit_label(e, fail_l);
            emit_drops_until(e, e->try_ctx ? e->try_ctx->scope : NULL);
            emit_fail_br(e);
        }
```

**★ 第25章で `emit_drops_until(スコープ)` という形にしておいたので、
`return` と `break` に続いて 3 つ目の出口が増えても、呼ぶだけで済みました。**

所有権解析（ownck）にも `try` / `raise` を教えます。
`try` の本体は途中で抜けることがあるので、**`except` の入口は「try に入る前の状態」**
から始めます（分からないものは安全側に倒す）。

---

## 27.9 動作確認

✍️ テストを 10 本足します。

| ファイル | 確認すること |
|---|---|
| `raises_basic.po` | 成功の経路と、捕まえる経路 |
| `raises_propagate.po` | `raises` 関数から `raises` 関数を呼ぶと自動で伝播する |
| `raises_multi.po` | `raises A | B` でタグどおりの `except` が選ばれる |
| `raises_nested_try.po` | 内側が捕まえない型は外側へ渡る |
| `drop_raises.po` | **失敗の経路でも解放される**（`make drop-asan` でも検査） |
| `err_raises_unhandled.po` | `E-RAISE-1`（握りつぶせない） |
| `err_raises_undeclared.po` | `E-RAISE-2` |
| `err_raises_main.po` | `E-RAISE-4` |
| `err_raises_not_class.po` | エラーはクラスだけ |
| `warn_raises_useless_try.po` | `E-RAISE-5`（警告） |

**⚠️ stage1（Polonium 版）はまだ `raises` を読めません。**
そこで `tests/selfhost.sh` に `# STAGE1-SKIP:` の印を足し、
**その印が付いたケースだけ比較を飛ばす**ようにしました。第29章で移植したら外します。

```bash
$ make test
全 382 件パス
全 9 件が AddressSanitizer で問題なし
（10 件スキップ：C 版にしかない機能）

$ make bootstrap
★ 不動点に到達しました（stage2 == stage3）
```

---

## 27.10 まとめと次章の予告

この章でやったこと：

- **仕様の穴（`raise` が無い）を実装が見つけ、仕様に足した**
- エラーを「出力引数 + タグ」で返す ABI にした（**アンワインドしない**）
- 失敗しうる呼び出しの直後にタグ検査を挿し、飛び先を 2 通りに絞った
- `try` の振り分けを「当たらなければ外側へ」で作り、**入れ子が自然に動く**ようにした
- 握りつぶせないことを 2 行の検査で保証し、**`E-RAISE-3` を不要にした**
- `|` の意味が文脈で変わることに気づき、読む関数を分けた
- 失敗の経路でも解放が走るようにした（第25章の道具がそのまま使えた）

次章（第28章）は **`rc[T]`**——所有者を 1 つに決められないデータのための逃げ道です。
第26章で残した `selfhost/` の 228 件は、**すべてこれを待っています**。

**★ そして第29章で、`ownck` と解放と `raises` を Polonium 版へ移植し、
不動点を取り戻します。** そこが v2 の完成です。
