// 制作アイテムの品質固定 (ファンタジーライフ 日本語版)
//
// 仕組み:
//   0x3464B8 は「アイテムを作る関数」。第2引数 r1 = 作成の依頼内容(req) で、
//   品質の値は [req+0x10] から読まれる (0x346554 の ldr r4,[r4,#0x10])。
//   [req+0x2C] にも同じ値が入る (0x2FC4F8 付近)。
//   この関数の入口で2か所を書き換えれば、乱数に関係なく品質が決まる。
//
// 品質の値: 0=通常 1=上 2=特上 3=最高 4=ゴッド
//
// 使い方 (main.cpp に2行足す):
//   1) "Life.hpp" を include している行の下に   #include "QualityFix.hpp"
//   2) main() の AddLifeChange(menu); の前に   AddQualityFix(menu);
//
// 実機未検証 (コンパイルも未確認)。バックアップを取ってから使うこと。

#pragma once

#include <3ds.h>
#include <CTRPluginFramework.hpp>
#include <string>
#include <vector>

extern "C"
{
    void jp_hookQuality(void);

    // 0xFFFFFFFF = 固定しない
    volatile u32 g_forceQuality = 0xFFFFFFFF;

    // ゲーム本体のスレッドから呼ばれる。req = 作成の依頼内容
    void QualityPrepare(u32 req)
    {
        const u32 q = g_forceQuality;
        if (q > 4)
            return;
        if (req < 0x00100000 || req > 0x20000000)   // 念のための範囲チェック
            return;
        *(volatile u32 *)(req + 0x10) = q;
        *(volatile u32 *)(req + 0x2C) = q;
    }
}

// 0x3464B8 の置き換えた2命令を再現し、0x3464C0 に戻る。
//   0x3464B8: push {r4-r12,lr}   (E92D5FF0)
//   0x3464BC: mov  r4, r1        (E1A04001)
// 入口の状態(r0-r3, lr, s0-s15)をそのまま保つため、全部退避してからC関数を呼ぶ。
asm(R"(
    .arm
    .text
    .align 2

    .global jp_hookQuality
    .type jp_hookQuality, %function
jp_hookQuality:
    push  {r0-r3, r12, lr}       @ 24バイト
    vpush {d0-d7}                @ 64バイト (合計88 = 8の倍数)
    mov   r0, r1                 @ r0 = req
    bl    QualityPrepare
    vpop  {d0-d7}
    pop   {r0-r3, r12, lr}
    push  {r4-r12, lr}           @ 0x3464B8 の元命令
    mov   r4, r1                 @ 0x3464BC の元命令
    ldr   pc, =0x3464C0          @ 続きの元コードへ
    .ltorg
)");

namespace CTRPluginFramework
{
    static constexpr u32 HOOK_QUALITY = 0x003464B8;

    static bool g_qualityHooked = false;

    static bool InstallQualityHook(void)
    {
        if (g_qualityHooked)
            return true;

        // 置き換える2命令が想定どおりか確認 (違うバージョンなら入れない)
        u32 a = 0, b = 0;
        if (!Process::Read32(HOOK_QUALITY, a) || !Process::Read32(HOOK_QUALITY + 4, b)
            || a != 0xE92D5FF0 || b != 0xE1A04001)
        {
            MessageBox("エラー",
                       "品質固定のフック位置の命令が想定と違います。\n"
                       "ゲームのバージョンが異なる可能性があるため、機能を有効にできません。")();
            return false;
        }

        // 先にジャンプ先(2語目)を書き、そのあとで ldr pc 命令(1語目)を書く。
        Process::Pause();
        Process::Write32(HOOK_QUALITY + 4, (u32)(uintptr_t)jp_hookQuality);
        Process::Write32(HOOK_QUALITY,     0xE51FF004);   // ldr pc, [pc, #-4]
        Process::Play();

        g_qualityHooked = true;
        return true;
    }

    // メニューを選んだときに1回だけ実行される
    static void QualityFixEntry(MenuEntry *entry)
    {
        (void)entry;

        if (!InstallQualityHook())
            return;

        std::vector<std::string> names;
        names.push_back("OFF (通常どおり乱数)");
        names.push_back("通常 (0)");
        names.push_back("上 (1)");
        names.push_back("特上 (2)");
        names.push_back("最高 (3)");
        names.push_back("ゴッド (4)");

        Keyboard kb("固定する品質を選択", names);
        int sel = kb.Open();
        if (sel < 0 || sel > 5)
            return;

        if (sel == 0)
        {
            g_forceQuality = 0xFFFFFFFF;
            OSD::Notify("Quality: OFF");
        }
        else
        {
            g_forceQuality = (u32)(sel - 1);
            OSD::Notify("Quality: fixed to " + std::to_string(sel - 1));
        }
    }

    // ------------------------------------------------------------
    //  制作の即完了
    //  0x420F48 の「bl 0x2F5C6C」(最後の工程が完了したか?を調べる呼び出し) を
    //  「mov r0,#1」に差し替える。最初の入力で完了の経路に入る。
    //  工程番号(A)は書き換えないので、左下の工程アイコンの表示は崩れない。
    // ------------------------------------------------------------
    static constexpr u32 PATCH_COMPLETE  = 0x00420F48;
    static constexpr u32 ORIG_COMPLETE   = 0xEBFB5347;   // bl 0x2F5C6C
    static constexpr u32 MOV_R0_1        = 0xE3A00001;   // mov r0, #1

    static void InstantCraftEntry(MenuEntry *entry)
    {
        (void)entry;

        u32 cur = 0;
        if (!Process::Read32(PATCH_COMPLETE, cur)
            || (cur != ORIG_COMPLETE && cur != MOV_R0_1))
        {
            MessageBox("エラー",
                       "即完了の位置の命令が想定と違います。\n"
                       "ゲームのバージョンが異なる可能性があるため、機能を有効にできません。")();
            return;
        }

        std::vector<std::string> names;
        names.push_back("OFF (通常)");
        names.push_back("ON (最初の入力で完了)");

        Keyboard kb("制作の即完了", names);
        int sel = kb.Open();
        if (sel < 0 || sel > 1)
            return;

        Process::Pause();
        Process::Write32(PATCH_COMPLETE, sel == 1 ? MOV_R0_1 : ORIG_COMPLETE);
        Process::Play();

        OSD::Notify(sel == 1 ? "Instant craft: ON" : "Instant craft: OFF");
    }

    static void AddQualityFix(PluginMenu *menu)
    {
        menu->Append(new MenuEntry("制作を即完了", nullptr, InstantCraftEntry,
                                   "制作ミニゲームを、最初の入力で完了させます。\n"
                                   "工程の番号は書き換えないので、画面の工程アイコンは通常どおり出ます。\n"
                                   "制作の前にONにして、終わったらOFFに戻してください。\n"
                                   "実機未検証です。必ずセーブのバックアップを取ってから使ってください。"));

        menu->Append(new MenuEntry("制作の品質を固定", nullptr, QualityFixEntry,
                                   "制作で作られるアイテムの品質を、選んだ値に固定します。\n"
                                   "(0=通常 1=上 2=特上 3=最高 4=ゴッド)\n"
                                   "アイテムを作る処理全般に効くので、制作の前に選び、終わったらOFFに戻してください。\n"
                                   "実機未検証です。必ずセーブのバックアップを取ってから使ってください。"));
    }
}
