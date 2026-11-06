#pragma once
// =====================================================================
//  BattleCheats.hpp  (CTRPluginFramework / 3GX)
//  対象: FLLINK (code.bin の 0x00100000 ベースのアドレス)
//
//  ★ゲーム側の「空き番地」は一切使いません。★
//    割り込み処理(アセンブリ)はプラグイン自身のコード領域に置き、
//    ゲーム側は下記の「フック位置」の2命令だけを書き換えます。
//
//  フック位置 (ほかのコードが同じ場所を書き換えていると使えません)
//    固定ダメージ        : 0x003F4280, 0x003F4284
//    ダメージ表示9999    : 0x001F9AFC, 0x001F9B00
//    近づいたら判定      : 0x001A6404, 0x001A6408
//  フック位置が他のコードに使われている場合は、書き込まずに通知して OFF にします。
//
//  収録:
//    1. 固定ダメージ(プレイヤーのみ) 値 0〜2147483647 (既定 0x7FFFFFFF)
//       - 値が 10000 以上のときは、ダメージ表示9999固定を自動で併用
//    2. 近づいたらダメージ判定 (倍率は等倍固定・範囲指定可)
//    3. HP減らない / SP減らない (プレイヤー)
//    4. テンションゲージ (自動 / 段階1固定 / 段階2固定)
//
//  使い方 (main.cpp など):
//      #include "BattleCheats.hpp"
//      BattleCheats::InitMenu(menu);   // PluginMenu * でも & でも可
//
//  ※未検証のコードです。
//    - コード領域への書き込みは Process::Patch(u32, u32) を使います。
//      ビルドエラーなら Patch32() の中身を Process::Write32(addr, v) に変えてください。
//    - アセンブリ部分は ARM モード(devkitARM 既定)前提です。
// =====================================================================

#include <CTRPluginFramework.hpp>
#include <cstring>
#include <cstddef>
#include <string>
#include <vector>

namespace BattleCheats
{
    using namespace CTRPluginFramework;

    // ---------------------------------------------------------------
    //  共通
    // ---------------------------------------------------------------
    static inline bool Patch32(u32 addr, u32 v)
    {
        return Process::Patch(addr, v);
    }

    static const u32 LDR_PC_NEXT = 0xE51FF004; // ldr pc, [pc, #-4] (次の1語が飛び先)

    // 元の命令 (code.bin より)
    static const u32 ORG_DMG_80 = 0xEE000A10; // 0x003F4280  vmov s0, r0
    static const u32 ORG_DMG_84 = 0xEDDFBAA3; // 0x003F4284  vldr s23, [pc, #...]
    static const u32 ORG_DSP_FC = 0xE1A08001; // 0x001F9AFC  mov r8, r1
    static const u32 ORG_DSP_00 = 0xE1A07002; // 0x001F9B00  mov r7, r2
    static const u32 ORG_PRX_04 = 0xED2D8B02; // 0x001A6404  vpush {d8}
    static const u32 ORG_PRX_08 = 0xEEB08A40; // 0x001A6408  vmov.f32 s16, s0

    static inline u32 FnAddr(void (*f)())
    {
        return reinterpret_cast<u32>(f);
    }

    // フック位置が「元のまま」または「自分のフック」なら true
    static bool HookIsFree(u32 addr, u32 org0, u32 org1, u32 fn)
    {
        u32 a = 0, b = 0;
        if (!Process::Read32(addr, a) || !Process::Read32(addr + 4, b))
            return false;
        if (a == org0 && b == org1)
            return true;
        if (a == LDR_PC_NEXT && b == fn)
            return true;
        return false;
    }

    static void InstallHook(u32 addr, u32 fn)
    {
        Patch32(addr, LDR_PC_NEXT);
        Patch32(addr + 4, fn);
    }

    static void RestoreHook(u32 addr, u32 org0, u32 org1)
    {
        Patch32(addr, org0);
        Patch32(addr + 4, org1);
    }

    // ---------------------------------------------------------------
    //  アセンブリから読む設定値 (asm からシンボル名で参照する)
    // ---------------------------------------------------------------
    static volatile u32 g_fixedDamage __asm__("BC_FixedDamage") __attribute__((used)) = 0x7FFFFFFF;
    static volatile u32 g_proxRange   __asm__("BC_ProxRange")   __attribute__((used)) = 0x41800000; // 距離の2乗(float)

