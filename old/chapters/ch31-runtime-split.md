# 第31章 ランタイムの分割と freestanding

> **この章のゴール**
> ランタイムを **libc に依存しない核**と、**PC 上でのフック実装**に分ける。
> `-c` と `--target` を足して、**別の機械向けのオブジェクト**を出せるようにする。
>
> ```bash
> $ ./build/poloniumc -c kernel.po -o kernel.o
> $ file kernel.o
> kernel.o: ELF 64-bit LSB relocatable, UCB RISC-V, ...
> ```

第30章で「番地を触る」道具はそろいました。しかし、いまのランタイムは
`printf` / `calloc` / `fopen` を直接呼んでいます。**ベアメタルにはそのどれもありません。**

---

## 目次

- [31.1 4 つのフックに追い出す](#311-4-つのフックに追い出す)
- [31.2 分けたあとの姿](#312-分けたあとの姿)
- [31.3 libc なしで書き直したもの](#313-libc-なしで書き直したもの)
- [31.4 `-c` と `--target`](#314--c-と---target)
- [31.5 `pragma target` / `pragma no_runtime`](#315-pragma-target--pragma-no_runtime)
- [31.6 動作確認](#316-動作確認)
- [31.7 まとめと次章の予告](#317-まとめと次章の予告)

---

## 31.1 4 つのフックに追い出す

**🤔 なぜ「OS 用のランタイム」をもう 1 本書かないのか**

同じ処理が 2 か所に増えると、**必ずずれます**。文字列の連結や `rc` の
数え札のような「言語の意味」に関わるコードを 2 本持つのは、
バグを 2 倍に増やす近道です。

**★ 環境ごとに違うのは、たった 4 つでした。**

```c
// runtime/core.h
void *pl_hook_alloc(long long size);   // メモリを確保する（ゼロ初期化つき）
void  pl_hook_free(void *p);           // 返す
void  pl_hook_write(const char *s, long long len);  // 「標準出力」へ書く
void  pl_hook_panic(const char *msg);  // 死ぬ（戻ってこない）
```

```
  ┌─────────────┐        ┌──────────────────┐
  │  core.c      │──呼ぶ──▶│ pl_hook_alloc     │  hosted.c（PC 上）
  │  言語の意味   │        │ pl_hook_free      │    → calloc / free / stdout
  │  （1 本だけ）│        │ pl_hook_write     │  kernel/hooks.c（ベアメタル）
  │              │        │ pl_hook_panic     │    → 自前ヒープ / UART
  └─────────────┘        └──────────────────┘
```

**★ この形にしたおかげで、第32章では `print` がそのまま動きます。**
カーネルの中でも、PC の上と同じ `print("...")` が使えます。

---

## 31.2 分けたあとの姿

| ファイル | 中身 | ベアメタルで使うか |
|---|---|---|
| `runtime/core.h` | フック 4 つの宣言 | — |
| `runtime/core.c` | 文字列・リスト・`rc`・解放・`print` の組み立て | ✅ |
| `runtime/hosted.c` | フックの libc 実装 + ファイル入出力・`argv`・環境変数 | ❌ |

`make` は 2 つをコンパイルし、**部分リンク**で 1 本にまとめます。

```make
	$(CC) $(RUNTIME_CFLAGS) -c $(RUNTIME_CORE) -o build/core.o
	$(CC) $(RUNTIME_CFLAGS) -c $(RUNTIME_HOSTED) -o build/hosted.o
	ld -r -o $@ build/core.o build/hosted.o
```

**★ `ld -r`（部分リンク）にしたのは、コンパイラ側を変えないためです。**
第9章から「ランタイムは `.o` が 1 本」という前提でやってきました。
その前提を守れば、`main.c` は 1 行も変わりません。

---

## 31.3 libc なしで書き直したもの

`core.c` は `<stdio.h>` / `<stdlib.h>` / `<string.h>` を **include しません**。
使っていた libc の関数は、自分で持ちます。

| 使っていたもの | 代わり |
|---|---|
| `printf("%lld\n", v)` | `pl_itoa` で桁を作り、`pl_hook_write` |
| `snprintf`（エラーメッセージ） | 同上（手で組み立てる） |
| `strtoll`（`int("42")`） | 符号と数字を自分で読む |
| `strcmp` | 1 バイトずつ比べる |
| `memcpy` | `pl_memcpy`（＋ freestanding では `memcpy` 自体も提供） |
| `calloc` / `free` | `pl_hook_alloc` / `pl_hook_free` |

**⚠️ freestanding でも clang は `memcpy` の呼び出しを勝手に作ります。**
「明らかにコピーしているループ」を見つけると置き換えるためです。
libc の無い世界では、その相手も自分で用意しておく必要があります。

```c
#ifdef PL_FREESTANDING
void *memcpy(void *dst, const void *src, unsigned long n) { ... }
void *memset(void *dst, int c, unsigned long n) { ... }
#endif
```

---

## 31.4 `-c` と `--target`

```bash
$ ./build/poloniumc -c --target=riscv64-unknown-elf kernel.po -o kernel.o
```

| オプション | 意味 |
|---|---|
| `-c` | リンクせず、オブジェクト（`.o`）を出して終わる |
| `--target=<triple>` | 生成する IR の target triple を指定する |

**⚠️ リンクはこちらの仕事ではありません。** ベアメタルでは
リンカスクリプトを渡し、起動アセンブリを混ぜ、専用のリンカを使います。
**「どこまでがコンパイラの仕事か」を線引きするのが `-c` です。**

環境変数も 2 つ用意しました。

| 変数 | 用途 |
|---|---|
| `PLC_CLANG` | 使う clang を差し替える（Apple の clang には RISC-V が無い） |
| `PLC_CFLAGS` | clang に渡す追加のフラグ（`-march=rv64g` など） |

---

## 31.5 `pragma target` / `pragma no_runtime`

コマンドラインだけでなく、**ソースにも書けます**（仕様 §10.3）。

```python
pragma target "riscv64-unknown-elf"
pragma no_runtime
```

| pragma | 意味 |
|---|---|
| `target "..."` | このファイルはどの機械向けか（`--target` があればそちらが優先） |
| `no_runtime` | **C の `main` ラッパを出さない**（第7節の方式 A を使わない） |

**🤔 `no_runtime` は何を止めるのか**

v1 から、入口モジュールには「C の `main` を出して Polonium の `main` を呼ぶ」
ラッパを付けていました（ir-conventions.md 第7節）。
ベアメタルには C の `main` も `argv` もありません。呼ぶのは**起動アセンブリ**です。

```c
        // ★ 第32章：ベアメタルでは C の main も argv も無い。
        const char *entry_main = (m == entry && !no_runtime) ? sb_str(&main_ir) : NULL;
```

**⚠️ 仕様 §10.4 は「プロファイル（hosted / freestanding）」を想定していましたが、
フックの設計にしたことで、切り替えは `no_runtime` の 1 つで足りました。**
`print` も `list` も `rc` も、フックさえあればベアメタルで動くからです。

---

## 31.6 動作確認

```bash
$ make                       # ランタイムが 2 本になっても、使い方は同じ
$ make test
全 388 件パス

$ PLC_CLANG=$(brew --prefix llvm)/bin/clang \
  PLC_CFLAGS="-march=rv64g -mabi=lp64 -mcmodel=medany -mno-relax" \
  ./build/poloniumc -c kernel/kernel.po -o kernel.o
$ file kernel.o
kernel.o: ELF 64-bit LSB relocatable, UCB RISC-V, soft-float ABI, version 1 (SYSV)
```

**★ Polonium のソースから、RISC-V の ELF オブジェクトが出ました。**
リンクして動かすのは次章です。

---

## 31.7 まとめと次章の予告

この章でやったこと：

- 環境ごとに違う操作を **4 つのフック**に追い出した
- ランタイムを `core.c`（libc 非依存）と `hosted.c`（libc 実装）に分けた
- `core.c` から libc の関数を全部追い出した（`itoa` / 比較 / コピーを自作）
- `-c` / `--target` / `PLC_CLANG` / `PLC_CFLAGS` を足した
- `pragma target` / `pragma no_runtime` を入れた

次章（第32章）は、いよいよ**ベアメタル起動**です。
QEMU の RISC-V マシンで、**Polonium で書いたカーネルの `print` が画面に出ます**。
