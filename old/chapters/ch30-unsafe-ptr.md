# 第30章 `unsafe` と `ptr[T]`

> **この章のゴール**
> **番地を直接触れるようにする。** ただし `unsafe:` の中でだけ。
>
> ```python
> extern def pl_alloc(size: int) -> int
>
> def main() -> int:
>     total: int = 0
>     unsafe:
>         base: int = pl_alloc(16)
>         p: ptr[int] = ptr_at(base)
>         poke8(p, 0, 65)          # 1 バイト書く
>         poke64(p, 1, 12345)      # 8 バイト書く
>         total = peek8(p, 0) + peek64(p, 1)
>     return total % 200           # → 10
> ```

ここから**フェーズ D（OS へ）**です。第22〜29章で作った安全性は、
「言語の中だけで完結する世界」の話でした。OS を書くには、その世界の外側——
**装置のレジスタや物理メモリ**——に手を伸ばす必要があります。

**★ 安全な言語に「危険な出口」を用意するのが、この章の仕事です。**
出口には名前を付けます（`unsafe:`）。名前が付いていれば、**探せます**。

---

## 目次

- [30.1 何を足すか](#301-何を足すか)
- [30.2 `unsafe:` は何を止めるのか](#302-unsafe-は何を止めるのか)
- [30.3 `ptr[T]` の中身は `int` だけ](#303-ptrt-の中身は-int-だけ)
- [30.4 6 つの操作](#304-6-つの操作)
- [30.5 なぜ volatile なのか](#305-なぜ-volatile-なのか)
- [30.6 動作確認](#306-動作確認)
- [30.7 まとめと次章の予告](#307-まとめと次章の予告)

---

## 30.1 何を足すか

| # | 足すもの | 触るファイル |
|---|---|---|
| ① | 型 `TY_PTR` と `ptr[T]` の解決 | `src/types.[ch]`, `src/sema.c` |
| ② | `unsafe:` ブロック（`ND_UNSAFE`） | `src/ast.[ch]`, `src/parser.c` |
| ③ | 低レベルの 6 操作と `E-UNSAFE-1` | `src/sema.c` |
| ④ | `inttoptr` / `ptrtoint` / volatile な load / store | `src/codegen.c` |
| ⑤ | テスト 3 本 | `tests/cases/unsafe_*.po`, `err_unsafe_*.po` |

仕様は [safety-spec.md §10](../../docs/spec/safety-spec.md)、設計は
[design/os-support.md](../../docs/design/os-support.md)。

---

## 30.2 `unsafe:` は何を止めるのか

**⚠️ `unsafe:` は借用検査を止めません。** 止めるのは「ポインタ操作の禁止」だけです
（仕様 §10.1）。

```python
def main() -> int:
    p: ptr[int] = ptr_at(0)     # ❌ E-UNSAFE-1
    return 0
```

```
error[E-UNSAFE-1]: 'ptr_at' は unsafe: の中でしか使えません
  --> t.po:2:19
   |
 2 |     p: ptr[int] = ptr_at(0)
   |                   ^^^^^^ 生ポインタを触っています
   |
   = ヒント: unsafe: ブロックで囲んでください:
             unsafe:
                 poke8(p, 0, 65)
```

実装は**深さを数えるだけ**です。

```c
        case ND_UNSAFE:
            s->unsafe_depth++;
            check_block(s, n->body);
            s->unsafe_depth--;
            break;
```

**★ 生成には何も出しません。** `unsafe:` は検査のための印なので、
codegen は中身をそのまま出します。

```c
        case ND_UNSAFE: return gen_stmt(e, n->body);
```

**🤔 なぜ「印」だけで意味があるのか**：`unsafe` を検索すれば、
**この言語で壊れうる場所が全部見つかる**からです。安全性の保証は
「絶対に壊れない」ではなく「**壊れうる場所が限られていて、そこが探せる**」ことです。

---

## 30.3 `ptr[T]` の中身は `int` だけ

```python
    p: ptr[str] = ptr_at(0)     # ❌
```
```
error: 'str' は ptr に入れられません
    = ヒント: いま ptr に書けるのは int だけです（例: ptr[int]）
```

**🤔 なぜ制限するのか**

番地の先にあるのは**ただのバイト列**です。そこに「Polonium の str」が
載っている保証はどこにもありません（`str` はヘッダ付きの表現でした。第15章）。
`ptr[str]` を許すと、「言語の型」と「メモリの中身」がずれたまま話が進みます。

`ptr[int]` に限れば、意味は「**そこから何バイトか読み書きする**」だけになり、
ずれようがありません。

---

## 30.4 6 つの操作

| 操作 | 意味 | 生成される IR |
|---|---|---|
| `ptr_at(addr)` | 番地からポインタを作る | `inttoptr i64 → ptr` |
| `addr_of(p)` | ポインタを番地に戻す | `ptrtoint ptr → i64` |
| `peek8(p, i)` | 1 バイト読む | `load volatile i8` + `zext` |
| `peek64(p, i)` | 8 バイト読む | `load volatile i64` |
| `poke8(p, i, v)` | 1 バイト書く | `trunc` + `store volatile i8` |
| `poke64(p, i, v)` | 8 バイト書く | `store volatile i64` |

**★ 添字（`i`）は「要素いくつぶん」です。** `peek8(p, 5)` は 5 バイト目、
`peek64(p, 1)` は 8 バイト目から。`getelementptr` に幅を任せているので、
掛け算を書く必要はありません。

```c
    char *addr = new_tmp(e);
    sb_printf(&e->fn, "  %s = getelementptr %s, ptr %s, i64 %s\n", addr, ity, p, off);
```

これらは**ランタイム関数を呼びません**。命令 1〜2 個です。
OS の中では「ランタイムを呼べない場所」があるので、
**低レベルの操作は必ず命令に落ちる**ようにしておきます。

---

## 30.5 なぜ volatile なのか

```llvm
  %v = load volatile i8, ptr %addr
```

**⚠️ 装置のレジスタ（MMIO）は、ふつうのメモリではありません。**

- 同じ番地を 2 回読むと**違う値**が返ることがある（受信バッファなど）
- 書いた値が**読めるとは限らない**（送信レジスタ）
- 読み書きの**順序と回数に意味がある**

最適化器は「同じ番地の 2 回目の読みは省ける」「使わない書き込みは消せる」と
考えます。それは**ふつうのメモリでは正しく、装置では致命的**です。
`volatile` は「この読み書きは減らすな・並べ替えるな」という指示です。

**★ 第9章で「ランタイムに押し込む」と決めた規約 R10 と、方針は同じです。**
「危ないものを 1 か所に閉じ込め、そこだけ特別扱いする」。

---

## 30.6 動作確認

✍️ テストを 3 本足します。

| ファイル | 確認すること |
|---|---|
| `unsafe_ptr_basic.po` | `ptr_at` / `poke8` / `poke64` / `peek8` / `peek64` が動く |
| `err_unsafe_outside.po` | `unsafe:` の外では使えない（`E-UNSAFE-1`） |
| `err_unsafe_ptr_elem.po` | `ptr` に入れられるのは `int` だけ |

**⚠️ stage1（Polonium 版）にはまだ `unsafe` / `ptr[T]` がありません。**
第27章で作った `# STAGE1-SKIP:` の印を使って、比較から外しています。

```bash
$ make test
全 388 件パス
```

---

## 30.7 まとめと次章の予告

この章でやったこと：

- `unsafe:` ブロックを入れ、**危険な操作をそこに閉じ込めた**
- `ptr[T]` を `int` 限定で入れた（メモリの中身と言語の型をずらさないため）
- 低レベルの 6 操作を、**ランタイムを呼ばない命令**として実装した
- MMIO のために `volatile` を付けた

次章（第31章）は **ランタイムの分割**です。
いまのランタイムは `printf` / `calloc` / `fopen` を直接呼んでいます。
**ベアメタルにはそのどれもありません。**
libc に触る所を数個のフックに追い出して、**核を 1 本のまま**両方の世界で使えるようにします。
