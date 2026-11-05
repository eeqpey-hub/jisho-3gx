// ItemEditor.hpp  --  アイテムエディタ (編集・生成・削除・一括変更)
//
// main.cpp から  #include "ItemEditor.hpp"  で取り込み、main() で AddItemEditor(menu); を1回呼ぶ。
// アイテム関連の変更は、このファイルだけで済む。
//
// 前提 (main.cpp 側で、この include より前に定義されていること):
//   ・<3ds.h> <CTRPluginFramework.hpp> <algorithm> <string> <vector> の include
//   ・struct NameEntry と、名前の表 g_itemNames / g_effectNames (ItemNames.hpp)
//   ・装備フック用の extern "C" 変数・関数: g_capPtr, g_capCount, jp_hookA, jp_hookB2
//   ・同じフォルダに JpInput.hpp があること
// このファイルが定義するもので、main.cpp の他の機能も使うもの:
//   AskChoice, AskValue, PickFromNames, LookupName, IdName, CheckWords
//
// 含まれる機能:
//   ・新しく装備したアイテムを編集 (Yボタン)        EquipEditEntry
//   ・カテゴリ別 一括変更                            BulkEditEntry
//   ・アイテム生成 (1つ / カテゴリ一括 / 連続 / N個 / 空き枠修復)
//   ・アイテム削除 (まとめて / 1つずつ)
//   ・製作者名、未実装アイテム・性別専用防具の一覧
//   ・固有効果変更 (武器・防具・ライフ道具)
//   ・日本語入力 (JpInput.hpp): 製作者名と、名前の検索 (ひらがな・カタカナ・漢字) に使う
#pragma once

