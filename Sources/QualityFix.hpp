// 制作まわりのチート (ファンタジーライフ 日本語版)
//
//  1) 制作の品質固定   : アイテム作成関数 0x3464B8 の入口で品質を書き換える
//                        (武器・防具などは品質値、料理・薬・爆弾は極上IDへの差し替え)
//  2) 制作を即完了     : 制作が始まって一定フレーム後に工程番号を最終にして、完了の流れに入れる
//
// 品質の値: 0=通常 1=上 2=特上 3=最高 4=ゴッド
//   料理・薬・爆弾は「通常」と「極上」の2種類だけなので、3以上→極上 / 2以下→通常 にします。
//
// 使い方 (main.cpp に2行足す):
//   1) "Life.hpp" を include している行の下に   #include "QualityFix.hpp"
//   2) main() の AddLifeChange(menu); の前に   AddQualityFix(menu);
//
// 実機未検証 (コンパイルも未確認)。セーブのバックアップを取ってから使うこと。

#pragma once

#include <3ds.h>
#include <CTRPluginFramework.hpp>
#include <string>
#include <vector>

// ============================================================
//  料理・薬・爆弾の 通常ID / 極上ID の対応表
// ============================================================
namespace QualityFixData
{
    // 通常ID, 極上ID  (料理_薬_爆弾.hpp から生成。143組)
    // 0x07C0(特製おんせんたまご) は極上が「未知のアイテム」で作れないため含めない。
    static const u16 kFoodPairs[][2] =
    {
    { 0x0666, 0x0667 },
    { 0x0668, 0x0669 },
    { 0x066A, 0x066B },
    { 0x066C, 0x066D },
    { 0x066E, 0x066F },
    { 0x0670, 0x0671 },
    { 0x0672, 0x0673 },
    { 0x0674, 0x0675 },
    { 0x0676, 0x0677 },
    { 0x0678, 0x0679 },
    { 0x067A, 0x067B },
    { 0x067C, 0x067D },
    { 0x067E, 0x067F },
    { 0x0680, 0x0681 },
    { 0x0682, 0x0683 },
    { 0x0684, 0x0685 },
    { 0x0686, 0x0687 },
    { 0x0688, 0x0689 },
    { 0x068A, 0x068B },
    { 0x068C, 0x068D },
    { 0x068E, 0x068F },
    { 0x0690, 0x0691 },
    { 0x0692, 0x0693 },
    { 0x0694, 0x0695 },
    { 0x0696, 0x0697 },
    { 0x0699, 0x069A },
    { 0x069B, 0x069C },
    { 0x069D, 0x069E },
    { 0x069F, 0x06A0 },
    { 0x06A1, 0x06A2 },
    { 0x06A3, 0x06A4 },
    { 0x06A5, 0x06A6 },
    { 0x06A7, 0x06A8 },
    { 0x06A9, 0x06AA },
    { 0x06AB, 0x06AC },
    { 0x06AD, 0x06AE },
    { 0x06AF, 0x06B0 },
    { 0x06B1, 0x06B2 },
    { 0x06B3, 0x06B4 },
    { 0x06B5, 0x06B6 },
    { 0x06B7, 0x06B8 },
    { 0x06B9, 0x06BA },
    { 0x06BB, 0x06BC },
    { 0x06BD, 0x06BE },
    { 0x06BF, 0x06C0 },
    { 0x06C1, 0x06C2 },
    { 0x06C3, 0x06C4 },
    { 0x06C5, 0x06C6 },
    { 0x06C7, 0x06C8 },
    { 0x06C9, 0x06CA },
    { 0x06CB, 0x06CC },
    { 0x06CD, 0x06CE },
    { 0x06CF, 0x06D0 },
    { 0x06D1, 0x06D2 },
    { 0x06D3, 0x06D4 },
    { 0x06D5, 0x06D6 },
    { 0x06D7, 0x06D8 },
    { 0x06D9, 0x06DA },
    { 0x06DB, 0x06DC },
    { 0x06DD, 0x06DE },
    { 0x06DF, 0x06E0 },
    { 0x06E1, 0x06E2 },
    { 0x06E3, 0x06E4 },
    { 0x06E5, 0x06E6 },
    { 0x06E7, 0x06E8 },
    { 0x06E9, 0x06EA },
    { 0x06EB, 0x06EC },
    { 0x06ED, 0x06EE },
    { 0x06EF, 0x06F0 },
    { 0x06F1, 0x06F2 },
    { 0x06F3, 0x06F4 },
    { 0x06F5, 0x06F6 },
    { 0x06F7, 0x06F8 },
    { 0x06F9, 0x06FA },
    { 0x06FB, 0x06FC },
    { 0x06FD, 0x06FE },
    { 0x06FF, 0x0700 },
    { 0x0701, 0x0702 },
    { 0x0703, 0x0704 },
    { 0x0705, 0x0706 },
    { 0x0707, 0x0708 },
    { 0x0709, 0x070A },
    { 0x070B, 0x070C },
    { 0x070D, 0x070E },
    { 0x070F, 0x0710 },
    { 0x0711, 0x0712 },
    { 0x0713, 0x0714 },
    { 0x0715, 0x0716 },
    { 0x0717, 0x0718 },
    { 0x0719, 0x071A },
    { 0x071B, 0x071C },
    { 0x071D, 0x071E },
    { 0x071F, 0x0720 },
    { 0x0721, 0x0722 },
    { 0x0723, 0x0724 },
    { 0x0725, 0x0726 },
    { 0x0727, 0x0728 },
    { 0x0729, 0x072A },
    { 0x072B, 0x072C },
    { 0x072E, 0x072F },
    { 0x0730, 0x0731 },
    { 0x0732, 0x0733 },
    { 0x0734, 0x0735 },
    { 0x0736, 0x0737 },
    { 0x0738, 0x0739 },
    { 0x073A, 0x073B },
    { 0x073C, 0x073D },
    { 0x073E, 0x073F },
    { 0x0740, 0x0741 },
    { 0x0742, 0x0743 },
    { 0x0744, 0x0745 },
    { 0x0746, 0x0747 },
    { 0x0748, 0x0749 },
    { 0x074A, 0x074B },
    { 0x074C, 0x074D },
    { 0x074E, 0x074F },
    { 0x0750, 0x0751 },
    { 0x0752, 0x0753 },
    { 0x077D, 0x077E },
    { 0x077F, 0x0780 },
    { 0x0781, 0x0782 },
    { 0x0783, 0x0784 },
    { 0x0785, 0x0786 },
    { 0x078D, 0x078E },
    { 0x078F, 0x0790 },
    { 0x0791, 0x0792 },
    { 0x0793, 0x0794 },
    { 0x0795, 0x0796 },
    { 0x0797, 0x0798 },
    { 0x0799, 0x079A },
    { 0x079B, 0x079C },
    { 0x079D, 0x079E },
    { 0x079F, 0x07A0 },
    { 0x07A1, 0x07A2 },
    { 0x07A3, 0x07A4 },
    { 0x07B8, 0x07B9 },
    { 0x07BA, 0x07BB },
    { 0x07BC, 0x07F8 },
    { 0x07BD, 0x07F9 },
    { 0x07BE, 0x07FA },
    { 0x07BF, 0x07FB },
    { 0x07C1, 0x07FD },
    { 0x07C2, 0x07FE }
    };
    static const u32 kFoodPairCount = sizeof(kFoodPairs) / sizeof(kFoodPairs[0]);

