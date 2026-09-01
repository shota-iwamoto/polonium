# Polonium コンパイラ (stage0) のビルド
#
# 使い方:
#   make            コンパイラをビルド
#   make test       テストを全部実行（C 版のテスト + 解放の検査 + セルフホストの検証）
#   make drop-asan  --drop で生成したプログラムを AddressSanitizer で検査（第25章）
#   make drop-leak  そのうえでリークも数える（Linux のみ。第25章の宿題の残量）
#   make kernel     ベアメタル（RISC-V）のカーネルをビルド（第32章）
#   make qemu       そのカーネルを QEMU で動かす（Ctrl-A X で終了）
#   make qemu-test  カーネルの出力を自動で検証する（第32〜33章）
#   make selfhost-test  Polonium 版と C 版の出力を比較（5 本）
#   make bootstrap      3 段ビルドと不動点の検証（第20章）
#   make bootstrap-test Polonium 製コンパイラでテストを全部通す
#   make asan       AddressSanitizer 付きでビルド（メモリバグ調査用）
#   make install    <prefix>/bin と <prefix>/lib/polonium に入れる（PREFIX=… で変更）
#   make dist       配布用のディレクトリを build/dist に作る
#   make clean      生成物を削除

# ── 動かす環境（Linux / macOS / Windows）──────────────────────
#
# ★ このコンパイラは「LLVM IR のテキストを出して、clang に渡す」作りなので、
#   clang さえあればどの OS でも同じように動きます。
#   OS ごとに違うのは、ここに集めた 4 つだけです。
UNAME_S := $(shell uname -s 2>/dev/null || echo Unknown)
IS_MAC  := $(filter Darwin,$(UNAME_S))
IS_WIN  := $(filter MINGW% MSYS% CYGWIN%,$(UNAME_S))

# Windows（MSYS2 / Git Bash）では実行ファイルに .exe が付きます
ifneq ($(IS_WIN),)
  EXEEXT := .exe
else
  EXEEXT :=
endif

# ★ コンパイラ本体は C11 が通れば何でビルドしても構いません（gcc でも可）。
#   ただし **IR を扱うのは clang** です（LLVM IR のテキストを読めるのは clang だけ）。
#   make CC=gcc のように明示したときは、その指定を尊重します。
ifeq ($(origin CC),default)
  CC := clang
endif
CLANG ?= clang
CFLAGS  := -std=c11 -g -O0 -Wall -Wextra -Wno-unused-parameter
RUNTIME_CFLAGS := -std=c11 -O2 -Wall -Wextra

# ── 言語の名前と拡張子（★ 改名するときはここだけ）───────────
# 言語名は将来変わるかもしれません。名前に依存する値をここに集めてあります。
#   LANG_NAME … 人が読む言語名（診断メッセージなどに出る）
#   LANG_EXT  … ソースの拡張子（ドット込み）
#   LANG_CC   … コンパイラのコマンド名＝生成される実行ファイル名
# 対になる定義: src/langinfo.h（C 版）/ selfhost/langinfo.po（Polonium 版）
LANG_NAME := Polonium
LANG_EXT  := .po
LANG_CC   := poloniumc
# ★ 版番号（--version が出す値）。3 か所を揃えること:
#   ここ / src/langinfo.h の PLC_LANG_VERSION / selfhost/langinfo.po の VERSION
LANG_VERSION := 0.5.1
CFLAGS  += -DPLC_LANG_NAME='"$(LANG_NAME)"' \
           -DPLC_LANG_EXT='"$(LANG_EXT)"' \
           -DPLC_LANG_CC='"$(LANG_CC)"' \
           -DPLC_LANG_VERSION='"$(LANG_VERSION)"'

