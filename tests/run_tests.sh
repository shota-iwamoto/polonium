#!/bin/bash
# Polonium テストランナー
#
# テストケースは tests/cases/*.po です。期待値はファイル先頭のコメントに書きます。
#
#   # EXIT: 42        → コンパイル・実行して終了コードが 42 であること
#   # OUTPUT: hello   → 標準出力が "hello" であること（複数行は行ごとに書く）
#   # ERROR: メッセージ → コンパイルが失敗し、stderr にその文字列を含むこと
#                       （複数行書くと、そのすべてを含むことを要求する）
#   # TOKENS: INT PUNCT INT NEWLINE EOF
#                     → --dump-tokens のトークン種別の並びが一致すること
#                       （複数行書くと空白で連結して比較する）
#   # WARN: メッセージ → コンパイルは成功し、stderr にその文字列を含むこと
#                       （第22章。既定が警告である検査を確かめるため）
#   # FLAGS: --deny-move
#                     → コンパイラに渡す追加のオプション
#   # EXPLAIN-MUT: 12:5: 'c' が変更されます
#                     → --explain-mut の出力にその文字列を含むこと（第24章）
#   # STAGE0-ONLY: 理由
#                     → C 版（build/poloniumc）でだけ実行する。
#                       ★ 第22章：所有権検査は C 版にしかありません。
#                         Polonium 版へ移植する第29章までは、
#                         make bootstrap-test でこのケースを飛ばします。
#
# 複数ファイル（import）のテストは 1 ケース 1 ディレクトリです（第13章）:
#
#   tests/mods/<ケース名>/main.po   ← 入口。期待値のコメントもここに書く
#   tests/mods/<ケース名>/lexer.po  ← import されるモジュール
#
# ★ 1 ディレクトリにまとめるのは、import の探索場所が
#   「入口ファイルのあるディレクトリ」だからです。tests/cases に置くと、
#   モジュール側のファイルまで単体のテストケースとして拾われてしまいます。
#
# 使い方:
#   tests/run_tests.sh                     全ケース
#   tests/run_tests.sh tests/cases/x.po    1 ケースだけ
#   tests/run_tests.sh tests/mods/y/main.po

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# ★ 第20章：使うコンパイラを差し替えられるようにする。
#   PLC_CC=build/stage2 tests/run_tests.sh とすれば、
#   **Polonium 製コンパイラでテスト全部を通す**ことができます。
PLC_CC="${PLC_CC:-$ROOT/build/poloniumc}"
# ★ Windows（MSYS2）では .exe が付きます
[ -x "$PLC_CC" ] || [ ! -x "$PLC_CC.exe" ] || PLC_CC="$PLC_CC.exe"
TMP="$ROOT/tests/tmp"

if [ ! -x "$PLC_CC" ]; then
    echo "コンパイラが見つかりません: $PLC_CC"
    echo "先に 'make' を実行してください。"
    exit 1
fi

mkdir -p "$TMP"

