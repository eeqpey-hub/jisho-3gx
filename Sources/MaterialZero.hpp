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
//  [使う素材の確定]        0x301F38 の関数 (3つの素材の枠を順に、持っている数と比べて確定させる)
//     0x302024: ldrb r6,[r0,#0x19]   必要個数を読む。持っている数(r9)が足りないと枠が空のまま  → mvn r6,#0 (-1)
//       → 必要個数を-1にして、「0ではない」「持っている数 >= -1」を満たす。
//         素材を持っていないとき、この枠が空欄(ハイフン)になって、制作が固まっていた原因
//  [画面の個数表示]  (選べる。メニューで「表示も0コにする」)
//     0x302C80 (0x3029C8 の関数): 下画面の素材の個数        → 「×0コ」になる想定
//     0x561990 / 0x5619A0 / 0x5619B0 (0x56196C の関数): 上画面の素材の個数        → 0
//     0x561BEC: beq 0x561CAC  (個数0なら数字を出さずに隠す分岐)                   → nop で無効にする
//                                                          → 上画面が「×0コ」になる想定
//
// (0x19 に枠の番号を足した位置を読むので、3枠とも同じ命令で足りる)
//
// アイテム表(レシピ表)のデータそのものは書き換えない
// (データを0にすると、個数0の素材の行が画面から消え、固まるため)。
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
    // core = 1: 判定・消費の差し替え (常に必要)
    // core = 0: 画面の個数表示の差し替え (選べる)
    struct MatPatch { u32 addr; u32 orig; u32 patched; int core; };
    static const MatPatch kMat[] =
    {
        { 0x002FC224, 0xE5D07019, 0xE3A07000, 1 },   // ldrb r7,[r0,#0x19] -> mov r7,#0   素材がそろっているかの判定
        { 0x00301478, 0xE5D08019, 0xE3A08000, 1 },   // ldrb r8,[r0,#0x19] -> mov r8,#0   素材の選択(持っている数との比較)
        { 0x003015E0, 0xE5D02019, 0xE3A02000, 1 },   // ldrb r2,[r0,#0x19] -> mov r2,#0   素材を減らす個数 (ミニゲーム中の表示)
        { 0x00302024, 0xE5D06019, 0xE3E06000, 1 },   // ldrb r6,[r0,#0x19] -> mvn r6,#0   使う素材の確定 (持っていなくても枠を埋める)
        { 0x00302C80, 0xE5D02019, 0xE3A02000, 0 },   // 下画面「しようする素材」の個数 (確認画面)
        { 0x00561990, 0xE5D60019, 0xE3A00000, 0 },   // 上画面「ひつような素材」の個数 (0にすると空欄になる)
        { 0x005619A0, 0xE5D6001A, 0xE3A00000, 0 },
        { 0x005619B0, 0xE5D6001B, 0xE3A00000, 0 },
        { 0x00561BEC, 0x0A00002E, 0xE1A00000, 0 },   // beq 0x561CAC -> nop  個数0のときに数字を隠す分岐を無効にする (上画面を「×0コ」にする)
    };
    static constexpr int kMatCount = (int)(sizeof(kMat) / sizeof(kMat[0]));

    static void MaterialZeroEntry(MenuEntry *entry)
    {
        (void)entry;

        // 位置の命令が想定どおりか確認 (元のまま、または自分が差し替えたあとの状態のみ許可)
        for (int i = 0; i < kMatCount; i++)
        {
            u32 v = 0;
            if (!Process::Read32(kMat[i].addr, v))
            {
                MessageBox("エラー", "素材の個数を読む位置を読み取れませんでした。")();
                return;
            }
            if (v != kMat[i].orig && v != kMat[i].patched)
            {
                MessageBox("エラー",
                           "素材の個数を読む位置の命令が想定と違います。\n"
                           "ゲームのバージョンが異なる可能性があるため、機能を有効にできません。")();
                return;
            }
        }

        std::vector<std::string> names;
        names.push_back("ON (表示は元の個数のまま)");
        names.push_back("ON (表示も0コにする)");
        names.push_back("OFF (通常)");

        Keyboard kb("制作の素材を0個にする", names);
        int sel = kb.Open();
        if (sel < 0 || sel > 2)
            return;

        Process::Pause();
        for (int i = 0; i < kMatCount; i++)
        {
            bool on = false;
            if (sel == 0) on = (kMat[i].core != 0);
            if (sel == 1) on = true;
            Process::Write32(kMat[i].addr, on ? kMat[i].patched : kMat[i].orig);
        }
        Process::Play();

        OSD::Notify(sel == 2 ? "Material zero: OFF" : (sel == 1 ? "Material zero: ON (display 0)" : "Material zero: ON"));
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
