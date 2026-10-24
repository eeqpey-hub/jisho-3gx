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
)");

namespace CTRPluginFramework
{
    // ------------------------------------------------------------
    //  日本語版のアドレス
    // ------------------------------------------------------------
    static constexpr u32 HOOK_A = 0x002D33F8;   // 海外版 0x2E091C
    static constexpr u32 HOOK_B2 = 0x002BA9AC;  // ライフ道具(実験) 関数の途中

    static constexpr u32 BAG_BASE  = 0x006DDF2C; // かばん1枠目のアイテムID
    static constexpr u32 BAG_SLOTS = 500;
    static constexpr u32 SLOT_SIZE = 0x3C;
    static constexpr u32 OFF_ID    = 0x00;       // u16
    static constexpr u32 OFF_COUNT = 0x04;       // u8 (要確認)
    static constexpr u32 MAX_COUNT = 99;
    static constexpr u32 MAX_ID    = 0x05C4;
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
    };

    static const Category g_cats[] =
    {
        { "武器",       0x0000, 0x008F, 0x10, 0x12, 0x1A, 0x1E, "攻撃力", "魔法攻撃力" },
        { "防具",       0x0099, 0x0386, 0x12, 0x14, 0x1A, 0x1E, "防御力", "魔法防御力" },
        { "ライフ道具", 0x0387, 0x0435, 0x1E, 0x20, 0x26,   -1, "道具力", nullptr },
        { "家具",       0x0436, 0x05C4, 0x20,   -1,   -1,   -1, nullptr,  nullptr },
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
            v.push_back({ "個数",   OFF_COUNT, 1, false, MAX_COUNT });
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
            std::string t = Utils::Format("0x%04X", v);
            const char *n = LookupName(f.kind == K_ITEMID ? g_itemNames : g_effectNames, v);
            if (n)
                t += std::string(" ") + n;
            return t;
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
                           Utils::Format("入力できるIDは 0x%04X 〜 0x%04X です。", lo, hi))();
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
            opts.push_back(Utils::Format("%04X ", (u32)e->id) + e->name);
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
    //  かばんの枠を指定して編集
    // ------------------------------------------------------------
    static void ItemEditorEntry(MenuEntry *entry)
    {
        (void)entry;
        u32 last = 1;
        for (;;)
        {
            Keyboard kb(Utils::Format("かばんの枠番号を入力 (1〜%u)", BAG_SLOTS));
            kb.IsHexadecimal(false);
            kb.SetMaxLength(3);
            u32 n = last;
            if (kb.Open(n, last) != 0)
                return;
            if (n < 1) n = 1;
            if (n > BAG_SLOTS) n = BAG_SLOTS;
            last = n;
            if (!EditAt(SlotAddr(n - 1), Utils::Format("かばん %u枠目", n), true))
                return;
        }
    }

    // ------------------------------------------------------------
    //  カテゴリ別 一括変更 (ARコードと同じ動作)
    // ------------------------------------------------------------
    static void BulkEditEntry(MenuEntry *entry)
    {
        (void)entry;

        std::vector<std::string> names;
        for (const Category &c : g_cats)
            names.push_back(Utils::Format("%s (ID 0x%04X〜0x%04X)", c.name, c.idMin, c.idMax));

        Keyboard kc("一括変更するカテゴリを選択", names);
        int ci = kc.Open();
        if (ci < 0)
            return;
        const Category &cat = g_cats[ci];

        std::vector<Field> fields = FieldsFor(&cat, true);   // [0]=ID [1]=個数 を前提
        std::vector<std::string> fn;
        for (size_t i = 1; i < fields.size(); ++i)
            fn.push_back(fields[i].label);

        Keyboard kf(Utils::Format("[%s] 変更する項目", cat.name), fn);
        int fi = kf.Open();
        if (fi < 0)
            return;
        const Field &f = fields[fi + 1];

        u32 val = 0;
        if (!AskFieldValue(f, &cat, 0, val))
            return;

        std::string msg = Utils::Format("かばん内の「%s」すべての%sを %s にします。よろしいですか?",
                                        cat.name, f.label.c_str(), FmtValue(f, val).c_str());
        if (!(MessageBox("確認", msg, DialogType::DialogYesNo))())
            return;

        u32 n = 0;
        for (u32 slot = 0; slot < BAG_SLOTS; ++slot)
        {
            const u32 base = SlotAddr(slot);
            u16 id = 0;
            if (!Process::Read16(base + OFF_ID, id))
                continue;
            if (id < cat.idMin || id > cat.idMax)
                continue;
            if (WriteField(base, f, val))
                ++n;
        }
        MessageBox("完了"