# ── ターゲット triple の自動取得 ──────────────────────────────
# 生成する LLVM IR に書き込む triple。
#
# ⚠️ `clang -print-target-triple` を使ってはいけません。
#    macOS ではそれが返す値（x86_64-apple-darwin25.5.0）と、clang が実際に
#    IR に書く値（x86_64-apple-macosx26.0.0）が異なり、警告の原因になります。
#    「clang 自身に空の C ファイルの IR を吐かせて、そこから抜き出す」のが確実です。
#    ⚠️ Windows には /dev/null が無いことがあるので、空ファイルを作って渡します。
HOST_TRIPLE := $(shell printf '' > .plc-empty.c 2>/dev/null; \
                 $(CLANG) -S -emit-llvm -x c .plc-empty.c -o - 2>/dev/null \
                 | sed -n 's/^target triple = "\(.*\)"$$/\1/p'; \
                 rm -f .plc-empty.c)
CFLAGS  += -DPLC_TARGET_TRIPLE='"$(HOST_TRIPLE)"'

# ── macOS のユニバーサルバイナリ（make UNIVERSAL=1）─────────────
#
# ★ 配布物を Intel Mac と Apple Silicon の**両方で動かす**ための指定です。
#   1 つの実行ファイルに 2 つの機械語を入れます（Mach-O の fat 形式）。
#
# ⚠️ **triple も 2 つ要ります。** 生成する IR に書く triple は
#    「いま動いている側」でなければならないのに、既定では
#    ビルド時に 1 つだけ埋め込まれます。x86_64 の Mac で arm64 の
#    triple を書いた IR を出すと、動かない実行ファイルができます。
#    そこで両方を渡し、C 側（src/codegen.c）で選ばせます。
#
#   普段のビルドには一切影響しません（UNIVERSAL を指定したときだけ）。
ifeq ($(UNIVERSAL),1)
  ARCHFLAGS := -arch x86_64 -arch arm64
  CFLAGS  += $(ARCHFLAGS)
  RUNTIME_CFLAGS += $(ARCHFLAGS)
  triple_for = $(shell printf '' > .plc-empty.c 2>/dev/null; \
                 $(CLANG) -arch $(1) -S -emit-llvm -x c .plc-empty.c -o - 2>/dev/null \
                 | sed -n 's/^target triple = "\(.*\)"$$/\1/p'; \
                 rm -f .plc-empty.c)
  CFLAGS  += -DPLC_TARGET_TRIPLE_X86_64='"$(call triple_for,x86_64)"' \
             -DPLC_TARGET_TRIPLE_ARM64='"$(call triple_for,arm64)"'
endif

# ── LLVM のツール（opt / lli / llvm-as / ld.lld）──────────────
#
# ★ 探す順番：① Homebrew（macOS）→ ② llvm-config → ③ PATH。
#   Linux では distro の LLVM がそのまま PATH にあります。
LLVM_BIN := $(shell brew --prefix llvm 2>/dev/null)/bin
ifeq ($(wildcard $(LLVM_BIN)/opt),)
  LLVM_BIN := $(shell llvm-config --bindir 2>/dev/null)
endif
ifeq ($(wildcard $(LLVM_BIN)/opt),)
  LLVM_BIN := $(patsubst %/,%,$(dir $(shell which opt 2>/dev/null)))
endif
OPT      := $(LLVM_BIN)/opt
LLI      := $(LLVM_BIN)/lli
LLVM_AS  := $(LLVM_BIN)/llvm-as

# ── ランタイム（第9章。第31章で 2 つに分割）─────────────────
# ユーザーのプログラムにリンクされる C のコード。
#
#   core.c   … libc に依存しない核（ベアメタルでもリンクできる）
#   hosted.c … PC 上で動かすときのフック実装 + ファイル入出力など
#
# ⚠️ コンパイラ本体（-O0 -g）とは目的が違うので -O2 でビルドします。
#    ランタイムは「ユーザーのプログラムの一部」として動くからです。
RUNTIME_CORE := runtime/core.c
RUNTIME_HOSTED := runtime/hosted.c