if [ $# -gt 0 ]; then
    CASES=("$@")
else
    # ソートして順序を安定させる（テスト結果が実行ごとに変わらないように）
    CASES=()
    while IFS= read -r line; do CASES+=("$line"); done < <(ls "$ROOT"/tests/cases/*.po | sort)
    # 複数モジュールのケース（tests/mods/<名前>/main.po）
    if [ -d "$ROOT/tests/mods" ]; then
        while IFS= read -r line; do CASES+=("$line"); done \
            < <(ls "$ROOT"/tests/mods/*/main.po 2>/dev/null | sort)
    fi
fi

pass=0
fail=0
skip=0
failed_names=()

# 色（端末でないときは付けない）
if [ -t 1 ]; then
    C_OK=$'\033[32m'; C_NG=$'\033[31m'; C_DIM=$'\033[2m'; C_END=$'\033[0m'
else
    C_OK=''; C_NG=''; C_DIM=''; C_END=''
fi

report_fail() {
    local name="$1" reason="$2"
    printf "  %sFAIL%s  %s\n" "$C_NG" "$C_END" "$name"
    # 理由をインデントして表示する
    printf "%s" "$reason" | sed 's/^/          /'
    echo
    fail=$((fail + 1))
    failed_names+=("$name")
}

for case_file in "${CASES[@]}"; do
    # 複数モジュールのケースは「ディレクトリ名」で呼ぶ（main.po ばかりになるため）
    case "$case_file" in
        *tests/mods/*) name="$(basename "$(dirname "$case_file")")/main.po" ;;
        *)              name="$(basename "$case_file")" ;;
    esac
    base="$(echo "${name%.po}" | tr '/' '_')"
    exe="$TMP/$base"

    # ── 期待値をヘッダコメントから読み取る ──
    want_exit="$(sed -n 's/^# *EXIT: *//p'   "$case_file" | head -1)"
    # ERROR は複数行書ける。すべてが stderr に含まれることを要求する。
    # 診断メッセージの note: / ヒント: 行まで検証できるようにするため。
    want_error="$(sed -n 's/^# *ERROR: *//p' "$case_file")"
    # OUTPUT は複数行を許す
    want_output="$(sed -n 's/^# *OUTPUT: *//p' "$case_file")"
    # TOKENS は複数行書けるので、空白 1 個で連結して 1 行にする
    want_tokens="$(sed -n 's/^# *TOKENS: *//p' "$case_file" \
                   | tr '\n' ' ' | tr -s ' ' | sed 's/ *$//')"
    # 第22章：警告の検証（コンパイルは成功する）と、追加のオプション
    want_warn="$(sed -n 's/^# *WARN: *//p' "$case_file")"
    want_explain="$(sed -n 's/^# *EXPLAIN-MUT: *//p' "$case_file")"
    extra_flags="$(sed -n 's/^# *FLAGS: *//p' "$case_file" | tr '\n' ' ')"
    stage0_only="$(sed -n 's/^# *STAGE0-ONLY: *//p' "$case_file" | head -1)"

    # ★ C 版でしか動かないケースは、Polonium 版で回すときに飛ばす（第22章）
    if [ -n "$stage0_only" ] && [ "$PLC_CC" != "$ROOT/build/poloniumc" ]; then
        printf "  %sskip%s  %s %s(%s)%s\n" "$C_DIM" "$C_END" "$name" \
               "$C_DIM" "$stage0_only" "$C_END"
        skip=$((skip + 1))
        continue
    fi

    if [ -z "$want_exit" ] && [ -z "$want_error" ] && [ -z "$want_output" ] \
       && [ -z "$want_tokens" ] && [ -z "$want_warn" ] && [ -z "$want_explain" ]; then
        report_fail "$name" \
            "期待値のコメント（# EXIT: / # OUTPUT: / # ERROR: / # TOKENS: / # WARN: / # EXPLAIN-MUT:）がありません"
        continue
    fi

    # ── TOKENS: 字句解析器の出力だけを検証する ──
    #
    # ★ 構文解析より前の段階を独立してテストできます。
    #   NEWLINE / INDENT / DEDENT のような仮想トークンは、それを消費する
    #   構文（if / def）が無くても、ここで正しさを確認できます。
    #   第16章では、Polonium 版字句解析器の検証にこの仕組みを使います。
    if [ -n "$want_tokens" ]; then
        actual_tokens="$("$PLC_CC" --dump-tokens "$case_file" 2>/dev/null \
                         | awk '{print $2}' | tr '\n' ' ' | tr -s ' ' | sed 's/ *$//')"
        if [ "$actual_tokens" != "$want_tokens" ]; then
            report_fail "$name" "トークン列が期待と違います
期待: $want_tokens
実際: $actual_tokens"
            continue
        fi
        # TOKENS だけのケースはここで合格
        if [ -z "$want_exit" ] && [ -z "$want_error" ] && [ -z "$want_output" ] \
           && [ -z "$want_warn" ]; then
            printf "  %sok%s    %s %s(tokens)%s\n" "$C_OK" "$C_END" "$name" \
                   "$C_DIM" "$C_END"
            pass=$((pass + 1))
            continue
        fi
    fi

    # ── EXPLAIN-MUT: 変更される実引数の一覧を検証する（第24章）──
    #
    # ★ --dump-tokens を TOKENS: で検証するのと同じ形です。
    #   「表示するだけ」の option は、その表示そのものをテストします。
    if [ -n "$want_explain" ]; then
        actual_explain="$("$PLC_CC" --explain-mut "$case_file" 2>/dev/null)"
        missing=""
        while IFS= read -r want; do
            [ -z "$want" ] && continue
            printf '%s' "$actual_explain" | grep -qF -- "$want" || missing="$missing
  - $want"
        done <<EOF_EXPLAIN
$want_explain
EOF_EXPLAIN
        if [ -n "$missing" ]; then
            report_fail "$name" "--explain-mut の出力に含まれていない期待文字列があります:$missing
実際の出力:
$actual_explain"
            continue
        fi
        if [ -z "$want_exit" ] && [ -z "$want_error" ] && [ -z "$want_output" ] \
           && [ -z "$want_warn" ]; then
            printf "  %sok%s    %s %s(explain-mut)%s\n" "$C_OK" "$C_END" "$name" \
                   "$C_DIM" "$C_END"
            pass=$((pass + 1))
            continue
        fi
    fi

    # ── コンパイル ──
    # shellcheck disable=SC2086  # extra_flags は複数のオプションに分かれてほしい
    compile_err="$("$PLC_CC" $extra_flags "$case_file" -o "$exe" 2>&1 >/dev/null)"
    compile_rc=$?

    # ── ERROR: コンパイルが失敗し、指定文字列を含むことを期待 ──
    if [ -n "$want_error" ]; then
        if [ "$compile_rc" -eq 0 ]; then
            report_fail "$name" "コンパイルが成功してしまいました（失敗を期待）
期待するエラー: $want_error"
            continue
        fi

        # 期待する文字列を 1 行ずつ確認する
        missing=""
        nchecks=0
        while IFS= read -r want; do
            [ -z "$want" ] && continue
            nchecks=$((nchecks + 1))
            printf '%s' "$compile_err" | grep -qF -- "$want" || missing="$missing
  - $want"
        done <<EOF_WANT
$want_error
EOF_WANT

        if [ -n "$missing" ]; then
            report_fail "$name" "エラー出力に含まれていない期待文字列があります:$missing
実際の出力:
$compile_err"
        else
            printf "  %sok%s    %s %s(error x%d)%s\n" "$C_OK" "$C_END" "$name" \
                   "$C_DIM" "$nchecks" "$C_END"
            pass=$((pass + 1))
        fi
        continue
    fi

    # ── ここから先はコンパイル成功を期待 ──
    if [ "$compile_rc" -ne 0 ]; then
        report_fail "$name" "コンパイルに失敗しました
$compile_err"
        continue
    fi

    # ── WARN: 成功したうえで、警告が出ていることを期待（第22章）──
    if [ -n "$want_warn" ]; then
        missing=""
        while IFS= read -r want; do
            [ -z "$want" ] && continue
            printf '%s' "$compile_err" | grep -qF -- "$want" || missing="$missing
  - $want"
        done <<EOF_WARN
$want_warn
EOF_WARN
        if [ -n "$missing" ]; then
            report_fail "$name" "警告に含まれていない期待文字列があります:$missing
実際の出力:
$compile_err"
            continue
        fi
    fi

    # ── 実行 ──
    # ★ Windows（MSYS2）では実行ファイルに .exe が付きます
    [ -x "$exe" ] || [ ! -x "$exe.exe" ] || exe="$exe.exe"
    actual_output="$("$exe" 2>/dev/null)"
    actual_exit=$?

    ok=1
    reason=""

    if [ -n "$want_exit" ] && [ "$actual_exit" -ne "$want_exit" ]; then
        ok=0
        reason="終了コードが違います: 期待 $want_exit, 実際 $actual_exit"
    fi

    if [ -n "$want_output" ] && [ "$actual_output" != "$want_output" ]; then
        ok=0
        reason="$reason
標準出力が違います:
--- 期待 ---
$want_output
--- 実際 ---
$actual_output"
    fi

    if [ "$ok" -eq 1 ]; then
        detail="exit=$actual_exit"
        printf "  %sok%s    %s %s(%s)%s\n" "$C_OK" "$C_END" "$name" "$C_DIM" "$detail" "$C_END"
        pass=$((pass + 1))
    else
        report_fail "$name" "$reason"
    fi
done

echo
echo "────────────────────────────────"
if [ "$fail" -eq 0 ]; then
    printf "%s全 %d 件パス%s\n" "$C_OK" "$pass" "$C_END"
    [ "$skip" -gt 0 ] && printf "%s（%d 件スキップ）%s\n" "$C_DIM" "$skip" "$C_END"
    exit 0
else
    printf "%s%d 件パス / %d 件失敗 / %d 件スキップ%s\n" \
           "$C_NG" "$pass" "$fail" "$skip" "$C_END"
    for n in "${failed_names[@]}"; do echo "  - $n"; done
    exit 1
fi
