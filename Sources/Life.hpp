// Life.hpp  ―  ライフ変更 と おすすめ(スクリーンショット無効)
//
// main.cpp への追加は3か所だけ(説明は返信を見る):
//   1) 先頭付近に   #include "Life.hpp"
//   2) main() の SynchronizeWithFrame の次の行に   AddRecommended(menu);
//   3) main() の menu->Run(); の直前に             AddLifeChange(menu);
#pragma once

// ------------------------------------------------------------
//  ライフ変更の本体 (ゲーム本体のスレッドから、毎フレーム呼ばれる)
//  元のARコードと同じ処理を C++ で書き直したもの。置き場(コード領域)は使わない。
// ------------------------------------------------------------
extern "C"
{
    void jp_lifeHook(void);

    volatile u32 g_lifeWant  = 0;   // 0 = 要求なし / 0x100 | ライフID (「やめる」を選ぶまで、ずっと有効)
    volatile u32 g_lifeCalls = 0;   // 差し込み位置を通った回数 (診断用)
    volatile u32 g_lifeSets  = 0;   // 実際にライフを書き換えた回数 (診断用)
    volatile u32 g_lifeState = 0;   // 直前の状態 0=要求なし 1=マネージャなし 2=プレイヤーなし 3=一致済み 4=変更した
    volatile u32 g_lifeCur   = 0;   // 直前に読んだ現在のライフ (診断用)
    volatile u32 g_lifePlayer = 0;  // 直前のプレイヤー構造体 (診断用)

    // 元のARコードと同じく、要求がある間は毎回「今のライフと違えば書き換える」。
    // (一度変えても、ゲーム側が元に戻すことがあるため、1回だけでは足りない)
    void jp_lifeApply(void)
    {
        g_lifeCalls = g_lifeCalls + 1;

        const u32 want = g_lifeWant;
        if (!(want & 0x100))
        {
            g_lifeState = 0;
            return;
        }
        const u32 life = want & 0xFF;

        const u32 mgr = *(volatile u32 *)0x0068BBB8;
        if (!mgr)
        {
            g_lifeState = 1;
            return;
        }
        const u32 player = *(volatile u32 *)(mgr + 0x7800 + 0x28);
        g_lifePlayer = player;
        if (!player)
        {
            g_lifeState = 2;
            return;
        }

        typedef u32  (*GetLifeFn)(u32, u32);   // 0x1ECAF0 (player+0xDA8, 3) → 現在のライフ(下位8bit)
        typedef void (*SetLifeFn)(u32, u32);   // 0x1D5F74 (player, life)
        typedef u32  (*GetObjFn)(void);        // 0x1AD4BC ()

        const s32 cur = (s32)(s8)((GetLifeFn)0x001ECAF0)(player + 0xC00 + 0x1A8, 3);
        g_lifeCur = (u32)cur;
        if (cur == (s32)life)
        {
            g_lifeState = 3;   // すでに一致
            return;
        }
        ((SetLifeFn)0x001D5F74)(player, life);
        const u32 obj = ((GetObjFn)0x001AD4BC)();
        *(volatile u8 *)(obj + 0x1FC00 + 0xC8) = (u8)life;
        g_lifeSets  = g_lifeSets + 1;
        g_lifeState = 4;
    }
}

// 0x5A4C30 の2命令 (ldr r0,=0x73FE3C / bl 0x23C83C) を置き換えて、ここに来る。
// 置き換えた2命令を再現してから、変更処理を呼び、0x5A4C38 に戻る。
asm(R"(
    .arm
    .text
    .align 2

    .global jp_lifeHook
    .type jp_lifeHook, %function
jp_lifeHook:
    ldr   r0, =0x0073FE3C
    ldr   r12, =0x0023C83C
    blx   r12
    bl    jp_lifeApply
    ldr   pc, =0x005A4C38
    .ltorg
)");

namespace CTRPluginFramework
{
    // ------------------------------------------------------------
    //  ライフ変更
    // ------------------------------------------------------------
    static Hook g_hookLife;
    static bool g_lifeHookInstalled = false;

    static bool InstallLifeHook(void)
    {
        if (g_lifeHookInstalled)
            return true;

        // 置き換える2命令が想定どおりか確認 (元のARコードのライフ変更がONのままだと、ここで止まる)
        u32 a = 0, b = 0;
        if (!Process::Read32(0x005A4C30, a) || !Process::Read32(0x005A4C34, b)
            || a != 0xE59F00EC || b != 0xEBF25F00)
        {
            MessageBox("エラー",
                       "ライフ変更の差し込み位置の命令が想定と違います。\n"
                       "ARコードのライフ変更がONのままか、ゲームのバージョンが違う可能性があります。")();
            return false;
        }

        g_hookLife.Initialize(0x005A4C30, (u32)(uintptr_t)jp_lifeHook);
        g_hookLife.Enable();
        g_lifeHookInstalled = true;
        return true;
    }

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

    // 歯車マークから開く (メニューが開いている間はゲームが止まっている)
    static void LifeChangeEntry(MenuEntry *entry)
    {
        (void)entry;

        std::vector<std::string> names;
        for (const LifeInfo &l : g_lives)
            names.push_back(l.name);
        const int nLives = (int)names.size();
        names.push_back("[ライフ変更をやめる]");
        names.push_back("[状態を見る]");

        Keyboard kb("変更するライフを選んでください", names);
        const int sel = kb.Open();
        if (sel < 0)
            return;

        if (sel == nLives + 1)   // 状態を見る
        {
            static const char *states[] = { "要求なし", "マネージャなし", "プレイヤーなし", "一致済み", "変更した" };
            MessageBox("ライフ変更の状態",
                       Utils::Format("要求: %s\n差し込み位置を通った回数: %u\n書き換えた回数: %u\n"
                                     "直前の状態: %s\n現在のライフ(読み取り): %d\nプレイヤー構造体: 0x%08X",
                                     (g_lifeWant & 0x100) ? Utils::Format("ライフ%02X", (u32)(g_lifeWant & 0xFF)).c_str() : "なし",
                                     (u32)g_lifeCalls, (u32)g_lifeSets,
                                     states[g_lifeState > 4 ? 0 : g_lifeState], (int)g_lifeCur, (u32)g_lifePlayer))();
            return;
        }

        if (sel == nLives)       // やめる
        {
            g_lifeWant = 0;
            MessageBox("ライフ変更", "ライフ変更をやめました。")();
            return;
        }

        if (!InstallLifeHook())
            return;

        g_lifeWant = 0x100 | g_lives[sel].id;
        MessageBox("ライフ変更",
                   Utils::Format("「%s」に変更します。\nメニューを閉じて、ゲームを進めてください。\n"
                                 "「やめる」を選ぶまで、このライフに保ちます。", g_lives[sel].name))();
    }

    static void AddLifeChange(PluginMenu *menu)
    {
        menu->Append(new MenuEntry("ライフ変更", nullptr, LifeChangeEntry,
                                   "歯車マークを押すと、ライフの一覧(王国兵士、傭兵、…)が出ます。\n"
                                   "選んだライフに変更し、「ライフ変更をやめる」を選ぶまで保ちます。\n"
                                   "「状態を見る」で、動作の回数を確認できます。\n"
                                   "ARコードの「ライフ変更」がONのままだと使えません。先にOFFにして、ゲームを再起動してください。"));
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