# ★ 静的ライブラリ（.a）にまとめます。
#   ⚠️ 以前は `ld -r`（部分リンク）でしたが、Windows では使えません。
#     `ar` はどの環境にもあり、clang のリンク行にそのまま渡せます。
RUNTIME_OBJ := build/runtime.a
AR ?= ar

# ★ 生成したプログラムをリンクするのに使う clang（実行時に呼ぶ相手）。
#   ⚠️ clang-18 のように名前が違う環境があるので、埋め込みつつ
#     環境変数 PLC_CLANG で上書きできるようにします。
CFLAGS  += -DPLC_CLANG='"$(CLANG)"'

# コンパイラにランタイムの場所を教える。
# ⚠️ stage0 だけの割り切り（ビルドツリー内で完結すればよい）。第20章で見直します。
CFLAGS  += -DPLC_RUNTIME_O='"$(abspath $(RUNTIME_OBJ))"'

# ── 標準ライブラリ（第14章）─────────────────────────────────
# import が探す 2 つ目の場所。Polonium で書かれた lib/*.po があります。
CFLAGS  += -DPLC_LIB_DIR='"$(abspath lib)"'

SRCS    := $(wildcard src/*.c)
OBJS    := $(SRCS:src/%.c=build/%.o)
DEPS    := $(OBJS:.o=.d)
TARGET  := build/$(LANG_CC)$(EXEEXT)

.PHONY: all clean test test-one selfhost-test bootstrap bootstrap-test asan drop-asan drop-leak info

all: $(TARGET) $(RUNTIME_OBJ)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@

# ★ 2 つを 1 つの静的ライブラリにまとめます。
#   こうしておくと、コンパイラ側は「ランタイムは 1 本のファイル」という
#   第9章からの前提のままで済みます（.o でも .a でも clang に渡せます）。
$(RUNTIME_OBJ): $(RUNTIME_CORE) $(RUNTIME_HOSTED) runtime/core.h
	@mkdir -p build
	$(CC) $(RUNTIME_CFLAGS) -c $(RUNTIME_CORE) -o build/core.o
	$(CC) $(RUNTIME_CFLAGS) -c $(RUNTIME_HOSTED) -o build/hosted.o
	$(AR) rcs $@ build/core.o build/hosted.o

# -MMD -MP でヘッダの依存関係を自動生成する。
# これがないと、ヘッダを直したのに再ビルドされず不思議なバグに悩まされます。
build/%.o: src/%.c
	@mkdir -p build
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

-include $(DEPS)

# ── テスト ──────────────────────────────────────────────────
test: $(TARGET) $(RUNTIME_OBJ)
	@tests/run_tests.sh
	@tests/drop_asan.sh
	@tests/selfhost.sh

# 1 ケースだけ実行: make test-one CASE=tests/cases/int_42.po
test-one: $(TARGET) $(RUNTIME_OBJ)
	@tests/run_tests.sh $(CASE)

# ── セルフホストの検証（第16〜19章）─────────────────────────
# Polonium 製のコンパイラ（stage1）が C 版と同じものを出すか。
#   トークン列 → AST → 診断 → IR → 実行結果 の 5 本を比べます。
#   ★ テストケースをそのまま検証データに使います。
selfhost-test: $(TARGET) $(RUNTIME_OBJ)
	@tests/selfhost.sh

# ── ブートストラップ（第20章）───────────────────────────────
# stage1（C 版がビルド）→ stage2（stage1 がビルド）→ stage3（stage2 がビルド）
# stage2 == stage3 なら不動点に到達＝セルフホスト完成。
bootstrap: $(TARGET) $(RUNTIME_OBJ)
	@tests/bootstrap.sh

# Polonium 製コンパイラ（stage2）でテストを全部通す。
#   ★ 「C 版と同じ出力を出す」より強い確認です。
bootstrap-test: bootstrap
	@PLC_CC=$(abspath build/boot/stage2) \
	 PLC_LIB_DIR=$(abspath lib) \
	 PLC_RUNTIME_O=$(abspath $(RUNTIME_OBJ)) \
	 PLC_TARGET_TRIPLE=$(HOST_TRIPLE) \
	 tests/run_tests.sh

# ── 解放（drop）の検査（第25章）─────────────────────────────
# tests/cases/drop_*.po を --drop 付きで生成し、AddressSanitizer 付きで
# リンクして走らせます。**二重解放と解放後の使用**を実行時に捕まえる網です。
drop-asan: $(TARGET)
	@tests/drop_asan.sh

# ★ リークの残り（第25章の宿題）を数える。Linux でだけ動きます
#   （macOS の AddressSanitizer に LeakSanitizer は入っていません）。
drop-leak: $(TARGET)
	@tests/drop_asan.sh --leaks

# ── AddressSanitizer ビルド ─────────────────────────────────
# セグフォの原因が分からないときに使います。
#   make asan && ./build/poloniumc-asan tests/cases/int_42.po
asan: $(RUNTIME_OBJ)
	@mkdir -p build
	$(CC) $(CFLAGS) -fsanitize=address,undefined $(SRCS) -o build/$(LANG_CC)-asan

# ── ベアメタル（第32章〜）──────────────────────────────────
#
# ★ ターゲットは RISC-V（riscv64-unknown-elf）です。
#   決め手は「手元にある道具」でした（第32章 32.1）：
#     - Apple の clang には RISC-V のバックエンドが無い → Homebrew の LLVM を使う
#     - x86 の ELF リンカは無いが、riscv64-elf-ld はある
#     - qemu-system-riscv64 の virt マシンは -bios none で ELF を直接起動できる
#
# 使うもの: brew install llvm riscv64-elf-binutils qemu
LLVM_CLANG := $(LLVM_BIN)/clang
ifeq ($(wildcard $(LLVM_CLANG)),)
  LLVM_CLANG := $(CLANG)
endif

# ★ リンカは環境にあるものを使います。
#   ld.lld（LLVM に付属。Linux で入れやすい）→ riscv64-elf-ld（Homebrew の cross binutils）
RV_LD := $(shell command -v ld.lld 2>/dev/null || command -v riscv64-elf-ld 2>/dev/null \
           || echo riscv64-elf-ld)
RV_TRIPLE  := riscv64-unknown-elf
RV_ARCH    := -march=rv64g -mabi=lp64 -mcmodel=medany -mno-relax
RV_CFLAGS  := --target=$(RV_TRIPLE) $(RV_ARCH) -ffreestanding -O2
KDIR       := build/kernel

.PHONY: kernel qemu qemu-test

kernel: $(TARGET) $(KDIR)/kernel.elf

$(KDIR)/kernel.elf: kernel/kernel.po kernel/boot.s kernel/hooks.c kernel/link.ld                     runtime/core.c runtime/core.h $(TARGET)
	@mkdir -p $(KDIR)
	@echo "── Polonium 本体（.po → RISC-V の .o）"
	PLC_CLANG=$(LLVM_CLANG) PLC_CFLAGS="$(RV_ARCH)" 	  ./$(TARGET) -c kernel/kernel.po -o $(KDIR)/kernel_main.o
	@echo "── ランタイムの核（libc なし）"
	$(LLVM_CLANG) $(RV_CFLAGS) -DPL_FREESTANDING -c runtime/core.c -o $(KDIR)/core.o
	@echo "── カーネル側のフック（UART と bump allocator）"
	$(LLVM_CLANG) $(RV_CFLAGS) -c kernel/hooks.c -o $(KDIR)/hooks.o
	@echo "── 起動アセンブリ"
	$(LLVM_CLANG) --target=$(RV_TRIPLE) $(RV_ARCH) -c kernel/boot.s -o $(KDIR)/boot.o
	@echo "── リンク"
	$(RV_LD) -T kernel/link.ld $(KDIR)/boot.o $(KDIR)/kernel_main.o 	         $(KDIR)/core.o $(KDIR)/hooks.o -o $@ 2>&1 | grep -v "RWX permissions" || true

# QEMU で動かす（Ctrl-A X で抜ける）
qemu: kernel
	qemu-system-riscv64 -machine virt -bios none -kernel $(KDIR)/kernel.elf -nographic

# ★ ベアメタルの自動検証：シリアルに出た文字列で判定します（第32章）。
#   道具（qemu / riscv64-elf-ld）が無い環境ではスキップして緑にします。
qemu-test: kernel
	@tests/qemu.sh

# ── インストールと配布（第31章）──────────────────────────────
#
# ★ 配る形（どの OS でも同じ）:
#     <prefix>/bin/poloniumc
#     <prefix>/lib/polonium/runtime.a
#     <prefix>/lib/polonium/lib/*.po
#
#   コンパイラは「実行ファイルからの相対」でこの 2 つを探すので、
#   展開した場所がどこでも動きます（src/main.c の runtime_o を参照）。
PREFIX ?= /usr/local
DESTDIR ?=

.PHONY: install uninstall dist

install: all
	@mkdir -p "$(DESTDIR)$(PREFIX)/bin" "$(DESTDIR)$(PREFIX)/lib/polonium/lib"
	cp $(TARGET) "$(DESTDIR)$(PREFIX)/bin/"
	cp $(RUNTIME_OBJ) "$(DESTDIR)$(PREFIX)/lib/polonium/"
	cp lib/*$(LANG_EXT) "$(DESTDIR)$(PREFIX)/lib/polonium/lib/"
	@echo "インストールしました: $(DESTDIR)$(PREFIX)/bin/$(LANG_CC)$(EXEEXT)"

uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/bin/$(LANG_CC)$(EXEEXT)"
	rm -rf "$(DESTDIR)$(PREFIX)/lib/polonium"

# 配布用のディレクトリ（そのまま zip / tar.gz にできる形）
DIST_NAME ?= polonium-$(UNAME_S)-$(shell uname -m 2>/dev/null || echo unknown)
dist: all
	rm -rf build/dist/$(DIST_NAME)
	@mkdir -p build/dist/$(DIST_NAME)/bin build/dist/$(DIST_NAME)/lib/polonium/lib
	cp $(TARGET) build/dist/$(DIST_NAME)/bin/
	cp $(RUNTIME_OBJ) build/dist/$(DIST_NAME)/lib/polonium/
	cp lib/*$(LANG_EXT) build/dist/$(DIST_NAME)/lib/polonium/lib/
	cp README.md LICENSE build/dist/$(DIST_NAME)/ 2>/dev/null || 	  cp README.md build/dist/$(DIST_NAME)/
	@echo "配布物: build/dist/$(DIST_NAME)"

# ── 情報表示 ────────────────────────────────────────────────
info:
	@echo "CC           = $(CC)"
	@echo "HOST_TRIPLE  = $(HOST_TRIPLE)"
	@echo "LLVM_BIN     = $(LLVM_BIN)"
	@echo "SRCS         = $(SRCS)"
	@echo "LANG_NAME    = $(LANG_NAME)"
	@echo "LANG_EXT     = $(LANG_EXT)"
	@echo "LANG_CC      = $(LANG_CC)"
	@echo "LANG_VERSION = $(LANG_VERSION)"
	@echo "UNAME_S      = $(UNAME_S)"
	@echo "CLANG        = $(CLANG)"
	@echo "RUNTIME      = $(RUNTIME_OBJ)"
	@echo "UNIVERSAL    = $(UNIVERSAL) $(ARCHFLAGS)"

clean:
	rm -rf build a.out a.out.ll tests/tmp