    // 通常IDから極上IDを探す。無ければ 0。
    static inline u32 FindSuper(u32 normalId)
    {
        for (u32 i = 0; i < kFoodPairCount; i++)
            if (kFoodPairs[i][0] == normalId)
                return kFoodPairs[i][1];
        return 0;
    }
}

extern "C"
{
    void jp_hookQuality(void);
    void jp_hookComplete(void);

    // 0xFFFFFFFF = 固定しない
    volatile u32 g_forceQuality = 0xFFFFFFFF;
    // 0 = 通常 / 1 = 制作が始まって一定フレーム後に完了
    volatile u32 g_instantCraft = 0;
    // 制作が始まってから、完了させるまで待つフレーム数 (60フレーム = 約1秒)
    volatile u32 g_instantDelay = 8;

    // アイテム作成関数 0x3464B8 の入口で呼ばれる。req = 作成の依頼内容
    //   [req+0x00] できあがるID   [req+0x04] 元(通常)のID
    //   [req+0x10] 品質           [req+0x28] 極上フラグ(料理など)  [req+0x2C] 品質
    void QualityPrepare(u32 req)
    {
        const u32 q = g_forceQuality;
        if (q > 4)
            return;
        if (req < 0x00100000 || req > 0x20000000)   // 念のための範囲チェック
            return;

        volatile u32 *r = (volatile u32 *)req;
        const u32 baseId = r[1];

        // 料理・薬・爆弾: 通常ID と 極上ID を差し替える
        const u32 superId = QualityFixData::FindSuper(baseId);
        if (superId != 0)
        {
            if (q >= 3)
            {
                r[0]    = superId;   // 極上のID
                r[0x10 / 4] = 3;
                r[0x28 / 4] = 1;
                r[0x2C / 4] = 3;
            }
            else
            {
                r[0]    = baseId;    // 通常のID
                r[0x10 / 4] = 0;
                r[0x28 / 4] = 0;
                r[0x2C / 4] = 0;
            }
            return;
        }

        // それ以外 (武器・防具・ライフ道具など): 品質の値を書き換える
        r[0x10 / 4] = q;
        r[0x2C / 4] = q;
    }

    // 0x41F1A8 (制作ミニゲームの毎フレームに通る「完了したか」の判定) の前に呼ばれる。
    //   obj = r4+0x20 の工程データ。 [obj+0xA28] = 現在の工程番号 A
    //   ON のとき、制作が始まってから g_instantDelay フレーム後に A を 0x23 (最大) にして、
    //   完了の流れに入れる。(ARコードと同じ効果。ただし工程アイコンを描いたあとに書く)
    //   そのあと、元の判定関数 0x2F5C6C を呼んで結果を返す。
    u32 InstantCompletePrepare(u32 obj)
    {
        static u32  s_obj    = 0;
        static u32  s_frames = 0;
        static bool s_done   = false;

        if (g_instantCraft && obj >= 0x00100000 && obj < 0x20000000)
        {
            volatile u32 *pA = (volatile u32 *)(obj + 0xA28);

            if (obj != s_obj)                       // 別のオブジェクト = 新しい制作
            {
                s_obj    = obj;
                s_frames = 0;
                s_done   = false;
            }
            if (s_done && *pA != 0x23)              // 自分で書いた値が消えている = 新しい制作が始まった
            {
                s_frames = 0;
                s_done   = false;
            }

            if (!s_done)
            {
                if (++s_frames >= g_instantDelay)
                    s_done = true;
            }
            if (s_done)
                *pA = 0x23;
        }
        else
        {
            s_obj    = 0;
            s_frames = 0;
            s_done   = false;
        }

        typedef u32 (*CheckFn)(u32);
        return ((CheckFn)(uintptr_t)0x002F5C6C)(obj);
    }
}

