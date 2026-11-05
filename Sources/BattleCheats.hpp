#pragma once
// =====================================================================
//  BattleCheats.hpp  (CTRPluginFramework / 3GX)
//  対象: FLLINK (code.bin の 0x00100000 ベースのアドレス)
//
//  収録:
//    1. 固定ダメージ(プレイヤーのみ)  値 0〜2147483647 (デフォルト 0x7FFFFFFF)
//       - 値が 10000 以上のときは「ダメージ表示9999固定」を自動で併用
//    2. 近づいたらダメージ判定(倍率は等倍固定・範囲指定可)
//
//  使い方 (main.cpp など):
//      #include "BattleCheats.hpp"
//      ...
//      BattleCheats::InitMenu(menu);   // PluginMenu &menu
//
//  ※未検証のコードです。コード領域への書き込みは Process::Patch を使います。
//    Process::Patch(u32, u32) がビルドエラーになる場合は、Patch32() の中身を
//    Process::Write32(addr, v) に置き換えてください。
//
//  使用する空きアドレス(ゲーム側に何も入っていない領域)
//    0x0061F950-0x0061F9FF / 0x0061FAC0-0x0061FAF3 / 0x0061FB80-0x0061FBEB
//    0x0061FD00-0x0061FD5B / 0x0061FEC8-0x0061FED3 / 0x0061FF40-0x0061FFD7
//  フック書き換え先
//    0x001A6404 / 0x001F9AFC-0x001F9B03 / 0x003F4280-0x003F4287
// =====================================================================

#include <CTRPluginFramework.hpp>
#include <cstring>
#include <cstddef>

namespace BattleCheats
{
    using namespace CTRPluginFramework;

    struct Word
    {
        u32 addr;
        u32 val;
    };

    // ---------------------------------------------------------------
    //  共通
    // ---------------------------------------------------------------
    static inline bool Patch32(u32 addr, u32 v)
    {
        return Process::Patch(addr, v);
    }

    template <size_t N>
    static inline void WriteList(const Word (&list)[N])
    {
        for (size_t i = 0; i < N; ++i)
            Patch32(list[i].addr, list[i].val);
    }

    // 元の命令(code.bin より)
    static const u32 ORG_DMG_80 = 0xEE000A10; // 0x003F4280  vmov s0, r0
    static const u32 ORG_DMG_84 = 0xEDDFBAA3; // 0x003F4284  vldr s23, [pc, #...]
    static const u32 ORG_DSP_FC = 0xE1A08001; // 0x001F9AFC  mov r8, r1
    static const u32 ORG_DSP_00 = 0xE1A07002; // 0x001F9B00  mov r7, r2
    static const u32 ORG_PRX_04 = 0xED2D8B02; // 0x001A6404  vpush {d8}

    // ---------------------------------------------------------------
    //  1. 固定ダメージ (プレイヤーのみ)
    // ---------------------------------------------------------------
    static const u32 FIX_VALUE_ADDR = 0x0061FD54;

    static const Word FIX_CAVE[] =
    {
        {0x0061FD00, 0xE92D400F}, {0x0061FD04, 0xE3E01063}, {0x0061FD08, 0xEBF90316},
        {0x0061FD0C, 0xE1A0C000}, {0x0061FD10, 0xE8BD400F}, {0x0061FD14, 0xE35C0000},
        {0x0061FD18, 0x0A000009}, {0x0061FD1C, 0xE155000C}, {0x0061FD20, 0x1A000007},
        {0x0061FD24, 0xE5D6C554}, {0x0061FD28, 0xE35C0001}, {0x0061FD2C, 0x1A000004},
        {0x0061FD30, 0xE5D6C3BC}, {0x0061FD34, 0xE35C0002}, {0x0061FD38, 0x135C000E},
        {0x0061FD3C, 0x1A000000}, {0x0061FD40, 0xE59F000C}, {0x0061FD44, 0xEE000A10},
        {0x0061FD48, 0xEDDFBA02}, {0x0061FD4C, 0xE51FF004}, {0x0061FD50, 0x003F4288},
        /* 0x0061FD54 は固定ダメージ値(実行時に書き込み) */
        {0x0061FD58, 0x3F000000},
    };

