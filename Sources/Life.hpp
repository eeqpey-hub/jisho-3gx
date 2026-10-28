// Life.hpp  ―  ライフ変更 と おすすめ(スクリーンショット無効)
//
// main.cpp への追加は3か所だけ:
//   1) 先頭付近に   #include "Life.hpp"
//   2) main() の SynchronizeWithFrame の次の行に   AddRecommended(menu);
//   3) main() の menu->Run(); の直前に             AddLifeChange(menu);
//
// ライフ変更は、フック(差し込み)を使わない。メニューが開いていて、ゲームが止まっている間に、
// 元のARコードと同じ手順で、ゲームの関数を直接呼んで変更する。
#pragma once

namespace CTRPluginFramework
{
    // ------------------------------------------------------------
    //  ライフ変更
    // ------------------------------------------------------------
    struct LifeInfo
    {
        const char *name;
        u32         id;
    };

    // 並びは、元のチートの並び(王国兵士、傭兵、狩人、…)
    static const LifeInfo g_lives[] =
    {
        { "王国兵士",   0x09 }, { "傭兵",       0x0E }, { "狩人",       0x03 },
        { "魔法使い",   0x11 }, { "採掘師",     0x12 }, { "木こり",     0x07 },
        { "釣り人",     0x02 }, { "料理人",     0x0D }, { "鍛冶屋",     0x05 },
        { "大工",       0x10 }, { "裁縫師",     0x08 }, { "錬金術師",   0x0B },
        { "無職",       0x00 }, { "冒険家",     0x01 }, { "商人",       0x04 },
        { "ポストマン", 0x06 }, { "学者",       0x0A }, { "占い師",     0x0C },
        { "大道芸人",   0x0F }, { "パイロット", 0x13 }, { "農民",       0x14 },
    };

    static std::string LifeName(int id)
    {
        for (const LifeInfo &l : g_lives)
            if ((int)l.id == id)
                return l.name;
        return Utils::Format("不明(%d)", id);
    }

    // プレイヤー構造体を探す (元のARコード: [[0x68BBB8] + 0x7800 + 0x28])
    static bool LifeGetPlayer(u32 &player)
    {
        u32 mgr = 0;
        if (!Process::Read32(0x0068BBB8, mgr) || !mgr)
            return false;
        if (!Process::Read32(mgr + 0x7800 + 0x28, player) || !player)
            return false;
        return true;
    }

    typedef u32  (*LifeGetFn)(u32, u32);   // 0x1ECAF0 (player+0xDA8, 3) → 現在のライフ(下位8bit)
    typedef void (*LifeSetFn)(u32, u32);   // 0x1D5F74 (player, life)
    typedef u32  (*LifeObjFn)(void);       // 0x1AD4BC ()

    static int LifeRead(u32 player)
    {
        return (int)(s8)((LifeGetFn)0x001ECAF0)(player + 0xC00 + 0x1A8, 3);
    }

    static void LifeChangeNow(u32 life)
    {
        u32 player = 0;
        if (!LifeGetPlayer(player))
        {
            MessageBox("ライフ変更",
                       "プレイヤーの情報が見つかりません。\nフィールドに出てから、もう一度試してください。")();
            return;
        }

        const int before = LifeRead(player);
        if (before == (int)life)
        {
            MessageBox("ライフ変更", Utils::Format("すでに「%s」です。", LifeName(before).c_str()))();
            return;
        }

        ((LifeSetFn)0x001D5F74)(player, life);
        const u32 obj = ((LifeObjFn)0x001AD4BC)();
        if (obj)
            Process::Write8(obj + 0x1FC00 + 0xC8, (u8)life);

        const int after = LifeRead(player);
        MessageBox("ライフ変更",
                   Utils::Format("変更前: %s\n変更後: %s\n\nメニューを閉じて、ゲームで確かめてください。",
                                 LifeName(before).c_str(), LifeName(after).c_str()))();
    }

    // 歯車マークから開く (メニューが開いている間はゲームが止まっている)
    static void LifeChangeEntry(MenuEntry *entry)
    {
        (void)entry;

        std::vector<std::string> names;
        for (const LifeInfo &l : g_lives)
            names.push_back(l.name);
        const int nLives = (int)names.size();
        names.push_back("[今のライフを見る]");

        Keyboard kb("変更するライフを選んでください", names);
        const int sel = kb.Open();
        if (sel < 0)
            return;

        if (sel == nLives)   // 今のライフを見る
        {
            u32 player = 0;
            if (!LifeGetPlayer(player))
            {
                MessageBox("ライフ変更", "プレイヤーの情報が見つかりません。\nフィールドに出てから、もう一度試してください。")();
                return;
            }
            const int cur = LifeRead(player);
            MessageBox("今のライフ",
                       Utils::Format("%s (ID: 0x%02X)\nプレイヤー構造体: 0x%08X",
                                     LifeName(cur).c_str(), (u32)(cur & 0xFF), player))();
            return;
        }

        LifeChangeNow(g_lives[sel].id);
    }

    static void AddLifeChange(PluginMenu *menu)
    {
        menu->Append(new MenuEntry("ライフ変更", nullptr, LifeChangeEntry,
                                   "歯車マークを押すと、ライフの一覧(王国兵士、傭兵、…)が出ます。\n"
                                   "選ぶと、その場でライフを変更します(メニューを開いている間に行います)。\n"
                                   "フィールドに出てから使ってください。"));
    }

    // ------------------------------------------------------------
    //  おすすめ (メニューの一番上。起動時からONで、チェックを外すとOFF)
    // ------------------------------------------------------------
    static constexpr u32 SCREENSHOT_ADDR = 0x0014B0C0;   // 関数の入り口
    static constexpr u32 SCREENSHOT_ORIG = 0xE92D4038;   // push {r3,r4,r5,lr}
    static constexpr u32 SCREENSHOT_OFF  = 0xE12FFF1E;   // bx lr (すぐ戻る)

    static MenuEntry *g_screenshotEntry   = nullptr;
    static bool       g_screenshotPatched = false;

    // チェックが入っている間、毎フレーム呼ばれる
    static void ScreenshotOffEntry(MenuEntry *entry)
    {
        (void)entry;
        Process::Write32(SCREENSHOT_ADDR, SCREENSHOT_OFF);
        g_screenshotPatched = true;
    }

    // 毎フレーム呼ばれる。チェックが外されたら、元の命令に戻す
    static void RecommendedSync(void)
    {
        if (g_screenshotPatched && g_screenshotEntry && !g_screenshotEntry->IsActivated())
        {
            Process::Write32(SCREENSHOT_ADDR, SCREENSHOT_ORIG);
            g_screenshotPatched = false;
        }
    }

    static void AddRecommended(PluginMenu *menu)
    {
        MenuFolder *folder = new MenuFolder("おすすめ",
                                            "ゲームを始めたときから、すでにチェックが入っています。不要ならチェックを外してください。");

        g_screenshotEntry = new MenuEntry("スクリーンショット無効", ScreenshotOffEntry, nullptr,
                                          "スクリーンショットを撮る処理を止めます。");
        folder->Append(g_screenshotEntry);
        menu->Append(folder);

        g_screenshotEntry->Enable();   // 起動時からON
        menu->Callback(RecommendedSync);
    }
}
