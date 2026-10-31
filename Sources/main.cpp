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
#include "QualityFix.hpp"
#include "MaterialZero.hpp"

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
    #define POS_FALLBACK_FRAMES 240
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
    // ------------------------------------------------------------
    //  日本語版のアドレス
    // ------------------------------------------------------------
    static constexpr u32 HOOK_A = 0x002D33F8;   // 海外版 0x2E091C
    static constexpr u32 HOOK_B2 = 0x002BA9AC;  // ライフ道具(実験) 関数の途中
    static constexpr u32 HOOK_WARP = 0x005A2AB8; // エリアワープ: フィールドタスク更新関数の入口

    static constexpr u32 BAG_BASE  = 0x006DDF2C; // かばん1枠目のアイテムID
    static constexpr u32 BAG_SLOTS = 500;
    static constexpr u32 SLOT_SIZE = 0x3C;
    static constexpr u32 OFF_ID    = 0x00;       // u16
    static constexpr u32 OFF_COUNT = 0x06;       // u16 (旧: 0x04 u8 は誤り。行の +6 が個数)
    static constexpr u32 MAX_COUNT = 99;
    static constexpr u32 MAX_ID    = 0x09FE;   // 素材・消耗品まで
    static constexpr u32 MAX_BONUS = 2147483647; // 追加数値の最大(海外版エディタと同じ)

    struct Category
    {
        const char *name;
        u16  idMin, idMax;
        int  offQuality;     // u8   / -1 = なし
        int  offEffect;      // u16
        int  offAtk;         // u16
        int  offMatk;        // u16
        const char *atkName;
        const char *matkName;
        bool stackable;      // true: 同じ種類を1枠にまとめる (素材・消耗品)
        int  offName;        // 製作者名 (UTF-16LE 最大6文字=12バイト) の位置 / -1 = なし
    };

    static const Category g_cats[] =
    {
        { "武器",       0x0000, 0x008F, 0x10, 0x12, 0x1A, 0x1E, "攻撃力", "魔法攻撃力", false, 0x20 },
        { "防具",       0x0099, 0x0386, 0x12, 0x14, 0x1A, 0x1E, "防御力", "魔法防御力", false, 0x24 },
        { "ライフ道具", 0x0387, 0x0435, 0x1E, 0x20, 0x26,   -1, "道具力", nullptr, false, 0x10 },
        { "家具",       0x0436, 0x05C4, 0x20,   -1,   -1,   -1, nullptr,  nullptr, false, 0x10 },
        { "素材・消耗品", 0x05C5, 0x09FE, -1, -1, -1, -1, nullptr, nullptr, true, -1 },
    };

    static constexpr int K_NORMAL = 0;
    static constexpr int K_ITEMID = 1;   // アイテムID (範囲チェック+一覧)
    static constexpr int K_EFFECT = 2;   // 追加効果ID (一覧)

    struct Field
    {
        std::string label;
        u32  off;
        u32  size;   // 1 / 2 / 4 (byte)
        bool hex;
        u32  maxV;
        bool isFloat;   // true: 32bit float として読み書きする
        int  kind;      // K_NORMAL / K_ITEMID / K_EFFECT
        std::vector<std::string> choices;   // 空でなければ「選択式」(値=選択肢の番号)

        // C++11 でもビルドできるよう、集成体初期化ではなくコンストラクタを使う
        Field(const std::string &l, u32 o, u32 sz, bool h, u32 mx, bool fl = false)
            : label(l), off(o), size(sz), hex(h), maxV(mx), isFloat(fl), kind(K_NORMAL) {}
    };

    static u32 SlotAddr(u32 slot) { return BAG_BASE + slot * SLOT_SIZE; }

    static const Category *FindCategory(u16 id)
    {
        for (const Category &c : g_cats)
            if (id >= c.idMin && id <= c.idMax)
                return &c;
        return nullptr;
    }

    // withCount=false のとき「個数」を出さない (装備編集用)
    static std::vector<Field> FieldsFor(const Category *c, bool withCount)
    {
        std::vector<Field> v;
        v.push_back({ "アイテムID", OFF_ID,    2, true,  MAX_ID });
        v.back().kind = K_ITEMID;
        if (withCount)
            v.push_back({ "個数",   OFF_COUNT, 2, false, MAX_COUNT });
        if (!c)
            return v;
        if (c->offQuality >= 0)
        {
            v.push_back({ "クオリティ", (u32)c->offQuality, 1, false, 4 });
            v.back().choices = { "なし", "上", "特上", "最高", "ゴッド" };   // 値 0〜4
        }
        if (c->offEffect >= 0)
        {
            v.push_back({ "追加効果", (u32)c->offEffect, 2, true, 0xFFFF });
            v.back().kind = K_EFFECT;
        }
        if (c->offAtk >= 0)
            v.push_back({ std::string("追加") + c->atkName, (u32)c->offAtk - 2, 4, false, MAX_BONUS, true });
        if (c->offMatk >= 0)
            v.push_back({ std::string("追加") + c->matkName, (u32)c->offMatk - 2, 4, false, MAX_BONUS, true });
        return v;
    }

    static bool ReadField(u32 base, const Field &f, u32 &out)
    {
        if (f.isFloat)
        {
            u32 raw = 0;
            if (!Process::Read32(base + f.off, raw)) return false;
            float t = 0.0f;
            std::memcpy(&t, &raw, 4);
            // 負数・NaN・巨大値は 0xFFFFFFFF(異常値)として扱う
            out = (t >= 0.0f && t < 4.0e9f) ? (u32)t : 0xFFFFFFFFu;
            return true;
        }
        if (f.size == 1)
        {
            u8 t = 0;
            if (!Process::Read8(base + f.off, t)) return false;
            out = t;
            return true;
        }
        u16 t = 0;
        if (!Process::Read16(base + f.off, t)) return false;
        out = t;
        return true;
    }

    static bool WriteField(u32 base, const Field &f, u32 val)
    {
        if (f.isFloat)
        {
            float t = (float)val;
            u32 raw = 0;
            std::memcpy(&raw, &t, 4);
            return Process::Write32(base + f.off, raw);
        }
        if (f.size == 1)
            return Process::Write8(base + f.off, (u8)val);
        return Process::Write16(base + f.off, (u16)val);
    }

    static const char *LookupName(const NameEntry *table, u32 id)
    {
        for (const NameEntry *e = table; e->name; ++e)
            if (e->id == id)
                return e->name;
        return nullptr;
    }

    static std::string FmtValue(const Field &f, u32 v)
    {
        if (!f.choices.empty() && v < f.choices.size())
            return f.choices[v];
        if (f.isFloat && v == 0xFFFFFFFFu)
            return "異常値";
        if (f.kind == K_ITEMID || f.kind == K_EFFECT)
        {
            // 名前が分かるときは名前だけを出す。表に無いIDのときだけ16進で出す。
            const char *n = LookupName(f.kind == K_ITEMID ? g_itemNames : g_effectNames, v);
            if (n)
                return std::string(n);
            return Utils::Format("0x%04X", v);
        }
        return f.hex ? Utils::Format("0x%04X", v) : Utils::Format("%u", v);
    }

    static bool AskValue(const std::string &msg, bool hex, u32 maxV, u32 cur, u32 &out)
    {
        Keyboard kb(msg);
        kb.IsHexadecimal(hex);
        int len = 4;
        if (!hex)
        {
            len = 1;
            for (u32 t = maxV; t >= 10; t /= 10)
                ++len;
        }
        kb.SetMaxLength(len);
        u32 v = cur;
        if (kb.Open(v, cur) != 0)
            return false;
        if (v > maxV)
            v = maxV;
        out = v;
        return true;
    }

    // 選択式の入力。キャンセルなら false
    static bool AskChoice(const std::string &msg, const std::vector<std::string> &choices, u32 &out)
    {
        Keyboard kb(msg, choices);
        int sel = kb.Open();
        if (sel < 0)
            return false;
        out = (u32)sel;
        return true;
    }

    // 16進のIDを入力する。restrict=true のときは lo〜hi の範囲外を弾く
    static bool AskHexId(const std::string &title, u32 cur, bool restrict, u32 lo, u32 hi, u32 &out)
    {
        for (;;)
        {
            Keyboard kb(title);
            kb.IsHexadecimal(true);
            kb.SetMaxLength(4);
            u32 v = cur;
            if (kb.Open(v, cur) != 0)
                return false;
            if (restrict && (v < lo || v > hi))
            {
                MessageBox("範囲外のID",
                           Utils::Format("入力できるIDは 0x%04X から 0x%04X までです。", lo, hi))();
                continue;
            }
            out = v;
            return true;
        }
    }

    // 「[ID入力]」+ 名前の一覧。一覧が空のときは、ID入力だけになる。
    static bool PickFromNames(const std::string &title, const NameEntry *table,
                              bool restrict, u32 lo, u32 hi, u32 cur, u32 &out)
    {
        std::vector<std::string> opts;
        std::vector<u32> ids;
        opts.push_back(Utils::Format("[ID入力] 現在 0x%04X", cur));
        ids.push_back(0);
        for (const NameEntry *e = table; e->name; ++e)
        {
            if (restrict && (e->id < lo || e->id > hi))
                continue;
            opts.push_back(e->name);
            ids.push_back(e->id);
        }

        const std::string hexTitle = title + " (16進)";
        if (opts.size() == 1)
            return AskHexId(hexTitle, cur, restrict, lo, hi, out);

        for (;;)
        {
            Keyboard kb(title, opts);
            int sel = kb.Open();
            if (sel < 0)
                return false;
            if (sel == 0)
            {
                if (AskHexId(hexTitle, cur, restrict, lo, hi, out))
                    return true;
                continue;
            }
            out = ids[sel];
            return true;
        }
    }

    // 項目ごとの入力方法をまとめたもの (装備編集・かばん編集・一括変更で共通)
    static bool AskFieldValue(const Field &f, const Category *cat, u32 cur, u32 &out)
    {
        if (!f.choices.empty())
            return AskChoice(f.label, f.choices, out);

        if (f.kind == K_ITEMID)
        {
            // 種類の範囲内のIDだけを許可する (範囲外は弾く)
            if (cat)
                return PickFromNames(Utils::Format("アイテムID [%s]", cat->name), g_itemNames,
                                     true, cat->idMin, cat->idMax, cur, out);
            return PickFromNames("アイテムID", g_itemNames, false, 0, MAX_ID, cur, out);
        }
        if (f.kind == K_EFFECT)
            return PickFromNames(f.label, g_effectNames, false, 0, 0xFFFF, cur, out);

        return AskValue(f.label + (f.hex ? " (16進)" : ""), f.hex, f.maxV, cur, out);
    }

    static void ShowDump(u32 base)
    {
        std::string s;
        for (u32 i = 0; i < SLOT_SIZE; ++i)
        {
            u8 b = 0;
            Process::Read8(base + i, b);
            s += Utils::Format("%02X ", b);
            if (i % 12 == 11)
                s += "\n";
        }
        MessageBox(Utils::Format("アドレス 0x%08X", base), s)();
    }

    // ------------------------------------------------------------
    //  指定アドレスのアイテムを編集する共通UI
    //  slotMode=true のときは「別の枠を選ぶ」を出す
    //  戻り値: true = 別の枠を選ぶ / false = 終了
    // ------------------------------------------------------------
    static bool EditAt(u32 base, const std::string &title, bool slotMode)
    {
        for (;;)
        {
            u16 id = 0;
            if (!Process::Read16(base + OFF_ID, id))
            {
                MessageBox("エラー", "メモリを読み取れません。")();
                return false;
            }

            const Category *cat = FindCategory(id);
            std::vector<Field> fields = FieldsFor(cat, slotMode);   // 装備編集では個数を出さない

            std::vector<std::string> opts;
            for (const Field &f : fields)
            {
                u32 v = 0;
                ReadField(base, f, v);
                opts.push_back(f.label + ": " + FmtValue(f, v));
            }
            opts.push_back("Hexダンプを表示");
            if (slotMode)
                opts.push_back("別の枠を選ぶ");

            Keyboard kb(Utils::Format("%s [%s]", title.c_str(), cat ? cat->name : "空き/未対応"), opts);
            int sel = kb.Open();
            if (sel < 0)
                return false;

            if ((size_t)sel < fields.size())
            {
                const Field &f = fields[sel];

                u32 cur = 0, val = 0;
                ReadField(base, f, cur);
                if (AskFieldValue(f, cat, cur, val))
                {
                    if (!WriteField(base, f, val))
                        MessageBox("エラー", "書き込みに失敗しました。")();
                }
            }
            else if ((size_t)sel == fields.size())
                ShowDump(base);
            else
                return true;
        }
    }

    // ------------------------------------------------------------
    //  海外版と同じ「新しく装備したアイテムを編集」
    // ------------------------------------------------------------
    // エディタを開いている間、ゲームを止める(止めないとボタン入力がゲームにも届く)
    struct PauseGuard
    {
        PauseGuard()  { Process::Pause(); }
        ~PauseGuard() { Process::Play(); }
    };

    static Hook g_hookA;
    static Hook g_hookB2;
    static bool g_hooksInstalled = false;

    static bool CheckWords(u32 addr, const u32 *expect, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            u32 v = 0;
            if (!Process::Read32(addr + 4 * i, v) || v != expect[i])
                return false;
        }
        return true;
    }

    static bool InstallHooks(void)
    {
        if (g_hooksInstalled)
            return true;

        // 置き換える3命令が想定どおりか確認 (違うバージョンなら入れない)
        static const u32 expectA[3] = { 0xE24DD00C, 0xE1A07000, 0xE3E00000 }; // sub sp,#0xC / mov r7,r0 / mvn r0,#0
        // ライフ道具(ハンマー等): str r4,[r5,r6,lsl#2] / ldr r0,[r4,#0xc] / orr r0,r0,#1
        static const u32 expectB2[3] = { 0xE7854106, 0xE594000C, 0xE3800001 };
        if (!CheckWords(HOOK_A - 4, expectA, 3) || !CheckWords(HOOK_B2 - 4, expectB2, 3))
        {
            PauseGuard pg;
            MessageBox("エラー",
                       "フック位置の命令が想定と違います。\nゲームのバージョンが異なる可能性があるため、機能を有効にできません。")();
            return false;
        }

        // PLGと同じ手順: found-4 に str pc,[sp,#-4]! を書き、found に ldr pc 形式のフックを入れる
        Process::Write32(HOOK_A - 4, 0xE52DF004);

        g_hookA.Initialize(HOOK_A, (u32)(uintptr_t)jp_hookA);
        g_hookA.Enable();

        Process::Write32(HOOK_B2 - 4, 0xE52DF004);
        g_hookB2.Initialize(HOOK_B2, (u32)(uintptr_t)jp_hookB2);
        g_hookB2.Enable();

        g_hooksInstalled = true;
        return true;
    }

    // 有効な間、毎フレーム呼ばれる(SynchronizeWithFrame により 約60回/秒)
    //
    //  ・装備を検出すると案内を出し、PENDING_FRAMES の間だけ Y で編集できる。
    //  ・編集後に「反映のため同じ装備をつけ直す」操作は、編集の対象にしない。
    //    (通常プレイ画面に戻った後で Y を押しても編集メニューが出ないようにする)
    static constexpr u32 PENDING_FRAMES = 600;    // 約10秒
    static constexpr u32 APPLIED_FRAMES = 1800;   // 約30秒

    static void EquipEditEntry(MenuEntry *entry)
    {
        static u32 lastCount    = 0;
        static u32 pending      = 0;   // Yで編集できる対象
        static u32 pendingTimer = 0;
        static u32 appliedPtr   = 0;   // 編集済み(次の同一装備は反映用として無視)
        static u32 appliedTimer = 0;

        if (!InstallHooks())
        {
            entry->Disable();
            return;
        }

        if (pendingTimer && --pendingTimer == 0)
            pending = 0;
        if (appliedTimer && --appliedTimer == 0)
            appliedPtr = 0;

        const u32 c = g_capCount;
        if (c != lastCount)
        {
            lastCount = c;
            const u32 p = g_capPtr;
            if (appliedPtr && p == appliedPtr)
            {
                // 編集後の「反映のための装備」: 案内も出さず、Yでも開かない
                appliedPtr   = 0;
                appliedTimer = 0;
                pending      = 0;
                pendingTimer = 0;
            }
            else
            {
                pending      = p;
                pendingTimer = PENDING_FRAMES;
                appliedPtr   = 0;
                appliedTimer = 0;
                // 画面上部の通知(OSD)は日本語が表示できないため、英数字のみ
                OSD::Notify("Item captured. Press Y to edit.");
            }
        }

        if (pending && Controller::IsKeyPressed(Key::Y))
        {
            const u32 target = pending;
            pending      = 0;
            pendingTimer = 0;

            {
                PauseGuard pg;
                EditAt(target, "装備アイテムの編集", false);
                MessageBox("編集の終了", "外して装備し直すと反映されます。")();
            }

            // この後の同じ装備のつけ直しは「反映」として扱う
            appliedPtr   = target;
            appliedTimer = APPLIED_FRAMES;
        }
    }

    // ------------------------------------------------------------
    //  カテゴリ別 一括変更
    //  1) カテゴリを選ぶ (かばん内の該当アイテムの件数を数える)
    //  2) 変えたい項目だけ、変更後の値を選ぶ (この時点では書き込まない)
    //  3) 「決定して適用」で、かばん内の該当アイテムすべてに書き込む
    //  アイテムIDは、ID範囲を指定して「被りなしの連番」で割り当てるか、
    //  1件ずつ選ぶ。選べるIDは、そのカテゴリのID範囲内だけ。
    // ------------------------------------------------------------
    static constexpr u32 NO_ID = 0xFFFFFFFFu;   // 「このアイテムのIDは変えない」の印

    static std::string IdName(u32 id)
    {
        const char *n = LookupName(g_itemNames, id);
        return n ? std::string(n) : Utils::Format("0x%04X", id);
    }

    // かばんの連結リスト(管理領域 0x6DCD2C)をたどって、使用中の枠番号を集める。想定と違えば false。
    // 捨てた・削除した枠には、古いIDが残ることがあるので、IDだけでは使用中かどうか判断できない。
    static bool WalkLiveBagSlots(std::vector<bool> &inUse)
    {
        static const u32 LIVE = 0x006DCD2C;
        u32 p = 0;
        u16 head = 0, cnt = 0;
        if (!Process::Read32(LIVE + 0x08, p) || p != BAG_BASE - 4)
            return false;
        if (!Process::Read16(LIVE + 0x1E, head) || !Process::Read16(LIVE + 0x1C, cnt))
            return false;
        inUse.assign(BAG_SLOTS, false);
        u32 n = 0;
        u16 idx = head;
        while (idx != 0xFFFF)
        {
            if (idx >= BAG_SLOTS || inUse[idx])
                return false;
            inUse[idx] = true;
            ++n;
            u16 next = 0;
            if (!Process::Read16(BAG_BASE - 4 + (u32)idx * SLOT_SIZE, next))
                return false;
            idx = next;
        }
        return n == cnt;
    }

    // かばんの中で、このカテゴリのアイテムがある枠と、その現在のIDを集める
    static void CollectBagSlots(const Category &cat, std::vector<u32> &slots, std::vector<u32> &ids)
    {
        std::vector<bool> inUse;
        const bool useList = WalkLiveBagSlots(inUse);   // 失敗したときは、IDだけで判断する
        for (u32 slot = 0; slot < BAG_SLOTS; ++slot)
        {
            if (useList && !inUse[slot])
                continue;
            u16 id = 0;
            if (!Process::Read16(SlotAddr(slot) + OFF_ID, id))
                continue;
            if (id < cat.idMin || id > cat.idMax)
                continue;
            slots.push_back(slot);
            ids.push_back(id);
        }
    }

    // 開始ID(と終了ID)を選び、その範囲の「被らないID」を小さい順に n 個決める
    static bool PlanRange(const Category &cat, size_t n, std::vector<u32> &out, std::string &summary)
    {
        u32 from = cat.idMin;
        if (!PickFromNames(Utils::Format("開始ID [%s]", cat.name), g_itemNames,
                           true, cat.idMin, cat.idMax, cat.idMin, from))
            return false;

        u32 to = cat.idMax;
        std::vector<std::string> how;
        how.push_back(Utils::Format("かばんの件数(%u件)ぶん、連番にする", (u32)n));
        how.push_back("終了IDも自分で選ぶ");
        Keyboard kh(Utils::Format("開始ID: %s", IdName(from).c_str()), how);
        const int h = kh.Open();
        if (h < 0)
            return false;
        if (h == 1)
        {
            if (!PickFromNames(Utils::Format("終了ID [%s]", cat.name), g_itemNames,
                               true, from, cat.idMax, cat.idMax, to))
                return false;
        }

        // 範囲内にある、実在するIDを集める (名前の表が無いときは番号をそのまま使う)
        std::vector<u32> pool;
        if (g_itemNames[0].name)
        {
            for (const NameEntry *e = g_itemNames; e->name; ++e)
                if (e->id >= from && e->id <= to)
                    pool.push_back(e->id);
            std::sort(pool.begin(), pool.end());
            pool.erase(std::unique(pool.begin(), pool.end()), pool.end());
        }
        else
        {
            for (u32 id = from; id <= to; ++id)
                pool.push_back(id);
        }

        if (pool.size() < n)
        {
            MessageBox("IDが足りません",
                       Utils::Format("選んだ範囲のアイテムは %u 個で、かばんの %u 件より少ないです。\n"
                                     "範囲を広げて選び直してください。",
                                     (u32)pool.size(), (u32)n))();
            return false;
        }
        out.assign(pool.begin(), pool.begin() + n);
        summary = Utils::Format("連番 0x%04X-0x%04X (%u件)", out.front(), out.back(), (u32)n);
        return true;
    }

    // 1件ずつ選ぶ。newIds は枠ごとの変更後ID (NO_ID なら変えない)
    static bool PlanEach(const Category &cat, const std::vector<u32> &slots,
                         const std::vector<u32> &curIds, std::vector<u32> &newIds)
    {
        for (;;)
        {
            std::vector<std::string> rows;
            for (size_t i = 0; i < slots.size(); ++i)
            {
                std::string s = Utils::Format("%u枠目 ", slots[i] + 1) + IdName(curIds[i]);
                if (newIds[i] != NO_ID)
                    s += " > " + IdName(newIds[i]);
                rows.push_back(s);
            }
            Keyboard kb(Utils::Format("1件ずつ選ぶ [%s] (Bで戻る)", cat.name), rows);
            const int sel = kb.Open();
            if (sel < 0)
                break;

            const u32 cur = (newIds[sel] != NO_ID) ? newIds[sel] : curIds[sel];
            u32 pick = 0;
            if (PickFromNames(Utils::Format("%u枠目 [%s]", slots[sel] + 1, cat.name), g_itemNames,
                              true, cat.idMin, cat.idMax, cur, pick))
                newIds[sel] = (pick == curIds[sel]) ? NO_ID : pick;
        }
        for (u32 v : newIds)
            if (v != NO_ID)
                return true;
        return false;
    }

    static void BulkEditEntry(MenuEntry *entry)
    {
        (void)entry;

        std::vector<std::string> names;
        for (const Category &c : g_cats)
            names.push_back(Utils::Format("%s (ID 0x%04X-0x%04X)", c.name, c.idMin, c.idMax));

        Keyboard kc("一括変更するカテゴリを選択", names);
        int ci = kc.Open();
        if (ci < 0)
            return;
        const Category &cat = g_cats[ci];

        std::vector<u32> slots, curIds;
        CollectBagSlots(cat, slots, curIds);
        if (slots.empty())
        {
            MessageBox("対象なし", Utils::Format("かばんに「%s」がありません。", cat.name))();
            return;
        }
        const u32 N = (u32)slots.size();

        std::vector<Field> fields = FieldsFor(&cat, cat.stackable);   // [0]=ID、[1]以降=ほかの項目 (個数は素材・消耗品だけ)
        const size_t F = fields.size();
        std::vector<bool> use(F, false);        // その項目を変更するか
        std::vector<u32>  val(F, 0);            // 変更後の値
        std::vector<u32>  newIds(slots.size(), NO_ID);
        std::string       idSummary;

        for (;;)
        {
            bool anyId = false;
            for (u32 v : newIds)
                if (v != NO_ID) { anyId = true; break; }
            bool anyField = false;
            for (size_t i = 1; i < F; ++i)
                if (use[i]) anyField = true;

            // 行の番号 = 項目の番号 (0=アイテムID)。その次が「決定」「元に戻す」。
            std::vector<std::string> rows;
            rows.push_back(std::string("アイテムID: ") + (anyId ? idSummary : std::string("変更しない")));
            for (size_t i = 1; i < F; ++i)
                rows.push_back(fields[i].label + ": " +
                               (use[i] ? FmtValue(fields[i], val[i]) : std::string("変更しない")));
            rows.push_back(Utils::Format("[決定して適用] 対象 %u件", N));
            rows.push_back("[すべて元に戻す]");

            Keyboard kb(Utils::Format("一括変更 [%s] かばん内 %u件", cat.name, N), rows);
            const int sel = kb.Open();

            if (sel < 0)
            {
                if ((anyId || anyField) &&
                    !(MessageBox("確認", "変更内容を破棄して戻りますか?", DialogType::DialogYesNo))())
                    continue;
                return;
            }

            if ((size_t)sel == F + 1)           // すべて元に戻す
            {
                for (size_t i = 0; i < F; ++i) { use[i] = false; val[i] = 0; }
                for (size_t k = 0; k < newIds.size(); ++k) newIds[k] = NO_ID;
                idSummary.clear();
                continue;
            }

            if ((size_t)sel == F)               // 決定して適用
            {
                if (!anyId && !anyField)
                {
                    MessageBox("変更なし", "変更する項目を選んでください。")();
                    continue;
                }
                std::string msg = Utils::Format("かばん内の「%s」%u件に、次を適用します。\n", cat.name, N);
                if (anyId)
                    msg += "・アイテムID: " + idSummary + "\n";
                for (size_t i = 1; i < F; ++i)
                    if (use[i])
                        msg += "・" + fields[i].label + ": " + FmtValue(fields[i], val[i]) + "\n";
                msg += "よろしいですか?";
                if (!(MessageBox("確認", msg, DialogType::DialogYesNo))())
                    continue;

                u32 n = 0;
                for (size_t k = 0; k < slots.size(); ++k)
                {
                    const u32 base = SlotAddr(slots[k]);
                    bool ok = true;
                    for (size_t i = 1; i < F; ++i)
                        if (use[i] && !WriteField(base, fields[i], val[i]))
                            ok = false;
                    if (newIds[k] != NO_ID && !WriteField(base, fields[0], newIds[k]))
                        ok = false;
                    if (ok)
                        ++n;
                }
                MessageBox("完了", Utils::Format("%u 件を変更しました。", n))();
                return;
            }

            if (sel == 0)                       // アイテムID
            {
                std::vector<std::string> m;
                m.push_back(Utils::Format("範囲を指定して連番で割り当て (被りなし・%u件)", N));
                m.push_back("1件ずつ選ぶ");
                m.push_back("変更しない");
                Keyboard km(Utils::Format("アイテムID [%s] かばん内 %u件", cat.name, N), m);
                const int mi = km.Open();
                if (mi == 0)
                {
                    std::vector<u32> ids;
                    std::string sm;
                    if (PlanRange(cat, slots.size(), ids, sm))
                    {
                        newIds = ids;
                        idSummary = sm;
                    }
                }
                else if (mi == 1)
                {
                    PlanEach(cat, slots, curIds, newIds);
                    u32 c = 0;
                    for (u32 v : newIds)
                        if (v != NO_ID) ++c;
                    idSummary = Utils::Format("1件ずつ指定 (%u件)", c);
                }
                else if (mi == 2)
                {
                    for (size_t k = 0; k < newIds.size(); ++k) newIds[k] = NO_ID;
                    idSummary.clear();
                }
                continue;
            }

            // ほかの項目
            const size_t i = (size_t)sel;
            if (use[i])
            {
                std::vector<std::string> m;
                m.push_back("値を選び直す");
                m.push_back("変更しない (取り消し)");
                Keyboard km(Utils::Format("%s: %s", fields[i].label.c_str(), FmtValue(fields[i], val[i]).c_str()), m);
                const int mi = km.Open();
                if (mi < 0)
                    continue;
                if (mi == 1)
                {
                    use[i] = false;
                    continue;
                }
            }
            u32 v = 0;
            if (AskFieldValue(fields[i], &cat, use[i] ? val[i] : 0, v))
            {
                use[i] = true;
                val[i] = v;
            }
        }
    }

    // ------------------------------------------------------------
    //  アイテム生成 / 一括生成 (かばん・倉庫の連結リストを直接操作)
    //
    //  ゲーム本人が1個買ったときのRAM差分(Before/After)から分かったこと:
    //   ・行の本体は 0x38 バイト。行の直前4バイト(= 前の行の +0x38)が (次u16,前u16) のリンク。
    //   ・かばんの「生きている」先頭と件数は、保存用の 0x6DDF24 ではなく
    //       0x6DCD2C を先頭とする管理領域にある: +0x1C=件数(u16)、+0x1E=先頭の行番号(u16, 空=FFFF)
    //   ・新しい行は 499 番から使われ、行は「ID・個数=1・+0xC=FFFE・+0xE=1・+0x28から 1,2,3,4」
    //     以外は 0 で初期化される。(防具で確認。他の種類は未検証)
    // ------------------------------------------------------------
    //   ・倉庫も同じ作り(ダンプで確認: 管理領域 0x6DCD70、リンク配列 0x6E5460、1500枠、先頭は1499番から)。
    static constexpr u32 ROW_DATA   = 0x38;
    static constexpr u16 NO_LINK    = 0xFFFF;
    static constexpr u32 EMPTY_ID   = 0x0666;              // 空き行の初期値(ゲームが最初に全行へ入れているID)。削除した行に書く
    static constexpr size_t DUP_THRESHOLD = 3;             // 名前がこの件数以上かぶると「未実装」とみなす

    // 連結リストを持つ入れ物。live=管理領域 (+8 リンク配列のアドレス、+0x1C 件数、+0x1E 先頭)
    // リンク配列のk番目 = linkBase + 0x3C*k、行のk番目 = linkBase + 4 + 0x3C*k
    struct Container
    {
        const char *name;
        u32 live;
        u32 linkBase;
        u32 slots;
        u32 arr;      // 空き枠の番号表 (u16 が slots 個)。管理領域の +0x10 が指す
    };
    static const Container g_bag   = { "かばん", 0x006DCD2C, 0x006DDF28,  500, 0x006DCDF8 };
    static const Container g_store = { "倉庫",   0x006DCD70, 0x006E5460, 1500, 0x006DD1E0 };

    struct BagState
    {
        const Container  *c = &g_bag;
        std::vector<u16>  order;   // 先頭から末尾までの行番号
        std::vector<bool> used;
        u16               authorMark = 0x16D7;   // 製作者名つきの行の +0xE の値 (かばん内の実物から取る)
    };

    static u32 RowOf(const BagState &b, u32 slot) { return b.c->linkBase + 4 + slot * SLOT_SIZE; }
    static u32 LnkOf(const BagState &b, u32 slot) { return b.c->linkBase + slot * SLOT_SIZE; }

    // 管理領域と連結リストを読む。想定と違えば false (何も書かない)
    static bool LoadBag(BagState &b, const Container &c, std::string &err)
    {
        b.c = &c;
        u32 p = 0;
        if (!Process::Read32(c.live + 0x08, p) || p != c.linkBase)
        {
            err = Utils::Format("%sの管理領域が想定と違います。(バージョン違いの可能性。何も書いていません)", c.name);
            return false;
        }
        u32 ap = 0;
        if (!Process::Read32(c.live + 0x10, ap) || ap != c.arr)
        {
            err = Utils::Format("%sの空き枠の表が想定と違います。(何も書いていません)", c.name);
            return false;
        }
        u16 head = 0, cnt = 0;
        if (!Process::Read16(c.live + 0x1E, head) || !Process::Read16(c.live + 0x1C, cnt))
        {
            err = "管理領域を読めません。";
            return false;
        }
        b.order.clear();
        b.used.assign(c.slots, false);
        u16 idx = head;
        while (idx != NO_LINK)
        {
            if (idx >= c.slots || b.used[idx] || b.order.size() >= c.slots)
            {
                err = Utils::Format("%sの連結リストが想定と違います。(何も書いていません)", c.name);
                return false;
            }
            b.used[idx] = true;
            b.order.push_back(idx);
            u16 next = 0;
            if (!Process::Read16(LnkOf(b, idx), next))
            {
                err = "リンクを読めません。";
                return false;
            }
            idx = next;
        }
        // 製作者名つきの行は +0xE が 0x16D7 (名前なしの行は 0x0001)。かばんに実物があればその値を使う
        b.authorMark = 0x16D7;
        for (u16 s : b.order)
        {
            u16 c0 = 0, e = 0;
            if (Process::Read16(RowOf(b, s) + 0x0C, c0) && Process::Read16(RowOf(b, s) + 0x0E, e) &&
                c0 == 0xFFFE && e != 0 && e != 1)
            {
                b.authorMark = e;
                break;
            }
        }
        if (cnt != (u16)b.order.size())
        {
            err = Utils::Format("件数(%u)と連結リストの長さ(%u)が合いません。(何も書いていません)",
                                (u32)cnt, (u32)b.order.size());
            return false;
        }
        return true;
    }

    static int PickFreeRow(const BagState &b)
    {
        for (int k = (int)b.c->slots - 1; k >= 0; --k)
            if (!b.used[k])
                return k;
        return -1;
    }

    static u32 FreeRows(const BagState &b)
    {
        u32 n = 0;
        for (u32 k = 0; k < b.c->slots; ++k)
            if (!b.used[k])
                ++n;
        return n;
    }

    // 空き枠の表を作り直す。ゲームは「表の arr[件数] を次に使う枠」として取り出し、
    // 削除では arr[件数-1] に外した枠を入れる。arr[件数 以降] = 今あいている枠すべて、が成り立つ必要がある。
    // (これが崩れると、次にゲームがアイテムを入手したときに使用中の枠を取り出してフリーズする)
    static void SyncFreeArray(const BagState &b)
    {
        u32 w = (u32)b.order.size();
        for (int k = (int)b.c->slots - 1; k >= 0; --k)
            if (!b.used[k])
                Process::Write16(b.c->arr + (w++) * 2, (u16)k);
    }

    static void CommitCount(const BagState &b)
    {
        Process::Write16(b.c->live + 0x1C, (u16)b.order.size());
        SyncFreeArray(b);
    }

    // ------------------------------------------------------------
    //  生成時に指定する追加項目 (クオリティ・追加効果・追加数値・素材の個数)
    //  fields[] はアイテムIDを除いた項目。use[i]=true の項目だけ値を書く。
    // ------------------------------------------------------------
    struct GenSpec
    {
        std::vector<Field> fields;
        std::vector<bool>  use;
        std::vector<u32>   val;
        // 製作者名 (武器・防具・ライフ道具・家具だけ)
        bool               hasName = false;
        bool               useName = false;
        u32                nameOff = 0;
        std::vector<u16>   name;       // 最大6文字 (UTF-16)
    };

    // ---- 製作者名の入力 ----
    static constexpr size_t NAME_CHARS = 6;

    static std::vector<u16> Utf8ToUtf16(const std::string &s)
    {
        std::vector<u16> o;
        size_t i = 0;
        while (i < s.size())
        {
            const u8 c = (u8)s[i];
            u32 cp = 0;
            size_t n = 0;
            if (c < 0x80)              { cp = c;        n = 1; }
            else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 2; }
            else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 3; }
            else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 4; }
            else { ++i; continue; }
            if (i + n > s.size())
                break;
            for (size_t k = 1; k < n; ++k)
                cp = (cp << 6) | ((u8)s[i + k] & 0x3F);
            i += n;
            if (cp != 0 && cp <= 0xFFFF)
                o.push_back((u16)cp);
        }
        return o;
    }

    static std::string Utf16ToUtf8(const std::vector<u16> &v)
    {
        std::string o;
        for (u16 c : v)
        {
            if (c == 0)
                break;
            if (c < 0x80)
                o += (char)c;
            else if (c < 0x800)
            {
                o += (char)(0xC0 | (c >> 6));
                o += (char)(0x80 | (c & 0x3F));
            }
            else
            {
                o += (char)(0xE0 | (c >> 12));
                o += (char)(0x80 | ((c >> 6) & 0x3F));
                o += (char)(0x80 | (c & 0x3F));
            }
        }
        return o;
    }

    static const char *const KANA_ROWS[] =
    {
        "あいうえお", "かきくけこ", "さしすせそ", "たちつてと", "なにぬねの",
        "はひふへほ", "まみむめも", "やゆよ", "らりるれろ", "わをん",
        "がぎぐげご", "ざじずぜぞ", "だぢづでど", "ばびぶべぼ", "ぱぴぷぺぽ", "ぁぃぅぇぉっゃゅょ",
        "アイウエオ", "カキクケコ", "サシスセソ", "タチツテト", "ナニヌネノ",
        "ハヒフヘホ", "マミムメモ", "ヤユヨ", "ラリルレロ", "ワヲン",
        "ガギグゲゴ", "ザジズゼゾ", "ダヂヅデド", "バビブベボ", "パピプペポ", "ヴァィゥェォッャュョ",
        "０１２３４５６７８９", "ＡＢＣＤＥＦＧＨＩＪ", "ＫＬＭＮＯＰＱＲＳＴ", "ＵＶＷＸＹＺ", "ー・！？"
    };

    // かばん・倉庫の中にある、製作者名つきのアイテムから名前をコピーする
    static bool CopyNameFromBag(std::vector<u16> &out)
    {
        std::vector<std::string> labels;
        std::vector<std::vector<u16> > names;
        const Container *conts[2] = { &g_bag, &g_store };
        for (const Container *ct : conts)
        {
            BagState b;
            std::string err;
            if (!LoadBag(b, *ct, err))
                continue;
            for (u16 s : b.order)
            {
                u32 id = 0;
                Process::Read32(RowOf(b, s) + OFF_ID, id);
                const Category *cg = FindCategory((u16)id);
                if (!cg || cg->offName < 0)
                    continue;
                std::vector<u16> nm;
                for (size_t i = 0; i < NAME_CHARS; ++i)
                {
                    u16 ch = 0;
                    Process::Read16(RowOf(b, s) + (u32)cg->offName + (u32)i * 2, ch);
                    if (ch == 0)
                        break;
                    nm.push_back(ch);
                }
                if (nm.empty())
                    continue;
                labels.push_back(Utils::Format("%s%u枠目 %s: ", ct->name, s + 1, IdName(id).c_str()) + Utf16ToUtf8(nm));
                names.push_back(nm);
                if (labels.size() >= 60)
                    break;
            }
        }
        if (labels.empty())
        {
            MessageBox("コピーできません", "かばん・倉庫に、製作者名つきのアイテムがありません。")();
            return false;
        }
        Keyboard kb("名前をコピーするアイテム", labels);
        const int sel = kb.Open();
        if (sel < 0)
            return false;
        out = names[sel];
        return true;
    }

    // 製作者名を決める。決定したら true (name に入る)、キャンセルなら false
    static bool AskMakerName(std::vector<u16> &name)
    {
        std::vector<u16> cur = name;
        for (;;)
        {
            std::vector<std::string> rows;
            rows.push_back("[かな・記号を1文字追加]");
            rows.push_back("[キーボードで英数字を入力]");
            rows.push_back("[かばんのアイテムから名前をコピー]");
            rows.push_back("[1文字消す]");
            rows.push_back("[全部消す]");
            rows.push_back("[決定]");
            Keyboard kb(Utils::Format("製作者名: 「%s」 (%u/%u文字)", Utf16ToUtf8(cur).c_str(),
                                      (u32)cur.size(), (u32)NAME_CHARS), rows);
            const int sel = kb.Open();
            if (sel < 0)
                return false;
            if (sel == 5)
            {
                name = cur;
                return true;
            }
            if (sel == 0)
            {
                if (cur.size() >= NAME_CHARS)
                {
                    MessageBox("文字数", "6文字までです。")();
                    continue;
                }
                std::vector<std::string> groups;
                for (const char *g : KANA_ROWS)
                    groups.push_back(g);
                Keyboard kg("文字の行を選ぶ", groups);
                const int gi = kg.Open();
                if (gi < 0)
                    continue;
                const std::vector<u16> chars = Utf8ToUtf16(KANA_ROWS[gi]);
                std::vector<std::string> cs;
                for (u16 c : chars)
                    cs.push_back(Utf16ToUtf8(std::vector<u16>(1, c)));
                Keyboard kk("文字を選ぶ", cs);
                const int ki = kk.Open();
                if (ki >= 0)
                    cur.push_back(chars[ki]);
            }
            else if (sel == 1)
            {
                std::string in;
                Keyboard ka("英数字を入力 (最大6文字)");
                if (ka.Open(in) == 0)
                {
                    std::vector<u16> v = Utf8ToUtf16(in);
                    if (v.size() > NAME_CHARS)
                        v.resize(NAME_CHARS);
                    cur = v;
                }
            }
            else if (sel == 2)
            {
                std::vector<u16> v;
                if (CopyNameFromBag(v))
                    cur = v;
            }
            else if (sel == 3)
            {
                if (!cur.empty())
                    cur.pop_back();
            }
            else if (sel == 4)
                cur.clear();
        }
    }

    static u32 SpecValue(const GenSpec *sp, u32 off, u32 def)
    {
        if (!sp)
            return def;
        for (size_t i = 0; i < sp->fields.size(); ++i)
            if (sp->fields[i].off == off && sp->use[i])
                return sp->val[i];
        return def;
    }

    // 追加項目を選ぶ画面 (一括変更の画面と同じ作り)。キャンセルなら false
    static bool AskGenSpec(const Category *cat, const std::string &title, GenSpec &sp, u32 &count)
    {
        sp.fields = FieldsFor(cat, cat && cat->stackable);   // [0] = アイテムID
        sp.fields.erase(sp.fields.begin());
        const size_t F = sp.fields.size();
        sp.use.assign(F, false);
        sp.val.assign(F, 0);
        sp.hasName = (cat && cat->offName >= 0);
        sp.useName = false;
        sp.nameOff = sp.hasName ? (u32)cat->offName : 0;
        sp.name.clear();
        count = 1;
        if (F == 0 && !sp.hasName)
            return true;

        const size_t N    = sp.hasName ? 1 : 0;
        const size_t iGen = F + N;
        for (;;)
        {
            std::vector<std::string> rows;
            for (size_t i = 0; i < F; ++i)
                rows.push_back(sp.fields[i].label + ": " +
                               (sp.use[i] ? FmtValue(sp.fields[i], sp.val[i]) : std::string("指定しない")));
            if (sp.hasName)
                rows.push_back(std::string("製作者名: ") +
                               (sp.useName ? "「" + Utf16ToUtf8(sp.name) + "」" : std::string("指定しない")));
            rows.push_back("[この内容で生成]");
            rows.push_back("[すべて指定しないに戻す]");

            Keyboard kb(title, rows);
            const int sel = kb.Open();
            if (sel < 0)
                return false;
            if ((size_t)sel == iGen)
                break;
            if ((size_t)sel == iGen + 1)
            {
                for (size_t i = 0; i < F; ++i) { sp.use[i] = false; sp.val[i] = 0; }
                sp.useName = false;
                sp.name.clear();
                continue;
            }
            if (sp.hasName && (size_t)sel == F)
            {
                std::vector<u16> nm = sp.name;
                if (AskMakerName(nm))
                {
                    sp.name    = nm;
                    sp.useName = !nm.empty();
                }
                continue;
            }
            u32 cur = sp.use[sel] ? sp.val[sel] : 0, v = 0;
            if (AskFieldValue(sp.fields[sel], cat, cur, v))
            {
                sp.use[sel] = true;
                sp.val[sel] = v;
            }
        }
        count = SpecValue(&sp, OFF_COUNT, 1);
        if (count == 0)
            count = 1;
        return true;
    }

    static std::string SpecSummary(const GenSpec &sp)
    {
        std::string s;
        for (size_t i = 0; i < sp.fields.size(); ++i)
            if (sp.use[i])
                s += "・" + sp.fields[i].label + ": " + FmtValue(sp.fields[i], sp.val[i]) + "\n";
        if (sp.useName)
            s += "・製作者名: " + Utf16ToUtf8(sp.name) + "\n";
        return s;
    }

    // ------------------------------------------------------------
    //  未実装アイテムの判定 (名前の表から)
    //   ・名前に「未知」が付く / 「そざい」「ぶき」などの仮の名前
    //   ・同じ名前が DUP_THRESHOLD 件以上ある (兵士の剣などは2件までなので残る)
    // ------------------------------------------------------------
    static bool IsPlaceholderName(const std::string &name)
    {
        if (name.find("未知") != std::string::npos)
            return true;
        std::string t = name;
        size_t e = t.size();
        while (e > 0 && ((t[e - 1] >= '0' && t[e - 1] <= '9') || (t[e - 1] >= 'A' && t[e - 1] <= 'Z') ||
                         (t[e - 1] >= 'a' && t[e - 1] <= 'z')))
            --e;
        t.resize(e);
        return t == "そざい" || t == "ぶき" || t == "ぼうぐ" || t == "どうぐ" || t == "かぐ";
    }

    static bool NameTableLoaded() { return g_itemNames[0].name != nullptr; }

    static bool IsUnimplemented(u32 id)
    {
        static std::vector<char> tbl;
        static bool built = false;
        if (!built)
        {
            built = true;
            tbl.assign(MAX_ID + 1, 0);
            std::vector<std::pair<std::string, u32> > v;
            for (const NameEntry *e = g_itemNames; e->name; ++e)
                if (e->id <= MAX_ID)
                    v.push_back(std::make_pair(std::string(e->name), (u32)e->id));
            std::sort(v.begin(), v.end());
            v.erase(std::unique(v.begin(), v.end()), v.end());
            for (size_t i = 0; i < v.size();)
            {
                size_t j = i;
                while (j < v.size() && v[j].first == v[i].first)
                    ++j;
                const bool dup = (j - i) >= DUP_THRESHOLD;
                for (size_t k = i; k < j; ++k)
                    if (dup || IsPlaceholderName(v[k].first))
                        tbl[v[k].second] = 1;
                i = j;
            }
        }
        return id <= MAX_ID && tbl[id] != 0;
    }

    // ------------------------------------------------------------
    //  行の作成・追加
    // ------------------------------------------------------------
    // 空き行 row に新しいアイテムを作り、リストの末尾につなぐ (件数はここでは更新しない)
    static void AppendRow(BagState &b, u32 row, u32 id, u32 count, const GenSpec *sp)
    {
        const u32 base = RowOf(b, row);
        for (u32 o = 0; o < ROW_DATA; o += 4)
            Process::Write32(base + o, 0);
        Process::Write32(base + OFF_ID, id);
        Process::Write16(base + OFF_COUNT, (u16)count);
        Process::Write16(base + 0x0C, 0xFFFE);
        Process::Write16(base + 0x0E, 0x0001);
        Process::Write32(base + 0x28, 1);
        Process::Write32(base + 0x2C, 2);
        Process::Write32(base + 0x30, 3);
        Process::Write32(base + 0x34, 4);

        if (sp)
            for (size_t i = 0; i < sp->fields.size(); ++i)
                if (sp->use[i] && sp->fields[i].off != OFF_COUNT)
                    WriteField(base, sp->fields[i], sp->val[i]);

        // 製作者名: UTF-16LE で最大6文字(12バイト)。余りは0。名前つきの行は +0xE に印が付き、
        // 末尾の 1,2,3,4 の目印は無い (実物の行に合わせる)
        if (sp && sp->useName)
        {
            for (u32 o = 0x28; o < ROW_DATA; o += 4)
                Process::Write32(base + o, 0);
            for (u32 i = 0; i < NAME_CHARS; ++i)
                Process::Write16(base + sp->nameOff + i * 2, i < sp->name.size() ? sp->name[i] : (u16)0);
            Process::Write16(base + 0x0E, b.authorMark);
        }

        const u16 tail = b.order.empty() ? NO_LINK : b.order.back();
        Process::Write16(LnkOf(b, row),     NO_LINK);
        Process::Write16(LnkOf(b, row) + 2, tail);
        if (tail != NO_LINK)
            Process::Write16(LnkOf(b, tail), (u16)row);
        else
            Process::Write16(b.c->live + 0x1E, (u16)row);

        b.order.push_back((u16)row);
        b.used[row] = true;
    }

    // 1つ追加。成功したら行番号、満杯なら -1。
    // 素材・消耗品(と範囲外のID)は、同じID(とクオリティ)の行があれば個数を足す
    static int AddOne(BagState &b, u32 id, u32 count, const GenSpec *sp)
    {
        const Category *cat = FindCategory((u16)id);
        if (!cat || cat->stackable)
        {
            const u32 wantQ = cat ? SpecValue(sp, (u32)cat->offQuality, 0) : 0;
            for (u16 s : b.order)
            {
                u32 cur = 0;
                if (!Process::Read32(RowOf(b, s) + OFF_ID, cur) || cur != id)
                    continue;
                if (cat && cat->offQuality >= 0)
                {
                    u8 q = 0;
                    Process::Read8(RowOf(b, s) + (u32)cat->offQuality, q);
                    if (q != wantQ)
                        continue;
                }
                u16 c = 0;
                Process::Read16(RowOf(b, s) + OFF_COUNT, c);
                u32 n = c + count;
                if (n > MAX_COUNT)
                    n = MAX_COUNT;
                Process::Write16(RowOf(b, s) + OFF_COUNT, (u16)n);
                return (int)s;
            }
        }
        const int f = PickFreeRow(b);
        if (f < 0)
            return -1;
        AppendRow(b, (u32)f, id, count, sp);
        return f;
    }

    // ------------------------------------------------------------
    //  アイテム生成の画面 (かばん/倉庫を選ぶ → カテゴリを選ぶ → その中から選ぶ)
    // ------------------------------------------------------------
    static bool AskTarget(const std::string &title, const Container *&c)
    {
        std::vector<std::string> v;
        v.push_back(g_bag.name);
        v.push_back(g_store.name);
        Keyboard k(title, v);
        const int s = k.Open();
        if (s < 0)
            return false;
        c = (s == 0) ? &g_bag : &g_store;
        return true;
    }

    // カテゴリを選ぶ。キャンセルなら false。cat=nullptr は「カテゴリ外 / IDを直接入力」
    static bool PickCategory(const std::string &title, bool allowFree, const Category *&cat)
    {
        std::vector<std::string> names;
        for (const Category &c : g_cats)
            names.push_back(Utils::Format("%s (ID 0x%04X-0x%04X)", c.name, c.idMin, c.idMax));
        if (allowFree)
            names.push_back("カテゴリ外 / IDを直接入力");
        Keyboard kc(title, names);
        const int ci = kc.Open();
        if (ci < 0)
            return false;
        cat = ((size_t)ci < sizeof(g_cats) / sizeof(g_cats[0])) ? &g_cats[ci] : nullptr;
        return true;
    }

    // カテゴリの中から1つ選ぶ (一覧の先頭に [ID入力] あり)。cat=nullptr のときはID入力だけ
    static bool PickOne(const Category *cat, const std::string &title, u32 &id)
    {
        if (cat)
            return PickFromNames(Utils::Format("%s [%s]", title.c_str(), cat->name), g_itemNames,
                                 true, cat->idMin, cat->idMax, cat->idMin, id);
        return AskHexId(title + " (16進)", 0, false, 0, MAX_ID, id);
    }

    // 1つ生成する流れ (追加項目 → 確認 → 生成 → 続けて編集)
    static void CreateOneFlow(const Container &ct, u32 id)
    {
        if (id > MAX_ID)
        {
            MessageBox("範囲外のID", Utils::Format("IDは 0x%04X までです。", MAX_ID))();
            return;
        }

        const Category *cat = FindCategory((u16)id);
        u32 count = 1;
        GenSpec sp;
        if (cat)
        {
            if (!AskGenSpec(cat, Utils::Format("追加項目 [%s] %s", cat->name, IdName(id).c_str()), sp, count))
                return;
        }
        else
        {
            if (!(MessageBox("確認",
                             Utils::Format("0x%04X は武器・防具・道具・家具・素材のどれでもありません。\n"
                                           "消耗品などとして追加しますか?", id),
                             DialogType::DialogYesNo))())
                return;
            if (!AskValue("個数", false, MAX_COUNT, 1, count) || count == 0)
                return;
        }

        std::string msg = Utils::Format("「%s」を%sに追加します。\n", IdName(id).c_str(), ct.name);
        msg += SpecSummary(sp);
        msg += "セーブのバックアップは取りましたか?";
        if (!(MessageBox("確認", msg, DialogType::DialogYesNo))())
            return;

        BagState b;
        std::string err;
        if (!LoadBag(b, ct, err))
        {
            MessageBox("追加できませんでした", err)();
            return;
        }
        const int slot = AddOne(b, id, count, cat ? &sp : nullptr);
        if (slot < 0)
        {
            MessageBox("追加できませんでした", "空き枠がありません。")();
            return;
        }
        CommitCount(b);

        MessageBox("完了", Utils::Format("%d枠目に追加しました。\n"
                                         "かばん・倉庫の画面は、一度閉じて開き直すと反映されます。", slot + 1))();

        if (MessageBox("続けて編集", "個数や追加効果などを編集しますか?", DialogType::DialogYesNo)())
            EditAt(RowOf(b, (u32)slot), "生成したアイテムの編集", true);
    }

    // アイテムを1つ生成: カテゴリを選ぶ → 一覧から選ぶ (ID入力も可)
    static void CreateItemEntry(MenuEntry *entry)
    {
        (void)entry;
        const Container *ct = nullptr;
        if (!AskTarget("アイテムを1つ生成: 生成先", ct))
            return;
        const Category *cat = nullptr;
        if (!PickCategory(Utils::Format("アイテムを1つ生成 (%s): カテゴリを選ぶ", ct->name), true, cat))
            return;
        u32 id = 0;
        if (!PickOne(cat, "生成するアイテム", id))
            return;
        CreateOneFlow(*ct, id);
    }

    // 範囲内の実在するID (名前の表が無いときは番号をそのまま) を小さい順に集める
    static void PoolIds(u32 from, u32 to, std::vector<u32> &out)
    {
        out.clear();
        if (NameTableLoaded())
        {
            for (const NameEntry *e = g_itemNames; e->name; ++e)
                if (e->id >= from && e->id <= to)
                    out.push_back(e->id);
            std::sort(out.begin(), out.end());
            out.erase(std::unique(out.begin(), out.end()), out.end());
        }
        else
        {
            for (u32 id = from; id <= to; ++id)
                out.push_back(id);
        }
    }

    // ids が全部同じカテゴリならそれを返す (違う/カテゴリ外が混ざるなら nullptr)
    static const Category *CommonCategory(const std::vector<u32> &ids)
    {
        const Category *c0 = nullptr;
        for (u32 id : ids)
        {
            const Category *c = FindCategory((u16)id);
            if (!c || (c0 && c != c0))
                return nullptr;
            c0 = c;
        }
        return c0;
    }

    // ------------------------------------------------------------
    //  IDを指定して1つずつ生成 (連続)
    //  カテゴリを選び、追加項目を1回決めたら、一覧から選ぶたびにその場で生成する。
    //  一覧はページ分けで、生成しても同じページに戻る。Bで終了。
    // ------------------------------------------------------------
    static void BuildPool(const Category &cat, bool hideUnimpl, std::vector<u32> &pool)
    {
        std::vector<u32> all;
        PoolIds(cat.idMin, cat.idMax, all);
        pool.clear();
        for (u32 id : all)
            if (!(hideUnimpl && IsUnimplemented(id)))
                pool.push_back(id);
    }

    static void ContinuousCreateEntry(MenuEntry *entry)
    {
        (void)entry;
        const Container *ct = nullptr;
        if (!AskTarget("1つずつ生成: 生成先", ct))
            return;
        const Category *cat = nullptr;
        if (!PickCategory(Utils::Format("1つずつ生成 (%s): カテゴリを選ぶ", ct->name), true, cat))
            return;

        GenSpec sp;
        u32 count = 1;
        if (cat)
        {
            if (!AskGenSpec(cat, Utils::Format("追加項目 [%s] (このあと選ぶ全部に適用)", cat->name), sp, count))
                return;
        }
        else if (!AskValue("個数 (このあと選ぶ全部に適用)", false, MAX_COUNT, 1, count) || count == 0)
            return;

        std::string msg = Utils::Format("[%s / %s] 一覧から選ぶたびに、追加します。(Bで終了)\n",
                                        ct->name, cat ? cat->name : "ID入力");
        msg += SpecSummary(sp);
        msg += "セーブのバックアップは取りましたか?";
        if (!(MessageBox("確認", msg, DialogType::DialogYesNo))())
            return;

        BagState b;
        std::string err;
        if (!LoadBag(b, *ct, err))
        {
            MessageBox("追加できませんでした", err)();
            return;
        }

        static constexpr u32 PAGE = 20;
        static constexpr size_t HEAD = 5;     // 一覧の先頭の操作行の数
        std::vector<u32> pool;
        bool hide = false;
        u32 page = 0, made = 0;
        if (cat)
            BuildPool(*cat, hide, pool);

        for (;;)
        {
            u32 id = 0;
            bool have = false;

            if (!cat)
            {
                if (!AskHexId(Utils::Format("生成するID (16進) 生成済み %u個", made), 0, false, 0, MAX_ID, id))
                    break;
                have = true;
            }
            else
            {
                const u32 total = (u32)pool.size();
                const u32 pages = total ? (total + PAGE - 1) / PAGE : 1;
                if (page >= pages)
                    page = pages - 1;

                std::vector<std::string> rows;
                rows.push_back("[ID入力]");
                rows.push_back(Utils::Format("← 前のページ (%u/%u)", page + 1, pages));
                rows.push_back(Utils::Format("→ 次のページ (%u/%u)", page + 1, pages));
                rows.push_back("[ページ番号を指定]");
                rows.push_back(!NameTableLoaded() ? std::string("(名前の表がないので未実装の判定なし)")
                               : hide ? std::string("[未実装を隠している → 表示する]")
                                      : std::string("[未実装を隠す]"));
                for (u32 i = page * PAGE; i < total && i < (page + 1) * PAGE; ++i)
                    rows.push_back(Utils::Format("%s (%X)", IdName(pool[i]).c_str(), pool[i]));

                Keyboard kb(Utils::Format("[%s] 生成済み %u個 / 空き枠 %u  (Bで終了)", cat->name, made, FreeRows(b)),
                            rows);
                const int sel = kb.Open();
                if (sel < 0)
                    break;

                if (sel == 0)
                {
                    have = AskHexId(Utils::Format("生成するID (0x%04X-0x%04X)", cat->idMin, cat->idMax),
                                    cat->idMin, true, cat->idMin, cat->idMax, id);
                }
                else if (sel == 1)
                    page = page ? page - 1 : pages - 1;
                else if (sel == 2)
                    page = (page + 1 < pages) ? page + 1 : 0;
                else if (sel == 3)
                {
                    u32 v = 0;
                    if (AskValue(Utils::Format("ページ番号 (1-%u)", pages), false, pages, page + 1, v) && v >= 1)
                        page = v - 1;
                }
                else if (sel == 4)
                {
                    if (NameTableLoaded())
                    {
                        hide = !hide;
                        BuildPool(*cat, hide, pool);
                        page = 0;
                    }
                }
                else
                {
                    const size_t k = (size_t)page * PAGE + (size_t)(sel - (int)HEAD);
                    if (k < pool.size())
                    {
                        id = pool[k];
                        have = true;
                    }
                }
            }

            if (!have)
                continue;

            const int slot = AddOne(b, id, count, cat ? &sp : nullptr);
            if (slot < 0)
            {
                MessageBox("かばんがいっぱい", "空き枠がありません。")();
                break;
            }
            CommitCount(b);
            ++made;
            OSD::Notify(Utils::Format("生成: %s → %d枠目", IdName(id).c_str(), slot + 1));
        }

        MessageBox("完了", Utils::Format("%u 個を生成しました。\n"
                                         "かばん・倉庫の画面は、一度閉じて開き直すと反映されます。", made))();
    }

    // ------------------------------------------------------------
    //  まとめて作る共通処理 (未実装の除外 → 追加項目 → 確認 → 生成)
    // ------------------------------------------------------------
    static void RunBulk(const Container &ct, std::vector<u32> ids, std::string what, bool askUnimpl)
    {
        // 未実装アイテムの除外 (カテゴリ・ID範囲のとき)
        if (askUnimpl && NameTableLoaded() && !ids.empty())
        {
            u32 unimpl = 0;
            for (u32 id : ids)
                if (IsUnimplemented(id))
                    ++unimpl;
            if (unimpl > 0)
            {
                if ((MessageBox("未実装アイテム",
                                Utils::Format("対象 %u 件のうち、未実装らしいものが %u 件あります。\n"
                                              "(未知・そざい・ぶき等、名前が%u件以上かぶるもの)\n"
                                              "除外しますか?", (u32)ids.size(), unimpl, (u32)DUP_THRESHOLD),
                                DialogType::DialogYesNo))())
                {
                    std::vector<u32> keep;
                    for (u32 id : ids)
                        if (!IsUnimplemented(id))
                            keep.push_back(id);
                    ids.swap(keep);
                    what += " (未実装を除外)";
                }
            }
        }

        if (ids.empty())
        {
            MessageBox("対象なし", "該当するアイテムがありません。")();
            return;
        }

        // 追加項目 (全部が同じカテゴリのときだけ指定できる)
        GenSpec sp;
        u32 count = 1;
        const Category *cc = CommonCategory(ids);
        bool useSpec = false;
        if (cc)
        {
            if (!AskGenSpec(cc, Utils::Format("追加項目 [%s] (全件に適用)", cc->name), sp, count))
                return;
            useSpec = true;
        }
        else
        {
            MessageBox("追加項目", "対象が複数のカテゴリにまたがるため、追加項目は指定できません。\n"
                                   "(カテゴリ別に分けて生成すると指定できます)")();
        }

        BagState b;
        std::string err;
        if (!LoadBag(b, ct, err))
        {
            MessageBox("追加できませんでした", err)();
            return;
        }
        const u32 freeN = FreeRows(b);
        std::string msg = Utils::Format("%s (%s)\n対象 %u 件 / %sの空き枠 %u\n", what.c_str(), ct.name,
                                        (u32)ids.size(), ct.name, freeN);
        if (useSpec)
            msg += SpecSummary(sp);
        if (ids.size() > freeN)
            msg += "空き枠が足りないため、先頭から入る分だけ作ります。\n";
        msg += "セーブのバックアップは取りましたか?\n実行しますか?";
        if (!(MessageBox("一括生成の確認", msg, DialogType::DialogYesNo))())
            return;

        u32 made = 0, skipped = 0;
        for (u32 id : ids)
        {
            if (AddOne(b, id, count, useSpec ? &sp : nullptr) < 0)
            {
                skipped = (u32)ids.size() - made;
                break;
            }
            ++made;
        }
        CommitCount(b);

        MessageBox("完了", Utils::Format("%u 件を処理しました。(入りきらず未処理: %u)\n"
                                         "かばん・倉庫の画面は、一度閉じて開き直すと反映されます。", made, skipped))();
    }

    static void BulkCreateAllEntry(MenuEntry *entry)
    {
        (void)entry;
        const Container *ct = nullptr;
        if (!AskTarget("一括生成: 生成先", ct))
            return;
        const Category *cat = nullptr;
        if (!PickCategory(Utils::Format("カテゴリの全アイテムを一括生成 (%s)", ct->name), false, cat) || !cat)
            return;
        std::vector<u32> ids;
        PoolIds(cat->idMin, cat->idMax, ids);
        RunBulk(*ct, ids, Utils::Format("%s の全アイテム", cat->name), true);
    }

    static void CreateNEntry(MenuEntry *entry)
    {
        (void)entry;
        const Container *ct = nullptr;
        if (!AskTarget("同じアイテムをN個: 生成先", ct))
            return;
        const Category *cat = nullptr;
        if (!PickCategory(Utils::Format("同じアイテムをN個 (%s): カテゴリを選ぶ", ct->name), true, cat))
            return;
        u32 id = 0, n = 1;
        if (!PickOne(cat, "生成するアイテム", id))
            return;
        if (id > MAX_ID)
        {
            MessageBox("範囲外のID", Utils::Format("IDは 0x%04X までです。", MAX_ID))();
            return;
        }
        if (!AskValue("個数(枠の数)", false, ct->slots, 1, n) || n == 0)
            return;
        std::vector<u32> ids(n, id);
        RunBulk(*ct, ids, Utils::Format("%s を %u 個", IdName(id).c_str(), n), false);
    }

    // ------------------------------------------------------------
    //  アイテム削除 (かばん / 倉庫)
    //  連結リストから外し、件数を減らし、空き枠の表を整え、行のIDを空き行の値(0x666)にする。
    //  (通常の「捨てる」と前後ダンプを比べて、リンクを FFFF/FFFF にすること、
    //   空き枠の表に外した枠を入れることを確認した。行のIDを消すのはチート独自の処理で、
    //   空き枠の表を整えたうえで消しても入手時にフリーズしないかは、実機で未確認)
    //  装備中の印: かばんの行の +8 の bit0 (装備を付け替えたときの前後ダンプで確認。
    //  武器1・防具6・ライフ道具8 が印つきだった)。装備中の行は、どの削除でも消さない。
    // ------------------------------------------------------------
    static bool IsEquipped(const BagState &b, u32 slot)
    {
        u8 f = 0;
        Process::Read8(RowOf(b, slot) + 8, f);
        return (f & 1) != 0;
    }

    // del の行を消して、残りのリンク・先頭・件数を作り直す
    static void DeleteRows(BagState &b, const std::vector<u16> &del)
    {
        std::vector<char> isDel(b.c->slots, 0);
        for (u16 s : del)
            isDel[s] = 1;
        std::vector<u16> keep;
        for (u16 s : b.order)
            if (!isDel[s])
                keep.push_back(s);

        for (u16 s : del)
        {
            u8 flag = 0;
            Process::Read8(RowOf(b, s) + 8, flag);
            if (flag & 1)
                Process::Write32(RowOf(b, s) + 8, 0);          // 装備中の印も消す
            Process::Write32(RowOf(b, s) + OFF_ID, EMPTY_ID);  // アイテムデータを消す
            Process::Write16(LnkOf(b, s),     NO_LINK);
            Process::Write16(LnkOf(b, s) + 2, NO_LINK);
            b.used[s] = false;
        }
        for (size_t i = 0; i < keep.size(); ++i)
        {
            Process::Write16(LnkOf(b, keep[i]),     i + 1 < keep.size() ? keep[i + 1] : NO_LINK);
            Process::Write16(LnkOf(b, keep[i]) + 2, i > 0 ? keep[i - 1] : NO_LINK);
        }
        Process::Write16(b.c->live + 0x1E, keep.empty() ? NO_LINK : keep[0]);
        Process::Write16(b.c->live + 0x1C, (u16)keep.size());
        b.order = keep;
        SyncFreeArray(b);
    }

    // カテゴリ単位 / 未実装だけ、まとめて削除
    static void BulkDeleteEntry(MenuEntry *entry)
    {
        (void)entry;

        const Container *c = nullptr;
        if (!AskTarget("一括削除: 対象", c))
            return;

        std::vector<std::string> names;
        for (const Category &cg : g_cats)
            names.push_back(Utils::Format("%s (ID 0x%04X-0x%04X)", cg.name, cg.idMin, cg.idMax));
        names.push_back(Utils::Format("%s内のすべて", c->name));
        Keyboard kc("一括削除するカテゴリ", names);
        const int ci = kc.Open();
        if (ci < 0)
            return;
        const bool all = ((size_t)ci == sizeof(g_cats) / sizeof(g_cats[0]));

        std::vector<std::string> modes;
        modes.push_back(all ? std::string("全てのアイテムを削除")
                            : Utils::Format("%sを全て削除", g_cats[ci].name));
        modes.push_back("未実装アイテムだけ削除");
        Keyboard kmo("削除する対象", modes);
        const int mo = kmo.Open();
        if (mo < 0)
            return;
        if (mo == 1 && !NameTableLoaded())
        {
            MessageBox("名前の表なし", "名前の表が無いので、未実装の判定ができません。")();
            return;
        }

        BagState b;
        std::string err;
        if (!LoadBag(b, *c, err))
        {
            MessageBox("削除できませんでした", err)();
            return;
        }

        std::vector<u16> del;
        u32 protectedN = 0;
        for (u16 s : b.order)
        {
            u32 id = 0;
            Process::Read32(RowOf(b, s) + OFF_ID, id);
            bool hit = all || (id >= g_cats[ci].idMin && id <= g_cats[ci].idMax);
            if (hit && mo == 1 && !IsUnimplemented(id))
                hit = false;
            if (!hit)
                continue;
            if (IsEquipped(b, s))
            {
                ++protectedN;
                continue;
            }
            del.push_back(s);
        }
        if (del.empty())
        {
            MessageBox("対象なし", "削除するアイテムがありません。")();
            return;
        }

        std::string msg = Utils::Format("%s / %s\n%s\n%u 件を削除します。(残り %u 件)\n",
                                        c->name, all ? "すべて" : g_cats[ci].name,
                                        mo == 1 ? "未実装アイテムのみ" : "すべて",
                                        (u32)del.size(), (u32)(b.order.size() - del.size()));
        if (protectedN)
            msg += Utils::Format("装備中の %u 件は残します。\n", protectedN);
        msg += "セーブのバックアップは取りましたか?\n実行しますか?";
        if (!(MessageBox("一括削除の確認", msg, DialogType::DialogYesNo))())
            return;

        DeleteRows(b, del);
        MessageBox("完了", Utils::Format("%u 件を削除しました。\n"
                                         "かばん・倉庫の画面は、一度閉じて開き直すと反映されます。", (u32)del.size()))();
    }

    // 空き枠の表の修復 (以前のチート生成・削除で崩れた分を直す)
    static void RepairFreeEntry(MenuEntry *entry)
    {
        (void)entry;
        if (!(MessageBox("空き枠情報の修復",
                         "かばん・倉庫の空き枠の表を、実際の使用状況に合わせて作り直します。\n"
                         "チートで生成・削除したあと、通常のプレイで入手してフリーズする場合に使います。\n"
                         "セーブのバックアップは取りましたか?", DialogType::DialogYesNo))())
            return;
        std::string res;
        const Container *conts[2] = { &g_bag, &g_store };
        for (const Container *ct : conts)
        {
            BagState b;
            std::string err;
            if (!LoadBag(b, *ct, err))
            {
                res += Utils::Format("%s: 修復できません (%s)\n", ct->name, err.c_str());
                continue;
            }
            SyncFreeArray(b);
            res += Utils::Format("%s: %u 件 / 空き %u 枠を整えました\n", ct->name, (u32)b.order.size(), FreeRows(b));
        }
        MessageBox("完了", res)();
    }

    // 装備中の武器・防具・ライフ道具も含めて、かばんのすべてを削除
    static void DeleteAllIncludingEquippedEntry(MenuEntry *entry)
    {
        (void)entry;

        BagState b;
        std::string err;
        if (!LoadBag(b, g_bag, err))
        {
            MessageBox("削除できませんでした", err)();
            return;
        }
        if (b.order.empty())
        {
            MessageBox("対象なし", "かばんにアイテムがありません。")();
            return;
        }

        u32 eqW = 0, eqA = 0, eqT = 0, eqOther = 0;
        for (u16 s : b.order)
        {
            if (!IsEquipped(b, s))
                continue;
            u32 id = 0;
            Process::Read32(RowOf(b, s) + OFF_ID, id);
            const Category *cg = FindCategory((u16)id);
            if (cg == &g_cats[0])      ++eqW;
            else if (cg == &g_cats[1]) ++eqA;
            else if (cg == &g_cats[2]) ++eqT;
            else                       ++eqOther;
        }

        std::string msg = Utils::Format("かばんの全 %u 件を削除します。(家具・素材も含む)\n", (u32)b.order.size());
        msg += Utils::Format("装備中の 武器 %u / 防具 %u / ライフ道具 %u", eqW, eqA, eqT);
        if (eqOther)
            msg += Utils::Format(" / その他 %u", eqOther);
        msg += " も消えます。\n装備は外れた状態になります。(実機未検証)\n"
               "セーブのバックアップは取りましたか?\n実行しますか?";
        if (!(MessageBox("装備中も含めて全て削除", msg, DialogType::DialogYesNo))())
            return;
        if (!(MessageBox("最終確認", "本当に、かばんのすべてを削除しますか?", DialogType::DialogYesNo))())
            return;

        std::vector<u16> del = b.order;
        DeleteRows(b, del);
        MessageBox("完了", Utils::Format("%u 件を削除しました。\n"
                                         "かばん画面は、一度閉じて開き直すと反映されます。\n"
                                         "装備は、装備画面で付け直してください。", (u32)del.size()))();
    }

    // 1つずつ削除 (連続)。カテゴリを選び、一覧から選ぶたびにその場で削除する。Bで終了。
    static void DeleteOneByOneEntry(MenuEntry *entry)
    {
        (void)entry;

        const Container *c = nullptr;
        if (!AskTarget("1つずつ削除: 対象", c))
            return;
        const Category *cat = nullptr;
        if (!PickCategory(Utils::Format("1つずつ削除 (%s): カテゴリを選ぶ", c->name), false, cat) || !cat)
            return;

        BagState b;
        std::string err;
        if (!LoadBag(b, *c, err))
        {
            MessageBox("削除できませんでした", err)();
            return;
        }
        if (!(MessageBox("確認",
                         Utils::Format("[%s / %s] 一覧から選ぶたびに、そのアイテムを削除します。(Bで終了)\n"
                                       "装備中のものは削除されません。\n"
                                       "セーブのバックアップは取りましたか?", c->name, cat->name),
                         DialogType::DialogYesNo))())
            return;

        static constexpr u32 PAGE = 20;
        static constexpr int HEAD = 3;
        u32 page = 0, deleted = 0;

        for (;;)
        {
            std::vector<u16> slots;
            for (u16 s : b.order)
            {
                u32 id = 0;
                Process::Read32(RowOf(b, s) + OFF_ID, id);
                if (id >= cat->idMin && id <= cat->idMax)
                    slots.push_back(s);
            }
            const u32 total = (u32)slots.size();
            if (total == 0)
            {
                MessageBox("なし", Utils::Format("%s の「%s」は、もうありません。", c->name, cat->name))();
                break;
            }
            const u32 pages = (total + PAGE - 1) / PAGE;
            if (page >= pages)
                page = pages - 1;

            std::vector<std::string> rows;
            rows.push_back(Utils::Format("← 前のページ (%u/%u)", page + 1, pages));
            rows.push_back(Utils::Format("→ 次のページ (%u/%u)", page + 1, pages));
            rows.push_back("[ページ番号を指定]");
            for (u32 i = page * PAGE; i < total && i < (page + 1) * PAGE; ++i)
            {
                u32 id = 0;
                u16 cnt = 0;
                Process::Read32(RowOf(b, slots[i]) + OFF_ID, id);
                Process::Read16(RowOf(b, slots[i]) + OFF_COUNT, cnt);
                std::string label = Utils::Format("%s", IdName(id).c_str());
                if (cat->stackable)
                    label += Utils::Format(" x%u", (u32)cnt);
                label += Utils::Format(" (%u枠)", slots[i] + 1);
                if (IsEquipped(b, slots[i]))
                    label += " [装備中]";
                rows.push_back(label);
            }

            Keyboard kb(Utils::Format("[%s/%s] 削除済み %u個 / 残り %u個 (Bで終了)", c->name, cat->name, deleted, total),
                        rows);
            const int sel = kb.Open();
            if (sel < 0)
                break;

            if (sel == 0)
                page = page ? page - 1 : pages - 1;
            else if (sel == 1)
                page = (page + 1 < pages) ? page + 1 : 0;
            else if (sel == 2)
            {
                u32 v = 0;
                if (AskValue(Utils::Format("ページ番号 (1-%u)", pages), false, pages, page + 1, v) && v >= 1)
                    page = v - 1;
            }
            else
            {
                const size_t k = (size_t)page * PAGE + (size_t)(sel - HEAD);
                if (k >= slots.size())
                    continue;
                const u16 s = slots[k];
                if (IsEquipped(b, s))
                {
                    OSD::Notify("装備中のアイテムは削除できません");
                    continue;
                }
                u32 id = 0;
                Process::Read32(RowOf(b, s) + OFF_ID, id);
                std::vector<u16> one(1, s);
                DeleteRows(b, one);
                ++deleted;
                OSD::Notify(Utils::Format("削除: %s (%u枠)", IdName(id).c_str(), s + 1));
            }
        }

        MessageBox("完了", Utils::Format("%u 個を削除しました。\n"
                                         "かばん・倉庫の画面は、一度閉じて開き直すと反映されます。", deleted))();
    }


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
    static void RequestSavedPos(const SavedPos &p)
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

        if (g_warpTask != 0 && obj && cur == p.area)
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
        // true にすると、スロット選択画面(オレンジ色の背景)を開かずに進めます
        // (必ず先にGDBで動作確認してから true にしてください)
        static constexpr bool HIDE_SELECT_UI = false;

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
        static constexpr u32 ARM_NOP        = 0xE1A00000;
        static constexpr u32 MAX_FRAMES     = 60 * 90;

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
        static bool g_mgrInit = false, g_vtblPatched = false, g_uiAttached = false, g_uiHidden = false;
        static int  g_lastSt = -1;

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
                g_uiHidden = Process::Write32(UI_OPEN_ADDR, ARM_NOP);
            g_vtblPatched = Process::Write32(VTBL_EXSEL_UPDATE, (u32)&ExSelUpdateWrapper);
            g_sendResult = ((SendFn)FN_SEND)(ROOT_TASK, DESC_EXSEL);
            g_sent = true;
        }

        // menu->Callback から毎フレーム呼ばれる (切り替え中以外は即 return)
        static void OnFrame(void)
        {
            if (g_phase == IDLE)
                return;

            // 重くならないよう、切り替え中も3フレームに1回だけ調べる
            if (g_phase != D_BLANK && (++g_tick % 3) != 0)
                return;

            u32 cur = 0;
            Process::Read32(FLOW_OBJ + 48, cur);

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
                OSD::Notify("Fieldでのみ使えます");
                return;
            }
            if (g_phase != IDLE)
            {
                OSD::Notify("処理中です");
                return;
            }
            Keyboard kb("プレイヤーを選択", { "プレイヤー 1", "プレイヤー 2", "プレイヤー 3" });
            int r = kb.Open();
            if (r < 0)
                return;

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

        menu->Append(new MenuEntry("プレイヤー切り替え", nullptr, PlayerSwitch::Entry,
                                   "フィールドで実行します。タイトルを経由せず、選んだプレイヤーのデータに切り替えます。\n"
                                   "使う前に、セーブのバックアップを取ってください。"));
        menu->Callback(PlayerSwitch::OnFrame);
        
        AddMaterialZero(menu);

        AddLifeChange(menu);

        menu->Append(new MenuEntry("新しく装備したアイテムを編集", EquipEditEntry, nullptr,
                                   "有効にして、編集したい装備を着けます(着けている場合は外して着け直し)。\n"
                                   "「Press Y to edit」と表示されてから約10秒の間だけYで編集できます。\n"
                                   "編集後、同じ装備をもう一度つけ直すと反映されます(そのつけ直しでは編集メニューは出ません)。\n"
                                   "武器・防具・ライフ道具8種すべてに対応しています。"));
        menu->Append(new MenuEntry("カテゴリ別 一括変更", nullptr, BulkEditEntry,
                                   "武器・防具・ライフ道具・家具ごとに、かばん内の該当アイテムをまとめて変更します。\n"
                                   "変えたい項目を選んでから「決定して適用」で書き込みます。アイテムIDは、範囲を指定して被りなしの連番にするか、1件ずつ選べます。"));

        {
            MenuFolder *gen = new MenuFolder("アイテム生成",
                                             "かばん・倉庫の空き枠に新しいアイテムを作ります。必ずセーブのバックアップを取ってから使ってください。");
            gen->Append(new MenuEntry("アイテムを1つ生成", nullptr, CreateItemEntry,
                                      "カテゴリを選び、一覧から選んで1つ作ります。(ID入力も可)\n"
                                      "武器・防具・ライフ道具・家具は、クオリティ・追加効果・追加数値・製作者名を指定して作れます。"));
            gen->Append(new MenuEntry("カテゴリの全アイテムを一括生成", nullptr, BulkCreateAllEntry,
                                      "選んだカテゴリの全アイテムを1つずつ、まとめて作ります。\n"
                                      "未実装らしいアイテムを除外するか選べます。"));
            gen->Append(new MenuEntry("IDを指定して1つずつ生成", nullptr, ContinuousCreateEntry,
                                      "カテゴリを選び、追加項目を決めたら、一覧から選ぶたびにその場で作ります。\n"
                                      "一覧は同じページに戻るので、続けて選べます。Bで終了します。"));
            gen->Append(new MenuEntry("空き枠情報を修復 (フリーズ対策)", nullptr, RepairFreeEntry,
                                      "チートで生成・削除したあと、通常プレイでの入手時にフリーズする場合に使います。\n"
                                      "かばん・倉庫の空き枠の表を、実際の使用状況に合わせて作り直します。"));
            gen->Append(new MenuEntry("同じアイテムをN個(別枠)", nullptr, CreateNEntry,
                                      "同じアイテムを、別々の枠にN個作ります。(素材・消耗品は1枠にまとまります)"));
            menu->Append(gen);
        }
        {
            MenuFolder *del = new MenuFolder("アイテム削除",
                                             "かばん・倉庫のアイテムを削除します。「装備中の…を含む全てを削除」以外は、装備中のものを残します。必ずセーブのバックアップを取ってから使ってください。");
            del->Append(new MenuEntry("カテゴリ・未実装をまとめて削除", nullptr, BulkDeleteEntry,
                                      "かばんまたは倉庫で、カテゴリ単位 / 未実装アイテムだけ、まとめて削除します。"));
            del->Append(new MenuEntry("装備中の武器・防具・ライフ道具を含む全てを削除", nullptr, DeleteAllIncludingEquippedEntry,
                                      "かばんの中のすべてを、装備中の武器・防具・ライフ道具も含めて削除します。(家具・素材も含む)\n"
                                      "装備は外れた状態になります。必ずバックアップを取ってから使ってください。"));
            del->Append(new MenuEntry("1つずつ削除", nullptr, DeleteOneByOneEntry,
                                      "かばんまたは倉庫で、カテゴリを選び、一覧から選ぶたびにその場で削除します。Bで終了します。"));
            menu->Append(del);
        }
        menu->Append(new MenuEntry("エリアワープ", nullptr, WarpEntry,
                                   "移動先を選ぶと、通常のロード画面つきで、そのエリアへ移動します。\n"
                                   "出る場所は、選んだ行き先へ向かう扉の前です(扉がある側のエリア)。\n"
                                   "一覧の末尾の[保存]は、「現在位置を保存」で保存した位置です。\n使う前に必ずバックアップを取ってください。"));
        menu->Append(new MenuEntry("現在位置を保存 (ワープ一覧に追加)", nullptr, SavePosEntry,
                                   "いま立っている位置に名前をつけて保存します。保存した位置は「エリアワープ」の一覧の末尾に並びます。\n"
                                   "別のエリアの位置を選ぶと、そのエリアへ入ってから、保存した位置へ移動します。\n"
                                   "保存先は SD直下の fll_warp_points.txt です。"));
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

        AddQualityFix(menu);
        AddRecommended(menu);
        
        menu->Append(new MenuEntry("プレイヤー切り替え 直接版 (実験)", nullptr, PlayerSwitch::EntryDirect,
                                   "タイトルを経由せず、フィールドを片付けてから読み込みだけを行って戻ります。\n"
                                   "使う前に必ずセーブのバックアップを取ってください。切り替え後、確認できるまでセーブしないでください。\n"
                                   "固まった場合は、セーブせずにゲームを再起動してください。"));
        menu->Append(new MenuEntry("プレイヤー切り替え タイトル経由 (実験)", nullptr, PlayerSwitch::EntryTitle,
                                   "タイトル画面を一瞬経由して、自動でスロットを選びます。"));
    
        menu->Callback(PosNotifyFrame);
        
        menu->Run();
        delete menu;
        return 0;
    }
}
