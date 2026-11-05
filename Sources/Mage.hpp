// Mage.hpp -- 魔法使い関連 (ファンタジーライフ日本版 1.7L)
//
// このファイルを main.cpp から1回だけ #include し、メニュー作成の中で AddMageRapid(menu); を呼ぶ。
// 魔法使いの機能を変えたいときは、このファイルだけを差し替えればよい (main.cpp は触らなくてよい)。
//
// 中身:
//   1. C 言語の部分 (フックから呼ばれる関数と、状態の変数)
//   2. アセンブラの部分 (ゲームのコードに入れるフックの入口)
//   3. メニューと設定 (AddMageRapid)
#pragma once

#include <3ds.h>
#include <CTRPluginFramework.hpp>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// ============================================================
//  1. C 言語の部分
// ============================================================
extern "C" {
    // ============================================================
    //  魔法使い: チャージ技を、プレイヤーのモーションなしで出す (実験)
    //
    //   ・「ゴースト」: 魔法の判定を作る関数 (0x22C254) を、プレイヤーのコントローラ (C+0x558) に
    //     ついて、スキルIDと時間を一時的に書き換えながら直接呼ぶ。呼び終わったら元に戻す。
    //     プレイヤー自身のアクションは変わらない(待機のまま)。
    //   ・毎フレームの進行は、ユニット更新 (0x1A63FC) の入口で、プレイヤー自身のときに行う。
    //     (敵即死のARコードと同じ場所。プレイヤーが止まっていても、毎フレーム呼ばれる)
    //   ・発動のきっかけは3種類。
    //       A    : Aスキル枠の状態0 (0x240258) で押下を消費して、状態機械を進めない
    //       B,Y,L,R : プラグイン側でキー入力を数える
    //       自動 : 同じユニット更新の入口で、近くの敵を見つけたとき
    //   ・Cは起動のたびに変わるので、毎回 [[0x68BBB8]+0x7828] から取り直す。
    // ============================================================
    void jp_hookMageS0(void);
    void jp_hookMageUnit(void);
    void jp_hookMageTick(void);
    void jp_hookMageChan(void);
    void jp_callBody(u32 ctrl);

    volatile u32 g_mageWatch    = 0;      // 手動発動: 毎フレームプラグイン側が補充。0ならオフ
    volatile u32 g_mageAutoWatch = 0;     // 自動発動: 同上
    volatile u32 g_mageAlive    = 0;      // ユニット更新が補充。0なら (フックが動いていない) A を消費しない
    volatile u32 g_mageTmax     = 120;    // 判定を作る最大フレーム (10〜600)
    volatile u32 g_mageCool     = 0;      // 連続発動の最短間隔 (フレーム)。0 = 押した瞬間の1回だけ (長押しで連射しない)
    volatile u32 g_mageStep     = 1;      // ゴーストが1フレームに進める時間 (1 = 等速。大きいほど魔法が早く出る)
    volatile u32 g_mageMask     = 0xFFFF; // 手動で出す技 (ビット0〜15 = g_mageIds の順)
    volatile u32 g_mageAutoMask = 0xFFFF; // 自動で出す技
    volatile u32 g_mageAutoCool = 30;     // 自動発動の最短間隔 (フレーム)
    volatile u32 g_mageAutoLeft = 0;
    volatile float g_mageAutoDist2 = 36.0f; // 自動発動の距離の2乗 (初期値 6.0 の2乗)
    volatile u32 g_mageKey      = 0;      // 発動ボタン: 0 = A (ゲームのAを横取り) / 1 = B / 2 = Y / 3 = L / 4 = R
    volatile u32 g_mageReq      = 0;      // プラグイン側が押下を知らせる回数 (A以外のボタン用)
    volatile u32 g_mageReqSeen  = 0;
    volatile u32 g_mageHeld     = 0;      // A以外のボタンを押している間 1
    volatile u32 g_magePressTick = 0;
    volatile u32 g_mageCoolLeft = 0;
    volatile u32 g_mageCasts    = 0;
    volatile u32 g_mageAutoCasts = 0;
    volatile u32 g_mageTicks    = 0;
    volatile u32 g_mageCtrl     = 0;
    // --- 周りの敵の一覧 (ターゲットの調査・全敵への発動のため) ---
    volatile u32 g_mageTgtMove  = 0;      // 1 = 敵ごとに、魔法の出る位置も敵の方へずらす (ターゲットだけでは出ない技のため)
    volatile float g_mageTgtMoveR = 1.0f; // 敵の方へずらす割合 (1.0 = 敵の位置、0.5 = 半分)
    volatile u32 g_mageCatBits[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };   // 自動で覚えた「敵の種類」 (ビット = 種類の値。ボスなど)
    volatile u32 g_mageLearnCat = 1;      // 1 = ターゲットした相手の種類を、敵の種類として自動で覚える
    volatile u32 g_mageEnemyCatX = 0;     // 敵として扱う、追加の種類 (ボスなど。0 = なし)
    volatile float g_mageEnemyX[16];      // 敵の座標
    volatile float g_mageEnemyZ[16];
    static float g_mageEnemyBuildX[16], g_mageEnemyBuildZ[16];
    volatile u32 g_mageUnitN    = 0;      // 診断: 周りのユニットの種類の一覧
    volatile u32 g_mageUnitCat[16];
    volatile u32 g_mageUnitFlag[16];
    volatile float g_mageUnitD2[16];
    static u32 g_mageUnitBuildN = 0;
    static u32 g_mageUnitBuildCat[16], g_mageUnitBuildFlag[16];
    static float g_mageUnitBuildD2[16];
    volatile u32 g_mageTgtAll   = 0;      // 1 = 周りの敵ぜんぶに向けて、同じ魔法を出す (ターゲットは変えない)
    volatile u32 g_mageTgtMax   = 4;      // 向ける敵の最大数 (1〜8)
    volatile float g_mageTgtRange2 = 400.0f; // 向ける敵の距離の2乗 (初期値 20 の2乗)
    volatile u32 g_mageTgtWatch = 0;      // ターゲット調査を有効にしている間、プラグイン側が補充
    volatile u32 g_mageEnemyN   = 0;      // 直前のフレームで見つかった敵の数
    volatile u32 g_mageEnemy[16];         // 敵のユニットのアドレス
    volatile float g_mageEnemyD2[16];     // プレイヤーからの距離の2乗
    static u32 g_mageEnemyBuildN = 0;
    static u32 g_mageEnemyBuild[16];
    static float g_mageEnemyBuildD2[16];
    // --- 位置の調整 (実験): 魔法を出す瞬間だけ、プレイヤーの座標をずらして、本体を呼ぶ ---
    volatile float g_mageFwdF   = 0.0f;   // 前方へずらす距離 (正 = 前、負 = 後ろ)
    volatile u32   g_mageDirs   = 1;      // 同時に出す方向の数: 1 / 4 / 8
    volatile float g_mageDirD   = 6.0f;   // 方向ごとにずらす距離
    volatile float g_mageFx     = 0.0f;   // 向きの目安 (直前の移動方向)
    volatile float g_mageFz     = 1.0f;
    volatile u32 g_mageCntUnit  = 0;      // 診断: ユニット更新フックが、プレイヤーで呼ばれた回数
    volatile u32 g_mageCntTick  = 0;      // 診断: アクション更新フックが、プレイヤーで呼ばれた回数
    volatile u32 g_mageCntChan  = 0;      // 診断: 入力更新フックが呼ばれた回数
    volatile u32 g_mageUltId    = 0;      // Xの必殺技のアクションID (0 = 未設定。ビット16が、この技)
    volatile float g_mageRate   = 1.0f;   // ゴーストの時間の進み方 (1フレームあたりの時間の増え方。AR準拠の実測に合わせる)
    volatile u32 g_mageArStagger = 0;     // AR準拠で、同時に出す技を何フレームずつ遅らせるか (0 = ARコードと同じ。全部同じ時間)
    volatile u32 g_mageStagger  = 1;      // 同時に選んだ技を、1つずつ何フレームずつ遅らせて出すか (0 = 全部同じフレーム)
    volatile u32 g_mageUltTmax  = 600;    // 必殺技だけの最大フレーム (隕石などを長く降らせるため、ほかより長い)
    volatile u32 g_mageBudget   = 48;     // 1フレームに魔法の判定を作る関数を呼ぶ回数の上限 (重くなりすぎないように)
    volatile u32 g_mageClear    = 0;      // 1にすると、動いているゴーストを全部止める
    volatile u32 g_mageFrame    = 0;      // プラグイン側が毎フレーム進める (同じフレームで2回進めないための目印)
    volatile u32 g_mageDrvFrame = 0;
    // --- AR準拠モード (プレイヤーのモーションあり。Aスキルを2段階目まで一瞬で溜めて撃ち、全属性・A/Xを同時に出す) ---
    volatile u32 g_mageArWatch  = 0;
    volatile u32 g_mageArMask   = 0xFFFF;
    volatile u32 g_mageArKey    = 0;      // 0 = A / 1 = X / 2 = B / 3 = Y / 4 = L / 5 = R
    volatile u32 g_mageArN      = 30;     // 「押している」扱いにするフレーム数
    volatile u32 g_mageArReq    = 0;
    volatile u32 g_mageArReqSeen = 0;
    volatile s32 g_mageSim      = -1;     // 入力の再現中のフレーム (-1 = なし)
    volatile u32 g_mageChR2     = 0;      // MageChan の結果 (アセンブラが読む)
    volatile u32 g_mageChR3     = 0;
    volatile u32 g_mageArCasts  = 0;
    volatile u32 g_mageArUlt    = 0;      // 今のAスキルの間に、必殺技のゴーストを始めたか
    volatile u32 g_mageSince    = 1000;   // 最後に手動で発動してからのフレーム数
    volatile u32 g_mageCancelWin = 40;    // 手動で発動してから何フレームの間、通常攻撃を消すか (0でオフ)
    volatile u32 g_mageActLog   = 0;      // 消した動きの回数 (診断用)
    volatile u32 g_mageLastAct  = 0;      // 最後に消した動きのID (診断用)
    #define MAGE_MAXG 6
    static volatile s32 g_mageT[MAGE_MAXG]  = { -1, -1, -1, -1, -1, -1 };
    static volatile u32 g_mageGM[MAGE_MAXG] = { 0, 0, 0, 0, 0, 0 };   // ゴーストごとの「出す技」

    static const u32 g_mageIds[16] = {
        0x01000018, 0x0100001E, 0x0100002A, 0x0100002E,   // A 2段階目 (火 水 風 地)
        0x01000017, 0x0100001D, 0x01000023, 0x0100002D,   // A 1段階目
        0x01000019, 0x0100001F, 0x0100002B, 0x0100002F,   // X 1段階目
        0x0100001A, 0x01000020, 0x0100002C, 0x01000030    // X 2段階目
    };

    static u32 MageObj(void)     // C = [[0x68BBB8] + 0x7828]
    {
        const u32 mgr = *(volatile u32 *)0x68BBB8;
        if (mgr < 0x08000000u || mgr >= 0x1F000000u || (mgr & 3))
            return 0;
        const u32 obj = *(volatile u32 *)(mgr + 0x7828);
        if (obj < 0x08000000u || obj >= 0x1F000000u || (obj & 3))
            return 0;
        return obj;
    }

    // n = 0〜15: 表のスキル / 16: Xの必殺技 (未設定なら 0)
    static u32 MageIdAt(int n)
    {
        return n < 16 ? g_mageIds[n] : g_mageUltId;
    }

    static bool MageIsId(u32 id)
    {
        for (int i = 0; i < 16; ++i)
            if (g_mageIds[i] == id)
                return true;
        return false;
    }

    // ゴーストを1つ作る (全部使用中なら、いちばん進んだものを作り直す)
    static void MageStart(u32 mask, bool manual)
    {
        // 必殺技は同時に1つだけ (長く続くので、重なると重くなりすぎる)。すでに動いていたら、必殺技を外す。
        if ((mask >> 16) & 1)
        {
            for (int i = 0; i < MAGE_MAXG; ++i)
                if (g_mageT[i] >= 0 && ((g_mageGM[i] >> 16) & 1))
                    mask &= ~0x10000u;
        }
        if (!(mask & 0x1FFFF))
            return;
        if (manual)
        {
            g_mageCoolLeft = g_mageCool;
            g_mageSince = 0;
            g_mageCasts = g_mageCasts + 1;
        }
        else
        {
            g_mageAutoCasts = g_mageAutoCasts + 1;
        }

        int k = -1, oldest = 0;
        for (int i = 0; i < MAGE_MAXG; ++i)
        {
            if (g_mageT[i] < 0) { k = i; break; }
            if (g_mageT[i] > g_mageT[oldest]) oldest = i;
        }
        if (k < 0) k = oldest;
        g_mageGM[k] = mask;
        g_mageT[k] = 0;
    }

    // Aのスキル枠の状態0 (押された瞬間) で呼ばれる。1を返すと、状態機械は何もせず終わる。
    u32 MageS0Check(u32 slot, u32 pressed)
    {
        if (!g_mageWatch || !g_mageAlive || !pressed)
            return 0;
        if (g_mageKey)                                     // Aを発動に使わない設定
            return 0;
        const u32 C = MageObj();
        if (!C || slot != C + 0x888)                       // Aのスキル枠だけ
            return 0;
        if (!MageIsId(*(volatile u32 *)(slot + 0x48)))     // 魔法使いのスキルIDのときだけ
            return 0;

        // 「押している間」ずっと呼ばれるので、前のフレームも押していたかを見て、押した瞬間を判定する
        const bool edge = (g_mageTicks - g_magePressTick) > 1;
        g_magePressTick = g_mageTicks;

        if (g_mageCool == 0)
        {
            if (!edge)
                return 1;                                  // 長押し中は、押下を消費して何もしない
        }
        else if (g_mageCoolLeft)
        {
            return 1;                                      // 間隔の間は、押下を消費して何もしない
        }
        MageStart(g_mageMask, true);
        return 1;
    }

    // プレイヤーの座標のコピー全部 (x,y,z 各5か所) を保存・ずらす・戻す。
    // 座標はユニットの中に複数のコピーがあるため、全部ずらさないと魔法の位置が変わらないことがある。
    static const u16 g_mageOffX[5] = { 0x7C, 0x40, 0x10, 0x20C, 0x218 };
    static const u16 g_mageOffZ[5] = { 0x84, 0x60, 0x30, 0x214, 0x220 };
    static float g_mageSavX[5], g_mageSavZ[5];

    static void MageShiftSave(u32 obj)
    {
        for (int i = 0; i < 5; ++i)
        {
            g_mageSavX[i] = *(volatile float *)(obj + g_mageOffX[i]);
            g_mageSavZ[i] = *(volatile float *)(obj + g_mageOffZ[i]);
        }
    }

    static void MageShiftSet(u32 obj, float dx, float dz)
    {
        for (int i = 0; i < 5; ++i)
        {
            *(volatile float *)(obj + g_mageOffX[i]) = g_mageSavX[i] + dx;
            *(volatile float *)(obj + g_mageOffZ[i]) = g_mageSavZ[i] + dz;
        }
    }

    // ターゲット (魔法が狙う敵) の場所: プレイヤーのデータの +0x684 と +0x688 に、敵のユニットのアドレスが入っている。
    // (調査で、ターゲットを変えると、この2か所の値が同じ敵に切り替わることを確認した)
    static const u32 MAGE_TGT_A = 0x684;
    static const u32 MAGE_TGT_B = 0x688;

    // 距離が近い順に、範囲内の敵を最大 maxN 体選ぶ。選んだ数を返す。
    static float g_magePickX[8], g_magePickZ[8];
    static u32 MagePickTargets(u32 *out, u32 maxN)
    {
        if (maxN > 8) maxN = 8;
        bool used[16];
        for (int i = 0; i < 16; ++i) used[i] = false;
        const float lim = g_mageTgtRange2;
        u32 cnt = 0;
        for (u32 k = 0; k < maxN; ++k)
        {
            int best = -1;
            float bd = 0.0f;
            for (u32 j = 0; j < g_mageEnemyN && j < 16; ++j)
            {
                if (used[j]) continue;
                const float d = g_mageEnemyD2[j];
                if (d <= lim && (best < 0 || d < bd)) { best = (int)j; bd = d; }
            }
            if (best < 0) break;
            used[best] = true;
            g_magePickX[cnt] = g_mageEnemyX[best];
            g_magePickZ[cnt] = g_mageEnemyZ[best];
            out[cnt++] = g_mageEnemy[best];
        }
        return cnt;
    }

    // 方向 d (0 〜 nd-1) のときの、座標のずらし量 (ox, oz)。前 = 向きの目安、右 = それを90度回した向き。
    // nd = 1 のときは、前へずらす距離だけ。nd = 4 / 8 のときは、さらに方向ごとの距離を足す。
    static void MageDirOffset(u32 nd, u32 d, float &ox, float &oz)
    {
        static const float cs[8][2] = { {1.0f, 0.0f}, {0.7071f, 0.7071f}, {0.0f, 1.0f}, {-0.7071f, 0.7071f},
                                        {-1.0f, 0.0f}, {-0.7071f, -0.7071f}, {0.0f, -1.0f}, {0.7071f, -0.7071f} };
        const float fx = g_mageFx, fz = g_mageFz;
        const float rx = fz, rz = -fx;
        const u32 ci = (nd == 8) ? d : d * 2;               // 4方向のときは、前・右・後ろ・左
        const float a = (nd > 1) ? cs[ci][0] : 0.0f;        // 前方向の成分
        const float b = (nd > 1) ? cs[ci][1] : 0.0f;        // 右方向の成分
        const float dist = (nd > 1) ? g_mageDirD : 0.0f;
        ox = fx * (g_mageFwdF + a * dist) + rx * (b * dist);
        oz = fz * (g_mageFwdF + a * dist) + rz * (b * dist);
    }

    // プレイヤーのユニット更新で、毎フレーム1回。ゴーストの進行と、通常攻撃の消去。
    static void MageTickCore(u32 K)
    {
        g_mageAlive = 20;
        g_mageTicks = g_mageTicks + 1;
        {   // 向きの目安: 直前の移動方向 (止まっても最後の向きを覚えておく)
            static float px = 0.0f, pz = 0.0f;
            static bool have = false;
            const u32 obj = K - 0x558;
            const float x = *(volatile float *)(obj + 0x7C), z = *(volatile float *)(obj + 0x84);
            if (have)
            {
                const float dx = x - px, dz = z - pz;
                const float d2 = dx * dx + dz * dz;
                if (d2 > 0.0004f && d2 < 4.0f)
                {
                    const float inv = 1.0f / __builtin_sqrtf(d2);
                    g_mageFx = dx * inv;
                    g_mageFz = dz * inv;
                }
            }
            px = x; pz = z; have = true;
        }
        if (g_mageWatch) g_mageWatch = g_mageWatch - 1;
        if (g_mageAutoWatch) g_mageAutoWatch = g_mageAutoWatch - 1;
        if (g_mageCoolLeft) g_mageCoolLeft = g_mageCoolLeft - 1;
        if (g_mageAutoLeft) g_mageAutoLeft = g_mageAutoLeft - 1;
        if (g_mageSince < 1000) g_mageSince = g_mageSince + 1;

        if (g_mageClear)                                   // 緊急停止
        {
            for (int i = 0; i < MAGE_MAXG; ++i) g_mageT[i] = -1;
            g_mageClear = 0;
        }

        if (K != g_mageCtrl)                               // エリア移動などで作り直されたら、ゴーストを捨てる
        {
            for (int i = 0; i < MAGE_MAXG; ++i) g_mageT[i] = -1;
            g_mageCtrl = K;
            g_mageReqSeen = g_mageReq;
            return;
        }

        // A以外のボタンで発動する設定 (プラグイン側が押下を数えている)
        if (g_mageWatch && g_mageKey)
        {
            while (g_mageReqSeen != g_mageReq)
            {
                g_mageReqSeen = g_mageReqSeen + 1;
                if (g_mageCool == 0 || !g_mageCoolLeft)
                    MageStart(g_mageMask, true);
            }
            if (g_mageHeld && g_mageCool && !g_mageCoolLeft)
                MageStart(g_mageMask, true);
        }

        const u32 step = g_mageStep < 1 ? 1 : g_mageStep;
        u32 budget = g_mageBudget < 17 ? 17 : g_mageBudget;
        const u32 budget0 = budget;
        for (int i = 0; i < MAGE_MAXG; ++i)
        {
            s32 t = g_mageT[i];
            if (t < 0)
                continue;
            const u32 mask = g_mageGM[i];
            // 必殺技を含むゴーストは、必殺技の最大フレームまで続ける (ほかの技は、従来の最大フレームで止める)
            u32 nsel = 0;
            for (int n = 0; n < 17; ++n)
                if (((mask >> n) & 1) && MageIdAt(n))
                    ++nsel;
            const u32 baseTmax = (((mask >> 16) & 1) && g_mageUltId && g_mageUltTmax > g_mageTmax) ? g_mageUltTmax : g_mageTmax;
            const u32 tmaxEff  = baseTmax + (nsel ? (nsel - 1) * g_mageStagger : 0);   // 遅らせた分だけ、続ける時間も延ばす
            const u32 savedId   = *(volatile u32 *)(K + 8);
            const u32 savedTime = *(volatile u32 *)(K + 0xC4);
            for (u32 sp = 0; sp < step; ++sp)              // 1フレームに step だけ時間を進める (飛ばさず全部呼ぶ)
            {
                if ((u32)t > tmaxEff)
                    break;
                // 選んだ技を順番に並べ、k 番目の技は k×g_mageStagger フレーム遅らせて出す。
                // 全部を同じフレームで出すと、先に出した技 (Aの2段階目など) だけが優先され、
                // 残りが出ないことがある (ゲーム側の同時に出せる数の上限のため)。
                const u32 stg = g_mageStagger;
                u32 need = 0;                                  // この時間で呼ぶ回数
                {
                    u32 rank = 0;
                    for (int n = 0; n < 17; ++n)
                    {
                        if (!((mask >> n) & 1) || !MageIdAt(n))
                            continue;
                        const s32 tt = t - (s32)(rank * stg);
                        const u32 lim = (n < 16) ? g_mageTmax : g_mageUltTmax;
                        if (tt >= 0 && (u32)tt <= lim)
                            ++need;
                        ++rank;
                    }
                }
                const u32 nd = (g_mageDirs >= 8) ? 8 : (g_mageDirs >= 4 ? 4 : 1);
                u32 tg[8];                                     // 全敵ターゲット: 周りの敵 (近い順)
                u32 nT = 0;
                if (g_mageTgtAll)
                    nT = MagePickTargets(tg, g_mageTgtMax);
                const u32 nE = nT ? nT : 1;
                need *= nd * nE;
                if (need > budget && budget != budget0)
                    break;                                     // 今フレームの上限を超えるので、続きは次のフレームへ (先頭のゴーストは必ず進める)
                budget = (need > budget) ? 0 : budget - need;

                const bool useMove = nT && g_mageTgtMove;      // 魔法の出る位置も、敵の方へずらす
                const bool shifted = (nd > 1) || (g_mageFwdF != 0.0f) || useMove;
                const u32 pobj = K - 0x558;
                const u32 origA = *(volatile u32 *)(pobj + MAGE_TGT_A);
                const u32 origB = *(volatile u32 *)(pobj + MAGE_TGT_B);
                if (shifted)
                    MageShiftSave(pobj);
                for (u32 d = 0; d < nd; ++d)
                {
                    float bx = 0.0f, bz = 0.0f;
                    if (shifted)
                        MageDirOffset(nd, d, bx, bz);
                    for (u32 e = 0; e < nE; ++e)
                    {
                        if (shifted)
                        {
                            float ox = bx, oz = bz;
                            if (useMove)                       // プレイヤー → 敵 の向きへ、割合ぶんずらす
                            {
                                ox += (g_magePickX[e] - g_mageSavX[0]) * g_mageTgtMoveR;
                                oz += (g_magePickZ[e] - g_mageSavZ[0]) * g_mageTgtMoveR;
                            }
                            MageShiftSet(pobj, ox, oz);
                        }
                        if (nT)                                // この敵に向けて出す (ターゲットを一時的に差し替える)
                        {
                            *(volatile u32 *)(pobj + MAGE_TGT_A) = tg[e];
                            *(volatile u32 *)(pobj + MAGE_TGT_B) = tg[e];
                        }
                        u32 rank = 0;
                        for (int n = 0; n < 17; ++n)
                        {
                            if (!((mask >> n) & 1) || !MageIdAt(n))
                                continue;
                            const s32 tt = t - (s32)(rank * stg);
                            const u32 lim = (n < 16) ? g_mageTmax : g_mageUltTmax;
                            ++rank;
                            if (tt < 0 || (u32)tt > lim)
                                continue;
                            *(volatile u32 *)(K + 8)      = MageIdAt(n);
                            *(volatile float *)(K + 0xC4) = (float)tt * g_mageRate;
                            jp_callBody(K);                    // 魔法の判定・エフェクトを作る本体
                        }
                    }
                }
                if (nT)
                {
                    *(volatile u32 *)(pobj + MAGE_TGT_A) = origA;  // 元のターゲットに戻す
                    *(volatile u32 *)(pobj + MAGE_TGT_B) = origB;
                }
                if (shifted)
                    MageShiftSet(pobj, 0.0f, 0.0f);        // 元の座標に戻す (保存した値そのまま)
                ++t;
            }
            *(volatile u32 *)(K + 8)    = savedId;
            *(volatile u32 *)(K + 0xC4) = savedTime;
            g_mageT[i] = ((u32)t > tmaxEff) ? -1 : t;
        }

        // 手動で発動した直後に、ゲーム側が別経路で始める通常攻撃を、待機の強制予約で消す。
        // 通常攻撃は ID 0x01000005 だけ (歩きなどは消さない)。動きの最初の1〜2フレームだけ見える。
        if ((g_mageWatch || g_mageArWatch) && g_mageCancelWin && g_mageSince <= g_mageCancelWin)
        {
            const u32 cur = *(volatile u32 *)(K + 8);
            if (cur == 0x01000005)
            {
                g_mageLastAct = cur;
                g_mageActLog  = g_mageActLog + 1;
                // 0x1BE758(コントローラ, ID, フラグ, フラグ, 強制) = アクションの予約。ID 1 = 待機
                ((void (*)(u32, u32, u32, u32, u32))0x1BE758)(K, 1, 0x03FFFF0A, 0x6464, 1);
            }
        }
    }

    // 敵が近いか (敵即死のコードと同じ条件)
    // 敵か (敵即死のコードと同じ条件)
    static bool MageIsEnemy(u32 unit)
    {
        if (*(volatile u8 *)(unit + 0x554) != 1)
            return false;
        const u32 cat = *(volatile u8 *)(unit + 0x3BC);
        const bool learned = (g_mageCatBits[(cat >> 5) & 7] >> (cat & 31)) & 1;
        if (cat != 2 && cat != 0xE && !(g_mageEnemyCatX && cat == g_mageEnemyCatX) && !learned)   // 敵の種類 (追加・覚えた種類も含む)
            return false;
        if (((u32 (*)(u32))0x5AF9EC)(unit + 0xD4) != 0)    // 体力が0 (倒れている)
            return false;
        return true;
    }

    // プレイヤーからの距離の2乗 (敵の座標 ex, ez も返す)
    static float MageDistPos(u32 C, u32 unit, float &ex, float &ez)
    {
        typedef const float *(*PosFn)(u32);
        const PosFn fe = (PosFn)(*(volatile u32 *)(*(volatile u32 *)unit + 0x18));
        const PosFn fp = (PosFn)(*(volatile u32 *)(*(volatile u32 *)C + 0x18));
        const float *pe = fe(unit);
        const float *pp = fp(C);
        ex = pe[0];
        ez = pe[2];
        const float dx = pe[0] - pp[0], dy = pe[1] - pp[1], dz = pe[2] - pp[2];
        return dx * dx + dy * dy + dz * dz;
    }

    static float MageDist2(u32 C, u32 unit)
    {
        float ex, ez;
        return MageDistPos(C, unit, ex, ez);
    }

    static bool MageEnemyNear(u32 C, u32 unit)
    {
        return MageIsEnemy(unit) && MageDist2(C, unit) <= g_mageAutoDist2;
    }

    // 1フレームに1回だけ、ゴーストの進行を行う (ユニット更新とアクション更新の両方から呼ばれる)。
    // プレイヤーが動いているときと止まっているときで、呼ばれる側が違うため、両方から呼ぶ。
    static void MageDrive(u32 K)
    {
        if (g_mageDrvFrame == g_mageFrame && g_mageFrame != 0)
            return;
        g_mageDrvFrame = g_mageFrame;
        MageTickCore(K);
    }

    static bool MageAnyWatch(void)
    {
        return g_mageWatch || g_mageAutoWatch || g_mageArWatch;
    }

    // ユニット更新 (0x1A63FC) の入口で、全ユニットについて毎フレーム呼ばれる。
    void MageUnitCheck(u32 unit)
    {
        const bool any = MageAnyWatch();
        if (!any && !g_mageTgtWatch)
            return;
        const u32 C = MageObj();
        if (!C)
            return;
        if (unit == C)
        {
            // 敵の一覧を確定する (直前のフレームぶん)
            g_mageEnemyN = g_mageEnemyBuildN;
            for (u32 i = 0; i < g_mageEnemyBuildN; ++i)
            {
                g_mageEnemy[i]   = g_mageEnemyBuild[i];
                g_mageEnemyD2[i] = g_mageEnemyBuildD2[i];
                g_mageEnemyX[i]  = g_mageEnemyBuildX[i];
                g_mageEnemyZ[i]  = g_mageEnemyBuildZ[i];
            }
            g_mageEnemyBuildN = 0;
            g_mageUnitN = g_mageUnitBuildN;                // 診断用の一覧も確定
            for (u32 i = 0; i < g_mageUnitBuildN; ++i)
            {
                g_mageUnitCat[i]  = g_mageUnitBuildCat[i];
                g_mageUnitFlag[i] = g_mageUnitBuildFlag[i];
                g_mageUnitD2[i]   = g_mageUnitBuildD2[i];
            }
            g_mageUnitBuildN = 0;
            if (any)
            {
                g_mageCntUnit = g_mageCntUnit + 1;
                MageDrive(C + 0x558);
            }
            return;
        }
        bool isEnemy = MageIsEnemy(unit);
        if (!isEnemy && g_mageLearnCat)
        {
            // 今ターゲットしている相手なのに、敵として数えられていないとき (ボスなど)、その種類を覚える。
            // ゲーム本体の追尾はボスにも効くので、ターゲットできる相手は敵とみなせる。
            if (unit == *(volatile u32 *)(C + MAGE_TGT_A) && *(volatile u8 *)(unit + 0x554) == 1)
            {
                const u32 cat = *(volatile u8 *)(unit + 0x3BC);
                g_mageCatBits[(cat >> 5) & 7] = g_mageCatBits[(cat >> 5) & 7] | (1u << (cat & 31));
                isEnemy = MageIsEnemy(unit);               // 覚えたので、このフレームから敵として扱う
            }
        }
        if (isEnemy && g_mageEnemyBuildN < 16)
        {
            float ex, ez;
            const float d2 = MageDistPos(C, unit, ex, ez);
            g_mageEnemyBuild[g_mageEnemyBuildN]   = unit;
            g_mageEnemyBuildD2[g_mageEnemyBuildN] = d2;
            g_mageEnemyBuildX[g_mageEnemyBuildN]  = ex;
            g_mageEnemyBuildZ[g_mageEnemyBuildN]  = ez;
            g_mageEnemyBuildN = g_mageEnemyBuildN + 1;
        }
        if (g_mageTgtWatch && g_mageUnitBuildN < 16)       // 診断: 敵かどうかに関わらず、周りのユニットの種類を記録する
        {
            g_mageUnitBuildCat[g_mageUnitBuildN]  = *(volatile u8 *)(unit + 0x3BC);
            g_mageUnitBuildFlag[g_mageUnitBuildN] = *(volatile u8 *)(unit + 0x554);
            g_mageUnitBuildD2[g_mageUnitBuildN]   = MageDist2(C, unit);
            g_mageUnitBuildN = g_mageUnitBuildN + 1;
        }
        if (!g_mageAutoWatch || g_mageAutoLeft || !g_mageCtrl)
            return;
        if (isEnemy && MageDist2(C, unit) <= g_mageAutoDist2)
        {
            g_mageAutoLeft = g_mageAutoCool;
            MageStart(g_mageAutoMask, false);
        }
    }

    // プレイヤーのアクション更新 (0x22C254) の入口で呼ばれる。r0 = コントローラ。
    void MageTick(u32 K)
    {
        if (!MageAnyWatch())
            return;
        const u32 C = MageObj();
        if (!C || K != C + 0x558)
            return;
        g_mageCntTick = g_mageCntTick + 1;
        MageDrive(K);

        // AR準拠: Aの2段階目のスキルが動いている間、同じ時間のまま、ほかの技の判定も作る
        if (g_mageArWatch && g_mageAlive)
        {
            const u32 id = *(volatile u32 *)(K + 8);
            if (id == g_mageIds[0] || id == g_mageIds[1] || id == g_mageIds[2] || id == g_mageIds[3])
            {
                const u32 mask = g_mageArMask;
                // 位置の設定 (前へずらす・同時方向) をここにも反映する。
                // 本物のAスキルは、このあとゲーム本体がそのままの位置で出す。
                // 方向が複数のときだけ、Aスキル自身も、ずらした位置から追加で出す。
                const u32 nd = (g_mageDirs >= 8) ? 8 : (g_mageDirs >= 4 ? 4 : 1);
                // 全敵ターゲット (あとで選ぶ) のときは、位置も敵の方へずらすことがあるので、先に敵を選んでおく
                u32 tg[8];
                u32 nT = 0;
                if (g_mageTgtAll)
                    nT = MagePickTargets(tg, g_mageTgtMax);
                const u32 nE = nT ? nT : 1;
                const bool useMove = nT && g_mageTgtMove;
                const bool shifted = (nd > 1) || (g_mageFwdF != 0.0f) || useMove;
                if (shifted)
                    MageShiftSave(C);
                // 選んだ技を順番に並べ、k 番目の技は k×g_mageStagger フレーム遅らせた時間で出す。
                // 全部を同じ時間で出すと、本物のAスキルが優先されて、ほかが出ないことがあるため。
                const u32 savedTimeBits = *(volatile u32 *)(K + 0xC4);
                const float tReal = *(volatile float *)(K + 0xC4);
                const u32 stg = g_mageArStagger;
                // 全敵ターゲット: ほかの技を、周りの敵ぜんぶに向けて出す (本物のAスキルは、今のターゲットのまま)
                const u32 origA = *(volatile u32 *)(C + MAGE_TGT_A);
                const u32 origB = *(volatile u32 *)(C + MAGE_TGT_B);
                for (u32 d = 0; d < nd; ++d)
                {
                    float bx = 0.0f, bz = 0.0f;
                    if (shifted)
                        MageDirOffset(nd, d, bx, bz);
                    for (u32 e = 0; e < nE; ++e)
                    {
                        if (shifted)
                        {
                            float ox = bx, oz = bz;
                            if (useMove)
                            {
                                ox += (g_magePickX[e] - g_mageSavX[0]) * g_mageTgtMoveR;
                                oz += (g_magePickZ[e] - g_mageSavZ[0]) * g_mageTgtMoveR;
                            }
                            MageShiftSet(C, ox, oz);
                        }
                        if (nT)
                        {
                            *(volatile u32 *)(C + MAGE_TGT_A) = tg[e];
                            *(volatile u32 *)(C + MAGE_TGT_B) = tg[e];
                        }
                        u32 rank = 0;
                        for (int n = 0; n < 16; ++n)                   // 表のスキル: Aスキルと同じ時間で判定を作る
                        {
                            if (!((mask >> n) & 1))
                                continue;
                            const u32 k = rank++;
                            if (g_mageIds[n] == id && nd == 1 && !nT)  // 1方向・今のターゲットのみのときは、本物のAスキルがあるので省く
                                continue;
                            const float tt = tReal - (float)(k * stg);
                            if (tt < 0.0f)
                                continue;
                            *(volatile u32 *)(K + 8)      = g_mageIds[n];
                            *(volatile float *)(K + 0xC4) = tt;
                            jp_callBody(K);
                        }
                    }
                }
                if (nT)
                {
                    *(volatile u32 *)(C + MAGE_TGT_A) = origA;         // 元のターゲットに戻す
                    *(volatile u32 *)(C + MAGE_TGT_B) = origB;
                }
                *(volatile u32 *)(K + 0xC4) = savedTimeBits;           // 時間を元に戻す
                if (shifted)
                    MageShiftSet(C, 0.0f, 0.0f);                       // 元の座標に戻す (保存した値そのまま)
                *(volatile u32 *)(K + 8) = id;
                g_mageArCasts = g_mageArCasts + 1;

                // 必殺技は、Aスキルより動きが長い (隕石が何秒も降る) ので、Aスキルの時間では足りない。
                // Aスキルが始まったときに、必殺技だけ、別の時間で進めるゴーストを1つ始める。
                if (((mask >> 16) & 1) && g_mageUltId && !g_mageArUlt)
                {
                    g_mageArUlt = 1;
                    MageStart(0x10000, false);
                }
            }
            else
            {
                g_mageArUlt = 0;
            }
        }
    }

    // チャージ入力の更新 (0x240E50) の途中 (0x240E58) で呼ばれる。
    // chanBase = オブジェクト + チャンネル番号×8, held = 押している, just = 押した瞬間。
    // Aのチャンネルについて、押下を一定フレームの間「押している」扱いにして、溜めを一瞬で満たす。
    void MageChan(u32 chanBase, u32 held, u32 just)
    {
        g_mageChR2 = held;
        g_mageChR3 = just;
        if (!MageAnyWatch())
            return;
        const u32 C = MageObj();
        if (!C)
            return;
        // チャージ入力の更新は、プレイヤーが止まっていても動いていても、毎フレーム必ず呼ばれる。
        // ゴーストの進行は、ここでも行う (1フレームに1回だけ。MageDrive が重複を防ぐ)。
        g_mageCntChan = g_mageCntChan + 1;
        MageDrive(C + 0x558);
        if (!g_mageArWatch || !g_mageAlive)
            return;
        const u32 obj = *(volatile u32 *)(C + 0x888 + 4);              // Aスキル枠の持ち主 (入力オブジェクト)
        if (obj < 0x08000000u || obj >= 0x1F000000u || (obj & 3))
            return;
        const u32 chA = *(volatile u8 *)(obj + 0x1E8);
        if (chA > 3 || chanBase != obj + chA * 8)                      // Aのチャンネルだけ
            return;
        if (!MageIsId(*(volatile u32 *)(C + 0x888 + 0x48)))            // 魔法使いのときだけ
            return;

        if (g_mageSim < 0)
        {
            bool start = false;
            if (g_mageArKey == 0)
                start = (just != 0);                                   // 実際のAを押した瞬間
            else if (g_mageArReqSeen != g_mageArReq)                   // ほかのボタン (プラグイン側が数えている)
            {
                g_mageArReqSeen = g_mageArReq;
                start = true;
            }
            if (start)
            {
                g_mageSim = 0;
                g_mageSince = 0;                                       // 通常攻撃を消す期間の起点 (AR準拠でも使う)
            }
        }
        if (g_mageSim >= 0)
        {
            // Aスキル枠の状態機械が「2段階目まで溜まった」状態 (状態4) になったら、すぐ離した扱いにする。
            // 固定のフレーム数 (g_mageArN) は、状態4にならなかったときの上限として使う。
            const u32 st = *(volatile u8 *)(C + 0x888 + 0x24);
            const bool ready = (st == 4) && g_mageSim >= 2;
            if ((u32)g_mageSim < g_mageArN && !ready)
            {
                g_mageChR2 = 1;
                g_mageChR3 = (g_mageSim == 0) ? 1 : 0;
                *(volatile float *)(chanBase + 0x1CC) = 100000.0f;     // 溜め量を一瞬で最大にする
                g_mageSim = g_mageSim + 1;
            }
            else
            {
                g_mageChR2 = 0;                                        // 離した扱い → 溜め技が出る
                g_mageChR3 = 0;
                g_mageSim = -1;
            }
        }
    }
}

