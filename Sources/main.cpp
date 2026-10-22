// ファンタジーライフ(日本語版) アイテムエディタ  (CTRPluginFramework / .3gx)
//
// 構造はユーザー提供のARコードから起こしたもの:
//   かばん 500枠 x 0x3C 、1枠目のアイテムID = 0x006DDF2C
//   カテゴリはIDの範囲で判定し、カテゴリごとに編集できる項目とオフセットが違う
//
// ※ 未コンパイル・未実機テスト。個数のオフセット(+4)は「たぶん」なので　
//    「Hexダンプを表示」で確認してから使うこと。

#include <3ds.h>
#include "csvc.h"
#include <CTRPluginFramework.hpp>
#include <string>
#include <vector>

namespace CTRPluginFramework
{
    // ------------------------------------------------------------
    //  アドレス・構造 (日本語版)
    // ------------------------------------------------------------
    static constexpr u32 BAG_BASE  = 0x006DDF2C; // 1枠目のアイテムID
    static constexpr u32 BAG_SLOTS = 500;
    static constexpr u32 SLOT_SIZE = 0x3C;
    static constexpr u32 OFF_ID    = 0x00;       // u16
    static constexpr u32 OFF_COUNT = 0x04;       // u8 (要確認)
    static constexpr u32 MAX_COUNT = 99;
    static constexpr u32 MAX_ID    = 0x09DF;

    struct Category
    {
        const char *name;
        u16  idMin, idMax;   // 両端を含む
        int  offQuality;     // u8   / -1 = なし
        int  offEffect;      // u16
        int  offAtk;         // u16
        int  offMatk;        // u16
        const char *atkName;
        const char *matkName;
    };

    static const Category g_cats[] =
    {
　　　　 { "アイテム", 0x05C5, 0x09DF, -1, -1, -1, -1, nullptr, nullptr },
        { "武器",       0x0000, 0x008F, 0x10, 0x12, 0x1A, 0x1E, "攻撃力", "魔法攻撃力" },
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

    static void ShowDump(u32 slot)
    {
        const u32 base = SlotAddr(slot);
        std::string s;
        for (u32 i = 0; i < SLOT_SIZE; ++i)
        {
            u8 b = 0;
            Process::Read8(base + i, b);
            s += Utils::Format("%02X ", b);
            if (i % 12 == 11)
                s += "\n";
        }
        MessageBox(Utils::Format("%u枠目 (0x%08X)", slot + 1, base), s)();
    }

    // ------------------------------------------------------------
    //  1枠ずつ編集
    //  戻り値: true = 別の枠を選ぶ / false = 終了
    // ------------------------------------------------------------
    static bool EditSlot(u32 slot)
    {
        for (;;)
        {
            const u32 base = SlotAddr(slot);
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
            opts.push_back("別の枠を選ぶ");

            Keyboard kb(Utils::Format("かばん %u枠目 [%s]", slot + 1, cat ? cat->name : "空き/未対応"), opts);
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
                ShowDump(slot);
            else
                return true;
        }
    }

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
            if (!EditSlot(n - 1))
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
            names.push_back(c.name);

        Keyboard kc("一括変更するカテゴリを選択", names);
        int ci = kc.Open();
        if (ci < 0)
            return;
        const Category &cat = g_cats[ci];

        std::vector<Field> fields = FieldsFor(&cat);
        std::vector<std::string> fn;
        for (size_t i = 1; i < fields.size(); ++i) // IDは一括対象から除く
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
        // 日本語版のベースTID (更新データ 0004000E000E9C00 から推定)
        if (Process::GetTitleID() != 0x00040000000E9C00ULL)
            OSD::Notify("警告: 想定外のタイトルです");

        PluginMenu *menu = new PluginMenu("ファンタジーライフ アイテムエディタ", 0, 1, 0,
                                          "日本語版用。かばん(500枠)のアイテムを編集します。");
        menu->SynchronizeWithFrame(true);

        menu->Append(new MenuEntry("アイテムエディタ(かばん)", nullptr, ItemEditorEntry,
                                   "かばんの枠番号を指定して、ID・個数・クオリティ・追加効果などを編集します。"));
        menu->Append(new MenuEntry("カテゴリ別 一括変更", nullptr, BulkEditEntry,
                                   "武器・防具・ライフ道具・家具ごとに、かばん内の全アイテムの項目をまとめて変更します。"));

        menu->Run();
        delete menu;
        return 0;
    }
}
