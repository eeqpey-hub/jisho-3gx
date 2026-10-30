// 制作の素材を0個にする (実験) - ファンタジーライフ 日本語版
//
// 素材画面を作るコード (0x56196C) は、アイテム表の1件から、素材の個数を
//   0x561990: ldrb r0,[r6,#0x19]   (素材1の個数)
//   0x5619A0: ldrb r0,[r6,#0x1A]   (素材2の個数)
//   0x5619B0: ldrb r0,[r6,#0x1B]   (素材3の個数)
// で読み出して、スタックに置いてから、画面表示や所持数との比較に使う。
// この3命令を「mov r0,#0」に差し替えて、個数を0として扱わせる。
//
// アイテム表のデータそのものは書き換えない
// (データを0にすると、個数0の素材の行が画面から消え、素材が空のレシピになって固まるため)。
//
// 使い方 (main.cpp に2行足す):
//   1) #include "MaterialZero.hpp"
//   2) main() の AddLifeChange(menu); の前に   AddMaterialZero(menu);
//
// 実機未検証。素材を消費する処理が別の場所で個数を読んでいる場合は、
// 持っていない素材で制作すると在庫の数が壊れる可能性がある。
// 必ずセーブのバックアップを取り、安いレシピで試すこと。

#pragma once

#include <3ds.h>
#include <CTRPluginFramework.hpp>
#include <string>
#include <vector>

namespace CTRPluginFramework
{
    static const u32 kMatSite[3] = { 0x00561990, 0x005619A0, 0x005619B0 };
    static const u32 kMatOrig[3] = { 0xE5D60019, 0xE5D6001A, 0xE5D6001B };   // ldrb r0,[r6,#0x19/0x1A/0x1B]
    static const u32 kMovR0Zero  = 0xE3A00000;                               // mov r0,#0

    static void MaterialZeroEntry(MenuEntry *entry)
    {
        (void)entry;

        // 位置の命令が想定どおりか確認 (元のまま、または自分が差し替えたあとの状態のみ許可)
        bool allOrig = true, allPatched = true;
        for (int i = 0; i < 3; i++)
        {
            u32 v = 0;
            if (!Process::Read32(kMatSite[i], v))
            {
                MessageBox("エラー", "素材の個数を読む位置を読み取れませんでした。")();
                return;
            }
            if (v != kMatOrig[i]) allOrig = false;
            if (v != kMovR0Zero)  allPatched = false;
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

        Keyboard kb("制作の素材を0個にする (実験)", names);
        int sel = kb.Open();
        if (sel < 0 || sel > 1)
            return;

        Process::Pause();
        for (int i = 0; i < 3; i++)
            Process::Write32(kMatSite[i], sel == 0 ? kMovR0Zero : kMatOrig[i]);
        Process::Play();

        OSD::Notify(sel == 0 ? "Material zero: ON" : "Material zero: OFF");
    }

    static void AddMaterialZero(PluginMenu *menu)
    {
        menu->Append(new MenuEntry("制作の素材を0個にする (実験)", nullptr, MaterialZeroEntry,
                                   "制作の素材の必要個数を0として読ませます。\n"
                                   "表示が「×0コ」になり、持っていない素材でも作れる想定です。\n"
                                   "素材を消費する処理が別の場所で個数を読んでいる場合、在庫の数が壊れることがあります。\n"
                                   "実験機能です。必ずセーブのバックアップを取り、安いレシピで試してください。"));
    }
}
