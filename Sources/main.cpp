// ファンタジーライフ(日本語版) アイテムエディタ  (CTRPluginFramework / .3gx)
//
// 海外版PLGのItemEditorと同じ仕組み:
//   1) ゲームの装備処理2か所にフックを入れ、装備したアイテムの構造体アドレスを取得
//   2) Yボタンでエディタを開く
//   3) 構造体を書き換え、「外して装備し直す」と反映される
//
// 日本語版のフック位置は、海外版コードとの命令列照合で求めたもの。
// 位置の命令列が違う場合は、フックを入れずに警告を出す。
// フックは A(0x2D33F8) のみ。ライフ道具もAで同じレコードが取れる(gdb確認済み)。
// フックB(0x2BA928)は、ライフ道具の装備でクラッシュしたため削除した。
//
// 構造体のオフセットは、ユーザー提供のARコード(かばん 0x6DDF2C から 0x3C 刻み)と
// PLGのエディタが使う位置(+0x1E, +0x20 など)に一致している。

#include <3ds.h>
#include "csvc.h"
#include <CTRPluginFramework.hpp>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// ------------------------------------------------------------
//  名前の表 (ID → 表示名)。末尾は name == nullptr。
//  ItemNames.hpp があれば、その中の g_itemNames / g_effectNames を使う。
//  無ければ空の表になり、IDの直接入力だけになる。
// ------------------------------------------------------------
struct NameEntry
{
    u16         id;
    const char *name;
};

#if defined(__has_include)
#  if __has_include("ItemNames.hpp")
#    include "ItemNames.hpp"
#    define HAVE_NAME_TABLES 1
#  endif
#endif
#ifndef HAVE_NAME_TABLES
static const NameEntry g_itemNames[]   = { { 0xFFFF, nullptr } };
static const NameEntry g_effectNames[] = { { 0xFFFF, nullptr } };
#endif
#include "Life.hpp"
#include "Craft.hpp"
#include "Mage.hpp"      // 魔法使い関連はすべてこのファイル

// ------------------------------------------------------------
//  フック本体 (アセンブラ)  ※PLGのコールバックと同じ動作
//  呼び出し側で「found-4」に str pc,[sp,#-4]! を書いてあるので、
//  スタックには (found+4) が積まれている。置き換えた3命令をここで再実行して、
//  found+8 に戻る。
// ------------------------------------------------------------
extern "C"
{
    void jp_hookA(void);
    void jp_hookB2(void);

    volatile u32 g_capPtr   = 0;
    volatile u32 g_capCount = 0;

    void jp_captureA(u32 r3) { g_capPtr = r3 + 4; g_capCount = g_capCount + 1; }
    void jp_captureB(u32 r4) { g_capPtr = r4 + 4; g_capCount = g_capCount + 1; }

    // ---- エリアワープ用 (実機未検証) ----
    void jp_hookWarp(void);
    volatile u32 g_warpPending = 0;
    volatile u32 g_warpArea    = 0;
    volatile u32 g_warpLink    = 0;
    volatile u32 g_warpUseLink = 1;   // 1 = 入口番号を渡す (0 も指定できる) / 0 = 渡さない (標準の入口)
    volatile u32 g_warpTask    = 0;   // 毎フレーム更新される。エリアID = g_warpTask + 0x6B2


    // ---- 保存した位置へ置く (到着後) ----
    // プレイヤーのデータ: [[0x68BBB8] + 0x7828] (クラス cAppUnit, vptr 0x671EB8)。
    // エリアを移るたびにアドレスが変わるので、毎回この経路で取り直す。
    // 座標 (x,y,z) は同じ値が複数の場所にある。GDBで、全部に書くと即座に移動し、
    // +0x7C だけだと、スティック操作のあとに反映されることを確認した。
    // 各コピーには「基準の座標との差」を足して書く (コピーごとに高さのずれがあるため)。
    // 「ゲームが初期位置に置く」ことを検出してから書く。タイミングが分からないので、
    //  ・新しいプレイヤーのデータが出来て、位置が大きく動いたあと (= 配置された)、
    //  ・位置が POS_SETTLE_FRAMES フレーム止まったら、書き始める。
    //  配置が見えなかった場合は、POS_FALLBACK_FRAMES 待ってから書く。
    //  書き始めたら、POS_HOLD_FRAMES フレームの間、毎フレーム書き続ける (あとからの上書きを直す)。
    #define POS_JUMP_DIST       2.0f     // 1フレームでこれ以上動いたら「配置された」とみなす
    #define POS_SETTLE_FRAMES   20
    #define POS_FALLBACK_FRAMES 10
    #define POS_LONG_FRAMES     900
    #define POS_REUSE_FRAMES    600      // プレイヤーのアドレスが変わらない場合の待ち
    #define POS_HOLD_FRAMES     120
    #define POS_TIMEOUT_FRAMES  (60 * 40)
    #define POS_REC_MAX         900      // 経過の記録 (「位置ワープの記録を保存」で書き出せる)

    volatile u32 g_posMode     = 0;      // 0 = なし / 1 = エリアの読み込み待ち / 2 = 書き込み中
    volatile u32 g_posArea     = 0;
    volatile u32 g_posOldObj   = 0;
    volatile u32 g_posFrames   = 0;
    volatile u32 g_posSince    = 0;
    volatile u32 g_posSettle   = 0;
    volatile u32 g_posJumps    = 0;
    volatile u32 g_posSawGap   = 0;
    volatile u32 g_posHold     = 0;
    volatile u32 g_posHaveLast = 0;
    volatile float g_posLast[3] = { 0.0f, 0.0f, 0.0f };
    volatile float g_posXYZ[3]  = { 0.0f, 0.0f, 0.0f };

    volatile u32 g_recN = 0;
    static u8    g_recMode[POS_REC_MAX];
    static u16   g_recArea[POS_REC_MAX];
    static u8    g_recState[POS_REC_MAX];
    static u32   g_recObj[POS_REC_MAX];
    static float g_recPos[POS_REC_MAX][3];

    static float PosAbs(float v) { return v < 0.0f ? -v : v; }
    static bool  PosSane(float v) { return v > -20000.0f && v < 20000.0f; }

    // プレイヤーのデータを取る。クラスの種類 (vptr) では見分けず、
    // 座標のコピー同士 (+0x7C と +0x40) が一致していることで確かめる。
    static u32 PosPlayerObj(void)
    {
        const u32 mgr = *(volatile u32 *)0x68BBB8;
        if (mgr < 0x08000000u || mgr >= 0x1F000000u || (mgr & 3))
            return 0;
        const u32 obj = *(volatile u32 *)(mgr + 0x7828);
        if (obj < 0x08000000u || obj >= 0x1F000000u || (obj & 3))
            return 0;
        const float x = *(volatile float *)(obj + 0x7C), y = *(volatile float *)(obj + 0x80), z = *(volatile float *)(obj + 0x84);
        const float x2 = *(volatile float *)(obj + 0x40), z2 = *(volatile float *)(obj + 0x60);
        if (!PosSane(x) || !PosSane(y) || !PosSane(z) || !PosSane(x2) || !PosSane(z2))
            return 0;
        if (PosAbs(x - x2) > 1.0f || PosAbs(z - z2) > 1.0f)
            return 0;
        return obj;
    }

    static void PosApply(u32 obj, float tx, float ty, float tz)
    {
        static const u16 offX[] = { 0x7C, 0x40, 0x10, 0x20C, 0x218 };
        static const u16 offY[] = { 0x80, 0x50, 0x20, 0x210, 0x21C };
        static const u16 offZ[] = { 0x84, 0x60, 0x30, 0x214, 0x220 };

        const float cx = *(volatile float *)(obj + 0x7C);
        const float cy = *(volatile float *)(obj + 0x80);
        const float cz = *(volatile float *)(obj + 0x84);
        if (!PosSane(cx) || !PosSane(cy) || !PosSane(cz))
            return;                                  // 変な値 (NaNなど) のときは書かない

        const float dx = tx - cx, dy = ty - cy, dz = tz - cz;
        if (dx == 0.0f && dy == 0.0f && dz == 0.0f)
            return;
        for (unsigned i = 0; i < sizeof(offX) / sizeof(offX[0]); ++i) *(volatile float *)(obj + offX[i]) += dx;
        for (unsigned i = 0; i < sizeof(offY) / sizeof(offY[0]); ++i) *(volatile float *)(obj + offY[i]) += dy;
        for (unsigned i = 0; i < sizeof(offZ) / sizeof(offZ[0]); ++i) *(volatile float *)(obj + offZ[i]) += dz;
    }

    static void PosResetDetect(void)
    {
        g_posSince    = 0;
        g_posSettle   = 0;
        g_posJumps    = 0;
        g_posHaveLast = 0;
    }

    // ゲーム本体のスレッドから、毎フレーム呼ばれる。
    static void PosTick(u32 task)
    {
        if (!g_posMode)
            return;

        const u32 obj   = PosPlayerObj();
        const u32 area  = *(volatile u16 *)(task + 0x6B2);
        const u32 state = *(volatile u8 *)(task + 624);
        float p[3] = { 0.0f, 0.0f, 0.0f };
        if (obj)
        {
            p[0] = *(volatile float *)(obj + 0x7C);
            p[1] = *(volatile float *)(obj + 0x80);
            p[2] = *(volatile float *)(obj + 0x84);
        }

        g_posFrames = g_posFrames + 1;

        const u32 n = g_recN;                        // 経過の記録
        if (n < POS_REC_MAX)
        {
            g_recMode[n]   = (u8)g_posMode;
            g_recArea[n]   = (u16)area;
            g_recState[n]  = (u8)state;
            g_recObj[n]    = obj;
            g_recPos[n][0] = p[0];
            g_recPos[n][1] = p[1];
            g_recPos[n][2] = p[2];
            g_recN         = n + 1;
        }

        if (g_posMode == 1)                          // エリアの読み込み待ち
        {
            if (g_posFrames > POS_TIMEOUT_FRAMES)
            {
                g_posMode = 0;                       // 間に合わなかったので、あきらめる
                return;
            }
            if (!obj)
            {
                g_posSawGap = 1;                     // 読み込み中は、プレイヤーのデータが一時的になくなる
                PosResetDetect();
                return;
            }
            bool fresh = (obj != g_posOldObj) || g_posSawGap;
            if (!fresh && g_posFrames > POS_REUSE_FRAMES)
                fresh = true;
            if (area != g_posArea || !fresh)
            {
                PosResetDetect();
                return;
            }

            g_posSince = g_posSince + 1;
            if (g_posHaveLast)
            {
                float m = PosAbs(p[0] - g_posLast[0]);
                const float my = PosAbs(p[1] - g_posLast[1]);
                const float mz = PosAbs(p[2] - g_posLast[2]);
                if (my > m) m = my;
                if (mz > m) m = mz;
                if (m > POS_JUMP_DIST)
                {
                    g_posJumps  = g_posJumps + 1;    // ゲームが位置を置いた
                    g_posSettle = 0;
                }
                else if (m == 0.0f)
                    g_posSettle = g_posSettle + 1;
                else
                    g_posSettle = 0;
            }
            g_posLast[0] = p[0];
            g_posLast[1] = p[1];
            g_posLast[2] = p[2];
            g_posHaveLast = 1;

            const bool settled = g_posSettle >= POS_SETTLE_FRAMES;
            if ((settled && (g_posJumps >= 1 || g_posSince >= POS_FALLBACK_FRAMES)) || g_posSince >= POS_LONG_FRAMES)
            {
                g_posHold = 0;
                g_posMode = 2;
            }
            return;
        }

        // g_posMode == 2: 書き込み中
        if (obj)
            PosApply(obj, g_posXYZ[0], g_posXYZ[1], g_posXYZ[2]);
        g_posHold = g_posHold + 1;
        if (g_posHold >= POS_HOLD_FRAMES)
            g_posMode = 0;
    }

    // ゲーム本体のスレッドから、毎フレーム呼ばれる (task = フィールドタスク)。
    // 予約があれば 0x21EDCC(入口番号) を呼び、「エリアID+1」を返す(0 = 何もしない)。
    // g_warpUseLink が 0 のときは 0x21EDCC を呼ばない (GDBで確認した「標準の入口」に出る動作)。
    // 入口番号 0 も、エリアID 0 も、普通の値として扱う。
    u32 WarpPrepare(u32 task)
    {
        g_warpTask = task;
        PosTick(task);
        if (!g_warpPending)
            return 0;
        g_warpPending = 0;
        if (g_warpUseLink)
        {
            typedef void (*SetArrivalLinkFn)(u32);
            ((SetArrivalLinkFn)0x21EDCC)(g_warpLink);
        }
        return g_warpArea + 1;   // エリアID+1 (0 = ワープなし と区別するため)
    }

}