    // ダメージ表示9999固定 (10000以上のときだけ自動で有効)
    static const Word DSP_CAVE[] =
    {
        {0x0061FAC0, 0xE1A08001}, {0x0061FAC4, 0xE1A07002}, {0x0061FAC8, 0xE92D400F},
        {0x0061FACC, 0xE3E01063}, {0x0061FAD0, 0xEBF903A4}, {0x0061FAD4, 0xE1A0C000},
        {0x0061FAD8, 0xE8BD400F}, {0x0061FADC, 0xE155000C}, {0x0061FAE0, 0x13580000},
        {0x0061FAE4, 0x13A08C27}, {0x0061FAE8, 0x1288800F}, {0x0061FAEC, 0xE51FF004},
        {0x0061FAF0, 0x001F9B04},
    };

    static const u32 DISPLAY_LIMIT = 10000; // これ以上は表示9999固定を併用

    static u32 &FixedDamageValue()
    {
        static u32 v = 0x7FFFFFFF;
        return v;
    }

    static void FixedDamageSettings(MenuEntry *entry)
    {
        (void)entry;
        u32 v = FixedDamageValue();
        Keyboard kb("固定ダメージ値 (0~2147483647)\n"
                    "10000以上は表示9999固定が自動で併用されます");
        kb.IsHexadecimal(false);
        if (kb.Open(v, v) == 0)
        {
            if (v > 0x7FFFFFFF)
                v = 0x7FFFFFFF;
            FixedDamageValue() = v;
        }
    }

    static void FixedDamageGame(MenuEntry *entry)
    {
        static bool fixApplied = false;
        static bool dspApplied = false;
        static u32  lastValue = 0;

        if (!entry->IsActivated())
        {
            // OFF: 元に戻す
            if (dspApplied)
            {
                Patch32(0x001F9AFC, ORG_DSP_FC);
                Patch32(0x001F9B00, ORG_DSP_00);
                dspApplied = false;
            }
            if (fixApplied)
            {
                Patch32(0x003F4280, ORG_DMG_80);
                Patch32(0x003F4284, ORG_DMG_84);
                fixApplied = false;
            }
            return;
        }

        const u32 value = FixedDamageValue();

        // 固定ダメージ本体 (コードの穴 → 最後にフック)
        if (!fixApplied)
        {
            WriteList(FIX_CAVE);
            Patch32(FIX_VALUE_ADDR, value);
            lastValue = value;
            Patch32(0x003F4280, 0xE51FF004);
            Patch32(0x003F4284, 0x0061FD00);
            fixApplied = true;
        }
        else if (value != lastValue)
        {
            Patch32(FIX_VALUE_ADDR, value);
            lastValue = value;
        }

        // ダメージ表示9999固定 (値に応じて自動 ON/OFF)
        if (value >= DISPLAY_LIMIT)
        {
            if (!dspApplied)
            {
                WriteList(DSP_CAVE);
                Patch32(0x001F9AFC, 0xE51FF004);
                Patch32(0x001F9B00, 0x0061FAC0);
                dspApplied = true;
            }
        }
        else if (dspApplied)
        {
            Patch32(0x001F9AFC, ORG_DSP_FC);
            Patch32(0x001F9B00, ORG_DSP_00);
            dspApplied = false;
        }
    }

    // ---------------------------------------------------------------
    //  2. 近づいたらダメージ判定 (倍率は等倍固定・範囲指定可)
    //     ダメージは通常の攻撃計算を通るので、固定ダメージと併用すると
    //     近くの敵に固定ダメージが入ります。
    // ---------------------------------------------------------------
    static const u32 PRX_RANGE_ADDR = 0x0061F9FC; // 距離の2乗(float)

