# selfhost/ — Polonium 製の Polonium コンパイラ (stage1)

**第16章から書き始め、第20章で完成しました。**
**Polonium コンパイラは自分自身をコンパイルできます**（`make bootstrap`）。

`src/` の C 版と **1:1 で対応**させます。この対応を崩さないでください。
崩すと「C 版のどこを見れば正解がわかるか」が失われます。

| C 版 | Polonium 版 | 章 |
|---|---|---|
| `src/lexer.h` | `selfhost/token.po` | 第16章 ✅ |
| `src/lexer.c` | `selfhost/lexer.po` | 第16章 ✅ |
| `src/parser.c` | `selfhost/parser.po` | 第17章 ✅ |
| `src/ast.c` | `selfhost/ast.po` | 第17章 ✅ |
| `src/sema.c` | `selfhost/sema.po` | 第18章 ✅ |
| `src/diag.c` | `selfhost/diag.po` | 第18章 ✅ |
| `src/types.c` | `selfhost/ast.po` に同居 | 第18章 ✅ |
| `src/module.c` | `selfhost/module.po` | 第18章 ✅ |
| `src/codegen.c` | `selfhost/codegen.po` | 第19章 ✅ |
| `src/main.c` | `selfhost/main.po` | 第20章 ✅ |

## 検証方法

各段階で C 版が「正解」を持っていることを利用します。

```bash
# 第16章：トークン列が一致するか（tests/selfhost.sh が全ファイルで自動比較）
make selfhost-test
#   → トークン列一致 348 件 / AST 一致 311 件 / 型検査 一致 174 件 /
#     IR 一致 174 件 / stage1 の IR で実行して一致 156 件
#
# 1 ファイルだけ見るなら
./build/poloniumc --dump-tokens tests/cases/x.po > /tmp/c.txt
./build/stage1-lexer          tests/cases/x.po > /tmp/m.txt
diff /tmp/c.txt /tmp/m.txt

# 第17章：AST（S 式）が一致するか
./build/poloniumc --dump-ast tests/cases/x.po > /tmp/c.txt
./build/stage1-ast         tests/cases/x.po > /tmp/m.txt
diff /tmp/c.txt /tmp/m.txt

# 第18章：型検査の診断が一致するか（メッセージ全文）
./build/poloniumc --check tests/cases/x.po 2> /tmp/c.txt
./build/stage1-check    tests/cases/x.po 2> /tmp/m.txt
diff /tmp/c.txt /tmp/m.txt

# 第19章：IR が一致するか／その IR が動くか
./build/poloniumc -S       tests/cases/x.po > /tmp/c.ll
./build/stage1-codegen   tests/cases/x.po > /tmp/m.ll
diff /tmp/c.ll /tmp/m.ll
clang /tmp/m.ll build/runtime.o -o /tmp/x && /tmp/x

# 第20章：不動点の検証
make bootstrap        # stage2 == stage3 なら成功
make bootstrap-test   # Polonium 製コンパイラでテストを全部通す
```

**移植は機械的に行ってください。ここで独創性を発揮しないこと。**
アルゴリズムと評価順序を C 版と揃えることで、出力の完全一致を目指せます。

詳細は [../docs/design/self-hosting.md](../docs/design/self-hosting.md)。
