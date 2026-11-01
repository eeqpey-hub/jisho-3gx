// 制作の素材を0個にする - ファンタジーライフ 日本語版
//
// 素材の必要個数を読む命令を「0を代入する命令」に差し替える。
//
//  [素材がそろっているか]  0x2FC1C4 の関数 (3つの素材を調べ、全部持っていれば1を返す)
//     0x2FC224: ldrb r7,[r0,#0x19]   必要個数を読む (持っている数 r9 と比べる)       → mov r7,#0
//       → 一覧の「そざい不足」と、「作る」を押したときの「素材が足りません」の元
//  [素材の選択]            0x301244 の関数 (3つの素材の枠をループして確認し、制作用の領域へ書き写す)
//     0x301478: ldrb r8,[r0,#0x19]   必要個数を読み、持っている数(r11)と比べる      → mov r8,#0
//     0x3015E0: ldrb r2,[r0,#0x19]   必要個数を読み直し、制作用の領域へ書き写す      → mov r2,#0
//       → 素材を減らす個数、ミニゲーム中の「×0コ」の元
//  [下画面の素材の欄]      0x3029C8 の関数 (3つの素材の ID と個数を配列へ書き写す)
//     0x302C80: ldrb r2,[r0,#0x19]   必要個数を配列へ書き写す                         → mov r2,#0
//
// (0x19 に枠の番号を足した位置を読むので、3枠とも同じ命令で足りる)
//
// アイテム表(レシピ表)のデータそのものは書き換えない
// (データを0にすると、個数0の素材の行が画面から消え、固まるため)。
// 上画面の「ひつような素材」の個数は、0にすると空欄になるため、書き換えない
// (実際の個数のまま表示される)。
//
// 使い方 (main.cpp に2行足す):
//   1) #include "MaterialZero.hpp"
//   2) main() の AddLifeChange(menu); の前に   AddMaterialZero(menu);
//
// 実機未検証。必ずセーブのバックアップを取り、安いレシピで試すこと。

#pragma once

#include <3ds.h>
#include <CTRPluginFramework.hpp>
#include <string>
#include <vector>

namespace CTRPluginFramework
{
    static constexpr int kMatCount = 4;
    static const u32 kMatSite[kMatCount] = { 0x002FC224, 0x00301478, 0x003015E0, 0x00302C80 };
    static const u32 kMatOrig[kMatCount] = { 0xE5D07019, 0xE5D08019, 0xE5D02019, 0xE5D02019 };
    static const u32 kMatNew [kMatCount] = { 0xE3A07000, 0xE3A08000, 0xE3A02000, 0xE3A02000 };
    // 0xE3A07000 = mov r7,#0 / 0xE3A08000 = mov r8,#0 / 0xE3A02000 = mov r2,#0

    static void MaterialZeroEntry(MenuEntry *entry)
    {
        (void)entry;

        // 位置の命令が想定どおりか確認 (元のまま、または自分が差し替えたあとの状態のみ許可)
        bool allOrig = true, allPatched = true;
        for (int i = 0; i < kMatCount; i++)
        {
            u32 v = 0;
            if (!Process::Read32(kMatSite[i], v))
            {
                MessageBox("エラー", "素材の個数を読む位置を読み取れませんでした。")();
                return;
            }
            if (v != kMatOrig[i]) allOrig = false;
            if (v != kMatNew[i])  allPatched = false;
        }
        if (!allOrig && !allPatched)
        {
            MessageBox("エラー",
                       "素材の個数を読む位置の命令が想定と違います。\n"
                       "ゲームのバージョンが異なる可能性があるため、機能を有効にできません。")();
            return;
        }

        std::vector<std::string> names;
        names.push_back("ON (素材を0個にする)");
        names.push_back("OFF (通常)");

        Keyboard kb("制作の素材を0個にする", names);
        int sel = kb.Open();
        if (sel < 0 || sel > 1)
            return;

        Process::Pause();
        for (int i = 0; i < kMatCount; i++)
            Process::Write32(kMatSite[i], sel == 0 ? kMatNew[i] : kMatOrig[i]);
        Process::Play();

        OSD::Notify(sel == 0 ? "Material zero: ON" : "Material zero: OFF");
    }

    // PluginMenu* でも MenuFolder* でも渡せる
    template <typename TMenu>
    static void AddMaterialZero(TMenu *menu)
    {
        menu->Append(new MenuEntry("制作の素材を0個にする", nullptr, MaterialZeroEntry,
                                   "制作に必要な素材の個数をゼロとして扱います。\n"
                                   "素材を持っていなくても制作でき、素材は減りません。\n"
                                   "制作の前にONにして、終わったらOFFに戻してください。\n"
                                   "必ずバックアップを取ってから使ってください。"));
    }
}