    static const Word PRX_CAVE[] =
    {
        {0x0061F950, 0xE92D50FF}, {0x0061F954, 0xED2D0B08}, {0x0061F958, 0xE3E01063},
        {0x0061F95C, 0xEBF90401}, {0x0061F960, 0xE1A04000}, {0x0061F964, 0xE1540005},
        {0x0061F968, 0x0A000193}, {0x0061F96C, 0xE5D50554}, {0x0061F970, 0xE3500001},
        {0x0061F974, 0x1A000190}, {0x0061F978, 0xE5D503BC}, {0x0061F97C, 0xE3500002},
        {0x0061F980, 0x1350000E}, {0x0061F984, 0x1A00018C}, {0x0061F988, 0xE28500D4},
        {0x0061F98C, 0xEBFE4016}, {0x0061F990, 0xE3500000}, {0x0061F994, 0x1A000188},
        {0x0061F998, 0xE1A00005}, {0x0061F99C, 0xE5901000}, {0x0061F9A0, 0xE5911018},
        {0x0061F9A4, 0xE12FFF31}, {0x0061F9A8, 0xED900A00}, {0x0061F9AC, 0xEDD00A01},
        {0x0061F9B0, 0xED901A02}, {0x0061F9B4, 0xE1A00004}, {0x0061F9B8, 0xE5901000},
        {0x0061F9BC, 0xE5911018}, {0x0061F9C0, 0xE12FFF31}, {0x0061F9C4, 0xEDD01A00},
        {0x0061F9C8, 0xED902A01}, {0x0061F9CC, 0xEDD02A02}, {0x0061F9D0, 0xEE300A61},
        {0x0061F9D4, 0xEE700AC2}, {0x0061F9D8, 0xEE311A62}, {0x0061F9DC, 0xEE200A00},
        {0x0061F9E0, 0xEE000AA0}, {0x0061F9E4, 0xEE010A01}, {0x0061F9E8, 0xEDDF0A03},
        {0x0061F9EC, 0xEEB40AE0}, {0x0061F9F0, 0xEEF1FA10}, {0x0061F9F4, 0xCA000170},
        {0x0061F9F8, 0xEA000132},
        /* 0x0061F9FC は範囲(実行時に書き込み) */
        {0x0061FB80, 0xE59F1060}, {0x0061FB84, 0xE5911000}, {0x0061FB88, 0xE2811004},
        {0x0061FB8C, 0xE2811C78}, {0x0061FB90, 0xE2811F5B}, {0x0061FB94, 0xE591201C},
        {0x0061FB98, 0xE3520000}, {0x0061FB9C, 0x03A00000}, {0x0061FBA0, 0x012FFF1E},
        {0x0061FBA4, 0xE5913020}, {0x0061FBA8, 0xE2822010}, {0x0061FBAC, 0xE3530000},
        {0x0061FBB0, 0x03A00000}, {0x0061FBB4, 0x012FFF1E}, {0x0061FBB8, 0xE592C000},
        {0x0061FBBC, 0xE35C0000}, {0x0061FBC0, 0x0A000005}, {0x0061FBC4, 0xE59C1008},
        {0x0061FBC8, 0xE1510000}, {0x0061FBCC, 0x059C0004}, {0x0061FBD0, 0x012FFF1E},
        {0x0061FBD4, 0xE59CC010}, {0x0061FBD8, 0xEAFFFFF7}, {0x0061FBDC, 0xE2822014},
        {0x0061FBE0, 0xE2433001}, {0x0061FBE4, 0xEAFFFFF0}, {0x0061FBE8, 0x0068BBB8},
        {0x0061FEC8, 0xE5940544}, {0x0061FECC, 0xE5850548}, {0x0061FED0, 0xEA00001A},
        {0x0061FF40, 0xE1A00005}, {0x0061FF44, 0xEBFFFF0D}, {0x0061FF48, 0xE1A06000},
        {0x0061FF4C, 0xE1A00004}, {0x0061FF50, 0xEBFFFF0A}, {0x0061FF54, 0xE1A07000},
        {0x0061FF58, 0xE3560000}, {0x0061FF5C, 0x13570000}, {0x0061FF60, 0x0A000015},
        {0x0061FF64, 0xE24DDF44}, {0x0061FF68, 0xE3A00000}, {0x0061FF6C, 0xE1A0100D},
        {0x0061FF70, 0xE3A02044}, {0x0061FF74, 0xE4810004}, {0x0061FF78, 0xE2522001},
        {0x0061FF7C, 0x1AFFFFFC}, {0x0061FF80, 0xE58D6008}, {0x0061FF84, 0xE58D7058},
        {0x0061FF88, 0xE28D00F0}, {0x0061FF8C, 0xE58D0070}, {0x0061FF90, 0xE59F0034},
        {0x0061FF94, 0xE58D00BC}, {0x0061FF98, 0xED9F0A0C}, {0x0061FF9C, 0xEDDF0A0C},
        {0x0061FFA0, 0xEE301A40}, {0x0061FFA4, 0xE1A0000D}, {0x0061FFA8, 0xE28D1050},
        {0x0061FFAC, 0xE3E02000}, {0x0061FFB0, 0xE3A03001}, {0x0061FFB4, 0xEBF75069},
        {0x0061FFB8, 0xE28DDF44}, {0x0061FFBC, 0xECBD0B08}, {0x0061FFC0, 0xE8BD50FF},
        {0x0061FFC4, 0xED2D8B02}, {0x0061FFC8, 0xEAEE190E}, {0x0061FFCC, 0x47C35000},
        {0x0061FFD0, 0x3F800000}, /* 倍率 1.0 (等倍固定) */
        {0x0061FFD4, 0x3F800000},
    };