asm(R"(
    .arm
    .text
    .align 2

    .global jp_hookA
    .type jp_hookA, %function
jp_hookA:
    ldr   r6, [sp], #4
    sub   sp, sp, #0xC
    mov   r7, r0
    mvn   r0, #0
    push  {r0-r6}
    mov   r0, r3
    bl    jp_captureA
    pop   {r0-r6}
    add   r6, r6, #4
    mov   pc, r6

    @ ---- ライフ道具用(実験): 0x2BA9AC に入れる関数の途中フック ----
    @ 置き換えた3命令 (0x2BA9A8/AC/B0) を再現し、0x2BA9B4 に戻る。
    @ この時点で r4 = 装備されるレコード。レジスタは全部保存する。
    .global jp_hookB2
    .type jp_hookB2, %function
jp_hookB2:
    push  {r0-r3, r12, lr}
    ldr   r0, [sp, #24]          @ 積まれている戻り先 (X+4)
    add   r0, r0, #4             @ X+8 に直す
    str   r0, [sp, #24]
    mov   r0, r4
    bl    jp_captureB
    pop   {r0-r3, r12, lr}
    str   r4, [r5, r6, lsl #2]   @ 0x2BA9A8 の元命令
    ldr   r0, [r4, #0xc]         @ 0x2BA9AC
    orr   r0, r0, #1             @ 0x2BA9B0
    ldr   pc, [sp], #4           @ X+8 へ戻りつつスタックを戻す

    @ ---- エリアワープ用: 0x5A2AB8 (フィールドタスクの毎フレーム更新) ----
    @ CTRPFのHookクラスは使わず、プラグイン側で2語だけ直接書き換える:
    @     0x5A2AB8: ldr pc, [pc, #-4]
    @     0x5A2ABC: .word jp_hookWarp
    @ 置き換わった2命令 (0x5A2AB8 push {r4-r11,lr} / 0x5A2ABC mov r4,r0) をここで再現し、
    @ 0x5A2AC0 (元の ldr r6,[pc,#3016]。書き換えていない) へ戻る。
    @ ldr pc は lr を変えないので、入口の状態 (r0=タスク, lr=呼び出し元, s0=経過時間) が
    @ そのまま届く。C関数 (WarpPrepare) は s0〜s15 を壊すので d0-d7 を退避してから呼ぶ。
    .global jp_hookWarp
    .type jp_hookWarp, %function
jp_hookWarp:
    push  {r0-r3, r12, lr}       @ 24バイト: 8バイト境界を保つ
    vpush {d0-d7}                @ 元の関数は s0 (float引数) を使うので退避
    bl    WarpPrepare            @ r0 = エリアID+1 (0 = ワープなし)
    vpop  {d0-d7}                @ s0〜s15 を元に戻す
    str   r0, [sp, #16]          @ 保存した r12 の位置に結果を入れる
    pop   {r0-r3, r12, lr}       @ r12 = エリアID, r0 = タスク, lr = 元のlr
    cmp   r12, #0
    beq   1f
    sub   r12, r12, #1           @ エリアID+1 → エリアID (0 も有効)
    mov   r1, r12                @ GDBで成功したのと同じ設定
    mov   r2, #0
    mov   r3, r12
    ldr   pc, =0x5A4854          @ 末尾ジャンプ: 元の呼び出し元へ戻る
1:  push  {r4-r11, lr}           @ 0x5A2AB8 の元命令
    mov   r4, r0                 @ 0x5A2ABC の元命令
    ldr   pc, =0x5A2AC0          @ 続きの元コードへ

    .ltorg
)");

namespace CTRPluginFramework
{
    static constexpr u32 HOOK_WARP = 0x005A2AB8; // エリアワープ: フィールドタスク更新関数の入口
}

// アイテムエディタ (編集・生成・削除・一括変更)。ここより後ろの機能も、この中の関数を使う。
#include "ItemEditor.hpp"

namespace CTRPluginFramework
{
    // ------------------------------------------------------------
    //  エリアワープ (実機未検証)
    //  移動先 = {表示名, エリアID, 出入口の接続番号}
    //  (エリアID, 接続番号) は実際の扉の記録から取った組み合わせ。
    //  ★ は GDB で実際に移動を確認したもの。それ以外は未確認。
    //  場所名は通った順番からの推定。
    // ------------------------------------------------------------
    struct WarpDest { const char *label; u16 area; u16 link; };

    // 規則 (ユーザーの実機テストで判明):
    //   ワープの組 (area, link) は「その行き先 area へ向かう、番号 link の扉」の前に出る。
    //   出る場所は、その扉があるエリア (= 行き先の隣のエリア) になる。
    //   例: (25,39) = 郊外南の、パーテル大平原東へ向かう扉の前
    //       ( 3,39) = パーテル大平原東の、郊外南へ向かう扉の前
    //   ★ はこの規則で実機確認したもの。他は規則からの推測 (場所名は通った順番からの推定)。
    //   エリアXの出入口に出たいときは、Xの中でその扉を通ったときの記録 (行き先, 番号) を使う。
    static const WarpDest g_warpDests[] =
    {
        { "パーテル大平原 東", 3, 39 },
        { "クルブルク城 謁見の間",143,27 },
        { "王国兵士の詰所",2,15 },
        { "バラード酒場",251,2 },
        { "ククの家",33,36 },
        { "イムカの家",33,37 },
        { "王立クルブルク工房",1,21 },
        { "コンガス食堂",152,5 },
        { "王立クルブルク図書館",2,16 },
        { "ヘイホフの家",33,38 },
        { "アイザックの家",32,34 },
        { "いにしえの塔 試練の間",97,136 },
        { "太古の試練その1",98,139 },
        { "闇の試練その1",98,140 },
        { "光の試練その1",98,141 },
        { "→クルブルク大通り へ向かう扉の前 (2/7)", 2, 7 },
        { "★郊外南: パーテル大平原東へ向かう扉の前 (25/39)", 25, 39 },
        { "→パーテル大平原東 ハニワ洞窟前 へ向かう扉の前 (25/128)", 25, 128 },
        { "→パーテル西 へ向かう扉の前 (24/125)", 24, 125 },
        { "→パーテル大平原東 へ向かう扉の前 (25/121)", 25, 121 },
        { "→ハーベスの大農園 へ向かう扉の前 (209/127)", 209, 127 },
        { "→始まりの島温泉街 へ向かう扉の前 (84/137)", 84, 137 },
        { "→中心街 へ向かう扉の前 (82/131)", 82, 131 },
        { "→太鼓の砂浜 へ向かう扉の前 (83/133)", 83, 133 },
        { "→ギガガビトのほら穴 へ向かう扉の前 (86/142)", 86, 142 },
        { "→崖の神殿 へ向かう扉の前 (85/132)", 85, 132 },
        { "→いにしえの塔 へ向かう扉の前 (97/134)", 97, 134 },
        { "→試練の間 へ向かう扉の前 (98/136)", 98, 136 },
        { "→ポルトポルト宮殿街 へ向かう扉の前 (7/71)", 7, 71 },
        { "→玄関街 へ向かう扉の前 (8/56)", 8, 56 },
        { "→郊外 宮殿街側 へ向かう扉の前 (10/55)", 10, 55 },
        { "→マバラッパ諸島 へ向かう扉の前 (15/74)", 15, 74 },
        { "→ダルスモルス魔王街 へ向かう扉の前 (17/104)", 17, 104 },
        { "→砂漠街 へ向かう扉の前 (16/89)", 16, 89 },
        { "→郊外 砂漠側 へ向かう扉の前 (19/88)", 19, 88 },
        { "→サンサン砂漠 へ向かう扉の前 (20/106)", 20, 106 },
        { "→着ぐるみキャンプ場 へ向かう扉の前 (11/197)", 11, 197 },
        { "→空島東 へ向かう扉の前 (31/83)", 31, 83 },
        { "→空島中央 へ向かう扉の前 (13/82)", 13, 82 },
        { "→空島西 へ向かう扉の前 (30/81)", 30, 81 },
        { "→コモレヴィ へ向かう扉の前 (36/196)", 36, 196 },
        { "→森の洞穴 へ向かう扉の前 (185/115)", 185, 115 },
        { "→森の奥地 へ向かう扉の前 (28/119)", 28, 119 },
        { "→森 へ向かう扉の前 (23/122)", 23, 122 },
    };
    static constexpr int WARP_COUNT = sizeof(g_warpDests) / sizeof(g_warpDests[0]);

    static bool g_warpHooked = false;

    static bool InstallWarpHook(void)
    {
        if (g_warpHooked)
            return true;

        // 入口の2命令が想定どおりか確認 (違うバージョンなら入れない)
        //   push {r4-r11,lr} / mov r4,r0
        static const u32 expectW[2] = { 0xE92D4FF0, 0xE1A04000 };
        if (!CheckWords(HOOK_WARP, expectW, 2))
        {
            MessageBox("エラー",
                       "ワープ用のフック位置の命令が想定と違います。\nゲームのバージョンが異なる可能性があるため、機能を有効にできません。")();
            return false;
        }

        // CTRPFのHookクラスは使わず、2語を直接書く。
        // 先にジャンプ先の値(2語目)を書き、そのあとで ldr pc 命令(1語目)を書く。
        Process::Write32(HOOK_WARP + 4, (u32)(uintptr_t)jp_hookWarp);
        Process::Write32(HOOK_WARP,     0xE51FF004);   // ldr pc, [pc, #-4]

        g_warpHooked = true;
        return true;
    }

    // ------------------------------------------------------------
    //  保存した位置 (名前つき) をワープの一覧に足す (実機未検証)
    //   ・「現在位置を保存」: いまのエリアIDと座標 (x,y,z) に、名前をつけて保存する
    //   ・保存した位置は、「エリアワープ」の一覧の末尾に [保存] として並ぶ
    //   ・選ぶと、同じエリアならそのまま移動、別のエリアなら、そのエリアへ
    //     「標準の入口」で入ってから、読み込みが終わるのを待って、座標を書き込む
    //  保存先: SD 直下の fll_warp_points.txt (1行 = エリアID / x / y / z / 名前)
    //  ※ x,y,z は float のビット列 (16進) で保存する。ファイルを直接編集するときは注意。
    // ------------------------------------------------------------
    #ifndef WARP_POINTS_TO_FILE
    #define WARP_POINTS_TO_FILE 1   // ビルドが通らないときは 0 にするとファイルの読み書きを外せる (再起動で消える)
    #endif
    #define WARP_POINTS_FILE "/fll_warp_points.txt"

    struct SavedPos { std::string name; u32 area; float x, y, z; };
    static std::vector<SavedPos> g_savedPos;
    static bool g_savedLoaded = false;

    static u32 FloatBits(float v) { u32 b = 0; memcpy(&b, &v, 4); return b; }
    static float BitsFloat(u32 b) { float v = 0.0f; memcpy(&v, &b, 4); return v; }

    // 小数2桁の文字にする (printf の %f が使えない環境でも動く)
    static std::string Fmt2(float v)
    {
        const bool neg = v < 0.0f;
        long n = (long)((neg ? -v : v) * 100.0f + 0.5f);
        return Utils::Format("%s%ld.%02ld", neg ? "-" : "", n / 100, n % 100);
    }

    static void LoadSavedPos(void)
    {
        if (g_savedLoaded)
            return;
        g_savedLoaded = true;
    #if WARP_POINTS_TO_FILE
        File f;
        if (File::Open(f, WARP_POINTS_FILE, File::READ) != 0)
            return;
        const u32 size = (u32)f.GetSize();
        if (size == 0 || size > 0x20000)
        {
            f.Close();
            return;
        }
        std::string text(size, '\0');
        f.Read(&text[0], size);
        f.Close();

        size_t pos = 0;
        while (pos < text.size())
        {
            size_t end = text.find('\n', pos);
            if (end == std::string::npos)
                end = text.size();
            std::string line = text.substr(pos, end - pos);
            pos = end + 1;
            if (!line.empty() && line[line.size() - 1] == '\r')
                line.erase(line.size() - 1);

            std::string col[4];
            size_t s0 = 0;
            int n = 0;
            while (n < 4)
            {
                const size_t t = line.find('\t', s0);
                if (t == std::string::npos)
                    break;
                col[n++] = line.substr(s0, t - s0);
                s0 = t + 1;
            }
            if (n != 4)
                continue;

            SavedPos p;
            p.area = (u32)strtoul(col[0].c_str(), nullptr, 10);
            p.x    = BitsFloat((u32)strtoul(col[1].c_str(), nullptr, 16));
            p.y    = BitsFloat((u32)strtoul(col[2].c_str(), nullptr, 16));
            p.z    = BitsFloat((u32)strtoul(col[3].c_str(), nullptr, 16));
            p.name = line.substr(s0);
            g_savedPos.push_back(p);
        }
    #endif
    }

    static void AppendSavedPos(const SavedPos &p)
    {
    #if WARP_POINTS_TO_FILE
        File f;
        if (File::Open(f, WARP_POINTS_FILE, File::WRITE | File::CREATE | File::APPEND) == 0)
        {
            f.WriteLine(Utils::Format("%u\t%08X\t%08X\t%08X\t", p.area, FloatBits(p.x), FloatBits(p.y), FloatBits(p.z)) + p.name);
            f.Close();
        }
        else
            OSD::Notify("points: file open failed (memory only)");
    #else
        (void)p;
    #endif
    }

    // 保存した位置へ移動する (同じエリアならそのまま、別のエリアなら入ってから)
    static void RequestSavedPos(const SavedPos &p, bool forceReload = false)
    {
        u16 cur = 0;
        if (g_warpTask != 0)
            Process::Read16(g_warpTask + 0x6B2, cur);
        const u32 obj = PosPlayerObj();

        g_posMode     = 0;
        g_posXYZ[0]   = p.x;
        g_posXYZ[1]   = p.y;
        g_posXYZ[2]   = p.z;
        g_posArea     = p.area;
        g_posOldObj   = obj;
        g_posFrames   = 0;
        g_posSawGap   = 0;
        g_posHold     = 0;
        PosResetDetect();
        g_recN        = 0;

         if (!forceReload && g_warpTask != 0 && obj && cur == p.area)
        {
            g_posMode = 2;                 // 同じエリア: そのまま移動
            OSD::Notify("Teleport requested.");
        }
        else
        {
            g_warpArea    = p.area;        // 別のエリア: 標準の入口で入ってから、位置を書く
            g_warpLink    = 0;
            g_warpUseLink = 0;
            g_posMode     = 1;
            g_warpPending = 1;
            OSD::Notify("Warp + teleport requested.");
        }
    }

    // いまのエリアに入り直して(ロード)、いまの位置に戻る
    static void ReloadHereEntry(MenuEntry *entry)
    {
        (void)entry;

        if (!InstallWarpHook())
            return;

        u16 area16 = 0;
        if (g_warpTask == 0 || !Process::Read16(g_warpTask + 0x6B2, area16))
        {
            MessageBox("準備中です",
                       "位置を読む準備をしています。\nメニューを閉じて、ゲームを1秒ほど動かしてから、もう一度実行してください。")();
            return;
        }
        const u32 obj = PosPlayerObj();
        if (!obj)
        {
            MessageBox("実行できません",
                       "プレイヤーのデータを取得できません。\n操作できる状態で、もう一度実行してください。")();
            return;
        }

        SavedPos p;
        p.area = area16;
        p.x = *(volatile float *)(obj + 0x7C);
        p.y = *(volatile float *)(obj + 0x80);
        p.z = *(volatile float *)(obj + 0x84);

        RequestSavedPos(p, true);   // true = 同じエリアでも、入り直す
        OSD::Notify("Reload here requested.");
    }

    static void SavePosEntry(MenuEntry *entry)
    {
        (void)entry;

        if (!InstallWarpHook())
            return;
        LoadSavedPos();

        u16 area16 = 0;
        if (g_warpTask == 0 || !Process::Read16(g_warpTask + 0x6B2, area16))
        {
            MessageBox("準備中です",
                       "位置を読む準備をしています。\nメニューを閉じて、ゲームを1秒ほど動かしてから、もう一度実行してください。")();
            return;
        }
        const u32 obj = PosPlayerObj();
        if (!obj)
        {
            MessageBox("保存できません",
                       "プレイヤーのデータを取得できません。\n操作できる状態で、もう一度実行してください。")();
            return;
        }

        SavedPos p;
        p.area = area16;
        p.x = *(volatile float *)(obj + 0x7C);
        p.y = *(volatile float *)(obj + 0x80);
        p.z = *(volatile float *)(obj + 0x84);

        std::string name;
        Keyboard kb("ワープ名を入力");
        if (kb.Open(name) != 0)
            return;
        for (size_t i = 0; i < name.size(); ++i)
            if (name[i] == '\t' || name[i] == '\n' || name[i] == '\r')
                name[i] = ' ';
        if (name.empty())
            name = Utils::Format("保存位置 %u", (u32)g_savedPos.size() + 1);
        p.name = name;

        g_savedPos.push_back(p);
        AppendSavedPos(p);

        MessageBox("保存しました",
                   Utils::Format("%s\nエリアID %u\nx %s / y %s / z %s\n\n「エリアワープ」の一覧の末尾に追加しました。",
                                 p.name.c_str(), p.area, Fmt2(p.x).c_str(), Fmt2(p.y).c_str(), Fmt2(p.z).c_str()))();
    }

    // 保存位置へのワープの経過 (毎フレームの、エリアID・状態・座標) を SD に書き出す。うまくいかないときの調査用。
    static void PosDumpEntry(MenuEntry *entry)
    {
        (void)entry;
        const u32 n = g_recN;
        if (n == 0)
        {
            MessageBox("記録がありません", "保存位置へのワープを実行したあとで、もう一度実行してください。")();
            return;
        }
    #if WARP_POINTS_TO_FILE
        File f;
        if (File::Open(f, "/fll_warp_notes.txt", File::WRITE | File::CREATE | File::APPEND) != 0)
        {
            MessageBox("書き出せません", "fll_warp_notes.txt を開けませんでした。")();
            return;
        }
        f.WriteLine(Utils::Format("TL-BEGIN target_area=%u frames=%u", (u32)g_posArea, n));
        for (u32 i = 0; i < n; ++i)
        {
            f.WriteLine(Utils::Format("TL %u m%u a%u s%u o%X  %s %s %s", i, (u32)g_recMode[i], (u32)g_recArea[i], (u32)g_recState[i],
                                      g_recObj[i], Fmt2(g_recPos[i][0]).c_str(), Fmt2(g_recPos[i][1]).c_str(), Fmt2(g_recPos[i][2]).c_str()));
        }
        f.WriteLine("TL-END");
        f.Close();
    #endif
        MessageBox("書き出しました", Utils::Format("%u 行を fll_warp_notes.txt に書き出しました。", n))();
    }

    // 位置ワープの状態が変わったときに、画面に通知する (プラグイン側のフレームごとの処理)
    static void PosNotifyFrame(void)
    {
        static u32 lastMode = 0;
        const u32 m = g_posMode;
        if (m == lastMode)
            return;
        if (m == 2)
            OSD::Notify("Pos: placing...");
        else if (m == 0 && lastMode == 2)
            OSD::Notify("Pos: done");
        else if (m == 0 && lastMode == 1)
            OSD::Notify("Pos: gave up (timeout)");
        lastMode = m;
    }

    // メニューから選んだときに1回だけ実行される (有効/無効の切り替えではない)
    static void WarpEntry(MenuEntry *entry)
    {
        (void)entry;

        if (!InstallWarpHook())
            return;
        LoadSavedPos();

        std::vector<std::string> names;
        for (int i = 0; i < WARP_COUNT; ++i)
            names.push_back(g_warpDests[i].label);
        for (size_t i = 0; i < g_savedPos.size(); ++i)
            names.push_back(Utils::Format("[保存] %s (エリア%u)", g_savedPos[i].name.c_str(), g_savedPos[i].area));

        Keyboard kb("移動先を選択 ([保存]は、保存した位置 / ほかは、行き先へ向かう扉の前)", names);
        int sel = kb.Open();
        if (sel < 0 || sel >= (int)names.size())
            return;

        if (sel >= WARP_COUNT)
        {
            RequestSavedPos(g_savedPos[sel - WARP_COUNT]);
            return;
        }

        g_posMode     = 0;
        g_warpArea    = g_warpDests[sel].area;
        g_warpLink    = g_warpDests[sel].link;
        g_warpUseLink = 1;
        g_warpPending = 1;     // メニューを閉じた後、ゲームが次にタスクを更新したときに実行される
        OSD::Notify("Warp requested.");
    }

    // ------------------------------------------------------------
    //  探索ワープ / 扉の記録 (開発用)
    //  ・探索ワープ: 「出たいエリア」と「入口番号」を数字で指定して移動する
    //  ・扉の自動記録: 有効にして扉を通ると、here / dest / 入口番号 を記録する
    //  記録は SD カード直下の fll_warp_notes.txt に追記される (書けない場合は画面の一覧のみ)。
    //  存在しないエリアIDへ移動すると、落ちる可能性がある。必ずバックアップを取ること。
    //
    //  here = 扉を通る前に居たエリア (扉がある側) / dest = 扉の向こう側のエリア
    //  入口番号 L の扉は、here 側と dest 側の2か所にある (両側で同じ番号)。
    //   ・ここ(here)に出たいとき      → (here, L) を指定
    //   ・扉の先(dest)に出たいとき    → (dest, L) を指定
    //  ゲーム本体は (行き先エリア, L) を渡すと「その行き先へ向かう扉の前」に出る
    //  (= 反対側のエリアに出る) ので、数字指定ワープは入口番号 L の反対側のエリアを
    //  調べて、本体に渡す値へ変換している。反対側が未登録の入口番号は、変換できない。
    // ------------------------------------------------------------
    #ifndef WARP_LOG_TO_FILE
    #define WARP_LOG_TO_FILE 1      // ビルドが通らないときは 0 にするとファイル出力を外せる
    #endif

    static std::vector<std::string> g_notes;
    static u32 g_expArea = 2;
    static u32 g_expLink = 0;
    static int g_expMode = 0;       // 0 = 入口番号を指定 / 1 = エリアだけ指定 (標準の入口)

    static void AppendNote(const std::string &line)
    {
        g_notes.push_back(line);
    #if WARP_LOG_TO_FILE
        File f;
        if (File::Open(f, "/fll_warp_notes.txt", File::WRITE | File::CREATE | File::APPEND) == 0)
        {
            f.WriteLine(line);
            f.Close();
        }
        else
            OSD::Notify("note: file open failed (memory only)");
    #endif
    }

    // 入口番号 → その入口が結ぶ2つのエリア。扉の自動記録のログ末尾の
    // 「{ 番号, here, dest },」をそのまま足せる。(扉を通ると、実行中も自動で覚える)
    struct LinkEnd { u16 link; u16 a; u16 b; };
    static const LinkEnd g_linkEnds[] =
    {
        {  39,   3,  25 },   // 郊外南 <-> パーテル大平原東
        { 125,  24,  25 },   // パーテル西 <-> パーテル大平原東
        { 128,  25, 210 },   // パーテル大平原東 <-> ハニワの洞窟
        { 133,  82,  83 },
        { 142,  83,  86 },
        { 132,  82,  85 },
        { 136,  97,  98 },
        {  88,  16,  19 },
        {  83,  11,  31 },
        {  81,  13,  30 },
    };
    static std::vector<LinkEnd> g_linkLearned;

    static void LearnLink(u32 link, u32 a, u32 b)
    {
        for (size_t i = 0; i < g_linkLearned.size(); ++i)
        {
            const LinkEnd &e = g_linkLearned[i];
            if (e.link == link && ((e.a == a && e.b == b) || (e.a == b && e.b == a)))
                return;
        }
        LinkEnd e;
        e.link = (u16)link;
        e.a    = (u16)a;
        e.b    = (u16)b;
        g_linkLearned.push_back(e);
    }

    // 戻り値: 0 = 反対側が見つかった / 1 = 入口番号が未登録 / 2 = そのエリアは、この入口の端ではない
    static int FindOtherEnd(u32 area, u32 link, u32 &other, u32 &ea, u32 &eb)
    {
        bool known = false;
        for (int pass = 0; pass < 2; ++pass)
        {
            const size_t n = (pass == 0) ? g_linkLearned.size() : (sizeof(g_linkEnds) / sizeof(g_linkEnds[0]));
            for (size_t i = 0; i < n; ++i)
            {
                const LinkEnd &e = (pass == 0) ? g_linkLearned[i] : g_linkEnds[i];
                if (e.link != link)
                    continue;
                known = true;
                ea = e.a;
                eb = e.b;
                if (e.a == area) { other = e.b; return 0; }
                if (e.b == area) { other = e.a; return 0; }
            }
        }
        return known ? 2 : 1;
    }

    // ゲーム本体へ予約する。area は本体に渡す値 (= その入口が向かう側のエリア)。
    static void RequestWarpRaw(u32 area, u32 link, bool useLink)
    {
        if (area > 999) area = 999;   // 0 も指定できる (存在しないIDだと落ちる可能性あり)
        g_warpArea    = area;
        g_warpLink    = link;
        g_warpUseLink = useLink ? 1 : 0;
        g_posMode     = 0;            // 保存位置の予約が残っていたら、取り消す
        g_warpPending = 1;
        OSD::Notify(Utils::Format("Warp: engine area %u link %u%s", area, link, useLink ? "" : " (default)"));
    }

    // 「area の、番号 link の入口の前」に出る。
    static void WarpToEntrance(u32 area, u32 link)
    {
        u32 other = 0, ea = 0, eb = 0;
        const int r = FindOtherEnd(area, link, other, ea, eb);

        if (r == 2)
        {
            MessageBox("移動できません",
                       Utils::Format("入口番号 %u は、エリア %u とエリア %u をつなぐ入口です。\n"
                                     "エリア %u には、この入口はありません。", link, ea, eb, area))();
            return;
        }
        if (r == 1)
        {
            if (!(MessageBox("入口番号が未登録",
                             Utils::Format("入口番号 %u の両側のエリアが、まだ登録されていません。\n\n"
                                           "このまま続けると、エリア %u へ向かう扉の前 (隣のエリア側) に出ます。\n"
                                           "続けますか?\n\n"
                                           "(「扉の自動記録」を有効にして、その入口の扉を通ると登録されます)",
                                           link, area),
                             DialogType::DialogYesNo))())
                return;
            RequestWarpRaw(area, link, true);
            return;
        }
        RequestWarpRaw(other, link, true);
    }

    static u32 CurrentAreaId(void)
    {
        u16 a = 0;
        if (g_warpTask != 0)
            Process::Read16(g_warpTask + 0x6B2, a);
        return a;
    }

    static void ShowNotes(void)
    {
        if (g_notes.empty())
        {
            MessageBox("記録", "まだ記録がありません。")();
            return;
        }
        std::string text;
        int shown = 0;
        for (int i = (int)g_notes.size() - 1; i >= 0 && shown < 12; --i, ++shown)
            text += g_notes[i] + "\n";
        MessageBox("記録 (新しい順 最大12件)", text)();
    }

    static void ExploreEntry(MenuEntry *entry)
    {
        (void)entry;

        if (!InstallWarpHook())
            return;

        const u32 cur = CurrentAreaId();

        // 項目の「表示」と「動作」は、必ずペアで追加する (opts と acts を同時に push_back する)。
        // 表示順を変えたいときは、下の並びの行を入れ替えるだけでよい (switch は動作名で分岐する)。
        enum Act { ACT_INPUT, ACT_AREA_ONLY, ACT_AGAIN, ACT_SHOW };
        std::vector<std::string> opts;
        std::vector<int> acts;

        opts.push_back("エリアIDと入口番号を数字で指定");
        acts.push_back(ACT_INPUT);
        opts.push_back("エリアIDだけ数字で指定 (標準の入口)");
        acts.push_back(ACT_AREA_ONLY);
        opts.push_back(Utils::Format("もう一度同じIDへ (%u)", g_expArea));
        acts.push_back(ACT_AGAIN);
        opts.push_back("記録を表示");
        acts.push_back(ACT_SHOW);

        u32 sel = 0;
        if (!AskChoice(Utils::Format("探索ワープ  いまのエリアID=%u / 前回の指定=ID %u 入口 %u", cur, g_expArea, g_expLink),
                       opts, sel))
            return;
        if (sel >= acts.size())
            return;

        switch (acts[sel])
        {
        case ACT_INPUT:
        {
            u32 area = g_expArea, link = g_expLink;
            if (!AskValue("出たいエリアID (0〜999)", false, 999, g_expArea, area))
                return;
            if (!AskValue("その入口番号 (0〜999)", false, 999, g_expLink, link))
                return;
            g_expArea = area;
            g_expLink = link;
            g_expMode = 0;
            WarpToEntrance(area, link);
            break;
        }
        case ACT_AREA_ONLY:
        {
            u32 area = g_expArea;
            if (!AskValue("エリアID (0〜999)", false, 999, g_expArea, area))
                return;
            g_expArea = area;
            g_expMode = 1;
            RequestWarpRaw(area, 0, false);
            break;
        }
        case ACT_AGAIN:
            if (g_expMode == 0)
                WarpToEntrance(g_expArea, g_expLink);
            else
                RequestWarpRaw(g_expArea, 0, false);
            break;
        case ACT_SHOW:
            ShowNotes();
            break;
        default:
            break;
        }
    }

    // 有効な間、毎フレーム呼ばれる。扉を通ってオブジェクトの中身が変わるたびに記録する。
    // (同じ扉を続けて通ると、中身が変わらないので記録されない)
    static void DoorLogEntry(MenuEntry *entry)
    {
        (void)entry;
        static u32 lastWord = 0xFFFFFFFF;

        // here (いま居るエリア) を読むには、ワープ用のフックが必要 (タスクの場所を毎フレーム受け取る)
        if (g_warpTask == 0)
        {
            InstallWarpHook();
            return;
        }

        u32 w = 0;
        if (!Process::Read32(0x16D5D4A8, w))   // ChangeMapオブジェクトの +12 (エリアID16bit) と +14 (入口番号16bit)
            return;
        if (w == lastWord)
            return;
        lastWord = w;

        const u32 dest = w & 0xFFFF;
        const u32 link = w >> 16;
        if (dest == 0)
            return;

        // here = 扉を通る前に居たエリア / dest = 扉の向こう
        const u32 here = CurrentAreaId();
        LearnLink(link, here, dest);
        AppendNote(Utils::Format("DOOR here=%u dest=%u link=%u | ここに出る: %u/%u  扉の先に出る: %u/%u | { %u, %u, %u },",
                                 here, dest, link, here, link, dest, link, link, here, dest));
        OSD::Notify(Utils::Format("Door: here %u dest %u link %u", here, dest, link));
    }

    // ============================================================
    //  プレイヤー切り替え (直接版) / 日本語版 v1.7L
    //  実機GDBで通し確認済み
    // ============================================================
    namespace PlayerSwitch
    {
        // true にすると、進行状況を画面に出します(不具合調査用)
        static constexpr bool VERBOSE = false;
        // true にすると、スロット選択画面(オレンジ色の背景とアバター)を出さずに進めます
        // (実機GDBで確認済み: UIの初期化と「UIを開く」を止め、UIの準備完了の印を手動で立てる)
        static constexpr bool HIDE_SELECT_UI = true;

        static constexpr u32 FLOW_OBJ       = 0x7208DC;   // シーンフロー (+36 切替要求, +44 要求状態, +48 現在状態)
        static constexpr u32 ROOT_TASK      = 0x6B95EC;   // ルートタスク
        static constexpr u32 VPTR_EXSEL     = 0x66D898;   // スロット選択タスクの vtable
        static constexpr u32 VPTR_FIELD     = 0x669038;
        static constexpr u32 STATE_IDLE     = 1;
        static constexpr u32 STATE_FIELD    = 9;
        static constexpr u32 IDLE_MSG_ADDR  = 0x687D14;
        static constexpr u32 FN_SEND        = 0x46FEEC;
        static constexpr u32 DESC_EXSEL     = 0x6D9D1C;
        static constexpr u32 MGR_OBJ        = 0x719374;
        static constexpr u32 FN_MGR_INIT    = 0x28299C;
        static constexpr u32 FN_MGR_UPDATE  = 0x28238C;
        static constexpr u32 FN_MGR_TERM    = 0x257D88;
        static constexpr u32 FN_EXSEL_UPDATE   = 0x411E98;
        static constexpr u32 VTBL_EXSEL_UPDATE = 0x66D8AC;
        static constexpr u32 UIMGR_OBJ      = 0x6B977C;
        static constexpr u32 FN_ATTACH      = 0x46FFE0;
        static constexpr u32 SLOT_VAR       = 0x71BA40;
        static constexpr u32 UI_OPEN_ADDR   = 0x41251C;   // bl 0x308888 (スロット選択UIを開く)
        static constexpr u32 UI_OPEN_ORIG   = 0xEBFBD8D9;
        static constexpr u32 UI_INIT_ADDR   = 0x412F9C;   // bl 0x30aba8 (スロット選択UIの初期化)
        static constexpr u32 UI_INIT_ORIG   = 0xEBFBDF01;
        static constexpr u32 ARM_NOP        = 0xE1A00000;
        static constexpr u32 MAX_FRAMES     = 60 * 90;
        static constexpr u32 STATE_TITLE    = 8;
        // スロット選択タスクの中の、スロットごとの「概要の入れ物」(0x800バイトおき)。
        // 2バイト目が、概要の読み込みに成功した印 (データのないスロットは 0)
        static constexpr u32 SUM_BASE       = 0x79B0;
        static constexpr u32 SUM_STRIDE     = 0x800;

        typedef s32  (*SendFn)(u32, u32);
        typedef void (*FnU32)(u32);
        typedef void (*FnU32x2)(u32, u32);

        enum Phase { IDLE, D_BLANK, D_SEL, D_DONE, D_RESTART };

        static volatile int  g_slot  = -1;
        static Phase         g_phase = IDLE;
        static u32           g_wait  = 0;
        static u32           g_tick  = 0;
        static volatile bool g_sent  = false;
        static volatile s32  g_sendResult = 0;
        static bool g_mgrInit = false, g_vtblPatched = false, g_uiAttached = false, g_uiHidden = false, g_uiInitHidden = false;
        static int  g_lastSt = -1;
        static int  g_slotValid[3] = { -1, -1, -1 };   // -1:不明 0:データなし(未作成) 1:データあり
        static u32  g_idleTick = 0;

        static void Say(const std::string &m) { if (VERBOSE) OSD::Notify(m); }

        static u32 FindByVptr(u32 node, u32 vptr, int depth)
        {
            u32 v = 0;
            if (depth > 4 || !Process::CheckAddress(node))
                return 0;
            if (Process::Read32(node, v) && v == vptr)
                return node;
            u32 child = 0;
            if (!Process::Read32(node + 340, child))
                return 0;
            for (int i = 0; i < 64 && child && Process::CheckAddress(child); i++)
            {
                u32 found = FindByVptr(child, vptr, depth + 1);
                if (found)
                    return found;
                if (!Process::Read32(child + 344, child))
                    break;
            }
            return 0;
        }

        // スロット選択タスクの概要から、各スロットのデータの有無を読み取って覚える
        static void SampleSummary(u32 obj)
        {
            u8 st = 0, nx = 0;
            Process::Read8(obj + 0x91B9, st);
            Process::Read8(obj + 0x91B8, nx);
            // 3人分の読み込みが終わって、決定待ちになるまで(状態0x0A / 0x0B)の間だけ有効
            if (!(st == 0x0A || st == 0x0B || nx == 0x0A || nx == 0x0B))
                return;
            for (u32 i = 0; i < 3; i++)
            {
                u8 ok = 0;
                if (Process::Read8(obj + SUM_BASE + i * SUM_STRIDE + 1, ok))
                    g_slotValid[i] = ok ? 1 : 0;
            }
        }

        static void RequestState(u32 st)
        {
            Process::Write32(FLOW_OBJ + 44, st);
            Process::Write8(FLOW_OBJ + 36, 1);
        }

        static void RestoreIdleMsg(void) { Process::Write16(IDLE_MSG_ADDR, 0); }

        // スロット選択タスクの更新の差し替え (ゲーム本来のスレッドで毎フレーム動く)
        //  1. UI管理を、このタスクの子として登録 (これがないと読み込みが完了しない)
        //  2. 状態0x0C(読み込み開始)の直前に、スロット番号を書く
        //  3. 管理オブジェクトを更新してから、本来の更新を呼ぶ
        //  4. 状態0x0E(リンク確認)に進もうとしたら 0x22 へ飛ばす
        static void ExSelUpdateWrapper(u32 self)
        {
            if (!g_uiAttached)
            {
                ((FnU32x2)FN_ATTACH)(self, UIMGR_OBJ);
                if (HIDE_SELECT_UI)
                    *(volatile u32 *)(self + 372) |= 3;   // UIの準備完了の印 (UIの初期化を止めたので手動で立てる)
                g_uiAttached = true;
            }
            volatile u8 *nextSt = (volatile u8 *)(self + 0x91B8);
            if (*nextSt == 0x0C && g_slot >= 0)
                *(volatile u8 *)SLOT_VAR = (u8)g_slot;

            ((FnU32)FN_MGR_UPDATE)(MGR_OBJ);
            ((FnU32)FN_EXSEL_UPDATE)(self);

            if (*nextSt == 0x0E)
                *nextSt = 0x22;
        }

        static void CleanupDirect(void)
        {
            if (g_vtblPatched)
            {
                Process::Write32(VTBL_EXSEL_UPDATE, FN_EXSEL_UPDATE);
                g_vtblPatched = false;
            }
            if (g_uiHidden)
            {
                Process::Write32(UI_OPEN_ADDR, UI_OPEN_ORIG);
                g_uiHidden = false;
            }
            if (g_uiInitHidden)
            {
                Process::Write32(UI_INIT_ADDR, UI_INIT_ORIG);
                g_uiInitHidden = false;
            }
            if (g_mgrInit)
            {
                ((FnU32)FN_MGR_TERM)(MGR_OBJ);
                g_mgrInit = false;
            }
        }

        static void Abort(const char *msg)
        {
            OSD::Notify(msg);
            Process::Write32(FLOW_OBJ + 12, 0);
            CleanupDirect();
            RestoreIdleMsg();
            g_phase = IDLE;
        }

        // ゲーム起動時の SlotReset と同じ初期化
        static void ResetSlotState(void)
        {
            Process::Write8(SLOT_VAR, 0);
            for (u32 o = 4; o <= 24; o += 4)
                Process::Write32(SLOT_VAR + o, 0);
        }

        // シーンフローのリスナー (FLOW+12)。状態変更の直後に、ゲーム本来のスレッドから1回だけ呼ばれる
        static void FlowListener(s32 state, u32 arg)
        {
            (void)arg;
            if (state != (s32)STATE_IDLE)
                return;
            Process::Write32(FLOW_OBJ + 12, 0);
            ResetSlotState();
            g_uiAttached = false;
            ((FnU32)FN_MGR_INIT)(MGR_OBJ);
            g_mgrInit = true;
            if (HIDE_SELECT_UI)
            {
                g_uiHidden = Process::Write32(UI_OPEN_ADDR, ARM_NOP);
                g_uiInitHidden = Process::Write32(UI_INIT_ADDR, ARM_NOP);
            }
            g_vtblPatched = Process::Write32(VTBL_EXSEL_UPDATE, (u32)&ExSelUpdateWrapper);
            g_sendResult = ((SendFn)FN_SEND)(ROOT_TASK, DESC_EXSEL);
            g_sent = true;
        }

        // menu->Callback から毎フレーム呼ばれる (切り替え中以外は即 return)
        static void OnFrame(void)
        {
            if (g_phase == IDLE)
            {
                // タイトル画面の間だけ、6フレームに1回、スロットのデータの有無を読み取って覚える
                if ((++g_idleTick % 6) == 0)
                {
                    u32 c = 0;
                    if (Process::Read32(FLOW_OBJ + 48, c) && c == STATE_TITLE)
                    {
                        u32 o = FindByVptr(ROOT_TASK, VPTR_EXSEL, 0);
                        if (o)
                            SampleSummary(o);
                    }
                }
                return;
            }

            // 重くならないよう、切り替え中も3フレームに1回だけ調べる
            if (g_phase != D_BLANK && (++g_tick % 3) != 0)
                return;

            u32 cur = 0;
            Process::Read32(FLOW_OBJ + 48, cur);
            (void)cur;

            switch (g_phase)
            {
            case D_BLANK:
            {
                if (!g_sent)
                {
                    if (++g_wait > 600)
                        Abort("PS: flow listener not called");
                    return;
                }
                Say(Utils::Format("PS: send=%d vtbl=%d", (int)g_sendResult, (int)g_vtblPatched));
                g_wait = 0;
                g_lastSt = -1;
                g_phase = D_SEL;
                break;
            }
            case D_SEL:
            {
                u32 obj = FindByVptr(ROOT_TASK, VPTR_EXSEL, 0);
                if (!obj)
                {
                    if (++g_wait > 100)
                        Abort("PS: select task not created");
                    return;
                }
                u8 st = 0, nx = 0;
                Process::Read8(obj + 0x91B9, st);
                Process::Read8(obj + 0x91B8, nx);
                if ((int)st != g_lastSt)
                {
                    g_lastSt = st;
                    Say(Utils::Format("PS: select state %02X", st));
                }
                SampleSummary(obj);   // 切り替えのたびに、データの有無を更新する
                // UIを開く(0A)/決定待ち(0B)に来たら、決定済みにして読み込み(0C以降)へ進める
                if (st == 0x0A || st == 0x0B || nx == 0x0A || nx == 0x0B)
                {
                    Process::Write32(obj + 0x91BC, (u32)g_slot);
                    Process::Write8(0x68BAD1, 0);
                    Process::Write8(obj + 0x91B8, 0x0C);
                    Say("PS: decide -> load");
                    g_wait = 0;
                    g_phase = D_DONE;
                }
                else if (++g_wait > MAX_FRAMES / 3)
                    Abort("PS: select timeout");
                break;
            }
            case D_DONE:
            {
                // スロット選択タスクが自分で終了するのを待つ
                if (FindByVptr(ROOT_TASK, VPTR_EXSEL, 0))
                {
                    if (++g_wait > MAX_FRAMES / 3)
                        Abort("PS: load timeout");
                    return;
                }
                Say("PS: load finished");
                CleanupDirect();
                g_wait = 0;
                g_phase = D_RESTART;
                break;
            }
            case D_RESTART:
            {
                if (FindByVptr(ROOT_TASK, VPTR_FIELD, 0))
                {
                    RestoreIdleMsg();
                    g_phase = IDLE;
                    Say("PS: done");
                    return;
                }
                // ゲーム側が自分でフィールドを作らない場合は、こちらから要求する
                if (++g_wait == 40)
                {
                    RestoreIdleMsg();
                    RequestState(STATE_FIELD);
                    Say("PS: request field");
                }
                else if (g_wait > 600)
                    Abort("PS: field timeout");
                break;
            }
            default:
                break;
            }
        }

        // メニュー項目から呼ばれる
        static void Entry(MenuEntry *entry)
        {
            (void)entry;
            u32 cur = 0;
            Process::Read32(FLOW_OBJ + 48, cur);
            if (cur != STATE_FIELD)
            {
                OSD::Notify("Field only");
                return;
            }
            if (g_phase != IDLE)
            {
                OSD::Notify("Busy");
                return;
            }
            std::vector<std::string> opts;
            for (int i = 0; i < 3; i++)
                opts.push_back(std::string("プレイヤー ") + (char)('1' + i) + (g_slotValid[i] == 0 ? " (未作成)" : ""));
            Keyboard kb("プレイヤーを選択", opts);
            int r = kb.Open();
            if (r < 0)
                return;
            if (g_slotValid[r] == 0)
            {
                MessageBox("プレイヤー切り替え", "プレイヤー未作成のため切り替えできません。")();
                return;
            }

            // 状態IDLEが新しいシーンを作らないよう、メッセージ番号を範囲外にしておく
            if (!Process::Write16(IDLE_MSG_ADDR, 0x7FFF))
            {
                OSD::Notify("PS: cannot patch table");
                return;
            }
            g_slot = r; g_wait = 0; g_tick = 0; g_lastSt = -1;
            g_sent = false;
            Process::Write32(FLOW_OBJ + 24, 0);
            Process::Write32(FLOW_OBJ + 12, (u32)&FlowListener);
            g_phase = D_BLANK;
            RequestState(STATE_IDLE);
        }
    }



    // ------------------------------------------------------------
    //  固有効果を変更 (武器・防具・ライフ道具)
    //  固有効果は、かばんの1つ1つのアイテムではなく、ゲームがメモリに読み込んでいる
    //  「データ表」(1アイテム1行。行の決まった位置の2バイトが効果ID)で決まる。
    //  この表をメモリ上で書き換える。同じIDのアイテムすべてに効き、自分のゲームの中だけで有効。
    //  ゲームを終了すると元に戻る。表の場所は、メモリを検索して見つける。
    //   武器: 行 = ID (0x0000-0x008F)、1行24バイト、行の+14
    //   防具: 行 = ID - 0x99 (0x0099-0x0386)、1行48バイト、行の+38
    //  ライフ道具: ゲームが持つ「表オブジェクト」から表の場所をたどる (メモリ検索は不要)。
    //    行 = ID - 0x387 (0x0387-0x0435)、行の+2。
    // ------------------------------------------------------------
    static constexpr u32 INN_MAGIC      = 0x1D038013u;   // 表ファイルの先頭の目印
    static constexpr u32 INN_ROWS_OFF_W = 0x110;
    static constexpr u32 INN_ROWS_OFF_A = 0x14C;
    static constexpr u32 INN_EFFECT_MAX = 0x240;         // 効果IDの最大
    static constexpr int INN_CATS       = 3;
    static constexpr u32 INN_MAX_ITEMS  = 0x400;         // 1カテゴリの最大アイテム数

    // 照合用: 固有効果より前にある列の値 (効果を書き換えても変わらない)
    static const u8 s_w_0[14] = { 0x06, 0x00, 0x10, 0x00, 0x64, 0x00, 0x01, 0x00, 0x00, 0x00, 0x65, 0x02, 0xE8, 0x03 };
    static const u8 s_w_1[14] = { 0x0C, 0x00, 0x10, 0x00, 0x64, 0x00, 0x01, 0x00, 0x00, 0x00, 0x66, 0x02, 0xE8, 0x03 };
    static const u8 s_w_81[14] = { 0x78, 0x00, 0x10, 0x00, 0x64, 0x00, 0x01, 0x00, 0x00, 0x00, 0x2C, 0x04, 0xE8, 0x03 };
    static const u8 s_w_143[14] = { 0x00, 0x00, 0x02, 0x00, 0x64, 0x00, 0x01, 0x00, 0x50, 0x00, 0x4C, 0x06, 0xE8, 0x03 };
    static const u8 s_a_0[36] = { 0x00, 0x00, 0x80, 0x3F, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x0A, 0x00, 0x0A, 0x00, 0x0A, 0x00 };
    static const u8 s_a_1[36] = { 0x00, 0x00, 0x80, 0x3F, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x0A, 0x00, 0x0A, 0x00, 0x0A, 0x00 };
    static const u8 s_a_150[36] = { 0x00, 0x00, 0x80, 0x3F, 0x16, 0x00, 0x16, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x0A, 0x00, 0x0A, 0x00, 0x0A, 0x00 };
    static const u8 s_a_300[36] = { 0x00, 0x00, 0x80, 0x3F, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x0A, 0x00, 0x0A, 0x00, 0x0A, 0x00 };
    static const u8 s_a_500[36] = { 0x00, 0x00, 0x80, 0x3F, 0x22, 0x00, 0x11, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x0A, 0x00, 0x0A, 0x00, 0x0A, 0x00 };
    static const u8 s_a_749[36] = { 0x00, 0x00, 0x80, 0x3F, 0x06, 0x00, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x0A, 0x00, 0x0A, 0x00, 0x0A, 0x00 };

    struct InnateRow { u32 row; const u8 *bytes; };
    static const InnateRow g_innSigW[] =
    {
        { 0, s_w_0 },
        { 1, s_w_1 },
        { 81, s_w_81 },
        { 143, s_w_143 }
    };
    static const InnateRow g_innSigA[] =
    {
        { 0, s_a_0 },
        { 1, s_a_1 },
        { 150, s_a_150 },
        { 300, s_a_300 },
        { 500, s_a_500 },
        { 749, s_a_749 }
    };

    struct InnateCat
    {
        const char       *name;
        u32               idMin, idMax;      // アイテムIDの範囲 (行 = id - idMin)
        u32               rowsOff;           // 表の先頭から最初の行まで
        u32               rowSize;
        u32               effOff;            // 行の中の固有効果の位置 (u16)
        u32               sigLen;            // 照合するバイト数
        const InnateRow  *sigs;
        int               sigN;
    };

    static const InnateCat g_innCats[INN_CATS] =
    {
        { "武器", 0x0000, 0x008F, INN_ROWS_OFF_W, 24, 14, 14, g_innSigW, 4 },
        { "防具", 0x0099, 0x0386, INN_ROWS_OFF_A, 48, 38, 36, g_innSigA, 6 },
        { "ライフ道具", 0x0387, 0x0435, 0, 0, 2, 0, nullptr, 0 },   // 行サイズは実行中に読む
    };

    static u32  g_innTab[INN_CATS][8];                          // 見つかった表(コピー)の先頭アドレス
    static int  g_innTabN[INN_CATS] = { 0, 0, 0 };
    static u32  g_innDynRowSize = 0;                            // ライフ道具の表の行サイズ (実行中に読む)
    static std::string g_innErr;                                // ライフ道具の表が見つからないときの理由
    static u16  g_innOrig[INN_CATS][INN_MAX_ITEMS];             // 変更前の効果ID
    static bool g_innHas[INN_CATS][INN_MAX_ITEMS];              // 変更済みか

    static u32 InnCount(const InnateCat &c) { return c.idMax - c.idMin + 1; }

    static u32 InnEffAddr(const InnateCat &c, u32 base, u32 id)
    {
        const u32 rs = c.rowSize ? c.rowSize : g_innDynRowSize;
        return base + c.rowsOff + rs * (id - c.idMin) + c.effOff;
    }

    static bool InnIsTable(const InnateCat &c, u32 base)
    {
        u32 v = 0;
        if (!Process::Read32(base, v) || v != INN_MAGIC)
            return false;
        for (int k = 0; k < c.sigN; ++k)
        {
            const u32 a = base + c.rowsOff + c.rowSize * c.sigs[k].row;
            const u8 *want = c.sigs[k].bytes;
            u32 i = 0;
            for (; i + 4 <= c.sigLen; i += 4)
            {
                u32 x = 0;
                if (!Process::Read32(a + i, x))
                    return false;
                const u32 w = (u32)want[i] | ((u32)want[i + 1] << 8) | ((u32)want[i + 2] << 16) | ((u32)want[i + 3] << 24);
                if (x != w)
                    return false;
            }
            if (i + 2 <= c.sigLen)
            {
                u16 x = 0;
                if (!Process::Read16(a + i, x))
                    return false;
                const u16 w = (u16)((u32)want[i] | ((u32)want[i + 1] << 8));
                if (x != w)
                    return false;
            }
        }
        return true;
    }

    static void InnAdd(int ci, u32 base)
    {
        for (int i = 0; i < g_innTabN[ci]; ++i)
            if (g_innTab[ci][i] == base)
                return;
        if (g_innTabN[ci] < 8)
            g_innTab[ci][g_innTabN[ci]++] = base;
    }

    // 読めない所はページ単位で飛ばしながら、16バイトおきに目印を探す
    // 見つかった表は、武器・防具のどちらのものか調べて、それぞれに記録する
    static void InnScanPass(u32 start, u32 end, u32 off)
    {
        u32 a = start + off;
        while (a < end)
        {
            u32 v = 0;
            if (!Process::Read32(a, v))
            {
                a = ((a | 0xFFFu) + 1) + off;
                continue;
            }
            if (v == INN_MAGIC)
                for (int ci = 0; ci < INN_CATS; ++ci)
                    if (g_innCats[ci].sigN > 0 && InnIsTable(g_innCats[ci], a))
                        InnAdd(ci, a);
            a += 16;
        }
    }

    // ライフ道具: ゲームの「データ管理」(0x6DCA1C) が、種類ごとの表オブジェクト(60バイト)を持つ。
    // ライフ道具の種類は2で、表5 = 管理 + 60*5。ゲーム自身が固有効果を読むときと同じ経路でたどる。
    static constexpr u32 INN_MGR        = 0x006DCA1Cu;
    static constexpr u32 INN_TAB_STRIDE = 60;
    static constexpr u32 INN_TYPEBASE   = 720;           // 管理 + 720 + 種類*4 = その種類の先頭ID
    static constexpr u32 INN_VTABLE     = 0x031C6040u;   // 表オブジェクトの目印

    static bool InnLocateTools(u32 &rowsBase, u32 &rowSize)
    {
        const InnateCat &c = g_innCats[2];
        const u32 obj = INN_MGR + INN_TAB_STRIDE * 5;
        u32 vt = 0, p = 0, cnt = 0, rs = 0, rb = 0, tb = 0;
        u8 st = 0;
        if (!Process::Read32(obj, vt) || vt != INN_VTABLE || !Process::Read8(obj + 20, st) || st != 2)
        {
            g_innErr = "表がまだ読み込まれていません。\nゲームの中で、ライフ道具を装備してから、もう一度開いてください。";
            return false;
        }
        if (!Process::Read32(obj + 4, p) || p == 0 || !Process::Read32(p, cnt) || !Process::Read32(p + 4, rs) ||
            !Process::Read32(p + 8, rb) || !Process::Read32(INN_MGR + INN_TYPEBASE + 2 * 4, tb))
        {
            g_innErr = "表の情報を読めませんでした。";
            return false;
        }
        if (tb != c.idMin || rs < 4 || rs > 0x200 || cnt < InnCount(c) || rb == 0)
        {
            g_innErr = Utils::Format("表の形が想定と違います。\n先頭ID=0x%X (想定0x%X)\n行数=%u (想定%u以上)\n行サイズ=%u",
                                     tb, c.idMin, cnt, InnCount(c), rs);
            return false;
        }
        // 教えてもらった道具の固有効果と照合する (すでに変更した道具は照合しない)
        static const struct { u32 id, eff; } chk[] =
        {
            { 0x038C, 0x00B1 }, { 0x03A6, 0x014F }, { 0x03F4, 0x01B0 },
            { 0x03F6, 0x01AE }, { 0x03FD, 0x0198 }, { 0x040A, 0x0195 },
        };
        for (const auto &k : chk)
        {
            if (g_innHas[2][k.id - c.idMin])
                continue;
            u16 v = 0;
            if (!Process::Read16(rb + rs * (k.id - c.idMin) + c.effOff, v) || v != k.eff)
            {
                g_innErr = Utils::Format("表の中身が想定と違います。\n%s: 想定0x%04X 実際0x%04X\n(行サイズ=%u)",
                                         IdName(k.id).c_str(), k.eff, v, rs);
                return false;
            }
        }
        rowsBase = rb;
        rowSize  = rs;
        return true;
    }

    // 0=やめた 1=見つかった 2=見つからなかった
    static int InnFind(int ci)
    {
        if (ci == 2)                         // ライフ道具: 検索は不要 (表オブジェクトからたどる)
        {
            u32 rb = 0, rs = 0;
            if (!InnLocateTools(rb, rs))
                return 2;
            g_innTab[2][0]  = rb;
            g_innTabN[2]    = 1;
            g_innDynRowSize = rs;
            return 1;
        }
        for (int c = 0; c < 2; ++c)            // 前回見つけた場所がまだ有効なものだけ残す
        {
            int k = 0;
            for (int i = 0; i < g_innTabN[c]; ++i)
                if (InnIsTable(g_innCats[c], g_innTab[c][i]))
                    g_innTab[c][k++] = g_innTab[c][i];
            g_innTabN[c] = k;
        }
        if (g_innTabN[ci] > 0)
            return 1;

        if (!(MessageBox("データの検索",
                         "メモリから固有効果のデータ表を探します。\n数十秒かかることがあります。\n(最初の1回だけです)",
                         DialogType::DialogYesNo))())
            return 0;

        static const u32 ranges[2][2] = { { 0x08000000u, 0x10000000u }, { 0x14000000u, 0x1C000000u } };
        for (u32 off = 0; off < 16 && g_innTabN[ci] == 0; off += 4)
            for (int r = 0; r < 2; ++r)
                InnScanPass(ranges[r][0], ranges[r][1], off);
        return g_innTabN[ci] > 0 ? 1 : 2;
    }

    static bool InnRead(int ci, u32 id, u16 &out)
    {
        return g_innTabN[ci] > 0 && Process::Read16(InnEffAddr(g_innCats[ci], g_innTab[ci][0], id), out);
    }

    static bool InnWrite(int ci, u32 id, u16 eff)
    {
        bool ok = false;
        for (int i = 0; i < g_innTabN[ci]; ++i)
        {
            const u32 a = InnEffAddr(g_innCats[ci], g_innTab[ci][i], id);
            u16 back = 0;
            if (Process::Write16(a, eff) && Process::Read16(a, back) && back == eff)
                ok = true;
        }
        return ok;
    }

    // 1件の効果を変える (元の値を保存しておく)。変更できたら true
    static bool InnSet(int ci, u32 id, u16 eff)
    {
        const InnateCat &c = g_innCats[ci];
        const u32 k = id - c.idMin;
        u16 cur = 0;
        InnRead(ci, id, cur);
        if (!g_innHas[ci][k])
        {
            g_innOrig[ci][k] = cur;
            g_innHas[ci][k]  = true;
        }
        if (!InnWrite(ci, id, eff))
            return false;
        if (eff == g_innOrig[ci][k])
            g_innHas[ci][k] = false;
        return true;
    }

    static std::string InnEffName(u32 id)
    {
        const char *n = LookupName(g_effectNames, id);
        return n ? std::string(n) : Utils::Format("0x%04X", id);
    }

    // カテゴリ全体 / ID範囲を、同じ固有効果にまとめて変える
    static void InnBulk(int ci)
    {
        const InnateCat &c = g_innCats[ci];
        u32 from = c.idMin, to = c.idMax;

        std::vector<std::string> sc;
        sc.push_back(Utils::Format("%sすべて (%u件)", c.name, InnCount(c)));
        sc.push_back("ID範囲を指定する");
        Keyboard ks(Utils::Format("%sの固有効果を一括変更: 対象", c.name), sc);
        const int s = ks.Open();
        if (s < 0)
            return;
        if (s == 1)
        {
            if (!PickFromNames(Utils::Format("開始 [%s]", c.name), g_itemNames, true, c.idMin, c.idMax, c.idMin, from))
                return;
            if (!PickFromNames(Utils::Format("終了 [%s]", c.name), g_itemNames, true, from, c.idMax, c.idMax, to))
                return;
        }

        std::vector<std::string> fl;
        fl.push_back("すべて");
        fl.push_back("今、固有効果が付いているものだけ");
        fl.push_back("今、固有効果が「なし」のものだけ");
        Keyboard kf("どれを変更しますか?", fl);
        const int f = kf.Open();
        if (f < 0)
            return;

        u32 eff = 0;
        if (!PickFromNames(Utils::Format("固有効果 [%s 一括]", c.name), g_effectNames, false, 0, 0, 0, eff))
            return;
        if (eff > INN_EFFECT_MAX)
        {
            MessageBox("範囲外", Utils::Format("効果IDは 0x%04X までです。", INN_EFFECT_MAX))();
            return;
        }

        std::vector<u32> ids;
        for (u32 id = from; id <= to; ++id)
        {
            u16 cur = 0;
            if (!InnRead(ci, id, cur))
                continue;
            if (f == 1 && cur == 0)
                continue;
            if (f == 2 && cur != 0)
                continue;
            ids.push_back(id);
        }
        if (ids.empty())
        {
            MessageBox("対象なし", "条件に合うものがありません。")();
            return;
        }
        if (!(MessageBox("確認",
                         Utils::Format("%s %s から %s までのうち %u件の\n固有効果を\n%s\nにします。よろしいですか?",
                                       c.name, IdName(from).c_str(), IdName(to).c_str(), (u32)ids.size(),
                                       InnEffName(eff).c_str()),
                         DialogType::DialogYesNo))())
            return;

        u32 n = 0;
        for (u32 id : ids)
            if (InnSet(ci, id, (u16)eff))
                ++n;
        MessageBox("変更しました",
                   Utils::Format("%u 件の固有効果を\n%s\nにしました。\n装備を つけ直すと反映されます。", n,
                                 InnEffName(eff).c_str()))();
    }

    static void InnateEditEntry(int ci)
    {
        const InnateCat &c = g_innCats[ci];
        const int found = InnFind(ci);
        if (found == 0)
            return;
        if (found == 2)
        {
            MessageBox("見つかりません",
                       ci == 2 ? g_innErr : Utils::Format("%sのデータ表がメモリ上に見つかりませんでした。", c.name))();
            return;
        }

        const u32 cnt = InnCount(c);
        for (;;)
        {
            std::vector<std::string> rows;
            rows.push_back("[カテゴリ全体を一括変更]");
            rows.push_back("[変更をすべて元に戻す]");
            for (u32 id = c.idMin; id <= c.idMax; ++id)
            {
                u16 e = 0;
                InnRead(ci, id, e);
                std::string s = IdName(id) + ": " + InnEffName(e);
                if (g_innHas[ci][id - c.idMin])
                    s += " *";
                rows.push_back(s);
            }
            Keyboard kb(Utils::Format("%sの固有効果を変更 (*は変更済み。Bで戻る)", c.name), rows);
            const int sel = kb.Open();
            if (sel < 0)
                return;

            if (sel == 0)
            {
                InnBulk(ci);
                continue;
            }
            if (sel == 1)
            {
                u32 n = 0;
                for (u32 k = 0; k < cnt; ++k)
                    if (g_innHas[ci][k] && InnWrite(ci, c.idMin + k, g_innOrig[ci][k]))
                    {
                        g_innHas[ci][k] = false;
                        ++n;
                    }
                MessageBox("元に戻しました", Utils::Format("%u 件を元に戻しました。", n))();
                continue;
            }

            const u32 id = c.idMin + (u32)(sel - 2);
            u16 cur = 0;
            InnRead(ci, id, cur);
            u32 pick = 0;
            if (!PickFromNames(Utils::Format("固有効果 [%s]", IdName(id).c_str()), g_effectNames,
                               false, 0, 0, cur, pick))
                continue;
            if (pick > INN_EFFECT_MAX)
            {
                MessageBox("範囲外", Utils::Format("効果IDは 0x%04X までです。", INN_EFFECT_MAX))();
                continue;
            }
            if (!InnSet(ci, id, (u16)pick))
            {
                MessageBox("失敗", "書き込めませんでした。")();
                continue;
            }
            MessageBox("変更しました",
                       IdName(id) + " の固有効果を\n" + InnEffName(pick) +
                       "\nにしました。\n装備をつけ直すと反映されます。")();
        }
    }

    // メニューの項目は「固有効果変更」の1つだけ。押すと、武器 / 防具 / ライフ道具 を選ぶ。
    // Bで、カテゴリの選択に戻る。カテゴリの選択でBを押すと終了。
    static void InnateMainEntry(MenuEntry *entry)
    {
        (void)entry;
        for (;;)
        {
            std::vector<std::string> cats;
            for (int i = 0; i < INN_CATS; ++i)
                cats.push_back(g_innCats[i].name);
            Keyboard kb("固有効果を変更するカテゴリを選択", cats);
            const int sel = kb.Open();
            if (sel < 0)
                return;
            InnateEditEntry(sel);
        }
    }

    // ------------------------------------------------------------
    //  CTRPF 必須関数
    // ------------------------------------------------------------
    void PatchProcess(FwkSettings &settings)
    {
        (void)settings;
    }

    void OnProcessExit(void)
    {
    }

    int main(void)
    {
        if (Process::GetTitleID() != 0x00040000000E9C00ULL)
            OSD::Notify("Warning: unexpected title.");

        // タイトルとバージョン表示。バージョン番号 (数字3つ) の版は使わず、名前に含める。
        PluginMenu *menu = new PluginMenu("ファンタジーライフ LINK ver1.7L",
                                          "日本語版用。海外版PLGのItemEditorと同じ方式です。");
        menu->SynchronizeWithFrame(true);
        
menu->Append(new MenuEntry("エリアワープ", nullptr, WarpEntry,
                                   "移動先を選ぶと、通常のロード画面つきで、そのエリアへ移動します。\n"
                                   "出る場所は、選んだ行き先へ向かう扉の前です(扉がある側のエリア)。\n"
                                   "一覧の末尾の[保存]は、「現在位置を保存」で保存した位置です。\n使う前に必ずバックアップを取ってください。"));
        AddLifeChange(menu);

        AddItemEditor(menu);   // アイテムエディタ (装備編集 / 生成 / 削除 / 一括変更)

        AddCraft(menu);   // 制作フォルダ (素材0個 / 99個 / 即完了 / 品質固定)

menu->Append(new MenuEntry("現在位置を保存 (ワープ一覧に追加)", nullptr, SavePosEntry,
                                   "いま立っている位置に名前をつけて保存します。保存した位置は「エリアワープ」の一覧の末尾に並びます。\n"
                                   "別のエリアの位置を選ぶと、そのエリアへ入ってから、保存した位置へ移動します。\n"
                                   "保存先は SD直下の fll_warp_points.txt です。"));
menu->Append(new MenuEntry("その場でロード (現在位置に戻る)", nullptr, ReloadHereEntry,
                                   "いまのエリアを読み込み直して、いまの位置に戻ります。\n"
                                   "モンスターやボスの再出現に使えます。"));
menu->Append(new MenuEntry("探索ワープ (開発用)", nullptr, ExploreEntry,
                                   "出たいエリアIDと入口番号を数字で指定して移動します(そのエリアの、その入口の前に出ます)。\n"
                                   "入口番号の反対側のエリアが未登録のときは、確認が出ます。「扉の自動記録」で扉を通ると登録されます。\n"
                                   "存在しないIDだと落ちる可能性があります。必ずバックアップを取ってください。"));
menu->Append(new MenuEntry("扉の自動記録 (開発用)", DoorLogEntry, nullptr,
                                   "有効にして扉を通ると、here(扉がある側)・dest(扉の向こう)・入口番号を記録します。\n"
                                   "ここに出たい → here/入口番号、扉の先に出たい → dest/入口番号 で数字指定できます。\n"
                                   "記録は SD直下の fll_warp_notes.txt に追記されます。"));
menu->Append(new MenuEntry("位置ワープの記録を保存 (開発用)", nullptr, PosDumpEntry,
                                   "保存位置へのワープの経過 (毎フレームの状態と座標) を、fll_warp_notes.txt に書き出します。\n"
                                   "別のエリアの保存位置へうまく移動できないときの、調査用です。"));

        AddRecommended(menu);
        AddMageRapid(menu);

        // 固有効果変更 (メニューの一番下。押すと 武器 / 防具 / ライフ道具 を選ぶ)
        menu->Append(new MenuEntry("固有効果変更", nullptr, InnateMainEntry,
                                   "武器・防具・ライフ道具の固有効果を、メモリ上のデータ表を書き換えて変更します。\n"
                                   "押すと、カテゴリ(武器 / 防具 / ライフ道具)を選べます。\n"
                                   "1つずつ変更するほか、カテゴリ全体 / ID範囲を同じ効果にまとめて変更できます。\n"
                                   "同じIDのアイテムすべてに効きます。自分のゲームの中だけで有効で、ゲームを終了すると元に戻ります。\n"
                                   "変更したあと、装備をつけ直すと反映されます。\n"
                                   "ライフ道具は、ゲームの中で一度装備してから開いてください。"));

        // プレイヤー切り替えは、アクセスしやすいように一番下に置く
menu->Append(new MenuEntry("プレイヤー切り替え", nullptr, PlayerSwitch::Entry,
                                   "フィールドで実行します。タイトルを経由せず、選んだプレイヤーのデータに切り替えます。\n"
                                   "使う前に、セーブのバックアップを取ってください。"));

        menu->Callback(PlayerSwitch::OnFrame);
        menu->Callback(PosNotifyFrame);
        
        menu->Run();
        delete menu;
        return 0;
    }
}
