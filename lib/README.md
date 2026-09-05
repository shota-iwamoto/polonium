# lib/ — Polonium 製の標準ライブラリ

**第14章で作りました。**

「Polonium 自身で書けるものは Polonium で書く」方針です。
C ランタイム（`runtime/`）に置くのは、C の機能が必要なものだけに限ります。

内容：

| ファイル | 中身 |
|---|---|
| `strings.po` | 文字列ヘルパ（`substr` / `find` / `split` / `join` / `strip` / `replace` …）。**C は 1 行も無い** |
| `io.po` | ファイル入出力。`extern` をここに閉じ込める |
| `sys.po` | `argv` と外部コマンド実行 |
| `dict.po` | 文字列キーの表（`str → int`。線形探索） |
| `time.po` | 時刻と経過時間（**唯一、ベアメタルで使えないモジュール**） |

`import strings` と書けば、コンパイラが `lib/` から自動で見つけます
（探索場所は「入口ファイルのディレクトリ」と `lib/` の 2 つ。
両方に同じ名前があればエラーです）。

なぜ f-string を言語機能にせずヘルパ関数で済ませるのかは
[../docs/design/self-hosting.md](../docs/design/self-hosting.md) 3.7 節を参照。
