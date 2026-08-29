# 第32章 ベアメタル起動（RISC-V / QEMU）

> **この章のゴール**
> **OS の無い機械で、Polonium で書いたコードを動かす。**
>
> ```bash
> $ make qemu
> =================================
>  Polonium kernel on RISC-V (virt)
> =================================
> 1 から 10 までの合計: 55
> v2 の言葉: own
> v2 の言葉: mut
> ...
> ```

`print` も `for` も `list[str]` も、**PC の上とまったく同じコード**が動きます。
第31章でランタイムの核を libc から切り離したからです。

---

## 目次

- [32.1 ターゲットは RISC-V（Q6 の決着）](#321-ターゲットは-risc-vq6-の決着)
- [32.2 起動の 3 行](#322-起動の-3-行)
- [32.3 リンカスクリプト](#323-リンカスクリプト)
- [32.4 カーネル側のフック](#324-カーネル側のフック)
- [32.5 Polonium で書いたカーネル](#325-polonium-で書いたカーネル)
- [32.6 つまずき 3 つ](#326-つまずき-3-つ)
- [32.7 動作確認](#327-動作確認)
- [32.8 まとめと次章の予告](#328-まとめと次章の予告)

---

## 32.1 ターゲットは RISC-V（Q6 の決着）

作業ボードの Q6 は「x86_64 と RISC-V のどちらから始めるか」でした。
**答えは手元の道具が出しました。**

| 調べたこと | 結果 |
|---|---|
| Apple の clang に RISC-V のバックエンドはあるか | ❌ 無い（Homebrew の LLVM にはある） |
| x86 の ELF リンカはあるか | ❌ 無い（macOS のリンカは Mach-O 専用） |
| RISC-V の ELF リンカはあるか | ✅ `riscv64-elf-ld` がある |
| QEMU はあるか | ✅ `qemu-system-riscv64` の `virt` マシン |

**★ 決定（Q6）：RISC-V から始める。**

理由は道具だけではありません。**x86 は起動が複雑**です（リアルモード →
プロテクトモード → ロングモード、GDT、A20…）。RISC-V の `virt` マシンは
**電源投入直後から 64 ビットで、0x80000000 に置いた ELF の先頭に飛ぶ**だけです。
「OS を書く」ことを学ぶのに、x86 の歴史を先に学ぶ必要はありません。

```bash
brew install llvm riscv64-elf-binutils qemu
```

---

## 32.2 起動の 3 行

```asm
_start:
    la sp, stack_top          # ① スタックを用意する
    la t0, trap_entry         # ② 割り込みの入口を登録する（第33章）
    csrw mtvec, t0
    call kernel.main          # ③ Polonium で書いた main を呼ぶ
```

**⚠️ ここだけはアセンブリでなければ書けません。**
「スタックがまだ無い状態で動くコード」は高級言語では書けないからです
（言語がレジスタの使い方を決めてしまうため）。

**★ `call kernel.main` の名前に注目してください。**
第13章で決めた「モジュール名で修飾する」規則がそのまま効いています。
`kernel/kernel.po` の `main` は `@kernel.main` という名前で出るので、
アセンブリから呼べます。

---

## 32.3 リンカスクリプト

```ld
ENTRY(_start)
SECTIONS {
    . = 0x80000000;          /* QEMU virt が最初に実行する番地 */
    .text : {
        *(.text.start)       /* ★ 起動コードを必ず先頭に */
        *(.text*)
    }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }
    .bss    : { *(.bss*) *(COMMON) }
    _end = .;                /* ヒープはここから（hooks.c が使う） */
}
```

**★ `_end` はリンカが教えてくれる「カーネルの終わり」です。**
ヒープの置き場所を人間が数える必要はありません。

---

## 32.4 カーネル側のフック

第31章で決めた 4 つを、カーネル向けに実装します（`kernel/hooks.c`、約 60 行）。

```c
static volatile unsigned char *const UART = (volatile unsigned char *)0x10000000UL;

void pl_hook_write(const char *s, long long len) {
    for (long long i = 0; i < len; i++) uart_putc(s[i]);
}
```

メモリは**いちばん単純な bump allocator**です。

```c
void *pl_hook_alloc(long long size) {
    if (!heap_next) heap_next = &_end;      // カーネルの後ろから
    size = (size + 15) & ~15LL;
    char *p = heap_next;
    heap_next += size;
    for (long long i = 0; i < size; i++) p[i] = 0;   // ★ ゼロ初期化が約束
    return p;
}
```

**⚠️ 解放できません**（`pl_hook_free` は何もしない）。
v1 のメモリモデルと同じ割り切りです（[memory-model.md §3](../design/memory-model.md)）。
`--drop` を使えば解放は起きますが、この bump allocator では再利用されません。

---

## 32.5 Polonium で書いたカーネル

```python
pragma target "riscv64-unknown-elf"
pragma no_runtime

def banner() -> None:
    print("=================================")
    print(" Polonium kernel on RISC-V (virt)")
    print("=================================")

def main() -> int:
    banner()
    total: int = 0
    for i in range(1, 11):
        total = total + i
    print("1 から 10 までの合計: " + str(total))
    ...
```

**★ ここに `unsafe` は 1 つもありません。**
`print` も `for` も `str` の連結も `list[str]` も、**PC 上と同じ**ように書けます。
装置に触るのはフック（C 側）だけなので、カーネルの本体は「ふつうの Polonium」です。

ビルドは `make kernel`：

```
── Polonium 本体（.po → RISC-V の .o）
── ランタイムの核（libc なし）
── カーネル側のフック（UART と bump allocator）
── 起動アセンブリ
── リンク
```

---

## 32.6 つまずき 3 つ

**⚠️ ① `-mcmodel=medany` が要る。**
既定（medlow）は「アドレスは下位 2GB に収まる」前提でコードを作ります。
0x80000000 に置くと届かず、`relocation truncated to fit` で落ちます。

**⚠️ ② シンボル名はモジュール名で決まる。**
`kernel/main.po` にしたら `@main.main` になり、`boot.s` の `call kernel.main` が
リンクできませんでした。**ファイル名がそのまま ABI になります**（第13章の帰結）。
`kernel/kernel.po` に改名して解決。

**⚠️ ③ zsh は変数を単語に分けない。**
`clang $RVFLAGS` と書いても、zsh では**フラグ全体が 1 つの引数**として渡ります
（bash とはここが違う）。`invalid arch name 'rv64g -mabi=lp64 ...'` で気づきました。
Makefile の中では起きませんが、手で試すときは注意。

---

## 32.7 動作確認

`tests/qemu.sh` が、**シリアルに出た文字列**で判定します。

```bash
$ make qemu-test
  ok    Polonium kernel on RISC-V (virt)
  ok    1 から 10 までの合計: 55
  ok    v2 の言葉: raises
  ...
★ ベアメタルの RISC-V で Polonium のカーネルが動きました
```

**⚠️ 道具が無い環境ではスキップして緑にします。**
`qemu-system-riscv64` も `riscv64-elf-ld` も、あるとは限らないからです。
**「動かせないから落とす」ではなく「動かせないことを言って先へ進む」**のが、
教材のテストとして正しい態度です。

---

## 32.8 まとめと次章の予告

この章でやったこと：

- ターゲットを **RISC-V** に決めた（道具の有無と、起動の単純さ）
- 起動アセンブリ・リンカスクリプト・フックの実装（合計 100 行ほど）を書いた
- **Polonium で書いたカーネルが、OS 無しで動いた**
- 出力をテストで固定した（`make qemu-test`）

次章（第33章）は**割り込み**です。タイマ割り込みを受け取り、
`print` で数えます。**ハンドラの中身も Polonium で書きます。**