    // ---------------------------------------------------------------
    //  フック本体 (プラグイン内のアセンブリ)
    //  プレイヤー = *( *(0x0068BBB8) + 0x7828 )   (ゲームの 0x00460968 と同じ手順)
    // ---------------------------------------------------------------

    // 0x003F4280 から飛んでくる。
    //   r0 = ダメージ, r5 = 攻撃した側, r6 = 受けた側
    //   プレイヤーが攻撃し、受けた側が敵のとき、ダメージを固定値にする。
    static void __attribute__((naked, used, noinline)) FixedDamageHook()
    {
        asm volatile(
            ".arm\n"
            "ldr   r12, =0x0068BBB8\n"
            "ldr   r12, [r12]\n"
            "cmp   r12, #0\n"
            "beq   .Lfd_end\n"
            "add   r12, r12, #0x7800\n"
            "ldr   r12, [r12, #0x28]\n"
            "cmp   r12, #0\n"
            "beq   .Lfd_end\n"
            "cmp   r5, r12\n"            // 攻撃した側がプレイヤー?
            "bne   .Lfd_end\n"
            "ldrb  r12, [r6, #0x554]\n"  // 受けた側が敵?
            "cmp   r12, #1\n"
            "bne   .Lfd_end\n"
            "ldrb  r12, [r6, #0x3BC]\n"
            "cmp   r12, #2\n"
            "cmpne r12, #0xE\n"
            "bne   .Lfd_end\n"
            "ldr   r0, =BC_FixedDamage\n"
            "ldr   r0, [r0]\n"
            ".Lfd_end:\n"
            "vmov  s0, r0\n"             // 元の命令 (0x3F4280)
            "vldr  s23, .Lfd_half\n"     // 元の命令 (0x3F4284)
            "ldr   pc, =0x003F4288\n"
            ".ltorg\n"
            ".align 2\n"
            ".Lfd_half: .float 0.5\n"
        );
    }

    // 0x001F9AFC から飛んでくる。
    //   r1 = ダメージ(表示用), r5 = 受けた側
    //   受けた側がプレイヤー以外で、ダメージが0でなければ 9999 にする。
    static void __attribute__((naked, used, noinline)) DisplayHook()
    {
        asm volatile(
            ".arm\n"
            "mov   r8, r1\n"             // 元の命令 (0x1F9AFC)
            "mov   r7, r2\n"             // 元の命令 (0x1F9B00)
            "ldr   r12, =0x0068BBB8\n"
            "ldr   r12, [r12]\n"
            "cmp   r12, #0\n"
            "addne r12, r12, #0x7800\n"
            "ldrne r12, [r12, #0x28]\n"
            "cmp   r5, r12\n"
            "cmpne r8, #0\n"
            "movne r8, #0x2700\n"
            "addne r8, r8, #0xF\n"       // 0x270F = 9999
            "ldr   pc, =0x001F9B04\n"
            ".ltorg\n"
        );
    }