#include "JpInput.hpp"      // 日本語入力 (ローマ字→かな・カタカナ、Googleで漢字変換)

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

    // 数値の入力。最初は空欄 (0や1が入っていない)。そのまま決定すると 0 になる
    static bool AskValueBlank(const std::string &msg, bool hex, u32 maxV, u32 &out)
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
        u32 v = 0;
        if (kb.Open(v) != 0)
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

    // ---- 名前で検索 (日本語入力: ローマ字→ひらがな・カタカナ・漢字) ----
    static bool AskSearchNeedles(std::vector<std::string> &needles)
    {
        std::string q;
        if (!Jp::AskJapanese("名前で検索", q) || q.empty())
            return false;
        needles = Jp::KanaVariants(q);     // ひらがな⇔カタカナの両方で探す
        return !needles.empty();
    }

    static bool NameHasAny(const std::string &name, const std::vector<std::string> &needles)
    {
        if (needles.empty())
            return true;
        for (const std::string &n : needles)
            if (name.find(n) != std::string::npos)
                return true;
        return false;
    }

    static std::string SearchRowLabel(const std::vector<std::string> &needles)
    {
        return needles.empty() ? std::string("[名前で検索]") : "[検索中「" + needles[0] + "」→ 解除]";
    }

    // 検索の行を押したときの動作: 検索中なら解除、そうでなければ入力。変わったら true
    static bool ToggleSearch(std::vector<std::string> &needles)
    {
        if (!needles.empty())
        {
            needles.clear();
            return true;
        }
        return AskSearchNeedles(needles);
    }

    // 「[ID入力]」「[名前で検索]」+ 名前の一覧。一覧が空のときは、ID入力だけになる。
    static bool PickFromNames(const std::string &title, const NameEntry *table,
                              bool restrict, u32 lo, u32 hi, u32 cur, u32 &out)
    {
        const std::string hexTitle = title + " (16進)";

        bool any = false;
        for (const NameEntry *e = table; e->name; ++e)
            if (!(restrict && (e->id < lo || e->id > hi)))
            {
                any = true;
                break;
            }
        if (!any)
            return AskHexId(hexTitle, cur, restrict, lo, hi, out);

        std::vector<std::string> needles;
        for (;;)
        {
            std::vector<std::string> opts;
            std::vector<u32> ids;
            opts.push_back(Utils::Format("[ID入力] 現在 0x%04X", cur));
            ids.push_back(0);
            opts.push_back(SearchRowLabel(needles));
            ids.push_back(0);
            for (const NameEntry *e = table; e->name; ++e)
            {
                if (restrict && (e->id < lo || e->id > hi))
                    continue;
                if (!NameHasAny(e->name, needles))
                    continue;
                opts.push_back(e->name);
                ids.push_back(e->id);
            }

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
            if (sel == 1)
            {
                ToggleSearch(needles);
                continue;
            }
            out = ids[sel];
            return true;
        }
    }

    // 項目ごとの入力方法をまとめたもの (装備編集・かばん編集・一括変更で共通)
    static bool AskFieldValue(const Field &f, const Category *cat, u32 cur, u32 &out, bool blank = false)
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

        if (blank)
            return AskValueBlank(f.label + (f.hex ? " (16進)" : ""), f.hex, f.maxV, out);
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

    // 製作者名の読み書き (定義は後ろ。編集画面から使う)
    static bool AskMakerName(std::vector<u16> &name);
    static std::string Utf16ToUtf8(const std::vector<u16> &v);
    static void ReadMakerName(u32 base, const Category &cat, std::vector<u16> &out);
    static int  WriteMakerName(u32 base, const Category &cat, const std::vector<u16> &name, bool force, std::string &err);

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

            const bool hasName = (cat && cat->offName >= 0);
            std::vector<std::string> opts;
            for (const Field &f : fields)
            {
                u32 v = 0;
                ReadField(base, f, v);
                opts.push_back(f.label + ": " + FmtValue(f, v));
            }
            if (hasName)
            {
                std::vector<u16> nm;
                ReadMakerName(base, *cat, nm);
                opts.push_back(std::string("製作者名(実験): ") + (nm.empty() ? std::string("なし") : "「" + Utf16ToUtf8(nm) + "」"));
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
            else if (hasName && (size_t)sel == fields.size())
            {
                std::vector<u16> nm;
                ReadMakerName(base, *cat, nm);
                if (AskMakerName(nm))
                {
                    std::string err;
                    int r = WriteMakerName(base, *cat, nm, false, err);
                    if (r == 1)
                    {
                        // 名前の場所に、ゲームが入れた別のデータがある → 確認して上書き
                        if ((MessageBox("製作者名",
                                        "名前の場所に、ゲームが入れた別のデータ(入手情報らしいもの)が入っています。\n"
                                        "自分で作った名前つきアイテムと同じ形にするため、上書きして名前を付けます。\n"
                                        "元に戻すにはバックアップを使います。上書きしますか?",
                                        DialogType::DialogYesNo))())
                            r = WriteMakerName(base, *cat, nm, true, err);
                        else
                            r = 0;
                    }
                    if (r == 2)
                        MessageBox("製作者名を書けません", err)();
                }
            }
            else if ((size_t)sel == fields.size() + (hasName ? 1 : 0))
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
    static constexpr u32 PENDING_FRAMES = 300;    // 約5秒
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
            rows.push_back("[日本語で追加 (ローマ字→かな・漢字)]");
            rows.push_back("[かな・記号を1文字ずつ選んで追加]");
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
            if (sel == 6)
            {
                name = cur;
                return true;
            }
            if (sel == 0)
            {
                // 日本語入力 (ローマ字 → ひらがな・カタカナ・漢字)。残りの文字数までを追加する
                if (cur.size() >= NAME_CHARS)
                {
                    MessageBox("文字数", "6文字までです。")();
                    continue;
                }
                std::string t;
                if (Jp::AskJapanese("製作者名", t))
                {
                    const std::vector<u16> v = Utf8ToUtf16(t);
                    bool over = false;
                    for (u16 ch : v)
                    {
                        if (cur.size() >= NAME_CHARS)
                        {
                            over = true;
                            break;
                        }
                        cur.push_back(ch);
                    }
                    if (over)
                        MessageBox("文字数", "6文字までです。入りきらなかった分は入れていません。")();
                }
            }
            else if (sel == 1)
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
            else if (sel == 2)
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
            else if (sel == 3)
            {
                std::vector<u16> v;
                if (CopyNameFromBag(v))
                    cur = v;
            }
            else if (sel == 4)
            {
                if (!cur.empty())
                    cur.pop_back();
            }
            else if (sel == 5)
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

    // ------------------------------------------------------------
    //  既存のアイテムの製作者名 (実験)
    //  名前は行の決まった場所 (武器+0x20 / 防具+0x24 / ライフ道具・家具+0x10) に UTF-16LE で12バイト。
    //  名前つきの行は +0xE が名前の印 (名前なしは 1)。
    // ------------------------------------------------------------
    static bool IsTemplateWord(u32 at, u32 v)
    {
        return (at == 0x28 && v == 1) || (at == 0x2C && v == 2) || (at == 0x30 && v == 3) || (at == 0x34 && v == 4);
    }

    static void ReadMakerName(u32 base, const Category &cat, std::vector<u16> &out)
    {
        out.clear();
        if (cat.offName < 0)
            return;
        u16 flag = 0;
        Process::Read16(base + 0x0E, flag);
        if (flag == 1 || flag == 0)          // 名前なし
            return;
        for (u32 i = 0; i < NAME_CHARS; ++i)
        {
            u16 ch = 0;
            Process::Read16(base + (u32)cat.offName + i * 2, ch);
            if (ch == 0)
                break;
            out.push_back(ch);
        }
    }

    // 名前の印の値: かばん・倉庫にある名前つきの実物から取る (なければ 0x16D7)
    static u16 CurrentAuthorMark()
    {
        u16 mark = 0x16D7;
        const Container *conts[2] = { &g_bag, &g_store };
        for (const Container *ct : conts)
        {
            BagState b;
            std::string err;
            if (!LoadBag(b, *ct, err))
                continue;
            if (b.authorMark != 0x16D7)
                return b.authorMark;
            mark = b.authorMark;
        }
        return mark;
    }

    // 名前を書く (空なら名前を消す)。
    //  戻り値: 0=書いた / 1=名前の場所に別のデータがある(force=true なら上書きして書く) / 2=書けない
    //  自分で作った名前つきの行に合わせて、名前の直後(終端)は0にする。
    //  武器・防具は、名前より後ろ(+0x37まで)も0にする。(ゲームが入れた入手情報らしいデータが
    //  名前の場所と、その後ろにあり、名前が6文字ちょうどのとき後ろが読まれないようにするため)
    static int WriteMakerName(u32 base, const Category &cat, const std::vector<u16> &nm, bool force, std::string &err)
    {
        if (cat.offName < 0)
        {
            err = "この種類のアイテムには製作者名がありません。";
            return 2;
        }
        u16 flag = 0;
        Process::Read16(base + 0x0E, flag);
        const bool named = (flag != 1 && flag != 0);
        const u32 off = (u32)cat.offName;
        const u32 end = off + NAME_CHARS * 2;

        if (!named && !force && !nm.empty())
        {
            for (u32 o = 0; o < NAME_CHARS * 2; o += 4)
            {
                u32 v = 0;
                Process::Read32(base + off + o, v);
                if (v != 0 && !IsTemplateWord(off + o, v))
                    return 1;
            }
        }

        if (nm.empty())
        {
            if (!named)
                return 0;                       // もともと名前なし: 何もしない
            for (u32 o = 0; o < NAME_CHARS * 2; o += 2)
                Process::Write16(base + off + o, 0);
            Process::Write16(base + 0x0E, 0x0001);
            return 0;
        }

        // 買った直後などの目印 (1,2,3,4) が残っていれば消す
        u32 w0 = 0, w1 = 0, w2 = 0, w3 = 0;
        Process::Read32(base + 0x28, w0);
        Process::Read32(base + 0x2C, w1);
        Process::Read32(base + 0x30, w2);
        Process::Read32(base + 0x34, w3);
        if (w0 == 1 && w1 == 2 && w2 == 3 && w3 == 4)
            for (u32 o = 0x28; o < 0x38; o += 4)
                Process::Write32(base + o, 0);

        for (u32 i = 0; i < NAME_CHARS; ++i)
            Process::Write16(base + off + i * 2, i < nm.size() ? nm[i] : (u16)0);

        if (off >= 0x20)
        {
            for (u32 o = end; o < 0x38; o += 2)          // 武器・防具: 名前の後ろをすべて0に
                Process::Write16(base + o, 0);
        }
        else
            Process::Write16(base + end, 0);               // ライフ道具・家具: 終端の0

        Process::Write16(base + 0x0E, CurrentAuthorMark());
        return 0;
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
            if (AskFieldValue(sp.fields[sel], cat, cur, v, true))
            {
                if (v == 0 && sp.fields[sel].off == OFF_COUNT)
                    sp.use[sel] = false;          // 個数が空欄のまま決定 = 指定しない (1個)
                else
                {
                    sp.use[sel] = true;
                    sp.val[sel] = v;
                }
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

    // ------------------------------------------------------------
    //  性別専用の防具 (男せんよう / 女せんよう)。ID順に並べてある。
    // ------------------------------------------------------------
    static const u16 MALE_ONLY_ARMOR[] =
        {
            0x009A, 0x009C, 0x009E, 0x00A0, 0x00A2, 0x00A4, 0x00A6, 0x00A9, 0x00B3, 0x00C1, 0x00C7, 0x00E9, 0x00EB,
            0x00ED, 0x00EF, 0x00F1, 0x00F3, 0x00F5, 0x00FD, 0x0101, 0x0107, 0x010D, 0x0113, 0x0115, 0x0131, 0x0133,
            0x0135, 0x0137, 0x0139, 0x013B, 0x013D, 0x0145, 0x0149, 0x014F, 0x0155, 0x015B, 0x015D, 0x0180, 0x0182,
            0x0184, 0x0186, 0x0188, 0x01DD, 0x01E9, 0x0258, 0x0259, 0x025A, 0x027E, 0x027F, 0x0280, 0x0286, 0x0287,
            0x0288, 0x029A, 0x029B, 0x029C, 0x02E3, 0x0301
        };

    static const u16 FEMALE_ONLY_ARMOR[] =
        {
            0x009B, 0x009D, 0x009F, 0x00A1, 0x00A3, 0x00A5, 0x00A7, 0x00AA, 0x00B4, 0x00C2, 0x00EA, 0x00EC, 0x00EE,
            0x00F0, 0x00F2, 0x00F4, 0x00F6, 0x00FE, 0x0108, 0x010E, 0x0114, 0x0116, 0x0132, 0x0134, 0x0136, 0x0138,
            0x013A, 0x013C, 0x013E, 0x0146, 0x0150, 0x0156, 0x015C, 0x015E, 0x0178, 0x0179, 0x017A, 0x017B, 0x017C,
            0x017D, 0x017E, 0x017F, 0x0181, 0x0183, 0x0185, 0x0187, 0x0189, 0x018F, 0x01DE, 0x01EA, 0x01F0, 0x0253,
            0x0254, 0x025C, 0x025D, 0x025E, 0x0266, 0x0267, 0x0268, 0x0270, 0x0271, 0x0272, 0x027A, 0x027B, 0x028A,
            0x028B, 0x028C, 0x0296, 0x029E, 0x029F, 0x02A0, 0x02A8, 0x02A9, 0x02AA, 0x02E4, 0x0302
        };

    static bool IsMaleOnlyArmor(u32 id)
    {
        return std::binary_search(MALE_ONLY_ARMOR, MALE_ONLY_ARMOR + sizeof(MALE_ONLY_ARMOR) / sizeof(u16), (u16)id);
    }

    static bool IsFemaleOnlyArmor(u32 id)
    {
        return std::binary_search(FEMALE_ONLY_ARMOR, FEMALE_ONLY_ARMOR + sizeof(FEMALE_ONLY_ARMOR) / sizeof(u16), (u16)id);
    }

    // 名前だけでは判定できない、未実装・イベント用アイテムのID (手作業の一覧)
    static const u16 EXTRA_UNIMPL[] =
    {
        0x637, 0x64B, 0x72D, 0x845, 0x865, 0x983, 0x984, 0x8D0, 0x9C7, 0x9C9, 0x9CA, 0x9CB, 0x910, 0x91F, 0x936,
        0x5B0, 0x597, 0x596, 0x594, 0x534, 0x50E, 0x50D, 0x50C, 0x50B, 0x50A, 0x509, 0x508, 0x507, 0x4FF,
        0x3DE, 0x3D3, 0x3C9, 0x3BE, 0x3B2, 0x3A7, 0x39C, 0x391,
        0x2B5, 0x2B6, 0x2BA, 0x2C0, 0x2C6, 0x2D0, 0x2D6, 0x2DE, 0x2DF, 0x2E2, 0x2FE, 0x2FF, 0x300, 0x30C, 0x311,
        0x315, 0x31C, 0x321, 0x323, 0x325, 0x329, 0x32B, 0x32E, 0x331, 0x334, 0x335, 0x367, 0x34A, 0x350, 0x344,
        0x340, 0x233, 0x215, 0x130, 0x06C, 0x039, 0x02C, 0x01B, 0x010, 0x00F,
        0x2CE, 0x31A, 0x339, 0x358, 0x37C
    };

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
            for (u16 x : EXTRA_UNIMPL)
                if (x <= MAX_ID)
                    tbl[x] = 1;
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

    static void PoolIds(u32 from, u32 to, std::vector<u32> &out);
    static void BuildPool(const Category &cat, bool hideUnimpl, std::vector<u32> &pool,
                          const std::vector<std::string> *needles = nullptr);

    static bool g_hideUnimpl = false;   // 「未実装を隠す」の状態 (生成の一覧どうしで共有)

    // カテゴリの中から1つ選ぶ (一覧の先頭に [ID入力] [未実装を隠す] [名前で検索] あり)。
    // cat=nullptr のときはID入力だけ
    static bool PickOne(const Category *cat, const std::string &title, u32 &id)
    {
        if (!cat)
            return AskHexId(title + " (16進)", 0, false, 0, MAX_ID, id);

        const std::string full = Utils::Format("%s [%s]", title.c_str(), cat->name);
        std::vector<u32> pool;
        std::vector<std::string> needles;
        for (;;)
        {
            BuildPool(*cat, g_hideUnimpl, pool, &needles);
            std::vector<std::string> opts;
            opts.push_back(Utils::Format("[ID入力] 現在 0x%04X", cat->idMin));
            opts.push_back(!NameTableLoaded() ? std::string("(名前の表がないので未実装の判定なし)")
                           : g_hideUnimpl ? std::string("[未実装を隠している → 表示する]")
                                          : std::string("[未実装を隠す]"));
            opts.push_back(SearchRowLabel(needles));
            for (u32 v : pool)
                opts.push_back(Utils::Format("%s (%X)", IdName(v).c_str(), v));

            Keyboard kb(full, opts);
            const int sel = kb.Open();
            if (sel < 0)
                return false;
            if (sel == 0)
            {
                if (AskHexId(full + " (16進)", cat->idMin, true, cat->idMin, cat->idMax, id))
                    return true;
                continue;
            }
            if (sel == 1)
            {
                if (NameTableLoaded())
                    g_hideUnimpl = !g_hideUnimpl;
                continue;
            }
            if (sel == 2)
            {
                ToggleSearch(needles);
                continue;
            }
            id = pool[(size_t)sel - 3];
            return true;
        }
    }

    // 1つ生成する流れ (追加項目 → 確認 → 生成)
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
            if (!AskValueBlank("個数", false, MAX_COUNT, count) || count == 0)
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
    static void BuildPool(const Category &cat, bool hideUnimpl, std::vector<u32> &pool,
                          const std::vector<std::string> *needles)
    {
        std::vector<u32> all;
        PoolIds(cat.idMin, cat.idMax, all);
        pool.clear();
        for (u32 id : all)
        {
            if (hideUnimpl && IsUnimplemented(id))
                continue;
            if (needles && !needles->empty() && !NameHasAny(IdName(id), *needles))
                continue;
            pool.push_back(id);
        }
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
        else if (!AskValueBlank("個数 (このあと選ぶ全部に適用)", false, MAX_COUNT, count) || count == 0)
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
        static constexpr size_t HEAD = 6;     // 一覧の先頭の操作行の数
        std::vector<u32> pool;
        bool hide = g_hideUnimpl;
        std::vector<std::string> needles;
        u32 page = 0, made = 0;
        if (cat)
            BuildPool(*cat, hide, pool, &needles);

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
                rows.push_back(SearchRowLabel(needles));
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
                        g_hideUnimpl = hide;
                        BuildPool(*cat, hide, pool, &needles);
                        page = 0;
                    }
                }
                else if (sel == 5)
                {
                    if (ToggleSearch(needles))
                    {
                        BuildPool(*cat, hide, pool, &needles);
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

        // 性別専用の防具の除外 (防具のカテゴリを一括生成するとき)
        if (askUnimpl && !ids.empty() && CommonCategory(ids) == &g_cats[1])
        {
            u32 nm = 0, nf = 0;
            for (u32 id : ids)
            {
                if (IsMaleOnlyArmor(id))
                    ++nm;
                else if (IsFemaleOnlyArmor(id))
                    ++nf;
            }
            if (nm > 0 || nf > 0)
            {
                std::vector<std::string> go;
                go.push_back("除外しない");
                go.push_back(Utils::Format("男専用防具を除外 (%u件)", nm));
                go.push_back(Utils::Format("女専用防具を除外 (%u件)", nf));
                Keyboard kg(Utils::Format("性別専用の防具 (対象 %u 件中)", (u32)ids.size()), go);
                const int gi = kg.Open();
                if (gi < 0)
                    return;                 // Bで中止 (まだ何も作っていない)
                if (gi == 1 || gi == 2)
                {
                    std::vector<u32> keep;
                    for (u32 id : ids)
                        if (!(gi == 1 ? IsMaleOnlyArmor(id) : IsFemaleOnlyArmor(id)))
                            keep.push_back(id);
                    ids.swap(keep);
                    what += (gi == 1) ? " (男専用を除外)" : " (女専用を除外)";
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
        if (!AskValueBlank("個数(枠の数)", false, ct->slots, n) || n == 0)
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

    // カテゴリ単位 / 未実装 / 性別専用防具 を、まとめて削除。
    // 武器・防具・ライフ道具・「すべて」(かばん)では、「(装備中含む)」の選択肢もある。
    static void BulkDeleteEntry(MenuEntry *entry)
    {
        (void)entry;

        enum { K_ALL, K_ALL_EQ, K_UNIMPL, K_MALE, K_FEMALE };

        const Container *c = nullptr;
        if (!AskTarget("一括削除: 対象", c))
            return;

        const size_t NCAT = sizeof(g_cats) / sizeof(g_cats[0]);
        std::vector<std::string> names;
        for (const Category &cg : g_cats)
            names.push_back(Utils::Format("%s (ID 0x%04X-0x%04X)", cg.name, cg.idMin, cg.idMax));
        names.push_back(Utils::Format("%s内のすべて", c->name));
        Keyboard kc("一括削除するカテゴリ", names);
        const int ci = kc.Open();
        if (ci < 0)
            return;
        const bool all = ((size_t)ci == NCAT);
        const Category *cat = all ? nullptr : &g_cats[ci];
        const char *catName = all ? "すべて" : cat->name;

        // 削除の種類の一覧 (表示する文字と種類を並べて持つ)
        std::vector<std::string> modes;
        std::vector<int> kinds;
        modes.push_back(all ? std::string("全てのアイテムを削除") : Utils::Format("%sを全て削除", cat->name));
        kinds.push_back(K_ALL);
        // 装備中の印があるのは、かばんの武器・防具・ライフ道具だけ
        const bool hasEquipCat = all || cat == &g_cats[0] || cat == &g_cats[1] || cat == &g_cats[2];
        if (c == &g_bag && hasEquipCat)
        {
            modes.push_back(all ? std::string("全てのアイテムを削除(装備中含む)")
                                : Utils::Format("%sを全て削除(装備中含む)", cat->name));
            kinds.push_back(K_ALL_EQ);
        }
        modes.push_back("未実装アイテムだけ削除");
        kinds.push_back(K_UNIMPL);
        if (cat == &g_cats[1])
        {
            modes.push_back("男専用防具だけ削除");
            kinds.push_back(K_MALE);
            modes.push_back("女専用防具だけ削除");
            kinds.push_back(K_FEMALE);
        }
        Keyboard kmo("削除する対象", modes);
        const int mo = kmo.Open();
        if (mo < 0)
            return;
        const int kind = kinds[mo];
        if (kind == K_UNIMPL && !NameTableLoaded())
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
        u32 eqW = 0, eqA = 0, eqT = 0, eqOther = 0;     // 削除する装備中の数 (装備中含む のとき)
        for (u16 s : b.order)
        {
            u32 id = 0;
            Process::Read32(RowOf(b, s) + OFF_ID, id);
            bool hit = all || (id >= cat->idMin && id <= cat->idMax);
            if (hit && kind == K_UNIMPL && !IsUnimplemented(id))
                hit = false;
            if (hit && kind == K_MALE && !IsMaleOnlyArmor(id))
                hit = false;
            if (hit && kind == K_FEMALE && !IsFemaleOnlyArmor(id))
                hit = false;
            if (!hit)
                continue;
            if (IsEquipped(b, s))
            {
                if (kind != K_ALL_EQ)
                {
                    ++protectedN;
                    continue;
                }
                const Category *cg = FindCategory((u16)id);
                if (cg == &g_cats[0])      ++eqW;
                else if (cg == &g_cats[1]) ++eqA;
                else if (cg == &g_cats[2]) ++eqT;
                else                       ++eqOther;
            }
            del.push_back(s);
        }
        if (del.empty())
        {
            MessageBox("対象なし", "削除するアイテムがありません。")();
            return;
        }

        const u32 eqDel = eqW + eqA + eqT + eqOther;
        std::string msg = Utils::Format("%s / %s\n%s\n%u 件を削除します。(残り %u 件)\n",
                                        c->name, catName,
                                        kind == K_UNIMPL ? "未実装アイテムのみ"
                                        : kind == K_MALE ? "男専用防具のみ"
                                        : kind == K_FEMALE ? "女専用防具のみ"
                                        : kind == K_ALL_EQ ? "すべて(装備中も含む)" : "すべて",
                                        (u32)del.size(), (u32)(b.order.size() - del.size()));
        if (protectedN)
            msg += Utils::Format("装備中の %u 件は残します。\n", protectedN);
        if (eqDel)
        {
            msg += Utils::Format("装備中の 武器 %u / 防具 %u / ライフ道具 %u", eqW, eqA, eqT);
            if (eqOther)
                msg += Utils::Format(" / その他 %u", eqOther);
            msg += " も消えます。\n装備は外れた状態になります。(実機未検証)\n";
        }
        msg += "セーブのバックアップは取りましたか?\n実行しますか?";
        if (!(MessageBox("一括削除の確認", msg, DialogType::DialogYesNo))())
            return;
        if (eqDel && !(MessageBox("最終確認", "装備中のアイテムも削除します。本当に実行しますか?", DialogType::DialogYesNo))())
            return;

        DeleteRows(b, del);
        MessageBox("完了", Utils::Format("%u 件を削除しました。\n"
                                         "かばん・倉庫の画面は、一度閉じて開き直すと反映されます。%s",
                                         (u32)del.size(),
                                         eqDel ? "\n装備は、装備画面で付け直してください。" : ""))();
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
        static constexpr int HEAD = 4;
        u32 page = 0, deleted = 0;
        std::vector<std::string> needles;

        for (;;)
        {
            std::vector<u16> slots;
            u32 inCat = 0;
            for (u16 s : b.order)
            {
                u32 id = 0;
                Process::Read32(RowOf(b, s) + OFF_ID, id);
                if (id >= cat->idMin && id <= cat->idMax)
                {
                    ++inCat;
                    if (NameHasAny(IdName(id), needles))
                        slots.push_back(s);
                }
            }
            const u32 total = (u32)slots.size();
            if (inCat == 0)
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
            rows.push_back(SearchRowLabel(needles));
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
            else if (sel == 3)
            {
                if (ToggleSearch(needles))
                    page = 0;
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
    //  メニューへの登録: 「アイテムエディタ」フォルダにまとめる
    //   1. 新しく装備したアイテムを編集
    //   2. アイテム生成 (フォルダ)
    //   3. アイテム削除 (フォルダ)
    //   4. カテゴリ別 一括変更
//   5. 固有効果変更
    // ------------------------------------------------------------
    static void AddItemEditor(PluginMenu *menu)
    {
        MenuFolder *root = new MenuFolder("アイテムエディタ",
                                          "アイテムの編集・生成・削除・一括変更です。必ずセーブのバックアップを取ってから使ってください。");

        root->Append(new MenuEntry("新しく装備したアイテムを編集", EquipEditEntry, nullptr,
                                   "有効にして、編集したい装備を着けます(着けている場合は外して着け直し)。\n"
                                   "「Press Y to edit」と表示されてから約5秒の間だけYで編集できます。\n"
                                   "編集後、同じ装備をもう一度つけ直すと反映されます(そのつけ直しでは編集メニューは出ません)。\n"
                                   "武器・防具・ライフ道具8種すべてに対応しています。"));

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
        root->Append(gen);

        MenuFolder *del = new MenuFolder("アイテム削除",
                                         "かばん・倉庫のアイテムを削除します。「(装備中含む)」を選んだとき以外は、装備中のものを残します。"
                                         "必ずセーブのバックアップを取ってから使ってください。");
        del->Append(new MenuEntry("カテゴリ・未実装をまとめて削除", nullptr, BulkDeleteEntry,
                                  "かばんまたは倉庫で、カテゴリ単位 / 未実装アイテムだけ / 性別専用防具だけ、まとめて削除します。\n"
                                  "武器・防具・ライフ道具・すべてでは、「(装備中含む)」も選べます。"));
        del->Append(new MenuEntry("1つずつ削除", nullptr, DeleteOneByOneEntry,
                                  "かばんまたは倉庫で、カテゴリを選び、一覧から選ぶたびにその場で削除します。Bで終了します。"));
        root->Append(del);

        root->Append(new MenuEntry("カテゴリ別 一括変更", nullptr, BulkEditEntry,
                                   "武器・防具・ライフ道具・家具ごとに、かばん内の該当アイテムをまとめて変更します。\n"
                                   "変えたい項目を選んでから「決定して適用」で書き込みます。アイテムIDは、範囲を指定して被りなしの連番にするか、1件ずつ選べます。"));

        root->Append(new MenuEntry("固有効果変更", nullptr, InnateMainEntry,
                                   "武器・防具・ライフ道具の固有効果を、メモリ上のデータ表を書き換えて変更します。\n"
                                   "押すと、カテゴリ(武器 / 防具 / ライフ道具)を選べます。\n"
                                   "1つずつ変更するほか、カテゴリ全体 / ID範囲を同じ効果にまとめて変更できます。\n"
                                   "同じIDのアイテムすべてに効きます。自分のゲームの中だけで有効で、ゲームを終了すると元に戻ります。\n"
                                   "変更したあと、装備をつけ直すと反映されます。\n"
                                   "ライフ道具は、ゲームの中で一度装備してから開いてください。"));

        menu->Append(root);
    }
}
