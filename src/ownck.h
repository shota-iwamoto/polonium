// ownck.h — 所有権検査（④ ownck。第22章）
//
// ★ 第22章で新設された 4 つ目のパスです。
//
//   ① 字句解析  … 文字の並びが「単語」に分けられるか
//   ② 構文解析  … 単語の並びが「文として成り立つ」か
//   ③ 意味解析  … 型と名前が合っているか
//   ④ 所有権検査 … **移動済みの値を使っていないか**   ← ここ
//   ⑤ コード生成
//
// 🤔 なぜ sema と分けるのか（docs/design/ownership.md 1 節）
//   sema は「型が合うか」を見るパスで、実行順序を考えません。
//   所有権の検査は逆に「どの文が先に走るか」が本質です（データフロー解析）。
//   性質の違う解析を 1 つのパスに混ぜると、両方が読めなくなります。
//
// 第22章で use-after-move（S1）、第23章で借用の保存・返却の禁止（S2）、
// 第24章で可変性と借用の衝突（S5）を見ます。解放（drop）の挿入は第25章です。
#ifndef PLC_OWNCK_H
#define PLC_OWNCK_H

#include <stdbool.h>

#include "module.h"
#include "types.h"

// 検査の振る舞い（コマンドライン option がそのまま入る）。
//
// ★ 昇格のスイッチは**検査ごとに分けます**（決定 D12）。
//   第26章で既存コードを直すとき、通ったものから順にエラーへ上げるためです。
typedef struct {
    bool deny_move;    // --deny-move   （E-MOVE-*   をエラーにする）
    bool deny_borrow;  // --deny-borrow （E-BORROW-* をエラーにする）
    bool deny_mut;     // --deny-mut    （E-MUT-*    をエラーにする）
    bool explain_mut;  // --explain-mut （診断を出さず、変更される実引数を並べる）
} OwnckOptions;

// 全モジュールの関数本体を解析する。
//
// deny_* が false（既定）なら診断は **警告**、
// true なら最初の 1 件でエラー終了します。
//
// ⚠️ 既定を警告にしてあるのは、この章の時点では selfhost/ と lib/ が
//    まだ v1 の参照セマンティクス前提で書かれているためです。
//    書き換えは第26章の仕事で、それまで既存のビルドを止めません。
void ownck_program(Module *mods, const OwnckOptions *opt);

// 所有型か（言語仕様 v2 §2 の分類）。
//   コピー型 : int / bool          … 代入しても元が使える
//   所有型   : str / list[T] / class … 代入で移動する
//
// ★ 型そのものには所有の情報を持たせません（Type はシングルトン共有なので、
//   フラグを足すとポインタ比較による型の同一性が壊れる）。
//   「所有型かどうか」は、このように **型から導く**だけにします。
bool ty_is_owned(Type *t);

#endif  // PLC_OWNCK_H