    // 0x001A6404 から飛んでくる (敵ごとの更新処理の入口)。
    //   r5 = 敵。プレイヤーに近い敵に対して、ダメージ処理(0x003F4160)を呼ぶ。
    //   倍率は 1.0 (等倍)。
    static void __attribute__((naked, used, noinline)) ProximityHook()
    {
        asm volatile(
            ".arm\n"
            "push  {r0-r7, r12, lr}\n"
            "vpush {d0-d3}\n"
            // r4 = プレイヤー
            "ldr   r12, =0x0068BBB8\n"
            "ldr   r12, [r12]\n"
            "mov   r4, #0\n"
            "cmp   r12, #0\n"
            "addne r12, r12, #0x7800\n"
            "ldrne r4, [r12, #0x28]\n"
            "cmp   r4, #0\n"
            "beq   .Lpx_exit\n"
            "cmp   r4, r5\n"
            "beq   .Lpx_exit\n"
            // 敵かどうか
            "ldrb  r0, [r5, #0x554]\n"
            "cmp   r0, #1\n"
            "bne   .Lpx_exit\n"
            "ldrb  r0, [r5, #0x3BC]\n"
            "cmp   r0, #2\n"
            "cmpne r0, #0xE\n"
            "bne   .Lpx_exit\n"
            "add   r0, r5, #0xD4\n"
            "ldr   r12, =0x005AF9EC\n"
            "blx   r12\n"
            "cmp   r0, #0\n"
            "bne   .Lpx_exit\n"
            // 敵の座標 -> s0,s1,s2
            "mov   r0, r5\n"
            "ldr   r1, [r0]\n"
            "ldr   r1, [r1, #0x18]\n"
            "blx   r1\n"
            "vldr  s0, [r0]\n"
            "vldr  s1, [r0, #4]\n"
            "vldr  s2, [r0, #8]\n"
            // プレイヤーの座標 -> s3,s4,s5
            "mov   r0, r4\n"
            "ldr   r1, [r0]\n"
            "ldr   r1, [r1, #0x18]\n"
            "blx   r1\n"
            "vldr  s3, [r0]\n"
            "vldr  s4, [r0, #4]\n"
            "vldr  s5, [r0, #8]\n"
            // 距離の2乗
            "vsub.f32 s0, s0, s3\n"
            "vsub.f32 s1, s1, s4\n"
            "vsub.f32 s2, s2, s5\n"
            "vmul.f32 s0, s0, s0\n"
            "vmla.f32 s0, s1, s1\n"
            "vmla.f32 s0, s2, s2\n"
            "ldr   r0, =BC_ProxRange\n"
            "vldr  s1, [r0]\n"
            "vcmpe.f32 s0, s1\n"
            "vmrs  APSR_nzcv, fpscr\n"
            "bgt   .Lpx_exit\n"          // 範囲外
            // 範囲内: ダメージ処理を呼ぶ
            "ldr   r0, [r4, #0x544]\n"
            "str   r0, [r5, #0x548]\n"
            "mov   r0, r5\n"
            "bl    .Lpx_find\n"
            "mov   r6, r0\n"
            "mov   r0, r4\n"
            "bl    .Lpx_find\n"
            "mov   r7, r0\n"
            "cmp   r6, #0\n"
            "cmpne r7, #0\n"
            "beq   .Lpx_exit\n"
            "sub   sp, sp, #0x144\n"
            "mov   r0, #0\n"
            "mov   r1, sp\n"
            "mov   r2, #0x44\n"
            ".Lpx_clr:\n"
            "str   r0, [r1], #4\n"
            "subs  r2, r2, #1\n"
            "bne   .Lpx_clr\n"
            "str   r6, [sp, #8]\n"
            "str   r7, [sp, #0x58]\n"
            "add   r0, sp, #0xF0\n"
            "str   r0, [sp, #0x70]\n"
            "ldr   r0, =0x47C35000\n"
            "str   r0, [sp, #0xBC]\n"
            "vldr  s0, .Lpx_one\n"       // 倍率 1.0 (等倍固定)
            "vldr  s1, .Lpx_one\n"
            "vsub.f32 s2, s0, s0\n"
            "mov   r0, sp\n"
            "add   r1, sp, #0x50\n"
            "mvn   r2, #0\n"
            "mov   r3, #1\n"
            "ldr   r12, =0x003F4160\n"
            "blx   r12\n"
            "add   sp, sp, #0x144\n"
            ".Lpx_exit:\n"
            "vpop  {d0-d3}\n"
            "pop   {r0-r7, r12, lr}\n"
            "vpush {d8}\n"               // 元の命令 (0x1A6404)
            "vmov.f32 s16, s0\n"         // 元の命令 (0x1A6408)
            "ldr   pc, =0x001A640C\n"
            ".ltorg\n"
            ".align 2\n"
            ".Lpx_one: .float 1.0\n"
            // r0 = 敵/プレイヤーのポインタ -> r0 = ゲーム内ハンドル (0 なら見つからない)
            ".Lpx_find:\n"
            "ldr   r1, =0x0068BBB8\n"
            "ldr   r1, [r1]\n"
            "add   r1, r1, #4\n"
            "add   r1, r1, #0x7800\n"
            "add   r1, r1, #0x16C\n"
            "ldr   r2, [r1, #0x1C]\n"
            "cmp   r2, #0\n"
            "moveq r0, #0\n"
            "bxeq  lr\n"
            "ldr   r3, [r1, #0x20]\n"
            "add   r2, r2, #0x10\n"
            ".Lpx_fa:\n"
            "cmp   r3, #0\n"
            "moveq r0, #0\n"
            "bxeq  lr\n"
            "ldr   r12, [r2]\n"
            ".Lpx_fb:\n"
            "cmp   r12, #0\n"
            "beq   .Lpx_fc\n"
            "ldr   r1, [r12, #8]\n"
            "cmp   r1, r0\n"
            "ldreq r0, [r12, #4]\n"
            "bxeq  lr\n"
            "ldr   r12, [r12, #0x10]\n"
            "b     .Lpx_fb\n"
            ".Lpx_fc:\n"
            "add   r2, r2, #0x14\n"
            "sub   r3, r3, #1\n"
            "b     .Lpx_fa\n"
            ".ltorg\n"
        );
    }

