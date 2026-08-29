#!/bin/bash
# 解放（drop）の検査 — 第25章
#
# ★ tests/cases/drop_*.po と rc_*.po を **--drop 付き**で生成し、AddressSanitizer を
#   リンクして実行します。通常のテスト（run_tests.sh）では観測できない
#   **二重解放・解放後の使用**を、実行時に捕まえるための網です。
#
# ⚠️ LeakSanitizer は macOS では使えません（Linux のみ）。
#    リークは「壊れない」種類の間違いなので、まず壊れないことを確かめます。
#
# 使い方:
#   tests/drop_asan.sh                     drop_*.po を全部
#   tests/drop_asan.sh tests/cases/x.po    1 本だけ

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PLC_CC="${PLC_CC:-$ROOT/build/poloniumc}"
TMP="$ROOT/tests/tmp"
mkdir -p "$TMP"

if [ $# -gt 0 ]; then
    CASES=("$@")
else
    CASES=()
    while IFS= read -r line; do CASES+=("$line"); done \
        < <(ls "$ROOT"/tests/cases/drop_*.po "$ROOT"/tests/cases/rc_*.po 2>/dev/null | sort)
fi

if [ -t 1 ]; then
    C_OK=$'\033[32m'; C_NG=$'\033[31m'; C_DIM=$'\033[2m'; C_END=$'\033[0m'
else
    C_OK=''; C_NG=''; C_DIM=''; C_END=''
fi

pass=0
fail=0

for f in "${CASES[@]}"; do
    name="$(basename "$f")"
    base="${name%.po}"

    # ★ --keep-ll で .ll を残させます（-S は複数モジュールを 1 本に並べるだけで
    #   リンクできないため。import を使うケースもここで検査したい）。
    rm -f "$TMP/$base.drop".*.ll
    if ! "$PLC_CC" --drop --keep-ll "$f" -o "$TMP/$base.drop" 2>"$TMP/$base.warn"; then
        printf "  %sFAIL%s  %s（IR の生成に失敗）\n" "$C_NG" "$C_END" "$name"
        head -5 "$TMP/$base.warn" | sed 's/^/          /'
        fail=$((fail + 1))
        continue
    fi

    # ★ ランタイムも一緒に ASan でビルドする（解放するのはランタイム側なので）
    if ! clang -fsanitize=address -O0 "$TMP/$base.drop".*.ll "$ROOT/runtime/runtime.c" \
            -o "$TMP/$base.asan" 2>"$TMP/$base.link"; then
        printf "  %sFAIL%s  %s（リンクに失敗）\n" "$C_NG" "$C_END" "$name"
        head -5 "$TMP/$base.link" | sed 's/^/          /'
        fail=$((fail + 1))
        continue
    fi

    out="$("$TMP/$base.asan" 2>"$TMP/$base.asan.err")"
    rc=$?

    # ASan は問題を見つけると stderr に "ERROR: AddressSanitizer" を出して
    # 終了コードを 1 にします。期待する終了コードはケースごとに違うので、
    # 「ASan の報告が出ていないこと」を合格条件にします。
    if grep -q "AddressSanitizer" "$TMP/$base.asan.err"; then
        printf "  %sFAIL%s  %s\n" "$C_NG" "$C_END" "$name"
        head -8 "$TMP/$base.asan.err" | sed 's/^/          /'
        fail=$((fail + 1))
        continue
    fi

    printf "  %sok%s    %s %s(asan, exit=%d)%s\n" "$C_OK" "$C_END" "$name" \
           "$C_DIM" "$rc" "$C_END"
    pass=$((pass + 1))
done

echo
echo "────────────────────────────────"
if [ "$fail" -eq 0 ]; then
    printf "%s全 %d 件が AddressSanitizer で問題なし%s\n" "$C_OK" "$pass" "$C_END"
    exit 0
else
    printf "%s%d 件パス / %d 件失敗%s\n" "$C_NG" "$pass" "$fail" "$C_END"
    exit 1
fi
