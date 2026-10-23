// ファンタジーライフ(日本語版) アイテムエディタ  (CTRPluginFramework / .3gx)
//
// 海外版PLGのItemEditorと同じ仕組み:
//   1) ゲームの装備処理2か所にフックを入れ、装備したアイテムの構造体アドレスを取得
//   2) Yボタンでエディタを開く
//   3) 構造体を書き換え、「外して装備し直す」と反映される
//
// 日本語版のフック位置は、海外版コードとの命令列照合で求めたもの(静的解析のみ・実機未確認)。
// 位置の命令列が違う場合は、フックを入れずに警告を出す。
//
// 構造体のオフセットは、ユーザー提供のARコード(かばん 0x6DDF2C から 0x3C 刻み)と
// PLGのエディタが使う位置(+0x1E, +0x20 など)に一致している。

#include <3ds.h>
#include "csvc.h"
#include <CTRPluginFramework.hpp>
#include <string>
#include <vector>

// ------------------------------------------------------------
//  フック本体 (アセンブラ)  ※PLGのコールバックと同じ動作
//  呼び出し側で「found-4」に str pc,[sp,#-4]! を書いてあるので、
//  スタックには (found+4) が積まれている。置き換えた3命令をここで再実行して、
//  found+8 に戻る。
// ------------------------------------------------------------
extern "C"
{
    void jp_hookA(void);
    void jp_hookB(void);

    volatile u32 g_capPtr   = 0;
    volatile u32 g_capCount = 0;

    void jp_captureA(u32 r3) { g_capPtr = r3 + 4; g_capCount = g_capCount + 1; }
    void jp_captureB(u32 r1) { g_capPtr = r1 + 4; g_capCount = g_capCount + 1; }
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

    .global jp_hookB
    .type jp_hookB, %function
jp_hookB:
    ldr   r2, [sp], #4
    push  {r4-r8, lr}
    push  {r0-r4}
    mov   r0, r1
    bl    jp_captureB
    pop   {r0-r4}
    movs  r4, r1
    mov   r5, r0
    add   r2, r2, #4
    mov   pc, r2
)");

namespace CTRPluginFramework
{
    // ------------------------------------------------------------
    //  日本語版のアドレス
    // ------------------------------------------------------------
    static constexpr u32 HOOK_A = 0x002D33F8;   // 海外版 0x2E091C
    static constexpr u32 HOOK_B = 0x002BA928;   // 海外版 0x2C846C

