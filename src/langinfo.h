// langinfo.h — 言語の「名前」と「拡張子」の唯一の定義場所
//
// ★ 言語名は将来変わるかもしれません。変わったときに直す場所を
//   ここ 1 か所（と Makefile の LANG_* / selfhost/langinfo.po）に閉じ込めます。
//   コンパイラ本体のコードは、名前の文字列を直接書かずに必ずこのマクロを使うこと。
//
// ⚠️ 内部の識別子（PLC_ 接頭辞、ランタイムの pl_ 接頭辞）は
//   **わざと言語名から独立**させてあります。改名しても変える必要はありません。
//   詳しくは docs/design/naming.md を参照。
#ifndef PLC_LANGINFO_H
#define PLC_LANGINFO_H

// Makefile が -DPLC_LANG_NAME=... などで上書きできます（既定はここ）。
#ifndef PLC_LANG_NAME
#define PLC_LANG_NAME "Polonium"      // 人が読む言語名
#endif

#ifndef PLC_LANG_EXT
#define PLC_LANG_EXT ".po"            // ソースファイルの拡張子（ドット込み）
#endif

#ifndef PLC_LANG_CC
#define PLC_LANG_CC "poloniumc"       // コンパイラのコマンド名
#endif

// 拡張子の長さ（"." を含む）。sizeof は終端の '\0' を数えるので 1 引く。
#define PLC_LANG_EXT_LEN (sizeof(PLC_LANG_EXT) - 1)

#endif  // PLC_LANGINFO_H