// ============================================================
//  2. アセンブラの部分
// ============================================================
asm(R"(
    .arm
    .text
    .align 2

    @ ---- 魔法使い: Aスキル枠の状態0 (押された瞬間) ----
    @ 0x240258: cmp r0,#0 / 0x24025C: ldrne r1,[r4,#0x48] の2命令を置き換え、0x240260 へ戻る。
    @ r0 = 押されたか (0以外), r4 = スキル枠。MageS0Check が 1 を返したら、
    @ 状態機械の終了処理 (0x240BA4: r0=0 で戻る) へ飛ぶ。
    .global jp_hookMageS0
    .type jp_hookMageS0, %function
jp_hookMageS0:
    push  {r0-r3, r12, lr}
    mov   r1, r0
    mov   r0, r4
    bl    MageS0Check
    cmp   r0, #0
    pop   {r0-r3, r12, lr}       @ pop はフラグを変えない
    bne   1f
    cmp   r0, #0                 @ 0x240258 の元命令
    ldrne r1, [r4, #0x48]        @ 0x24025C の元命令
    ldr   pc, =0x240260
1:  mov   r0, #0
    ldr   pc, =0x240BA4

    @ ---- 魔法使い: ユニット更新の入口 (0x1A6404) ----
    @ 置き換えた2命令 (vpush {d8} / vabs.f32 s16,s0) を再現し、0x1A640C へ戻る。
    @ r5 = ユニット。s0 (引数) を壊さないよう、d0-d3 を保存する。
    .global jp_hookMageUnit
    .type jp_hookMageUnit, %function
jp_hookMageUnit:
    push  {r0-r3, r12, lr}
    vpush {d0-d3}
    mov   r0, r5
    bl    MageUnitCheck
    vpop  {d0-d3}
    pop   {r0-r3, r12, lr}
    vpush {d8}                   @ 0x1A6404 の元命令
    vabs.f32 s16, s0             @ 0x1A6408 の元命令
    ldr   pc, =0x1A640C

    @ ---- 魔法使い: プレイヤーのアクション更新の入口 (0x22C254) ----
    @ 置き換えた2命令 (push {r0,r4-r11,lr} / vpush {d8-d10}) を再現し、0x22C25C へ戻る。
    .global jp_hookMageTick
    .type jp_hookMageTick, %function
jp_hookMageTick:
    push  {r0-r3, r12, lr}
    bl    MageTick               @ r0 = コントローラ
    pop   {r0-r3, r12, lr}
    push  {r0, r4-r11, lr}       @ 0x22C254 の元命令
    vpush {d8-d10}               @ 0x22C258 の元命令
    ldr   pc, =0x22C25C

    @ ---- 魔法の判定を作る本体を、C から呼ぶための橋 ----
    @ jp_callBody(r0 = コントローラ)。0x22C254/0x22C258 は私たちのフックで書き換えてあるので、
    @ そこへは飛ばず、同じ2命令をここで実行してから 0x22C25C へ入る。lr には戻り先 (1:) を積む。
    .global jp_callBody
    .type jp_callBody, %function
jp_callBody:
    push  {r4-r11, r12, lr}      @ 40バイト: 8バイト境界を保つ
    adr   lr, 1f
    push  {r0, r4-r11, lr}       @ 0x22C254 の元命令と同じ
    vpush {d8-d10}               @ 0x22C258 の元命令と同じ
    ldr   pc, =0x22C25C
1:  pop   {r4-r11, r12, pc}

    @ ---- 魔法使い: チャージ入力の更新 (0x240E50) の途中 (0x240E58) ----
    @ 置き換えた2命令 (ldrb r1,[r0,#0x1c8] / cmp r2,#0) を再現し、0x240E60 へ戻る。
    @ 入る時点で r0 = オブジェクト+ch*8, r2 = 押している, r3 = 押した瞬間, s0/s1 = 経過量/0。
    .global jp_hookMageChan
    .type jp_hookMageChan, %function
jp_hookMageChan:
    push  {r0-r3, r12, lr}
    vpush {d0-d1}
    mov   r1, r2
    mov   r2, r3
    bl    MageChan
    vpop  {d0-d1}
    pop   {r0-r3, r12, lr}
    ldr   r2, =g_mageChR2
    ldr   r2, [r2]
    ldr   r3, =g_mageChR3
    ldr   r3, [r3]
    ldrb  r1, [r0, #0x1c8]       @ 0x240E58 の元命令
    cmp   r2, #0                 @ 0x240E5C の元命令
    ldr   pc, =0x240E60
    .ltorg
)");

// ============================================================
//  3. メニューと設定
// ============================================================
namespace CTRPluginFramework
{
    // 命令が想定どおりか確認する (違うバージョンのゲームなら、フックを入れない)
    static bool MageCheckWords(u32 addr, const u32 *expect, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            u32 v = 0;
            if (!Process::Read32(addr + 4 * i, v) || v != expect[i])
                return false;
        }
        return true;
    }

    // ------------------------------------------------------------
    //  魔法使い: チャージ技をモーションなしで出す  【実験】
    //   ・ゲームのコード4か所を、2語ずつ書き換える (ワープと同じ方式)。
    //       0x240258 : Aスキル枠の状態0 (押された瞬間)
    //       0x1A6404 : ユニット更新の入口 (敵即死のARコードと同じ場所)
    //       0x22C254 : プレイヤーのアクション更新の入口 (魔法使い連射のARコードと同じ場所)
    //       0x240E58 : チャージ入力の更新の途中 (AR準拠モードで、Aの押下を再現する)
    //   ・以前の魔法使い連射のARコードと、敵即死のARコードとは同時に使えない。
    //     必ずOFFにして、ゲームを再起動してから使うこと (同じ場所を書き換えるため)。
    // ------------------------------------------------------------
    static constexpr u32 HOOK_MAGE_S0   = 0x00240258;
    static constexpr u32 HOOK_MAGE_UNIT = 0x001A6404;
    static constexpr u32 HOOK_MAGE_TICK = 0x0022C254;
    static constexpr u32 HOOK_MAGE_CHAN = 0x00240E58;

    static bool g_mageHooked = false;

    static bool InstallMageHooks(void)
    {
        if (g_mageHooked)
            return true;

        // 0x240258: cmp r0,#0 / ldrne r1,[r4,#0x48]
        static const u32 expectS0[2]   = { 0xE3500000, 0x15941048 };
        // 0x1A6404: vpush {d8} / vabs.f32 s16,s0
        static const u32 expectUnit[2] = { 0xED2D8B02, 0xEEB08A40 };
        // 0x22C254: push {r0,r4-r11,lr} / vpush {d8-d10}
        static const u32 expectTick[2] = { 0xE92D4FF1, 0xED2D8B06 };
        // 0x240E58: ldrb r1,[r0,#0x1c8] / cmp r2,#0
        static const u32 expectChan[2] = { 0xE5D011C8, 0xE3520000 };
        if (!MageCheckWords(HOOK_MAGE_S0, expectS0, 2) || !MageCheckWords(HOOK_MAGE_UNIT, expectUnit, 2) ||
            !MageCheckWords(HOOK_MAGE_TICK, expectTick, 2) || !MageCheckWords(HOOK_MAGE_CHAN, expectChan, 2))
        {
            MessageBox("エラー",
                       "魔法使い用のフック位置の命令が想定と違います。\n"
                       "魔法使い連射のARコードや、敵即死のARコードがONになっていないか確認し、OFFにしてゲームを再起動してください。\n"
                       "それでも出る場合は、ゲームのバージョンが異なる可能性があります。")();
            return false;
        }

        // 先にジャンプ先の値(2語目)を書き、そのあとで ldr pc 命令(1語目)を書く。
        Process::Write32(HOOK_MAGE_S0 + 4,   (u32)(uintptr_t)jp_hookMageS0);
        Process::Write32(HOOK_MAGE_S0,       0xE51FF004);   // ldr pc, [pc, #-4]
        Process::Write32(HOOK_MAGE_UNIT + 4, (u32)(uintptr_t)jp_hookMageUnit);
        Process::Write32(HOOK_MAGE_UNIT,     0xE51FF004);
        Process::Write32(HOOK_MAGE_TICK + 4, (u32)(uintptr_t)jp_hookMageTick);
        Process::Write32(HOOK_MAGE_TICK,     0xE51FF004);
        Process::Write32(HOOK_MAGE_CHAN + 4, (u32)(uintptr_t)jp_hookMageChan);
        Process::Write32(HOOK_MAGE_CHAN,     0xE51FF004);

        g_mageHooked = true;
        return true;
    }

    // 診断の表示 (手動・自動の両方のメニューから呼ぶ)
    static void MageNotify(void)
    {
        static u32 shownTick = 0, lastCasts = 0, lastAct = 0, lastAuto = 0;
        if (!shownTick && g_mageTicks)
        {
            shownTick = 1;
            OSD::Notify("Mage: hook OK");
        }
        if (g_mageActLog != lastAct)
        {
            lastAct = g_mageActLog;
            if (g_mageActLog <= 3)
                OSD::Notify(Utils::Format("Mage: cancel act %08X", (unsigned)g_mageLastAct));
        }
        if (g_mageCasts != lastCasts)
        {
            lastCasts = g_mageCasts;
            if (g_mageCasts <= 3)
                OSD::Notify(Utils::Format("Mage: cast #%u", (unsigned)g_mageCasts));
        }
        if (g_mageAutoCasts != lastAuto)
        {
            lastAuto = g_mageAutoCasts;
            if (g_mageAutoCasts <= 3)
                OSD::Notify(Utils::Format("Mage: auto #%u", (unsigned)g_mageAutoCasts));
        }
    }

    // 手動発動。有効な間、毎フレーム呼ばれる。メニューをOFFにすると呼ばれなくなり、
    // g_mageWatch が数フレームで0になって、機能が止まる。
    static void MageRapidEntry(MenuEntry *entry)
    {
        if (!InstallMageHooks())
        {
            entry->Disable();
            return;
        }
        g_mageWatch = 6;
        g_mageFrame = g_mageFrame + 1;
        if (g_mageAlive) g_mageAlive = g_mageAlive - 1;

        // A以外のボタンで発動する設定: 押した瞬間と押している間を、ここ(プラグイン側)で見る
        if (g_mageKey)
        {
            Key k = Key::B;
            if (g_mageKey == 2) k = Key::Y;
            else if (g_mageKey == 3) k = Key::L;
            else if (g_mageKey == 4) k = Key::R;
            if (Controller::IsKeyPressed(k))
                g_mageReq = g_mageReq + 1;
            g_mageHeld = Controller::IsKeyDown(k) ? 1 : 0;
        }
        else
        {
            g_mageHeld = 0;
        }
        MageNotify();
    }

    // 自動発動 (近づいた敵に対して)
    static void MageAutoEntry(MenuEntry *entry)
    {
        if (!InstallMageHooks())
        {
            entry->Disable();
            return;
        }
        g_mageAutoWatch = 6;
        g_mageFrame = g_mageFrame + 1;
        MageNotify();
    }

    static bool MageAskNumber(const std::string &title, u32 &value, u32 lo, u32 hi)
    {
        Keyboard kb(title + "\n今の値は " + std::to_string((unsigned)value) + " です。");
        kb.IsHexadecimal(false);
        u32 v = value;
        if (kb.Open(v, v) != 0)
            return false;
        if (v < lo) v = lo;
        if (v > hi) v = hi;
        value = v;
        return true;
    }

    // タイミングの設定 (手動発動)
    static void MageTimingEntry(MenuEntry *entry)
    {
        (void)entry;
        u32 v;
        v = g_mageCool;
        if (MageAskNumber("連続発動の間隔 (0〜600フレーム)\n0 = 1回押すと1回だけ (長押しで連射しない)\n1以上 = 長押しで、その間隔で連射する", v, 0, 600))
            g_mageCool = v;
        v = g_mageTmax;
        if (MageAskNumber("判定を作る最大フレーム (10〜600)\n魔法が途中で消えるなら増やす。", v, 10, 600))
            g_mageTmax = v;
        v = (u32)(g_mageRate * 100.0f + 0.5f);
        if (MageAskNumber("ゴーストの時間の進み方 (10〜1000 = 0.1〜10倍。100 = 等速)\n「必殺技IDと長さを調べる」でAR準拠を撃って出る w/f の値を100倍して入れると、AR準拠に近づく。", v, 10, 1000))
            g_mageRate = (float)v / 100.0f;
        v = g_mageStagger;
        if (MageAskNumber("同時に出す技のずらし (0〜5フレーム)\n選んだ技を、1つずつ、このフレーム数ずつ遅らせて出す。\n0 = 全部同じフレーム。全部選ぶと一部が出ないときは、1〜2にする。", v, 0, 5))
            g_mageStagger = v;
        v = g_mageUltTmax;
        if (MageAskNumber("必殺技の最大フレーム (10〜1800)\n隕石などが途中で止まるなら増やす。降りすぎるなら減らす。", v, 10, 1800))
            g_mageUltTmax = v;
        v = g_mageBudget;
        if (MageAskNumber("1フレームの処理の上限 (17〜200)\n重い・固まるときは減らす。減らしすぎると魔法が遅れる。", v, 17, 200))
            g_mageBudget = v;
        v = g_mageStep;
        if (MageAskNumber("魔法が出る速さ (1〜10)\n1 = 本来の速さ。大きいほど早く出る(遅延が減る)。", v, 1, 10))
            g_mageStep = v;
        v = g_mageCancelWin;
        if (MageAskNumber("通常攻撃を消す期間 (0〜120フレーム)\n0 = 消さない。発動してからこの間に始まる通常攻撃を消す。", v, 0, 120))
            g_mageCancelWin = v;
    }

    // 自動発動の設定
    static void MageAutoSettingsEntry(MenuEntry *entry)
    {
        (void)entry;
        u32 v = (u32)(__builtin_sqrtf(g_mageAutoDist2) + 0.5f);
        if (MageAskNumber("敵に近づいたと判定する距離 (1〜40)\n大きいほど遠くから出る。目安: 3 = すぐ近く、6 = 初期値", v, 1, 40))
            g_mageAutoDist2 = (float)(v * v);
        v = g_mageAutoCool;
        if (MageAskNumber("自動発動の間隔 (1〜600フレーム)\n敵が近くにいる間、この間隔で出し続ける。60 = 約1秒", v, 1, 600))
            g_mageAutoCool = v;
    }

    // 発動するボタン
    static void MageKeyEntry(MenuEntry *entry)
    {
        (void)entry;
        std::vector<std::string> opts;
        opts.push_back("A (ゲームのAを使う。通常攻撃は出ない)");
        opts.push_back("B (Aの通常攻撃はそのまま使える)");
        opts.push_back("Y");
        opts.push_back("L");
        opts.push_back("R");
        Keyboard kb("発動するボタンを選ぶ\n今は: " + opts[g_mageKey < 5 ? g_mageKey : 0], opts);
        const int r = kb.Open();
        if (r >= 0 && r < 5)
            g_mageKey = (u32)r;
    }

    // 出す技の選択 (押すと ON/OFF が切り替わる)。手動と自動で、別々の選択を持つ。
    static void MageSkillSelect(volatile u32 &mask, const std::string &title)
    {
        static const char *attr[4]  = { "火", "水", "風", "大地" };
        static const char *kind[4]  = { "Aチャージ 2段階目", "Aチャージ 1段階目", "Xチャージ 1段階目", "Xチャージ 2段階目" };
        while (true)
        {
            std::vector<std::string> opts;
            for (int i = 0; i < 16; ++i)
                opts.push_back(std::string(((mask >> i) & 1) ? "[ON]  " : "[OFF] ") + attr[i % 4] + " " + kind[i / 4]);
            opts.push_back(std::string(((mask >> 16) & 1) ? "[ON]  " : "[OFF] ") + "Xの必殺技" + (g_mageUltId ? "" : " (IDが未設定)"));
            opts.push_back("全部 ON");
            opts.push_back("全部 OFF");
            opts.push_back("A系だけ ON");
            opts.push_back("X系だけ ON");
            opts.push_back("1段階目だけ ON");
            opts.push_back("2段階目だけ ON");
            opts.push_back("戻る");
            Keyboard kb(title + " (押すと切り替わる)", opts);
            const int r = kb.Open();
            if (r < 0 || r == 23)
                break;
            if (r < 17)
                mask = mask ^ (1u << r);
            else if (r == 17) mask = 0x1FFFF;
            else if (r == 18) mask = 0;
            else if (r == 19) mask = 0x00FF;     // A系: ビット0-7 (A2段階目 + A1段階目)
            else if (r == 20) mask = 0xFF00;     // X系: ビット8-15
            else if (r == 21) mask = 0x0FF0;     // 1段階目: ビット4-7 (A1) + 8-11 (X1)
            else if (r == 22) mask = 0xF00F;     // 2段階目: ビット0-3 (A2) + 12-15 (X2)
        }
    }

    // 魔法の位置の設定 (実験)
    static void MagePosEntry(MenuEntry *entry)
    {
        (void)entry;
        u32 v = (u32)(g_mageFwdF < 0 ? -g_mageFwdF : g_mageFwdF);
        // 前方へずらす距離: 0〜40 を前方、41〜80 を後ろ (値 − 40 の距離) として入力する
        u32 cur = g_mageFwdF >= 0 ? v : 40 + v;
        if (MageAskNumber("魔法を出す位置を前へずらす距離 (0〜80)\n0 = ずらさない。1〜40 = 前へ。41〜80 = 後ろへ (値−40)。\n前 = 直前に動いた向きです。", cur, 0, 80))
            g_mageFwdF = cur <= 40 ? (float)cur : -(float)(cur - 40);
        v = g_mageDirs;
        if (MageAskNumber("同時に出す方向の数 (1 / 4 / 8)\n1 = 今まで通り。4 = 前・右・後ろ・左。8 = 斜めも。\n動いている魔法の数が増えるので、重くなります。", v, 1, 8))
            g_mageDirs = (v >= 8) ? 8 : (v >= 4 ? 4 : 1);
        v = (u32)g_mageDirD;
        if (MageAskNumber("方向ごとにずらす距離 (1〜40)\n方向の数が4か8のとき、プレイヤーからこの距離だけ離れた位置から出す。", v, 1, 40))
            g_mageDirD = (float)v;
    }

    // 全敵ターゲットの設定
    static void MageTargetEntry(MenuEntry *entry)
    {
        (void)entry;
        u32 v = g_mageTgtAll;
        if (MageAskNumber("周りの敵ぜんぶに向けて魔法を出す (0 = オフ, 1 = オン)\nターゲットは変えず、近い順に敵ごとに同じ魔法を出す。\n敵の数だけ処理が増えて重くなります。", v, 0, 1))
            g_mageTgtAll = v;
        v = g_mageTgtMax;
        if (MageAskNumber("向ける敵の最大数 (1〜8)\n近い順に、この数まで。", v, 1, 8))
            g_mageTgtMax = v;
        v = (u32)(__builtin_sqrtf(g_mageTgtRange2) + 0.5f);
        if (MageAskNumber("向ける敵の距離 (5〜60)\nプレイヤーからこの距離の中にいる敵だけ。", v, 5, 60))
            g_mageTgtRange2 = (float)(v * v);
        v = g_mageTgtMove;
        if (MageAskNumber("魔法の出る位置も、敵の方へずらす (0 = オフ, 1 = オン)\nターゲットだけでは敵に向かない技 (火など) を、敵の位置から出す。", v, 0, 1))
            g_mageTgtMove = v;
        v = (u32)(g_mageTgtMoveR * 100.0f + 0.5f);
        if (MageAskNumber("敵の方へずらす割合 (10〜100 %)\n100 = 敵の位置から出す。50 = 敵とプレイヤーの真ん中から出す。", v, 10, 100))
            g_mageTgtMoveR = (float)v / 100.0f;
        {
            Keyboard kb("敵として扱う、追加の種類 (16進)\n0 = 追加なし。ボスの種類は「周りのユニットの種類を調べる」で分かる。\n今の値は " +
                        Utils::Format("%02X", (unsigned)g_mageEnemyCatX) + " です。");
            kb.IsHexadecimal(true);
            u32 x = g_mageEnemyCatX;
            if (kb.Open(x, x) == 0)
                g_mageEnemyCatX = x & 0xFF;
        }
    }

    // 自動で覚えた敵の種類を消す
    static void MageForgetCatEntry(MenuEntry *entry)
    {
        (void)entry;
        for (int i = 0; i < 8; ++i)
            g_mageCatBits[i] = 0;
        OSD::Notify("Mage: forgot enemy types");
    }

    // AR準拠を、ARコードと同じ動きにする設定へ戻す
    static void MageArPresetEntry(MenuEntry *entry)
    {
        (void)entry;
        g_mageArN       = 30;         // 「押している扱い」の上限フレーム数 (状態4になれば、それより早く離す)
        g_mageArStagger = 0;          // 全部同じ時間で出す (ARコードと同じ)
        g_mageArMask    = 0x0FFFF;    // 表の16個だけ。必殺技は外す (ARコードにはない)
        g_mageFwdF      = 0.0f;       // 位置はずらさない
        g_mageDirs      = 1;          // 方向は1つ
        g_mageClear     = 1;
        OSD::Notify("Mage: AR style = AR code");
    }

    // 手動の設定を、AR準拠の動きに近い値にまとめて切り替える
    static void MagePresetEntry(MenuEntry *entry)
    {
        (void)entry;
        g_mageCool      = 0;      // 1回押すと1回だけ
        g_mageTmax      = 100;    // Aスキル1回ぶんくらいの長さ
        g_mageStep      = 1;      // 本来の速さ
        g_mageCancelWin = 40;     // 通常攻撃を消す
        g_mageBudget    = 48;     // 1フレームの処理の上限
        g_mageUltTmax   = 300;    // 必殺技は約5秒
        g_mageClear     = 1;      // 今動いているものは止める
        OSD::Notify("Mage: preset (AR-like)");
    }

    // 動いているゴーストを全部止める
    static void MageStopEntry(MenuEntry *entry)
    {
        (void)entry;
        g_mageClear = 1;
        OSD::Notify("Mage: stop");
    }

    // Xの必殺技のIDの設定
    static void MageUltIdEntry(MenuEntry *entry)
    {
        (void)entry;
        Keyboard kb("Xの必殺技のアクションID (16進)\n0 = 使わない。調べ方は「必殺技IDを調べる」を使う。\n今の値は " +
                    Utils::Format("%08X", (unsigned)g_mageUltId) + " です。");
        kb.IsHexadecimal(true);
        u32 v = g_mageUltId;
        if (kb.Open(v, v) == 0)
        {
            g_mageUltId = v;
            if (v)
            {
                g_mageMask = g_mageMask | 0x10000;
                g_mageAutoMask = g_mageAutoMask | 0x10000;
                g_mageArMask = g_mageArMask | 0x10000;
            }
        }
    }

    // 動作が切り替わるたびに、直前のアクションのIDと「続いたフレーム数」と、そのときの時間を画面に出す
    // (必殺技のIDと本来の長さ、AR準拠の時間の進み方を調べるための補助。待機(ID 1)は出さない)
    //   F = 続いたフレーム数, t = アクションの時間 (K+4), w = 魔法の判定用の時間 (K+0xC4) の増え方
    static void MageLearnEntry(MenuEntry *entry)
    {
        (void)entry;
        static u32 last = 0, frames = 0;
        static float t4 = 0.0f, wStart = 0.0f, wLast = 0.0f;
        const u32 C = MageObj();
        if (!C)
            return;
        const u32 K = C + 0x558;
        const u32 id = *(volatile u32 *)(K + 8);
        const float cur4 = *(volatile float *)(K + 4);
        const float curW = *(volatile float *)(K + 0xC4);
        if (id != last)
        {
            if (last && last != 1)
            {
                const float rate = frames > 1 ? (wLast - wStart) / (float)(frames - 1) : 0.0f;
                OSD::Notify(Utils::Format("Act %08X F=%u t=%d w/f=%d.%02d", (unsigned)last, (unsigned)frames, (int)t4,
                                          (int)rate, (int)((rate < 0 ? -rate : rate) * 100.0f) % 100));
            }
            last = id;
            frames = 0;
            wStart = curW;
        }
        ++frames;
        t4 = cur4;
        wLast = curW;
    }

    // ターゲットの場所を調べる: プレイヤーのデータの中から、「周りの敵のアドレスと同じ値」が入っている場所を探して、
    // 画面に出す。戦闘モードでYボタンを押してターゲットを変えると、出る距離 d が変わる場所が、ターゲットの場所。
    static void MageTgtEntry(MenuEntry *entry)
    {
        if (!InstallMageHooks())
        {
            entry->Disable();
            return;
        }
        g_mageTgtWatch = 6;
        static u32 n = 0;
        static u32 lastSig = 0;
        if (++n % 20)
            return;
        const u32 C = MageObj();
        if (!C || !g_mageEnemyN)
            return;
        u32 sig = 0, shown = 0;
        for (u32 off = 0; off < 0x1800 && shown < 3; off += 4)
        {
            u32 v = 0;
            if (!Process::Read32(C + off, v) || v < 0x08000000u || v >= 0x1F000000u)
                continue;
            for (u32 j = 0; j < g_mageEnemyN; ++j)
            {
                if (v == g_mageEnemy[j])
                {
                    const int d = (int)__builtin_sqrtf(g_mageEnemyD2[j]);
                    sig = sig * 31 + off + (u32)d * 7919;
                    if (sig != lastSig || true)
                        OSD::Notify(Utils::Format("TGT +%X d=%d (enemies %u)", (unsigned)off, d, (unsigned)g_mageEnemyN));
                    ++shown;
                    break;
                }
            }
        }
        lastSig = sig;
    }

    // 周りのユニットの種類を調べる: ボスなどが、敵として扱われていないときの調査用。
    // 出る c = 種類 (+0x3BC)、f = フラグ (+0x554)、d = プレイヤーからの距離。
    static void MageUnitInfoEntry(MenuEntry *entry)
    {
        if (!InstallMageHooks())
        {
            entry->Disable();
            return;
        }
        g_mageTgtWatch = 6;
        static u32 n = 0;
        if (++n % 60)
            return;
        // 近い順に、最大4つ表示する
        bool used[16];
        for (int i = 0; i < 16; ++i) used[i] = false;
        for (int k = 0; k < 4; ++k)
        {
            int best = -1;
            float bd = 0.0f;
            for (u32 j = 0; j < g_mageUnitN && j < 16; ++j)
                if (!used[j] && (best < 0 || g_mageUnitD2[j] < bd)) { best = (int)j; bd = g_mageUnitD2[j]; }
            if (best < 0)
                break;
            used[best] = true;
            OSD::Notify(Utils::Format("U c=%02X f=%u d=%d", (unsigned)g_mageUnitCat[best], (unsigned)g_mageUnitFlag[best],
                                      (int)__builtin_sqrtf(g_mageUnitD2[best])));
        }
    }

    // 診断: 1秒ごとに内部の数を画面に出す (T=進行の回数 Q=ボタン押下の数 G=動いている魔法 C=発動の数)
    static void MageDiagEntry(MenuEntry *entry)
    {
        (void)entry;
        static u32 n = 0, pu = 0, pt = 0, pc = 0, pd = 0;
        if (++n % 60)
            return;
        u32 g = 0;
        for (int i = 0; i < MAGE_MAXG; ++i)
            if (g_mageT[i] >= 0)
                ++g;
        // 直前の1秒間に、各フックが何回呼ばれたか (U=ユニット更新 A=アクション更新 I=入力更新 D=魔法の進行)
        OSD::Notify(Utils::Format("U=%u A=%u I=%u D=%u G=%u Q=%u C=%u",
                                  (unsigned)(g_mageCntUnit - pu), (unsigned)(g_mageCntTick - pt),
                                  (unsigned)(g_mageCntChan - pc), (unsigned)(g_mageTicks - pd),
                                  (unsigned)g, (unsigned)g_mageReq, (unsigned)g_mageCasts));
        pu = g_mageCntUnit; pt = g_mageCntTick; pc = g_mageCntChan; pd = g_mageTicks;
    }

    static void MageSkillEntry(MenuEntry *entry)     { (void)entry; MageSkillSelect(g_mageMask, "手動で出す技"); }
    static void MageAutoSkillEntry(MenuEntry *entry) { (void)entry; MageSkillSelect(g_mageAutoMask, "自動で出す技"); }

    // AR準拠モード (プレイヤーのモーションあり)
    static void MageArEntry(MenuEntry *entry)
    {
        static u32 lastAr = 0;
        if (!InstallMageHooks())
        {
            entry->Disable();
            return;
        }
        g_mageArWatch = 6;
        g_mageFrame = g_mageFrame + 1;
        if (g_mageAlive) g_mageAlive = g_mageAlive - 1;

        // A以外のボタンで発動する設定: 押した瞬間をここ(プラグイン側)で数える
        if (g_mageArKey)
        {
            Key k = Key::X;
            if (g_mageArKey == 2) k = Key::B;
            else if (g_mageArKey == 3) k = Key::Y;
            else if (g_mageArKey == 4) k = Key::L;
            else if (g_mageArKey == 5) k = Key::R;
            if (Controller::IsKeyPressed(k))
                g_mageArReq = g_mageArReq + 1;
        }
        MageNotify();
        if (g_mageArCasts != lastAr)
        {
            lastAr = g_mageArCasts;
            if (g_mageArCasts <= 2)
                OSD::Notify("Mage: AR style cast");
        }
    }

    static void MageArKeyEntry(MenuEntry *entry)
    {
        (void)entry;
        std::vector<std::string> opts;
        opts.push_back("A");
        opts.push_back("X");
        opts.push_back("B");
        opts.push_back("Y");
        opts.push_back("L");
        opts.push_back("R");
        Keyboard kb("AR準拠の発動ボタン\n今は: " + opts[g_mageArKey < 6 ? g_mageArKey : 0], opts);
        const int r = kb.Open();
        if (r >= 0 && r < 6)
            g_mageArKey = (u32)r;
    }

    static void MageArSkillEntry(MenuEntry *entry) { (void)entry; MageSkillSelect(g_mageArMask, "AR準拠で出す技"); }

    static void MageArSettingsEntry(MenuEntry *entry)
    {
        (void)entry;
        u32 v = g_mageArN;
        if (MageAskNumber("押している扱いにするフレーム数 (3〜60)\n溜めが足りず通常攻撃になるときは増やす。", v, 3, 60))
            g_mageArN = v;
        v = g_mageArStagger;
        if (MageAskNumber("AR準拠: 同時に出す技のずらし (0〜5フレーム)\n0 = ARコードと同じ(全部同じ時間)。全部選ぶと一部が出ないときだけ、1〜2にする。", v, 0, 5))
            g_mageArStagger = v;
    }

    static void AddMageRapid(PluginMenu *menu)
    {
        MenuFolder *f = new MenuFolder("魔法使い連射 (実験)",
                                       "魔法使いで、チャージ技(A/X・1段階/2段階・各属性)を、"
                                       "プレイヤーのモーションなしで出します。");
        f->Append(new MenuEntry("魔法使い: モーションなしでチャージ技を出す", MageRapidEntry, nullptr,
                                "魔法使いで有効にして、フィールドで発動ボタンを押します(初期はAボタン)。\n"
                                "以前の魔法使い連射のARコードと、敵即死のARコードとは同時に使えません。ARをOFFにして、ゲームを再起動してから使ってください。\n"
                                "止める場合は、このメニューをOFFにします(フック自体はゲームを再起動するまで残ります)。"));
        f->Append(new MenuEntry("発動するボタンを選ぶ (A / B / Y / L / R)", nullptr, MageKeyEntry,
                                "発動に使うボタンを選びます。"));
        f->Append(new MenuEntry("手動で出す技を選ぶ (属性・A/X・段階)", nullptr, MageSkillEntry,
                                "出す技を1つずつ選びます。"));
        f->Append(new MenuEntry("手動のタイミング設定 (間隔・最大フレーム・速さ・通常攻撃消し)", nullptr, MageTimingEntry,
                                "連続発動の間隔(0で1回押すと1回だけ)、判定を作る最大フレーム、魔法が出る速さ、通常攻撃を消す期間を変えます。"));
        f->Append(new MenuEntry("近づいたら自動でチャージ技を出す (敵)", MageAutoEntry, nullptr,
                                "敵が近づくと、自動でチャージ技を出します。プレイヤーのモーションは出ません。\n"
                                "上の「魔法使い: モーションなし…」をONにしなくても使えます。\n"
                                "ARコード(魔法使い連射・敵即死)とは同時に使えません。"));
        f->Append(new MenuEntry("自動で出す技を選ぶ", nullptr, MageAutoSkillEntry,
                                "自動発動で出す技を、1つずつON/OFFできます。"));
        f->Append(new MenuEntry("自動発動の設定 (距離・間隔)", nullptr, MageAutoSettingsEntry,
                                "敵に近づいたと判定する距離と、自動発動の間隔を変えます。"));
        f->Append(new MenuEntry("魔法の位置の設定 (前へずらす・同時方向)", nullptr, MagePosEntry,
                                "魔法を出す位置を前後にずらしたり、同じ技を前後左右に同時に出したりします(実験)。"));
        f->Append(new MenuEntry("手動の設定をAR準拠に近い値にする", nullptr, MagePresetEntry,
                                "手動発動の間隔、最大フレーム、速さなどを、おすすめの値にまとめて戻します。"));
        f->Append(new MenuEntry("動いている魔法を全部止める", nullptr, MageStopEntry,
                                "降り続けている魔法を止めます。重いときに使います。"));
        f->Append(new MenuEntry("Xの必殺技のIDを設定 (16進)", nullptr, MageUltIdEntry,
                                "Xの必殺技のアクションIDを入れます。入れると、技の選択で選べます。"));
        f->Append(new MenuEntry("必殺技IDと長さを調べる (画面に出す)", MageLearnEntry, nullptr,
                                "ONにして、Xの必殺技を撃つと、動作のIDと続いたフレーム数が画面の上に出ます。"));
        f->Append(new MenuEntry("全敵ターゲットの設定 (周りの敵ぜんぶへ)", nullptr, MageTargetEntry,
                                "ターゲットを変えずに、周りの敵ぜんぶに向けて、同じ魔法を出します(実験)。"));
        f->Append(new MenuEntry("覚えた敵の種類を消す", nullptr, MageForgetCatEntry,
                                "ターゲットした相手から自動で覚えた、敵の種類を消します。"));
        f->Append(new MenuEntry("周りのユニットの種類を調べる (画面に出す)", MageUnitInfoEntry, nullptr,
                                "ONにすると、近い順に4つのユニットの種類 c と距離 d が、1秒ごとに画面の上に出ます。ボスの c を調べるために使います。"));
        f->Append(new MenuEntry("ターゲットの場所を調べる (画面に出す)", MageTgtEntry, nullptr,
                                "戦闘モードでONにして、Yボタンでターゲットを変えると、画面の上に出る値が変わります。"));
        f->Append(new MenuEntry("診断 (1秒ごとに数を表示)", MageDiagEntry, nullptr,
                                "内部の数を1秒ごとに画面に出します。動作がおかしいときの調査用です。"));
        f->Append(new MenuEntry("AR準拠: Aスキル+全属性 (モーションあり)", MageArEntry, nullptr,
                                "以前のARコードと同じ動きです。Aのスキルを2段階目まで一瞬で溜めて撃ち、ほかの技も同時に出します。\n"
                                "魔法使い連射・敵即死のARコードとは同時に使えません。"));
        f->Append(new MenuEntry("AR準拠の発動ボタン (A / X / B / Y / L / R)", nullptr, MageArKeyEntry,
                                "AR準拠モードを発動するボタンを選びます。"));
        f->Append(new MenuEntry("AR準拠を、ARコードと同じ設定に戻す", nullptr, MageArPresetEntry,
                                "AR準拠の設定を、ARコードと同じ動きになる値に戻します。必殺技・位置のずらし・方向は外れます。"));
        f->Append(new MenuEntry("AR準拠で出す技を選ぶ", nullptr, MageArSkillEntry,
                                "AR準拠モードで、同時に出す技を選びます。"));
        f->Append(new MenuEntry("AR準拠の設定 (押している扱いのフレーム数)", nullptr, MageArSettingsEntry,
                                "溜めの再現に使うフレーム数を変えます。"));
        menu->Append(f);
    }
}