    // ---------------------------------------------------------------
    //  1. 固定ダメージ (プレイヤーのみ)
    // ---------------------------------------------------------------
    static const u32 DISPLAY_LIMIT = 10000; // これ以上は表示9999固定を併用

    static u32 FixedDamageValue()
    {
        return g_fixedDamage;
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
            g_fixedDamage = v;
        }
    }

    static void FixedDamageGame(MenuEntry *entry)
    {
        static bool fixApplied = false;
        static bool dspApplied = false;

        if (!entry->IsActivated())
        {
            // OFF: 元に戻す
            if (dspApplied)
            {
                RestoreHook(0x001F9AFC, ORG_DSP_FC, ORG_DSP_00);
                dspApplied = false;
            }
            if (fixApplied)
            {
                RestoreHook(0x003F4280, ORG_DMG_80, ORG_DMG_84);
                fixApplied = false;
            }
            return;
        }

        const u32 value = FixedDamageValue();

        if (!fixApplied)
        {
            if (!HookIsFree(0x003F4280, ORG_DMG_80, ORG_DMG_84, FnAddr(FixedDamageHook)))
            {
                OSD::Notify("固定ダメージ: フック位置が他のコードに使われています");
                entry->Disable();
                return;
            }
            InstallHook(0x003F4280, FnAddr(FixedDamageHook));
            fixApplied = true;
        }

        // ダメージ表示9999固定 (値に応じて自動 ON/OFF)
        if (value >= DISPLAY_LIMIT)
        {
            if (!dspApplied)
            {
                if (!HookIsFree(0x001F9AFC, ORG_DSP_FC, ORG_DSP_00, FnAddr(DisplayHook)))
                {
                    OSD::Notify("ダメージ表示9999: フック位置が他のコードに使われています");
                    entry->Disable();
                    return;
                }
                InstallHook(0x001F9AFC, FnAddr(DisplayHook));
                dspApplied = true;
            }
        }
        else if (dspApplied)
        {
            RestoreHook(0x001F9AFC, ORG_DSP_FC, ORG_DSP_00);
            dspApplied = false;
        }
    }

    // ---------------------------------------------------------------
    //  2. 近づいたらダメージ判定 (倍率は等倍固定・範囲指定可)
    //     ダメージは通常の攻撃計算を通るので、固定ダメージと併用すると
    //     近くの敵に固定ダメージが入ります。
    // ---------------------------------------------------------------
    // 範囲 (距離の整数値)。内部では距離の2乗(float)を使う。
    // 既定値 4 → 4*4 = 16.0 (従来の 0x41800000 と同じ)
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

        if (!entry->IsActivated())
        {
            if (applied)
            {
                RestoreHook(0x001A6404, ORG_PRX_04, ORG_PRX_08);
                applied = false;
            }
            return;
        }

        g_proxRange = RadiusToBits(ProximityRadius()); // 範囲の変更をすぐ反映

        if (!applied)
        {
            if (!HookIsFree(0x001A6404, ORG_PRX_04, ORG_PRX_08, FnAddr(ProximityHook)))
            {
                OSD::Notify("近づいたら判定: フック位置が他のコードに使われています");
                entry->Disable();
                return;
            }
            InstallHook(0x001A6404, FnAddr(ProximityHook));
            applied = true;
        }
    }

    // ---------------------------------------------------------------
    //  3. HP減らない / SP減らない (プレイヤー)
    //       manager = *(u32*)0x0068BBB8
    //       player  = *(u32*)(manager + 0x7828)
    //       HP = *(float*)(player + 0xD4 + 0x80)
    //       SP = *(float*)(player + 0xD4 + 0x84)   ※SPがHPの隣という推定
    // ---------------------------------------------------------------
    static bool PlayerStatusBase(u32 &base)
    {
        u32 mgr = 0, player = 0;
        if (!Process::Read32(0x0068BBB8, mgr) || mgr == 0)
            return false;
        if (!Process::CheckAddress(mgr + 0x7828))
            return false;
        if (!Process::Read32(mgr + 0x7828, player) || player == 0)
            return false;
        base = player + 0xD4;
        return Process::CheckAddress(base + 0x80) && Process::CheckAddress(base + 0x84);
    }

    // 減ったら、直前の最大値(回復で増えた値は更新)に戻す
    static void KeepNoDecrease(MenuEntry *entry, u32 offset, float &held, u32 &lastBase)
    {
        if (!entry->IsActivated())
        {
            held = 0.0f;
            lastBase = 0;
            return;
        }

        u32 base = 0;
        if (!PlayerStatusBase(base))
            return;

        if (base != lastBase) // プレイヤーのデータが入れ替わったらリセット
        {
            lastBase = base;
            held = 0.0f;
        }

        float cur = 0.0f;
        if (!Process::ReadFloat(base + offset, cur))
            return;
        if (!(cur >= 0.0f && cur < 10000000.0f)) // NaN・異常値には触らない
            return;

        if (cur >= held)
            held = cur;
        else
            Process::WriteFloat(base + offset, held);
    }

    static void HpNoDecreaseGame(MenuEntry *entry)
    {
        static float held = 0.0f;
        static u32 lastBase = 0;
        KeepNoDecrease(entry, 0x80, held, lastBase);
    }

    static void SpNoDecreaseGame(MenuEntry *entry)
    {
        static float held = 0.0f;
        static u32 lastBase = 0;
        KeepNoDecrease(entry, 0x84, held, lastBase);
    }

    // 確認用: HP/SPのアドレスを表示する
    static void ShowStatusAddr(MenuEntry *entry)
    {
        (void)entry;
        u32 base = 0;
        if (!PlayerStatusBase(base))
        {
            OSD::Notify("プレイヤーが見つかりません");
            return;
        }
        float hp = 0.0f, sp = 0.0f, tg = 0.0f;
        Process::ReadFloat(base + 0x80, hp);
        Process::ReadFloat(base + 0x84, sp);
        Process::ReadFloat(base + 0x88, tg);
        MessageBox("HP/SP/テンションのアドレス",
                   Utils::Format("HP: %08X (%.1f)\nSP: %08X (%.1f)\nテンション: %08X (%.1f)",
                                 base + 0x80, hp, base + 0x84, sp, base + 0x88, tg))();
    }

    // ---------------------------------------------------------------
    //  4. テンションゲージ (プレイヤー)
    //     テンション = *(float*)(player + 0xD4 + 0x88)
    //       (HPの+8。既存コードのアドレス 16EACA70→16EACA78 の差から推定)
    //     1段階目 = 50.0 (42480000) / 2段階目 = 100.0 (42C80000)
    //
    //     モード:
    //       0 自動   : 通常は 100.0 で待機 → 技を使ってゲージが減ったら
    //                  「2段階目が発動した」とみなし、効果時間のあいだ 50.0 に固定
    //                  (1段階目のダメージ技を連打できる)。時間が過ぎたら 100.0 に戻る。
    //       1 段階1固定 : 常に 50.0
    //       2 段階2固定 : 常に 100.0
    //     ※2段階目の効果が終わったかどうかはゲームから読めていないため、
    //       「効果時間(秒)」を設定値で指定します。
    // ---------------------------------------------------------------
    static u32 &TensionMode()
    {
        static u32 m = 0;
        return m;
    }

    static u32 &TensionBuffSec()
    {
        static u32 s = 30;
        return s;
    }

    static void TensionSettings(MenuEntry *entry)
    {
        (void)entry;
        std::vector<std::string> top;
        top.push_back("モード");
        top.push_back("段階2の効果時間(秒)");
        Keyboard kbTop("テンションゲージの設定", top);
        const int c = kbTop.Open();
        if (c == 0)
        {
            std::vector<std::string> modes;
            modes.push_back("自動(段階2発動→効果中は段階1固定)");
            modes.push_back("段階1固定(50.0)");
            modes.push_back("段階2固定(100.0)");
            Keyboard kbMode("モード", modes);
            const int m = kbMode.Open();
            if (m >= 0)
                TensionMode() = static_cast<u32>(m);
        }
        else if (c == 1)
        {
            u32 sec = TensionBuffSec();
            Keyboard kbSec("段階2の効果時間(秒) 1~600\n実際に計って合わせてください");
            kbSec.IsHexadecimal(false);
            if (kbSec.Open(sec, sec) == 0)
            {
                if (sec < 1)
                    sec = 1;
                if (sec > 600)
                    sec = 600;
                TensionBuffSec() = sec;
            }
        }
    }

    static void TensionGame(MenuEntry *entry)
    {
        static u32  lastBase = 0;
        static bool buff = false;   // 段階2の効果中とみなしている
        static bool primed = false; // 100.0 を書いた後
        static u32  frames = 0;

        if (!entry->IsActivated())
        {
            lastBase = 0;
            buff = false;
            primed = false;
            frames = 0;
            return;
        }

        u32 base = 0;
        if (!PlayerStatusBase(base))
            return;

        if (base != lastBase)
        {
            lastBase = base;
            buff = false;
            primed = false;
            frames = 0;
        }

        const u32 addr = base + 0x88;
        float cur = 0.0f;
        if (!Process::ReadFloat(addr, cur))
            return;
        if (!(cur >= 0.0f && cur < 1000.0f)) // NaN・異常値には触らない
            return;

        const u32 mode = TensionMode();
        if (mode == 1)
        {
            if (cur != 50.0f)
                Process::WriteFloat(addr, 50.0f);
            return;
        }
        if (mode == 2)
        {
            if (cur != 100.0f)
                Process::WriteFloat(addr, 100.0f);
            return;
        }

        // 自動モード
        if (!buff)
        {
            if (primed && cur < 99.0f) // 100.0 で待機中に減った = 段階2が発動した
            {
                buff = true;
                frames = 0;
                Process::WriteFloat(addr, 50.0f);
                return;
            }
            if (cur != 100.0f)
                Process::WriteFloat(addr, 100.0f);
            primed = true;
            return;
        }

        // 効果中: 50.0 に固定
        frames++;
        if (frames >= TensionBuffSec() * 60u) // 60フレーム = 約1秒
        {
            buff = false;
            primed = false; // 次のフレームで 100.0 に戻す
            return;
        }
        if (cur != 50.0f)
            Process::WriteFloat(addr, 50.0f);
    }

    // ---------------------------------------------------------------
    //  5. テンション調査 (開発用)
    //     プレイヤーのデータ(player+0 ~ +0x1000)を4つの状態で保存して、
    //     「2段階目の効果中フラグ」「残り時間」らしき場所をSDに書き出す。
    //       A  = 通常 / B1 = 効果中 / B2 = 効果中(数秒後) / C = 効果終了後
    //     結果は SD の fll_tension_scan.txt に出力される。
    // ---------------------------------------------------------------
    static const u32 SCAN_SIZE = 0x1000;

    static std::vector<u32> &ScanSnap(int i)
    {
        static std::vector<u32> snaps[4];
        return snaps[i];
    }

    static u32 &ScanPlayer(int i)
    {
        static u32 pl[4] = {0, 0, 0, 0};
        return pl[i];
    }

    static void ScanTake(int i)
    {
        u32 base = 0;
        if (!PlayerStatusBase(base))
        {
            MessageBox("プレイヤーが見つかりません")();
            return;
        }
        const u32 player = base - 0xD4;
        std::vector<u32> &v = ScanSnap(i);
        v.assign(SCAN_SIZE / 4, 0);
        for (u32 off = 0; off < SCAN_SIZE; off += 4)
        {
            u32 w = 0;
            if (Process::CheckAddress(player + off))
                Process::Read32(player + off, w);
            v[off / 4] = w;
        }
        ScanPlayer(i) = player;
        OSD::Notify(Utils::Format("保存しました (player=%08X)", player));
    }

    static float AsFloat(u32 w)
    {
        float f = 0.0f;
        std::memcpy(&f, &w, sizeof(f));
        return f;
    }

    static void ScanWrite()
    {
        for (int i = 0; i < 4; ++i)
        {
            if (ScanSnap(i).empty())
            {
                MessageBox("A, B1, B2, C をすべて保存してから書き出してください")();
                return;
            }
        }
        File file;
        if (File::Open(file, "fll_tension_scan.txt", File::RWC | File::TRUNCATE) != 0)
        {
            MessageBox("fll_tension_scan.txt を開けませんでした")();
            return;
        }
        file.WriteLine(Utils::Format("player: A=%08X B1=%08X B2=%08X C=%08X",
                                     ScanPlayer(0), ScanPlayer(1), ScanPlayer(2), ScanPlayer(3)));
        file.WriteLine("== フラグ候補 (A==C, B1==B2, B1!=A)");
        int n = 0;
        for (u32 i = 0; i < SCAN_SIZE / 4 && n < 300; ++i)
        {
            const u32 A = ScanSnap(0)[i], B1 = ScanSnap(1)[i], B2 = ScanSnap(2)[i], C = ScanSnap(3)[i];
            if (A == C && B1 == B2 && B1 != A)
            {
                file.WriteLine(Utils::Format("+%04X  %08X -> %08X  (float %.3f -> %.3f)",
                                             i * 4, A, B1, AsFloat(A), AsFloat(B1)));
                ++n;
            }
        }
        file.WriteLine("== タイマー候補 (A==C, B1!=B2)");
        n = 0;
        for (u32 i = 0; i < SCAN_SIZE / 4 && n < 300; ++i)
        {
            const u32 A = ScanSnap(0)[i], B1 = ScanSnap(1)[i], B2 = ScanSnap(2)[i], C = ScanSnap(3)[i];
            if (A == C && B1 != B2)
            {
                file.WriteLine(Utils::Format("+%04X  %08X -> %08X  (float %.3f -> %.3f)",
                                             i * 4, B1, B2, AsFloat(B1), AsFloat(B2)));
                ++n;
            }
        }
        file.Close();
        MessageBox("fll_tension_scan.txt に書き出しました")();
    }

    static void TensionScanEntry(MenuEntry *entry)
    {
        (void)entry;
        std::vector<std::string> opts;
        opts.push_back("A: 通常の状態を保存");
        opts.push_back("B1: 2段階目の効果中を保存");
        opts.push_back("B2: 効果中(数秒後)を保存");
        opts.push_back("C: 効果が切れた後を保存");
        opts.push_back("結果をSDに書き出す");
        Keyboard kb("テンション調査 (先に「テンションゲージ」をOFFに)", opts);
        const int c = kb.Open();
        if (c >= 0 && c <= 3)
            ScanTake(c);
        else if (c == 4)
            ScanWrite();
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

        folder->Append(new MenuEntry(
            "HP減らない", HpNoDecreaseGame, nullptr,
            "プレイヤーのHPが減ったら、直前の値に戻します。\n"
            "回復やレベルアップで増えた値は、新しい上限として覚えます。"));

        folder->Append(new MenuEntry(
            "SP減らない", SpNoDecreaseGame, nullptr,
            "プレイヤーのSPが減ったら、直前の値に戻します。"));

        folder->Append(new MenuEntry(
            "テンションゲージ", TensionGame, TensionSettings,
            "テンションゲージを自動で管理します(設定でモードと効果時間を指定)。\n"
            "自動: 通常は100.0で待機→段階2が発動したら効果時間のあいだ50.0固定。\n"
            "段階1だけ解放済みの場合は、設定で「段階1固定」にしてください。"));

        folder->Append(new MenuEntry(
            "テンション調査 (開発用)", nullptr, TensionScanEntry,
            "2段階目の効果中フラグ/残り時間を探すための調査です。\n"
            "通常→効果中→効果中(数秒後)→効果終了後 の4回保存して、書き出します。\n"
            "使う前に「テンションゲージ」をOFFにしてください。"));

        folder->Append(new MenuEntry(
            "HP/SPアドレス確認 (開発用)", nullptr, ShowStatusAddr,
            "プレイヤーのHP/SP/テンションのアドレスと現在値を表示します。"));

        menu += folder;
    }

    // menu がポインタ(PluginMenu *menu)の場合はこちらが呼ばれる
    static void InitMenu(PluginMenu *menu)
    {
        InitMenu(*menu);
    }

} // namespace BattleCheats