// ---- 品質固定のフック (0x3464B8) ----
// 置き換えた2命令を再現し、0x3464C0 に戻る。
//   0x3464B8: push {r4-r12,lr}   (E92D5FF0)
//   0x3464BC: mov  r4, r1        (E1A04001)
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

// ---- 即完了のフック (0x41F1A8) ----
// 0x41F1A8: bl  0x2F5C6C   (EBFB5AAF)  ← ldr pc,[pc,#-4] に置き換え
// 0x41F1AC: cmp r0, #0     (E3500000)  ← ジャンプ先アドレスを置く
// 入ってくるとき r0 = r4+0x20。判定結果 r0 を返し、cmp を実行して 0x41F1B0 へ戻る。
// (0x41F1B0 以降は cmp の結果で条件実行されるので、cmp の直後にジャンプする)
asm(R"(
    .arm
    .text
    .align 2

    .global jp_hookComplete
    .type jp_hookComplete, %function
jp_hookComplete:
    push  {r1-r3, r12}           @ 16バイト
    vpush {d0-d7}                @ 64バイト (合計80 = 8の倍数)
    bl    InstantCompletePrepare @ r0 = obj を渡し、判定結果を r0 で受ける
    vpop  {d0-d7}
    pop   {r1-r3, r12}
    cmp   r0, #0                 @ 0x41F1AC の元命令
    ldr   pc, =0x41F1B0          @ 続きの元コードへ
    .ltorg
)");

