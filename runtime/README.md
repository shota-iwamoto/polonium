# runtime/ — C 製ランタイムライブラリ

**第9章から使います。** 現在は空です。

生成される LLVM IR を単純に保つため、制御フローを含む処理は
ここに C 関数として置き、IR 側は `call` 1 行にします
（[../docs/design/ir-conventions.md](../docs/design/ir-conventions.md) 規約 R10）。

予定している内容：

- `pl_alloc` — ゼロ初期化つきメモリ確保（失敗時は即終了）
- `pl_print_int` / `pl_print_str` / `pl_print_bool` — `print` の実体
- `pl_str_concat` / `pl_str_eq` / `pl_str_len` / `pl_str_sub` — 文字列操作
- `pl_str_from_int` / `pl_int_from_str` — 相互変換
- `pl_list_new` / `pl_list_push_*` / `pl_list_get_*` — `list[T]`（第10章）
- `pl_check_not_none` — None 参照の親切なエラー（第12章）

API の一覧は [../docs/design/memory-model.md](../docs/design/memory-model.md) 第4節にあります。