    // 範囲 (距離の整数値)。内部では距離の2乗(float)を書き込む。
    // 既定値 4 → 4*4 = 16.0 (元コードの 0x41800000 と同じ)
    static u32 &ProximityRadius()
    {
        static u32 r = 4;
        return r;
    }

    static u32 RadiusToBits(u32 r)
    {
        const float sq = static_cast<float>(r) * static_cast<float>(r);
        u32 bits = 0;
        std::memcpy(&bits, &sq, sizeof(bits));
        return bits;
    }

    static void ProximitySettings(MenuEntry *entry)
    {
        (void)entry;
        u32 r = ProximityRadius();
        Keyboard kb("近づいたら判定の範囲 (1~200)\n既定値は 4 です");
        kb.IsHexadecimal(false);
        if (kb.Open(r, r) == 0)
        {
            if (r < 1)
                r = 1;
            if (r > 200)
                r = 200;
            ProximityRadius() = r;
        }
    }

    static void ProximityGame(MenuEntry *entry)
    {
        static bool applied = false;
        static u32  lastRadius = 0;

        if (!entry->IsActivated())
        {
            if (applied)
            {
                Patch32(0x001A6404, ORG_PRX_04);
                applied = false;
            }
            return;
        }

        const u32 r = ProximityRadius();

        if (!applied)
        {
            WriteList(PRX_CAVE);
            Patch32(PRX_RANGE_ADDR, RadiusToBits(r));
            lastRadius = r;
            Patch32(0x001A6404, 0xEA11E551); // フックは最後に書く
            applied = true;
        }
        else if (r != lastRadius)
        {
            Patch32(PRX_RANGE_ADDR, RadiusToBits(r));
            lastRadius = r;
        }
    }

    // ---------------------------------------------------------------
    //  メニュー登録
    // ---------------------------------------------------------------
    static void InitMenu(PluginMenu &menu)
    {
        MenuFolder *folder = new MenuFolder("バトル");

        folder->Append(new MenuEntry(
            "固定ダメージ(プレイヤー)", FixedDamageGame, FixedDamageSettings,
            "プレイヤーが敵に与えるダメージを固定します。\n"
            "設定(右の歯車)で 0~2147483647 を指定できます。\n"
            "10000以上は、表示9999固定が自動で併用されます。"));

        folder->Append(new MenuEntry(
            "近づいたらダメージ判定", ProximityGame, ProximitySettings,
            "近くの敵にダメージ判定を出します(倍率は等倍)。\n"
            "固定ダメージと併用すると、近くの敵に固定ダメージが入ります。\n"
            "設定で範囲を 1~200 で指定できます。"));

        menu += folder;
    }

    // menu がポインタ(PluginMenu *menu)の場合はこちらが呼ばれる
    static void InitMenu(PluginMenu *menu)
    {
        InitMenu(*menu);
    }

} // namespace BattleCheats