namespace CTRPluginFramework
{
    // ------------------------------------------------------------
    //  品質固定
    // ------------------------------------------------------------
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
    // ------------------------------------------------------------
    static constexpr u32 HOOK_COMPLETE      = 0x0041F1A8;
    static constexpr u32 ORIG_COMPLETE_0    = 0xEBFB5AAF;   // bl 0x2F5C6C
    static constexpr u32 ORIG_COMPLETE_1    = 0xE3500000;   // cmp r0, #0
    static bool g_completeHooked = false;

    static bool InstallCompleteHook(void)
    {
        if (g_completeHooked)
            return true;

        u32 a = 0, b = 0;
        if (!Process::Read32(HOOK_COMPLETE, a) || !Process::Read32(HOOK_COMPLETE + 4, b)
            || a != ORIG_COMPLETE_0 || b != ORIG_COMPLETE_1)
        {
            MessageBox("エラー",
                       "即完了のフック位置の命令が想定と違います。\n"
                       "ゲームのバージョンが異なる可能性があるため、機能を有効にできません。")();
            return false;
        }

        Process::Pause();
        Process::Write32(HOOK_COMPLETE + 4, (u32)(uintptr_t)jp_hookComplete);
        Process::Write32(HOOK_COMPLETE,     0xE51FF004);   // ldr pc, [pc, #-4]
        Process::Play();

        g_completeHooked = true;
        return true;
    }

    static void InstantCraftEntry(MenuEntry *entry)
    {
        (void)entry;

        if (!InstallCompleteHook())
            return;

        std::vector<std::string> names;
        names.push_back("OFF (通常)");
        names.push_back("ON (制作が始まって少し後に完了)");

        Keyboard kb("制作の即完了", names);
        int sel = kb.Open();
        if (sel < 0 || sel > 1)
            return;

        if (sel == 1)
        {
            // 完了させるまで待つフレーム数 (60 = 約1秒)。工程アイコンが描かれたあとに書くための待ち時間
            u32 frames = g_instantDelay;
            Keyboard kb2("待つフレーム数 (60=約1秒、既定8)");
            kb2.IsHexadecimal(false);
            if (kb2.Open(frames, frames) == 0 && frames <= 600)
                g_instantDelay = frames;
        }

        g_instantCraft = (u32)sel;
        OSD::Notify(sel == 1 ? "Instant craft: ON" : "Instant craft: OFF");
    }

    static void AddQualityFix(PluginMenu *menu)
    {
        menu->Append(new MenuEntry("制作を即完了", nullptr, InstantCraftEntry,
                                   "制作ミニゲームを、始まって少し後に完了させます。\n"
                                   "工程アイコンが出てから書き込むので、表示は崩れません。\n"
                                   "制作の前にONにして、終わったらOFFに戻してください。\n"
                                   "実機未検証です。必ずセーブのバックアップを取ってから使ってください。"));

        menu->Append(new MenuEntry("制作の品質を固定", nullptr, QualityFixEntry,
                                   "制作で作られるアイテムの品質を、選んだ値に固定します。\n"
                                   "(0=通常 1=上 2=特上 3=最高 4=ゴッド)\n"
                                   "料理・薬・爆弾は、3以上なら極上、2以下なら通常になります。\n"
                                   "アイテムを作る処理全般に効くので、制作の前に選び、終わったらOFFに戻してください。\n"
                                   "実機未検証です。必ずセーブのバックアップを取ってから使ってください。"));
    }
}
