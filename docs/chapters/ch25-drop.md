# 第25章 解放（drop の挿入）

> **この章のゴール**
> **v1 で諦めていた `free()` を入れる。** 所有者のスコープが終わるときに、値を解放する。
>
> ```bash
> $ cat t.po
> class Res:
>     name: str
>     def init(self, name: own str) -> None:
>         self.name = name
>     def drop(self) -> None:
>         print("drop " + self.name)
>
> def main() -> int:
>     a: Res = Res("a")
>     b: Res = Res("b")
>     print("main")
>     return 0
> $ ./build/poloniumc --drop t.po -o t && ./t
> main
> drop b
> drop a          ← 宣言と逆順に解放される
> ```

第22〜24章で「**誰が所有者か**」が静的に決まりました。所有者が決まったからこそ、
「**いつ解放してよいか**」も決まります。順番は逆にできません。

**★ この章で初めて生成される IR が変わります。**
第21〜24章はすべて「見て報告するだけ」でした。ここからは出力そのものが変わるので、
**壊れ方も変わります**（コンパイルは通るのに実行時に落ちる、という失敗が初めて起こります）。

---

## 目次

- [25.1 何を足すか](#251-何を足すか)
- [25.2 なぜ `--drop` を opt-in にしたのか](#252-なぜ---drop-を-opt-in-にしたのか)
- [25.3 スコープと、抜ける経路すべて](#253-スコープと抜ける経路すべて)
- [25.4 drop フラグを持たない](#254-drop-フラグを持たない)
- [25.5 何を解放しないか](#255-何を解放しないか)
- [25.6 つまずき：文字列リテラルは解放できない](#256-つまずき文字列リテラルは解放できない)
- [25.7 クラスの解放関数を生成する](#257-クラスの解放関数を生成する)
- [25.8 つまずき：上書きのときに古い値を落とす](#258-つまずき上書きのときに古い値を落とす)
- [25.9 フィールドからの取り出しを禁止した](#259-フィールドからの取り出しを禁止した)
- [25.10 `make drop-asan` — 二重解放を実行時に捕まえる](#2510-make-drop-asan--二重解放を実行時に捕まえる)
- [25.11 動作確認](#2511-動作確認)
- [25.12 この章で残した宿題](#2512-この章で残した宿題)
- [25.13 まとめと次章の予告](#2513-まとめと次章の予告)

---

## 25.1 何を足すか

| # | 足すもの | 触るファイル |
|---|---|---|
| ① | `pl_drop_str` / `pl_drop_list` / `pl_drop_obj` と「静的な文字列」の印 | `runtime/runtime.c` |
| ② | スコープ（`ScopeCtx`）と、出口すべてでの解放 | `src/codegen.c` |
| ③ | 型ごとの解放関数の生成（`@drop.C` / `@drop.list.N`） | `src/codegen.c` |
| ④ | 移動の印（`moved_out`）と借用の印（`binds_borrow`） | `src/ast.h`, `src/ownck.c` |
| ⑤ | `E-MOVE-2`（フィールドからの取り出しの禁止） | `src/ownck.c`, `docs/spec/safety-spec.md` |
| ⑥ | `--drop` | `src/main.c`, `src/codegen.h` |
| ⑦ | `make drop-asan`（二重解放の検査）とテスト 7 本 | `tests/drop_asan.sh`, `Makefile` |

設計は [ownership.md §6](../design/ownership.md)、仕様は
[safety-spec.md §6](../spec/safety-spec.md)。
[memory-model.md](../design/memory-model.md) の「解放しない」もこの章で改訂しました。

---

## 25.2 なぜ `--drop` を opt-in にしたのか

**この時点で `selfhost/` には 454 件の所有権違反があります**（第22〜24章ぶんと、
この章で足した `E-MOVE-2` の合計）。
その状態で解放を既定にすると、**コンパイラ自身が二重解放で落ちます**。

**★ 決定 D16：解放は `--drop` で opt-in。既定では 1 バイトも IR を変えない。**

```bash
$ ./build/poloniumc t.po -o t          # v1 とまったく同じ IR
$ ./build/poloniumc --drop t.po -o t   # 解放を挿入する
```

これで次の 2 つが同時に成り立ちます。

- `make bootstrap`（不動点）と `make selfhost-test`（C 版と Polonium 版の IR 比較）は**そのまま緑**
- 解放そのものは、小さなテストで**今すぐ検証できる**

第26章で `selfhost/` と `lib/` を移行し終えたら、既定を on に切り替えます。
**「検査を入れる」と「既存コードを直す」を混ぜない**という第22章からの方針
（決定 D10）を、コード生成にもそのまま適用しています。

---

## 25.3 スコープと、抜ける経路すべて

解放は「スコープの出口」に入れます。**出口は 1 つではありません。**

| 出口 | どこに入れるか |
|---|---|
| ブロックの終わり | 最後の文の後 |
| `return` | **戻り値を評価した後**、`ret` の前 |
| `break` / `continue` | ジャンプの前（**抜けるスコープぶん全部**） |
| 関数の終わり | 引数（`own` で受け取ったもの）を解放 |

実装は `LoopCtx`（第7章）とまったく同じ形で、C の呼び出しスタックに乗せます。

```c
typedef struct ScopeCtx ScopeCtx;
struct ScopeCtx {
    ScopeCtx *outer;
    DropEnt *ents;   // ★ 先頭に足すので、たどると自然に「宣言の逆順」
};
```

```c
        case ND_BLOCK: {
            ScopeCtx sc = {e->scope, NULL};
            e->scope = &sc;
            ... 文を生成 ...
            if (e->drop && !e->terminated) emit_scope_drops(e, &sc);
            e->scope = sc.outer;
        }
```

`break` / `continue` は「どこまで抜けるか」が要ります。ループに入ったときの
スコープを `LoopCtx` に覚えておけば、そこまで巻き戻すだけです。

```c
static void emit_drops_until(Emitter *e, ScopeCtx *stop) {
    for (ScopeCtx *sc = e->scope; sc && sc != stop; sc = sc->outer)
        emit_scope_drops(e, sc);
}
```

**★ `return` は「戻り値を評価してから」解放します。** 順番が命です。
戻り値が局所変数なら、評価の時点で**移動**が起きてスロットが空になるので、
その後の解放は何もしません（次節）。

---

## 25.4 drop フラグを持たない

設計（[ownership.md §6.3](../design/ownership.md)）では、Rust に倣って
「`MaybeMoved` の場所は `alloca i1` のフラグを持つ」と決めていました。

```python
    xs: list[int] = [1, 2]
    if flag:
        take(xs)          # 移動する経路
    # ← ここで xs を解放してよいのは flag が偽のときだけ
```

**🤔 本当にフラグが要るのか**

Polonium の所有型（`str` / `list[T]` / class）は**すべてポインタ**です。
そして解放関数はどれも `null` を受け取れます。
であれば、**移動したときにスロットへ `null` を書けば**、それがそのままフラグです。

```llvm
  %t0 = load ptr, ptr %xs           ; take(xs) の引数を読む
  store ptr null, ptr %xs           ; ★ 移動した印
  ...
  %t9 = load ptr, ptr %xs           ; スコープ終端
  call void @drop.list.0(ptr %t9)   ; null なら何もしない
```

**★ 決定 D17：drop フラグは持たない。スロットの `null` がフラグを兼ねる。**

- `alloca i1` が要らない
- 解放のたびの分岐（`br i1`）が要らない
- 分岐で片方だけ移動した場合も、**何もしなくても正しく動く**

ownck が「この参照で移動した」と印を付け（`Node.moved_out`）、codegen はそれを読むだけです。
**第9章の `builtin`、第12章の `cls` と同じ形**——解析が判断し、生成は従うだけ。

---

## 25.5 何を解放しないか

**解放してはいけないものを間違えると、二重解放になります。** こちらのほうが大事です。

| 解放しないもの | 理由 | 印 |
|---|---|---|
| 借用の引数（既定・`mut`） | 所有者は呼び出し側 | `binds_borrow` |
| 借りものを束縛した変数（`t = xs[i]`） | 所有者はコンテナ | `binds_borrow` |
| `for` のループ変数 | 要素は借りもの（仕様 §3.1） | 同上（脱糖も同じ道を通る） |
| **借用を返す関数の戻り値** | 所有者は呼ばれた側の `self`（仕様 §4.5） | `binds_borrow` |
| グローバル | プログラムの終わりまで生きる | — |
| 文字列リテラル | ヒープではない（§25.6） | ヘッダのビット |
| 移動済みの値 | 所有者は移動先 | スロットが `null` |

4 番目が曲者です。

```python
class Lexer:
    src: str
    def source(self) -> str:
        return self.src      # 仕様 §4.5：self の借用を返してよい

    ...
s: str = lx.source()          # ← s は借りもの。解放したら lx.src が壊れる
```

**呼び出し側からは「戻り値が所有か借用か」が見えません。**
そこで ownck が**定義を先に見て**「借用を返す関数」に印を付け、
呼び出し側の一時値をその印で分類します。

```c
static bool returns_borrow(Node *fn, Node *n) {
    if (n->kind == ND_RETURN) return rooted_in_param(fn, n->lhs);
    ...
}
```

**⚠️ Rust ならライフタイムで表現するところです。** 注釈を書かせない代わりに、
**コンパイラが定義を読んで判断する**——それがこの言語の取引です（仕様 §4.4）。

---

## 25.6 つまずき：文字列リテラルは解放できない

最初の実装で、`s: str = "abc"` を含むプログラムが落ちました。

```
malloc: *** error for object 0x...: pointer being freed was not allocated
```

**⚠️ リテラルはヒープにありません。** `.rodata`（読み取り専用データ）に
プログラムごと埋め込まれています（[memory-model.md §2](../design/memory-model.md)）。

```llvm
@.str.0 = private unnamed_addr constant { i64, [4 x i8] } { i64 3, [4 x i8] c"abc\00" }
```

実行時にポインタだけを見て「ヒープか定数か」を判定する移植性のある方法はありません。
**そこで、長さのヘッダに 1 ビットの印を立てます。**

```c
#define PL_STR_STATIC (1LL << 62)

long long pl_str_len(const char *s) {
    return ((const long long *)s)[-1] & ~PL_STR_STATIC;   // 印を落として読む
}

void pl_drop_str(char *s) {
    if (!s) return;
    if (((long long *)s)[-1] & PL_STR_STATIC) return;     // リテラルは解放しない
    free(s - 8);
}
```

**★ 第15章で「長さをヘッダに持つ」形にしておいたことが、ここで効きました。**
値そのものを変えずに、1 ビットの情報を足す場所があったからです。

**⚠️ 印を立てるのは `--drop` のときだけ**にしてあります。既定の IR を
1 バイトも変えない、というこの章の約束（§25.2）を守るためです。

---

## 25.7 クラスの解放関数を生成する

クラスごとに `@drop.C` を生成します。順序が仕様（§6.2）です。

```
  ① drop メソッド（デストラクタ）があれば呼ぶ
  ② 所有型のフィールドを宣言順に解放する
  ③ インスタンス自身を解放する
```

```llvm
define internal void @drop.t.Res(ptr %p) {
entry:
  %isnull = icmp eq ptr %p, null
  br i1 %isnull, label %done, label %body
body:
  call void @t.Res.drop(ptr %p)                  ; ① デストラクタ
  %f0 = getelementptr %t.Res.type, ptr %p, i32 0, i32 0
  %v0 = load ptr, ptr %f0
  call void @pl_drop_str(ptr %v0)                ; ② フィールド
  call void @pl_drop_obj(ptr %p)                 ; ③ 自分自身
  br label %done
done:
  ret void
}
```

`list[T]` は、`pl_drop_list(l, 要素の解放関数)` を包む関数を型ごとに 1 つ作ります。

```llvm
define internal void @drop.list.0(ptr %l) {
  call void @pl_drop_list(ptr %l, ptr @pl_drop_str)
  ret void
}
```

**★ 包む関数を作ると、どの型でも「ptr を 1 つ取る解放関数」の形にそろいます。**
そろえてしまえば、リストの要素でもフィールドでも局所変数でも、呼び出し方は同じです。

**⚠️ 自分自身を含むクラス（連結リスト）では再帰します。** 長いリストでは
スタックを使い切る可能性があります（`rc[T]` を入れる第28章で見直します）。

---

## 25.8 つまずき：上書きのときに古い値を落とす

```python
    s: str = "a"
    while i < 3:
        s = s + "!"      # ← 古い s は誰が解放するのか
```

上書きされた古い値は、**その代入の時点で誰からも参照されなくなります**。
v1 は黙って捨てていました（解放しないので問題にならなかった）。
v2 では、代入の前に古い値を解放します。

```c
            if (e->drop && !n->binds_borrow) {
                if (n->lhs->kind == ND_VAR && n->lhs->ir_name[0] == '%')
                    emit_drop_value(e, n->type, gen_load(e, n->type, n->lhs->ir_name));
```

**⚠️ ここで一度、二重解放を作りました。**

```python
def pick(xs: list[str]) -> int:
    t: str = xs[0]     # t は借りもの
    t = xs[1]          # ← 古い t（= xs[0]）を解放してしまった！
```

借りものを束縛している変数では、**古い値も他人のもの**です。
ownck が付ける `binds_borrow` の印は解析の途中で立つ（後の行の代入で立つこともある）ので、
**解析が終わってから代入ノードへ写す**という後処理を足しました。

```c
static void propagate_borrow_binds(Own *o, Node *n) { ... }
```

**★ 教訓：解析の結果を codegen に渡すときは「いつ確定するか」を確かめること。**
途中の値を読むと、たまたま動く（そして別の場所で落ちる）コードになります。

---

## 25.9 フィールドからの取り出しを禁止した

```python
    b: Box = Box("x")
    s: str = b.label      # ❌ E-MOVE-2
```

**🤔 なぜ禁止するのか**

オブジェクトを解放するときは、フィールドもまとめて解放します。
途中で 1 つだけ持ち出せると、「どのフィールドがまだ残っているか」を
**フィールドごとの実行時フラグ**で覚える必要が出てきます。Rust はそうしていますが、
v2 は「フィールドは借りて使う」で足りるので、その複雑さを買いません。

```
warning[E-MOVE-2]: フィールド 'b.label' から値を取り出せません
   = ヒント: フィールドはそのまま使ってください（値そのものが要るなら copy(...) を使います）
```

**★ これは言語仕様の変更なので、コードより先に
[safety-spec.md §3.1](../spec/safety-spec.md) を直しました。**
第22章に書いたテスト（`own_move_field_disjoint.po`）はこの規則に合わなくなったので、
「フィールドへ**別々に移動できる**」ことを確かめる形に書き直しています。
**仕様が変わったらテストも変わる。テストに合わせて仕様を曲げない。**

---

## 25.10 `make drop-asan` — 二重解放を実行時に捕まえる

解放の間違いは、**普通のテストでは観測できません**。二重解放しても、
たまたま動いてしまうことがあるからです。そこで AddressSanitizer を網にします。

```bash
$ make drop-asan
  ok    drop_alias_reassign.po (asan, exit=0)
  ok    drop_borrow_kept.po (asan, exit=0)
  ...
全 7 件が AddressSanitizer で問題なし
```

`tests/drop_asan.sh` は `drop_*.po` を **`--drop` 付きで IR にし、
ランタイムごと ASan でリンクして走らせます**。`make test` にも入れました。

**⚠️ LeakSanitizer は macOS では使えません**（Linux のみ）。
リークは「壊れない」種類の間違いなので、まず**壊れないこと**を確かめます。

---

## 25.11 動作確認

✍️ テストを 7 本足します（すべて `# FLAGS: --drop`）。

| ファイル | 確認すること |
|---|---|
| `drop_order.po` | 宣言と逆順に解放される |
| `drop_early_return.po` | 早期 `return` の経路でも解放される |
| `drop_loop.po` | `break` / `continue` の経路でも解放される |
| `drop_moved.po` | 移動した値は移動先が解放する（二重解放しない） |
| `drop_fields.po` | drop メソッド → フィールド → 自分自身 の順 |
| `drop_borrow_kept.po` | 借りものは解放しない（引数・要素・`self` の借用） |
| `drop_alias_reassign.po` | 借りものを束縛した変数は上書きでも解放しない |

**✅ 確認**

```bash
$ make test
全 371 件パス
全 7 件が AddressSanitizer で問題なし
トークン列一致 411 件 / AST 一致 369 件 / 型検査 一致 227 件 / IR 一致 227 件

$ make bootstrap
★ 不動点に到達しました（stage2 == stage3）
```

**★ `--drop` を付けないときの IR は 1 バイトも変わっていません。**
だから不動点も C 版・Polonium 版の IR 比較も、そのまま緑です。

---

## 25.12 この章で残した宿題

| 残したもの | いつ | なぜ後回しにしたか |
|---|---|---|
| 式の途中の一時値（`total(build())` の結果） | 次章以降 | 「どの値が捨てられるか」の追跡が要る。**リークであって、壊れはしない** |
| `xs[i] = v` で上書きされた古い要素 | 同上 | 同上 |
| `copy(x)`（D8）・`del x`・`xs.pop()` | 次章以降 | **新しい構文と解放を同じ章に入れると、落ちたときに切り分けられない**（第21章と同じ判断） |
| 要素の move out（`pop()` とセット） | 同上 | |
| `selfhost/` と `lib/` の移行（454 件 + 5 件） | 第26章 | |
| 既定を `--drop` on にする | 第26章の後 | |

**⚠️ 一時値のリークは仕様 §1 の S4（リークが無い）を満たしていません。**
「壊れない間違い」を残し、「壊れる間違い」から先に潰す、という優先順位です。

---

## 25.13 まとめと次章の予告

この章でやったこと：

- **v1 で諦めていた `free()` を入れた**（ランタイムに `pl_drop_*` を追加）
- スコープの出口すべて（終端・`return`・`break` / `continue`）に解放を挿入した
- **drop フラグを持たず、スロットの `null` で代用した**（決定 D17）
- 解放**しない**ものを印で分類した（借用・要素・`self` の借用の返却・リテラル）
- 文字列リテラルをヘッダの 1 ビットで見分けた
- クラスの `@drop.C`（デストラクタ → フィールド → 自分）を生成した
- フィールドからの取り出しを禁止し、**仕様のほうを直した**
- `make drop-asan` で二重解放を実行時に捕まえる網を張った

次章（第26章）は **既存コードの v2 移行**です。`lib/`（5 件）と `selfhost/`（400 件）を
新しい規則で書き直し、最後に**検査と解放を既定に切り替えます**。

**★ ここからは「コンパイラ自身が仕様のテストケース」になります。**
書き直せない形が出てきたら、それは仕様が厳しすぎるという証拠です
（[ownership.md §8](../design/ownership.md)）。そのときは**仕様を見直します**。
