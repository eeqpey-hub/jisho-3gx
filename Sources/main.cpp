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
    volatile u32 g_warpTask    = 0;   // 毎フレーム更新される。エリアID = g_warpTask + 0x6B2

    // ゲーム本体のスレッドから、毎フレーム呼ばれる (task = フィールドタスク)。
    // 予約があれば 0x21EDCC(接続番号) を呼び、エリアIDを返す(0 = 何もしない)。
    // 接続番号が0のときは 0x21EDCC を呼ばない (GDBで確認した「標準の入口」に出る動作)。
    u32 WarpPrepare(u32 task)
    {
        g_warpTask = task;
        if (!g_warpPending)
            return 0;
        g_warpPending = 0;
        if (g_warpLink != 0)
        {
            typedef void (*SetArrivalLinkFn)(u32);
            ((SetArrivalLinkFn)0x21EDCC)(g_warpLink);
        }
        return g_warpArea;
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
    bl    WarpPrepare            @ r0 = エリアID (0 = ワープなし)
    vpop  {d0-d7}                @ s0〜s15 を元に戻す
    str   r0, [sp, #16]          @ 保存した r12 の位置に結果を入れる
    pop   {r0-r3, r12, lr}       @ r12 = エリアID, r0 = タスク, lr = 元のlr
    cmp   r12, #0
    beq   1f
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
    };

    // 素材・消耗品のクオリティの位置。【未検証の仮置き】武器と同じ +0x10 と推測している。
    // 違っていたら、かばんの素材をHexダンプで見て、ここだけ直す。
    static constexpr int MAT_QUALITY_OFF = 0x10;

    static const Category g_cats[] =
    {
        { "武器",       0x0000, 0x008F, 0x10, 0x12, 0x1A, 0x1E, "攻撃力", "魔法攻撃力", false },
        { "防具",       0x0099, 0x0386, 0x12, 0x14, 0x1A, 0x1E, "防御力", "魔法防御力", false },
        { "ライフ道具", 0x0387, 0x0435, 0x1E, 0x20, 0x26,   -1, "道具力", nullptr, false },
        { "家具",       0x0436, 0x05C4, 0x20,   -1,   -1,   -1, nullptr,  nullptr, false },
        { "素材・消耗品", 0x05C5, 0x09FE, MAT_QUALITY_OFF, -1, -1, -1, nullptr, nullptr, true },
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

    // かばんの中で、このカテゴリのアイテムがある枠と、その現在のIDを集める
    static void CollectBagSlots(const Category &cat, std::vector<u32> &slots, std::vector<u32> &ids)
    {
        for (u32 slot = 0; slot < BAG_SLOTS; ++slot)
        {
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

        std::vector<Field> fields = FieldsFor(&cat, false);   // [0]=ID、[1]以降=ほかの項目 (個数は出さない)
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
    //  アイテム生成 / 一括生成 / 一括削除 (かばんの連結リストを直接操作)
    //
    //  ゲーム本人が1個買ったときのRAM差分(Before/After)から分かったこと:
    //   ・行の本体は 0x38 バイト。行の直前4バイト(= 前の行の +0x38)が (次u16,前u16) のリンク。
    //   ・かばんの「生きている」先頭と件数は、保存用の 0x6DDF24 ではなく
    //       0x6DCD2C を先頭とする管理領域にある: +0x1C=件数(u16)、+0x1E=先頭の行番号(u16, 空=FFFF)
    //   ・新しい行は 499 番から使われ、行は「ID・個数=1・+0xC=FFFE・+0xE=1・+0x28から 1,2,3,4」
    //     以外は 0 で初期化される。(防具で確認。他の種類は未検証)
    // ------------------------------------------------------------
    static constexpr u32 LIVE_BASE  = 0x006DCD2C;          // かばん管理領域
    static constexpr u32 LIVE_LINKP = LIVE_BASE + 0x08;    // リンク配列(=BAG_BASE-4)へのポインタ
    static constexpr u32 LIVE_COUNT = LIVE_BASE + 0x1C;    // 件数 (u16)
    static constexpr u32 LIVE_HEAD  = LIVE_BASE + 0x1E;    // 先頭の行番号 (u16)
    static constexpr u32 ROW_DATA   = 0x38;
    static constexpr u16 NO_LINK    = 0xFFFF;
    static constexpr u32 EMPTY_ID   = 0x0666;              // 空き行に入っていたID (削除時にこれを書く・推測)
    static constexpr size_t DUP_THRESHOLD = 3;             // 名前がこの件数以上かぶると「未実装」とみなす

    static u32 LinkAddr(u32 slot) { return BAG_BASE - 4 + slot * SLOT_SIZE; }

    struct BagState
    {
        std::vector<u16>  order;   // 先頭から末尾までの行番号
        std::vector<bool> used;
    };

    // 管理領域と連結リストを読む。想定と違えば false (何も書かない)
    static bool LoadBag(BagState &b, std::string &err)
    {
        u32 p = 0;
        if (!Process::Read32(LIVE_LINKP, p) || p != BAG_BASE - 4)
        {
            err = "かばんの管理領域が想定と違います。(バージョン違いの可能性。何も書いていません)";
            return false;
        }
        u16 head = 0, cnt = 0;
        if (!Process::Read16(LIVE_HEAD, head) || !Process::Read16(LIVE_COUNT, cnt))
        {
            err = "管理領域を読めません。";
            return false;
        }
        b.order.clear();
        b.used.assign(BAG_SLOTS, false);
        u16 idx = head;
        while (idx != NO_LINK)
        {
            if (idx >= BAG_SLOTS || b.used[idx] || b.order.size() >= BAG_SLOTS)
            {
                err = "かばんの連結リストが想定と違います。(何も書いていません)";
                return false;
            }
            b.used[idx] = true;
            b.order.push_back(idx);
            u16 next = 0;
            if (!Process::Read16(LinkAddr(idx), next))
            {
                err = "リンクを読めません。";
                return false;
            }
            idx = next;
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
        for (int k = (int)BAG_SLOTS - 1; k >= 0; --k)
            if (!b.used[k])
                return k;
        return -1;
    }

    static u32 FreeRows(const BagState &b)
    {
        u32 n = 0;
        for (u32 k = 0; k < BAG_SLOTS; ++k)
            if (!b.used[k])
                ++n;
        return n;
    }

    static void CommitCount(const BagState &b)
    {
        Process::Write16(LIVE_COUNT, (u16)b.order.size());
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
    };

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
        count = 1;
        if (F == 0)
            return true;

        for (;;)
        {
            std::vector<std::string> rows;
            for (size_t i = 0; i < F; ++i)
                rows.push_back(sp.fields[i].label + ": " +
                               (sp.use[i] ? FmtValue(sp.fields[i], sp.val[i]) : std::string("指定しない")));
            rows.push_back("[この内容で生成]");
            rows.push_back("[すべて指定しないに戻す]");

            Keyboard kb(title, rows);
            const int sel = kb.Open();
            if (sel < 0)
                return false;
            if ((size_t)sel == F)
                break;
            if ((size_t)sel == F + 1)
            {
                for (size_t i = 0; i < F; ++i) { sp.use[i] = false; sp.val[i] = 0; }
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
        const u32 base = SlotAddr(row);
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

        const u16 tail = b.order.empty() ? NO_LINK : b.order.back();
        Process::Write16(LinkAddr(row),     NO_LINK);
        Process::Write16(LinkAddr(row) + 2, tail);
        if (tail != NO_LINK)
            Process::Write16(LinkAddr(tail), (u16)row);
        else
            Process::Write16(LIVE_HEAD, (u16)row);

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
                if (!Process::Read32(SlotAddr(s) + OFF_ID, cur) || cur != id)
                    continue;
                if (cat && cat->offQuality >= 0)
                {
                    u8 q = 0;
                    Process::Read8(SlotAddr(s) + (u32)cat->offQuality, q);
                    if (q != wantQ)
                        continue;
                }
                u16 c = 0;
                Process::Read16(SlotAddr(s) + OFF_COUNT, c);
                u32 n = c + count;
                if (n > MAX_COUNT)
                    n = MAX_COUNT;
                Process::Write16(SlotAddr(s) + OFF_COUNT, (u16)n);
                return (int)s;
            }
        }
        const int f = PickFreeRow(b);
        if (f < 0)
            return -1;
        AppendRow(b, (u32)f, id, count, sp);
        return f;
    }

    static void CreateItemEntry(MenuEntry *entry)
    {
        (void)entry;

        u32 id = 0;
        if (!PickFromNames("生成するアイテムID", g_itemNames, false, 0, MAX_ID, 0, id))
            return;
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

        std::string msg = Utils::Format("「%s」をかばんに追加します。\n", IdName(id).c_str());
        msg += SpecSummary(sp);
        msg += "セーブのバックアップは取りましたか?";
        if (!(MessageBox("確認", msg, DialogType::DialogYesNo))())
            return;

        BagState b;
        std::string err;
        if (!LoadBag(b, err))
        {
            MessageBox("追加できませんでした", err)();
            return;
        }
        const int slot = AddOne(b, id, count, cat ? &sp : nullptr);
        if (slot < 0)
        {
            MessageBox("追加できませんでした", "かばんに空き枠がありません。")();
            return;
        }
        CommitCount(b);

        MessageBox("完了", Utils::Format("%d枠目に追加しました。\n"
                                         "かばん画面は、一度閉じて開き直すと反映されます。", slot + 1))();

        if (MessageBox("続けて編集", "クオリティや追加効果などを編集しますか?", DialogType::DialogYesNo)())
            EditAt(SlotAddr((u32)slot), "生成したアイテムの編集", true);
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

    static void BulkCreateEntry(MenuEntry *entry)
    {
        (void)entry;

        std::vector<std::string> modes;
        modes.push_back("カテゴリの全アイテムを1つずつ");
        modes.push_back("ID範囲を指定して1つずつ");
        modes.push_back("同じアイテムをN個 (別枠)");
        Keyboard km("一括生成", modes);
        const int mode = km.Open();
        if (mode < 0)
            return;

        std::vector<u32> ids;
        std::string what;

        if (mode == 0)
        {
            std::vector<std::string> names;
            for (const Category &c : g_cats)
                names.push_back(Utils::Format("%s (ID 0x%04X-0x%04X)", c.name, c.idMin, c.idMax));
            Keyboard kc("生成するカテゴリ", names);
            const int ci = kc.Open();
            if (ci < 0)
                return;
            PoolIds(g_cats[ci].idMin, g_cats[ci].idMax, ids);
            what = Utils::Format("%s の全アイテム", g_cats[ci].name);
        }
        else if (mode == 1)
        {
            u32 from = 0, to = MAX_ID;
            if (!PickFromNames("開始ID", g_itemNames, false, 0, MAX_ID, 0, from))
                return;
            if (!PickFromNames("終了ID", g_itemNames, false, 0, MAX_ID, MAX_ID, to))
                return;
            if (from > to)
                std::swap(from, to);
            if (to > MAX_ID)
                to = MAX_ID;
            PoolIds(from, to, ids);
            what = Utils::Format("ID 0x%04X-0x%04X", from, to);
        }
        else
        {
            u32 id = 0, n = 1;
            if (!PickFromNames("生成するアイテムID", g_itemNames, false, 0, MAX_ID, 0, id))
                return;
            if (id > MAX_ID)
            {
                MessageBox("範囲外のID", Utils::Format("IDは 0x%04X までです。", MAX_ID))();
                return;
            }
            if (!AskValue("個数(枠の数)", false, BAG_SLOTS, 1, n) || n == 0)
                return;
            ids.assign(n, id);
            what = Utils::Format("%s を %u 個", IdName(id).c_str(), n);
        }

        // 未実装アイテムの除外 (カテゴリ・ID範囲のとき)
        if (mode != 2 && NameTableLoaded() && !ids.empty())
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
        if (!LoadBag(b, err))
        {
            MessageBox("追加できませんでした", err)();
            return;
        }
        const u32 freeN = FreeRows(b);
        std::string msg = Utils::Format("%s\n対象 %u 件 / かばんの空き枠 %u\n", what.c_str(), (u32)ids.size(), freeN);
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
                                         "かばん画面は、一度閉じて開き直すと反映されます。", made, skipped))();
    }

    // ------------------------------------------------------------
    //  アイテム一括削除
    //  連結リストから外し、件数を減らし、行のIDを空き行の値(0x666)にする。
    //  ※ゲーム本人の削除処理は未確認。かばんから消すだけで、装備中のものは対象にしないこと。
    // ------------------------------------------------------------
    static void BulkDeleteEntry(MenuEntry *entry)
    {
        (void)entry;

        std::vector<std::string> names;
        for (const Category &c : g_cats)
            names.push_back(Utils::Format("%s (ID 0x%04X-0x%04X)", c.name, c.idMin, c.idMax));
        names.push_back("かばん内のすべて");
        Keyboard kc("一括削除するカテゴリ", names);
        const int ci = kc.Open();
        if (ci < 0)
            return;
        const bool all = ((size_t)ci == sizeof(g_cats) / sizeof(g_cats[0]));

        std::vector<std::string> modes;
        modes.push_back("そのカテゴリのアイテムをすべて削除");
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
        if (!LoadBag(b, err))
        {
            MessageBox("削除できませんでした", err)();
            return;
        }

        std::vector<u16> keep, del;
        for (u16 s : b.order)
        {
            u32 id = 0;
            Process::Read32(SlotAddr(s) + OFF_ID, id);
            bool hit = all || (id >= g_cats[ci].idMin && id <= g_cats[ci].idMax);
            if (hit && mo == 1 && !IsUnimplemented(id))
                hit = false;
            (hit ? del : keep).push_back(s);
        }
        if (del.empty())
        {
            MessageBox("対象なし", "削除するアイテムがありません。")();
            return;
        }

        std::string msg = Utils::Format("%s\n%s\nかばんから %u 件を削除します。(残り %u 件)\n"
                                        "装備中のアイテムは、先に外してから実行してください。\n"
                                        "セーブのバックアップは取りましたか?\n実行しますか?",
                                        all ? "かばん内のすべて" : g_cats[ci].name,
                                        mo == 1 ? "未実装アイテムのみ" : "すべて",
                                        (u32)del.size(), (u32)keep.size());
        if (!(MessageBox("一括削除の確認", msg, DialogType::DialogYesNo))())
            return;

        for (u16 s : del)
        {
            Process::Write32(SlotAddr(s) + OFF_ID, EMPTY_ID);
            Process::Write16(LinkAddr(s),     NO_LINK);
            Process::Write16(LinkAddr(s) + 2, NO_LINK);
        }
        for (size_t i = 0; i < keep.size(); ++i)
        {
            Process::Write16(LinkAddr(keep[i]),     i + 1 < keep.size() ? keep[i + 1] : NO_LINK);
            Process::Write16(LinkAddr(keep[i]) + 2, i > 0 ? keep[i - 1] : NO_LINK);
        }
        Process::Write16(LIVE_HEAD,  keep.empty() ? NO_LINK : keep[0]);
        Process::Write16(LIVE_COUNT, (u16)keep.size());

        MessageBox("完了", Utils::Format("%u 件を削除しました。\n"
                                         "かばん画面は、一度閉じて開き直すと反映されます。", (u32)del.size()))();
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
        { "→クルブルク大通り へ向かう扉の前 (2/7)", 2, 7 },
        { "★郊外南: パーテル大平原東へ向かう扉の前 (25/39)", 25, 39 },
        { "→パーテル大平原東 ハニワ洞窟前 へ向かう扉の前 (25/128)", 25, 128 },
        { "→ハニワの洞窟 へ向かう扉の前 (210/128)", 210, 128 },
        { "★パーテル大平原東: 郊外南へ向かう扉の前 (3/39)", 3, 39 },
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

    // メニューから選んだときに1回だけ実行される (有効/無効の切り替えではない)
    static void WarpEntry(MenuEntry *entry)
    {
        (void)entry;

        if (!InstallWarpHook())
            return;

        std::vector<std::string> names;
        for (int i = 0; i < WARP_COUNT; ++i)
            names.push_back(g_warpDests[i].label);

        Keyboard kb("移動先を選択 (その行き先へ向かう扉の前に出ます)", names);
        int sel = kb.Open();
        if (sel < 0 || sel >= WARP_COUNT)
            return;

        g_warpArea    = g_warpDests[sel].area;
        g_warpLink    = g_warpDests[sel].link;
        g_warpPending = 1;     // メニューを閉じた後、ゲームが次にタスクを更新したときに実行される
        OSD::Notify("Warp requested.");
    }

    // ------------------------------------------------------------
    //  探索ワープ / 場所の記録 (開発用・実機未検証)
    //  GDBなしで、移動先のデータ (エリアID・入口番号・場所名) を集めるための機能。
    //   ・探索ワープ: エリアIDを +1 / -1 / 直接入力 で移動し、着いた場所の名前を記録する
    //   ・扉の自動記録: 有効にして扉を通ると、行き先エリアIDと入口番号を記録する
    //  記録は SD カード直下の fll_warp_notes.txt に追記される (書けない場合は画面の一覧のみ)。
    //  存在しないエリアIDへ移動すると、落ちる可能性がある。必ずバックアップを取ること。
    // ------------------------------------------------------------
    #ifndef WARP_LOG_TO_FILE
    #define WARP_LOG_TO_FILE 1      // ビルドが通らないときは 0 にするとファイル出力を外せる
    #endif

    static std::vector<std::string> g_notes;
    static u32 g_expArea = 2;
    static u32 g_expLink = 0;

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

    static void RequestWarp(u32 area, u32 link)
    {
        if (area < 1)   area = 1;
        if (area > 999) area = 999;
        g_expArea = area;
        g_expLink = link;
        g_warpArea    = area;
        g_warpLink    = link;
        g_warpPending = 1;
        OSD::Notify(Utils::Format("Warp: area %u link %u", area, link));
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

    static void RecordHere(void)
    {
        std::string name;
        Keyboard kb("この場所の名前を入力 (記録します)");
        if (kb.Open(name) != 0)
            return;
        AppendNote(Utils::Format("area=%u link=%u name=%s", CurrentAreaId(), g_expLink, name.c_str()));
        OSD::Notify("Recorded.");
    }

    static void ExploreEntry(MenuEntry *entry)
    {
        (void)entry;

        if (!InstallWarpHook())
            return;

        const u32 cur = CurrentAreaId();

        std::vector<std::string> opts;
        opts.push_back(Utils::Format("次のIDへ移動 (%u)", g_expArea + 1));
        opts.push_back(Utils::Format("前のIDへ移動 (%u)", g_expArea > 1 ? g_expArea - 1 : 1));
        opts.push_back(Utils::Format("+10 へ移動 (%u)", g_expArea + 10));
        opts.push_back(Utils::Format("-10 へ移動 (%u)", g_expArea > 10 ? g_expArea - 10 : 1));
        opts.push_back("エリアIDと入口番号を数字で指定して移動");
        opts.push_back(Utils::Format("もう一度同じIDへ (%u)", g_expArea));
        opts.push_back("この場所を記録 (名前を入力)");
        opts.push_back("記録を表示");

        u32 sel = 0;
        if (!AskChoice(Utils::Format("探索ワープ  いまのエリアID=%u / 前回の移動=ID %u 入口 %u", cur, g_expArea, g_expLink),
                       opts, sel))
            return;

        switch (sel)
        {
        case 0: RequestWarp(g_expArea + 1, g_expLink); break;
        case 1: RequestWarp(g_expArea > 1 ? g_expArea - 1 : 1, g_expLink); break;
        case 2: RequestWarp(g_expArea + 10, g_expLink); break;
        case 3: RequestWarp(g_expArea > 10 ? g_expArea - 10 : 1, g_expLink); break;
        case 4:
        {
            u32 area = g_expArea, link = g_expLink;
            if (!AskValue("エリアID (1〜999)", false, 999, g_expArea, area))
                return;
            if (!AskValue("入口番号 (0 = 標準の入口)", false, 999, g_expLink, link))
                return;
            RequestWarp(area, link);
            break;
        }
        case 5: RequestWarp(g_expArea, g_expLink); break;
        case 6: RecordHere(); break;
        case 7: ShowNotes(); break;
        default: break;
        }
    }

    // 有効な間、毎フレーム呼ばれる。扉を通ってオブジェクトの中身が変わるたびに記録する。
    // (同じ扉を続けて通ると、中身が変わらないので記録されない)
    static void DoorLogEntry(MenuEntry *entry)
    {
        (void)entry;
        static u32 lastWord = 0xFFFFFFFF;

        u32 w = 0;
        if (!Process::Read32(0x16D5D4A8, w))   // ChangeMapオブジェクトの +12 (エリアID16bit) と +14 (入口番号16bit)
            return;
        if (w == lastWord)
            return;
        lastWord = w;

        const u32 area = w & 0xFFFF;
        const u32 link = w >> 16;
        if (area == 0)
            return;

        // here = いま居るエリア (扉を通る前)。この扉の前に出るワープの組は (dest, link)。
        //   → ワープすると「here のエリアの、この扉の前」に出る。
        const u32 here = CurrentAreaId();
        AppendNote(Utils::Format("DOOR here=%u dest=%u link=%u  (warp %u/%u lands in area %u)", here, area, link, area, link, here));
        OSD::Notify(Utils::Format("Door: here %u dest %u link %u", here, area, link));
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

        PluginMenu *menu = new PluginMenu("ファンタジーライフ アイテムエディタ", 0, 2, 0,
                                          "日本語版用。海外版PLGのItemEditorと同じ方式です。");
        menu->SynchronizeWithFrame(true);

        AddQualityFix(menu);
        AddRecommended(menu);

        menu->Append(new MenuEntry("新しく装備したアイテムを編集", EquipEditEntry, nullptr,
                                   "有効にして、編集したい装備を着けます(着けている場合は外して着け直し)。\n"
                                   "「Press Y to edit」と表示されてから約10秒の間だけYで編集できます。\n"
                                   "編集後、同じ装備をもう一度つけ直すと反映されます(そのつけ直しでは編集メニューは出ません)。\n"
                                   "武器・防具・ライフ道具8種すべてに対応しています。"));
        menu->Append(new MenuEntry("カテゴリ別 一括変更", nullptr, BulkEditEntry,
                                   "武器・防具・ライフ道具・家具ごとに、かばん内の該当アイテムをまとめて変更します。\n"
                                   "変えたい項目を選んでから「決定して適用」で書き込みます。アイテムIDは、範囲を指定して被りなしの連番にするか、1件ずつ選べます。"));

        menu->Append(new MenuEntry("アイテムを生成 (かばんに追加)", nullptr, CreateItemEntry,
                                   "IDを選ぶと、かばんの空き枠に新しいアイテムを作ります。\n"
                                   "作成後、クオリティ・追加効果なども続けて編集できます。\n"
                                   "必ずセーブのバックアップを取ってから使ってください。"));
        menu->Append(new MenuEntry("アイテム一括削除", nullptr, BulkDeleteEntry,
                                   "かばんのアイテムを、カテゴリ単位 / 未実装アイテムだけ、まとめて削除します。\n"
                                   "装備中のものは先に外してください。必ずセーブのバックアップを取ってから使ってください。"));
        menu->Append(new MenuEntry("アイテム一括生成", nullptr, BulkCreateEntry,
                                   "カテゴリの全アイテム / ID範囲 / 同じアイテムN個 を、かばんの空き枠にまとめて作ります。\n"
                                   "必ずセーブのバックアップを取ってから使ってください。"));
        menu->Append(new MenuEntry("エリアワープ (実験)", nullptr, WarpEntry,
                                   "移動先を選ぶと、通常のロード画面つきで、そのエリアへ移動します。\n"
                                   "出る場所は、選んだ行き先へ向かう扉の前です(扉がある側のエリア)。\n使う前に必ずバックアップを取ってください。★のついた2つだけ動作確認済みです。"));
        menu->Append(new MenuEntry("探索ワープ (開発用)", nullptr, ExploreEntry,
                                   "エリアIDを +1/-1/数字入力で移動し、着いた場所に名前をつけて記録します。\n"
                                   "記録は SD直下の fll_warp_notes.txt にも追記されます。\n"
                                   "存在しないIDだと落ちる可能性があります。必ずバックアップを取ってください。"));
        menu->Append(new MenuEntry("扉の自動記録 (開発用)", DoorLogEntry, nullptr,
                                   "有効にして扉を通ると、行き先のエリアIDと入口番号を記録します。\n"
                                   "記録は SD直下の fll_warp_notes.txt に追記されます。"));
        
        AddLifeChange(menu);
        menu->Run();
        delete menu;
        return 0;
    }
}
