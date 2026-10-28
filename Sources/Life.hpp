// Life.hpp  ―  ライフ変更 と おすすめ(スクリーンショット無効)
//
// main.cpp への追加は3か所だけ:
//   1) 先頭付近に   #include "Life.hpp"
//   2) main() の SynchronizeWithFrame の次の行に   AddRecommended(menu);
//   3) main() の menu->Run(); の直前に             AddLifeChange(menu);
#pragma once

// ------------------------------------------------------------
//  ライフ変更の本体 (ゲーム本体のスレッドから、毎フレーム呼ばれる)
//  元のARコードと同じ処理を C++ で書き直したもの。コード置き場は使わない。
//  差し込み位置は2か所:
//    A) 0x5A4C30 (元のARコードと同じ場所。ある条件のときだけ通る)
//    B) 0x5A4A5C (A を含む関数の入り口。毎フレーム通るはず)
// ------------------------------------------------------------
extern "C"
{
    void jp_lifeHookA(void);
    void jp_lifeHookB(void);

    volatile u32 g_lifeWant   = 0;   // 0 = 要求なし / 0x100 | ライフID (「やめる」を選ぶまで有効)
    volatile u32 g_lifeCallsA = 0;   // 差し込みAを通った回数 (診断用)
    volatile u32 g_lifeCallsB = 0;   // 差し込みBを通った回数 (診断用)
    volatile u32 g_lifeSets   = 0;   // 実際にライフを書き換えた回数 (診断用)
    volatile u32 g_lifeState  = 0;   // 0=要求なし 1=マネージャなし 2=プレイヤーなし 3=一致済み 4=変更した
    volatile u32 g_lifeCur    = 0;   // 直前に読んだ現在のライフ (診断用)
    volatile u32 g_lifePlayer = 0;   // 直前のプレイヤー構造体 (診断用)

    // 要求がある間は、毎回「今のライフと違えば書き換える」(元のARコードと同じ)
    static void LifeBody(void)
    {
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
            g_lifeState = 3;
            return;
        }
        ((SetLifeFn)0x001D5F74)(player, life);
        const u32 obj = ((GetObjFn)0x001AD4BC)();
        *(volatile u8 *)(obj + 0x1FC00 + 0xC8) = (u8)life;
        g_lifeSets  = g_lifeSets + 1;
        g_lifeState = 4;
    }

    void jp_lifeApplyA(void) { g_lifeCallsA = g_lifeCallsA + 1; LifeBody(); }
    void jp_lifeApplyB(void) { g_lifeCallsB = g_lifeCallsB + 1; LifeBody(); }
}

asm(R"(
    .arm
    .text
    .align 2

@ A: 0x5A4C30 の2命令 (ldr r0,=0x73FE3C / bl 0x23C83C) を置き換えて、ここに来る。
@    置き換えた2命令を再現してから、変更処理を呼び、0x5A4C38 に戻る。
    .global jp_lifeHookA
    .type jp_lifeHookA, %function
jp_lifeHookA:
    ldr   r0, =0x0073FE3C
    ldr   r12, =0x0023C83C
    blx   r12
    bl    jp_lifeApplyA
    ldr   pc, =0x005A4C38
    .ltorg

@ B: 0x5A4A5C の2命令 (push {r4-r8,lr} / mov r4,r0) を置き換えて、ここに来る。
@    s0 (浮動小数点の引数) など、関数の引数を壊さないように保存する。
    .global jp_lifeHookB
    .type jp_lifeHookB, %function
jp_lifeHookB:
    push  {r4-r8, lr}
    mov   r4, r0
    push  {r0-r3, r12, lr}
    vpush {d0-d7}
    bl    jp_lifeApplyB
    vpop  {d0-d7}
    pop   {r0-r3, r12, lr}
    ldr   pc, =0x005A4A64
    .ltorg
)");

namespace CTRPluginFramework
{
    // ------------------------------------------------------------
    //  ライフ変更
    // ------------------------------------------------------------
    static Hook g_hookLifeA;
    static Hook g_hookLifeB;
    static bool g_lifeInstalledA = false;
    static bool g_lifeInstalledB = false;
    static bool g_lifeInstallTried = false;

    // メニューを開いたときに、差し込みを入れる (ライフを選ぶ前から動かして、回数を数えるため)
    static void InstallLifeHooks(void)
    {
        if (g_lifeInstallTried)
            return;
        g_lifeInstallTried = true;

        u32 a0 = 0, a1 = 0, b0 = 0, b1 = 0;
        const bool okA = Process::Read32(0x005A4C30, a0) && Process::Read32(0x005A4C34, a1)
                         && a0 == 0xE59F00EC && a1 == 0xEBF25F00;
        const bool okB = Process::Read32(0x005A4A5C, b0) && Process::Read32(0x005A4A60, b1)
                         && b0 == 0xE92D41F0 && b1 == 0xE1A04000;

        if (okA)
        {
            g_hookLifeA.Initialize(0x005A4C30, (u32)(uintptr_t)jp_lifeHookA);
            g_hookLifeA.Enable();
            g_lifeInstalledA = true;
        }
        if (okB)
        {
            g_hookLifeB.Initialize(0x005A4A5C, (u32)(uintptr_t)jp_lifeHookB);
            g_hookLifeB.Enable();
            g_lifeInstalledB = true;
        }
        if (!okA || !okB)
            MessageBox("お知らせ",
                       Utils::Format("差し込み位置の命令が想定と違うため、入れなかった場所があります。\n"
                                     "A(0x5A4C30): %s\nB(0x5A4A5C): %s\n"
                                     "ARコードのライフ変更がONのままだと、Aは入りません。",
                                     okA ? "入れた" : "入れていない", okB ? "入れた" : "入れていない"))();
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

        InstallLifeHooks();   // 開いた時点で入れる。ライフを選ぶ前から回数を数える

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
            const std::string want = (g_lifeWant & 0x100)
                                         ? Utils::Format("ライフ%02X", (u32)(g_lifeWant & 0xFF))
                                         : std::string("なし");
            MessageBox("ライフ変更の状態",
                       Utils::Format("要求: %s\n差し込みA(%s)を通った回数: %u\n差し込みB(%s)を通った回数: %u\n"
                                     "書き換えた回数: %u\n直前の状態: %s\n現在のライフ(読み取り): %d\nプレイヤー構造体: 0x%08X",
                                     want.c_str(),
                                     g_lifeInstalledA ? "入れた" : "なし", (u32)g_lifeCallsA,
                                     g_lifeInstalledB ? "入れた" : "なし", (u32)g_lifeCallsB,
                                     (u32)g_lifeSets, states[g_lifeState > 4 ? 0 : g_lifeState],
                                     (int)g_lifeCur, (u32)g_lifePlayer))();
            return;
        }

        if (sel == nLives)       // やめる
        {
            g_lifeWant = 0;
            MessageBox("ライフ変更", "ライフ変更をやめました。")();
            return;
        }

        if (!g_lifeInstalledA && !g_lifeInstalledB)
        {
            MessageBox("エラー", "差し込みを入れられなかったので、ライフ変更は使えません。")();
            return;
        }

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
                                   "ARコードの「ライフ変更」がONのままだと、一部が使えません。先にOFFにして、ゲームを再起動してください。"));
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