    static constexpr u32 BAG_BASE  = 0x006DDF2C; // かばん1枠目のアイテムID
    static constexpr u32 BAG_SLOTS = 500;
    static constexpr u32 SLOT_SIZE = 0x3C;
    static constexpr u32 OFF_ID    = 0x00;       // u16
    static constexpr u32 OFF_COUNT = 0x04;       // u8 (要確認)
    static constexpr u32 MAX_COUNT = 99;
    static constexpr u32 MAX_ID    = 0x05C4;

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
        { "武器",       0x0001, 0x008F, 0x10, 0x12, 0x1A, 0x1E, "攻撃力", "魔法攻撃力" },
        { "防具",       0x0099, 0x0386, 0x12, 0x14, 0x1A, 0x1E, "防御力", "魔法防御力" },
        { "ライフ道具", 0x0387, 0x0435, 0x1E, 0x20, 0x26,   -1, "道具力", nullptr },
        { "家具",       0x0436, 0x05C4, 0x20,   -1,   -1,   -1, nullptr,  nullptr },
    };

    struct Field
    {
        std::string label;
        u32  off;
        u32  size;   // 1 or 2 (byte)
        bool hex;
        u32  maxV;
    };

    static u32 SlotAddr(u32 slot) { return BAG_BASE + slot * SLOT_SIZE; }

    static const Category *FindCategory(u16 id)
    {
        for (const Category &c : g_cats)
            if (id >= c.idMin && id <= c.idMax)
                return &c;
        return nullptr;
    }

    static std::vector<Field> FieldsFor(const Category *c)
    {
        std::vector<Field> v;
        v.push_back({ "アイテムID", OFF_ID,    2, true,  MAX_ID });
        v.push_back({ "個数",       OFF_COUNT, 1, false, MAX_COUNT });
        if (!c)
            return v;
        if (c->offQuality >= 0)
            v.push_back({ "クオリティ", (u32)c->offQuality, 1, false, 255 });
        if (c->offEffect >= 0)
            v.push_back({ "追加効果", (u32)c->offEffect, 2, true, 0xFFFF });
        if (c->offAtk >= 0)
            v.push_back({ std::string("追加") + c->atkName, (u32)c->offAtk, 2, false, 65535 });
        if (c->offMatk >= 0)
            v.push_back({ std::string("追加") + c->matkName, (u32)c->offMatk, 2, false, 65535 });
        return v;
    }

    static bool ReadField(u32 base, const Field &f, u32 &out)
    {
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
        if (f.size == 1)
            return Process::Write8(base + f.off, (u8)val);
        return Process::Write16(base + f.off, (u16)val);
    }

    static std::string FmtValue(const Field &f, u32 v)
    {
        return f.hex ? Utils::Format("0x%04X", v) : Utils::Format("%u", v);
    }

    static bool AskValue(const std::string &msg, bool hex, u32 maxV, u32 cur, u32 &out)
    {
        Keyboard kb(msg);
        kb.IsHexadecimal(hex);
        kb.SetMaxLength(hex ? 4 : 5);
        u32 v = cur;
        if (kb.Open(v, cur) != 0)
            return false;
        if (v > maxV)
            v = maxV;
        out = v;
        return true;
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
            std::vector<Field> fields = FieldsFor(cat);

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

                if (sel == 0
                    && !(MessageBox("注意",
                                    "別カテゴリのIDに変えると、他の項目が不正な値になる可能性があります。続けますか?",
                                    DialogType::DialogYesNo))())
                    continue;

                u32 cur = 0, val = 0;
                ReadField(base, f, cur);
                if (AskValue(f.label + (f.hex ? " (16進)" : ""), f.hex, f.maxV, cur, val))
                {
                    if (!WriteField(base, f, val))
                        OSD::Notify("書き込みに失敗しました");
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
    static Hook g_hookA;
    static Hook g_hookB;
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
        static const u32 expectB[3] = { 0xE92D41F0, 0xE1B04001, 0xE1A05000 }; // push {r4-r8,lr} / movs r4,r1 / mov r5,r0
        if (!CheckWords(HOOK_A - 4, expectA, 3) || !CheckWords(HOOK_B - 4, expectB, 3))
        {
            MessageBox("エラー",
                       "フック位置の命令が想定と違います。\nゲームのバージョンが異なる可能性があるため、機能を有効にできません。")();
            return false;
        }

        // PLGと同じ手順: found-4 に str pc,[sp,#-4]! を書き、found に ldr pc 形式のフックを入れる
        Process::Write32(HOOK_A - 4, 0xE52DF004);
        Process::Write32(HOOK_B - 4, 0xE52DF004);

        g_hookA.Initialize(HOOK_A, (u32)(uintptr_t)jp_hookA);
        g_hookB.Initialize(HOOK_B, (u32)(uintptr_t)jp_hookB);
        g_hookA.Enable();
        g_hookB.Enable();

        g_hooksInstalled = true;
        return true;
    }

    // 有効な間、毎フレーム呼ばれる
    static void EquipEditEntry(MenuEntry *entry)
    {
        static u32 lastCount = 0;
        static u32 pending   = 0;

        if (!InstallHooks())
        {
            entry->Disable();
            return;
        }

        const u32 c = g_capCount;
        if (c != lastCount)
        {
            lastCount = c;
            pending = g_capPtr;
            OSD::Notify("Yボタンで、いま装備したアイテムを編集できます");
        }

        if (pending && Controller::IsKeyPressed(Key::Y))
        {
            const u32 target = pending;
            pending = 0;
            EditAt(target, "装備アイテムの編集", false);
            OSD::Notify("外して装備し直すと反映されます");
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

        std::vector<Field> fields = FieldsFor(&cat);
        std::vector<std::string> fn;
        for (size_t i = 1; i < fields.size(); ++i)
            fn.push_back(fields[i].label);

        Keyboard kf(Utils::Format("[%s] 変更する項目", cat.name), fn);
        int fi = kf.Open();
        if (fi < 0)
            return;
        const Field &f = fields[fi + 1];

        u32 val = 0;
        if (!AskValue(f.label + (f.hex ? " (16進)" : ""), f.hex, f.maxV, 0, val))
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
        OSD::Notify(Utils::Format("%u 件を変更しました", n));
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
            OSD::Notify("警告: 想定外のタイトルです");

        PluginMenu *menu = new PluginMenu("ファンタジーライフ アイテムエディタ", 0, 2, 0,
                                          "日本語版用。海外版PLGのItemEditorと同じ方式です。");
        menu->SynchronizeWithFrame(true);

        menu->Append(new MenuEntry("新しく装備したアイテムを編集", EquipEditEntry, nullptr,
                                   "有効にして、編集したい装備を着けます(着けている場合は外して着け直し)。\n"
                                   "「Yボタンで編集」と表示されたらYを押し、編集後にもう一度外して着け直すと反映されます。"));
        menu->Append(new MenuEntry("アイテムエディタ(かばんの枠指定)", nullptr, ItemEditorEntry,
                                   "かばんの枠番号を指定して、ID・個数・クオリティ・追加効果などを編集します。"));
        menu->Append(new MenuEntry("カテゴリ別 一括変更", nullptr, BulkEditEntry,
                                   "武器・防具・ライフ道具・家具ごとに、かばん内の全アイテムの項目をまとめて変更します。"));

        menu->Run();
        delete menu;
        return 0;
    }
}
