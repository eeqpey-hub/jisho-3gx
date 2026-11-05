// JpInput.hpp  --  日本語入力 (ローマ字 → ひらがな / カタカナ、Google で漢字変換)
//
// ItemEditor.hpp から include される。CTRPF の画面キーボード (英数字のみ) で
// ローマ字を打つと、ひらがな・カタカナに直して選べる。漢字は、ネットにつながるときだけ
// Google 日本語入力 (transliterate) で変換できる。
//
//  Jp::AskJapanese(title, out)   日本語の文字列を1つ入力する。決定したら true (out は UTF-8)
//  Jp::KanaVariants(text)        検索用: ひらがなならカタカナも、カタカナならひらがなも返す
//
// 漢字変換は libctru の httpc を直接使う。次のときは、メッセージを出して使えない。
//   ・Wi-Fi につながっていない
//   ・このゲームのプロセスに、HTTP サービスへのアクセス権が無い (httpcInit が失敗)
//   ・Google 側と、3DS の TLS が合わない (https が失敗 → http を試す)
// 漢字変換が使えなくても、ひらがな・カタカナ・英数字の入力は、そのまま使える。
// 漢字変換を完全に切りたいときは、ItemEditor.hpp を include する前に
//   #define JPINPUT_NO_GOOGLE 1
// と書く。
#pragma once

namespace CTRPluginFramework
{
namespace Jp
{
    // ---------------- UTF-8 と コードポイント ----------------
    static void Utf8Decode(const std::string &s, std::vector<u32> &out)
    {
        out.clear();
        size_t i = 0;
        while (i < s.size())
        {
            const u8 c = (u8)s[i];
            u32 cp = 0;
            size_t n = 0;
            if (c < 0x80)                { cp = c;        n = 1; }
            else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 2; }
            else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 3; }
            else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 4; }
            else { ++i; continue; }
            if (i + n > s.size())
                break;
            for (size_t k = 1; k < n; ++k)
                cp = (cp << 6) | ((u8)s[i + k] & 0x3F);
            i += n;
            out.push_back(cp);
        }
    }

    static void Utf8Append(std::string &o, u32 cp)
    {
        if (cp < 0x80)
            o += (char)cp;
        else if (cp < 0x800)
        {
            o += (char)(0xC0 | (cp >> 6));
            o += (char)(0x80 | (cp & 0x3F));
        }
        else if (cp < 0x10000)
        {
            o += (char)(0xE0 | (cp >> 12));
            o += (char)(0x80 | ((cp >> 6) & 0x3F));
            o += (char)(0x80 | (cp & 0x3F));
        }
        else
        {
            o += (char)(0xF0 | (cp >> 18));
            o += (char)(0x80 | ((cp >> 12) & 0x3F));
            o += (char)(0x80 | ((cp >> 6) & 0x3F));
            o += (char)(0x80 | (cp & 0x3F));
        }
    }

    // ---------------- ひらがな ⇔ カタカナ ----------------
    static std::string HiraganaToKatakana(const std::string &s)
    {
        std::vector<u32> cps;
        Utf8Decode(s, cps);
        std::string o;
        for (u32 c : cps)
        {
            if ((c >= 0x3041 && c <= 0x3096) || c == 0x309D || c == 0x309E)
                c += 0x60;
            Utf8Append(o, c);
        }
        return o;
    }

    static std::string KatakanaToHiragana(const std::string &s)
    {
        std::vector<u32> cps;
        Utf8Decode(s, cps);
        std::string o;
        for (u32 c : cps)
        {
            if ((c >= 0x30A1 && c <= 0x30F6) || c == 0x30FD || c == 0x30FE)
                c -= 0x60;
            Utf8Append(o, c);
        }
        return o;
    }

    static bool IsAllHiragana(const std::vector<u32> &v)
    {
        if (v.empty())
            return false;
        for (u32 c : v)
            if (!((c >= 0x3041 && c <= 0x3096) || c == 0x30FC || c == 0x309D || c == 0x309E))
                return false;
        return true;
    }

    static bool IsAllKatakana(const std::vector<u32> &v)
    {
        if (v.empty())
            return false;
        for (u32 c : v)
            if (!((c >= 0x30A1 && c <= 0x30F6) || c == 0x30FC || c == 0x30FD || c == 0x30FE))
                return false;
        return true;
    }

    // 検索用: ひらがなを渡すとカタカナも、カタカナを渡すとひらがなも、いっしょに返す
    static std::vector<std::string> KanaVariants(const std::string &q)
    {
        std::vector<std::string> r;
        if (q.empty())
            return r;
        r.push_back(q);
        std::vector<u32> cps;
        Utf8Decode(q, cps);
        if (IsAllHiragana(cps))
            r.push_back(HiraganaToKatakana(q));
        else if (IsAllKatakana(cps))
            r.push_back(KatakanaToHiragana(q));
        return r;
    }

    // ---------------- ローマ字 → ひらがな ----------------
    struct RomajiRule
    {
        const char *r;
        const char *k;
    };

    static const RomajiRule ROMAJI_RULES[] =
    {
        // 母音
        { "a", "あ" }, { "i", "い" }, { "u", "う" }, { "e", "え" }, { "o", "お" },
        // か行
        { "ka", "か" }, { "ki", "き" }, { "ku", "く" }, { "ke", "け" }, { "ko", "こ" },
        { "kya", "きゃ" }, { "kyi", "きぃ" }, { "kyu", "きゅ" }, { "kye", "きぇ" }, { "kyo", "きょ" },
        { "ca", "か" }, { "cu", "く" }, { "co", "こ" }, { "ci", "し" }, { "ce", "せ" },
        { "qa", "くぁ" }, { "qi", "くぃ" }, { "qu", "く" }, { "qe", "くぇ" }, { "qo", "くぉ" },
        // が行
        { "ga", "が" }, { "gi", "ぎ" }, { "gu", "ぐ" }, { "ge", "げ" }, { "go", "ご" },
        { "gya", "ぎゃ" }, { "gyi", "ぎぃ" }, { "gyu", "ぎゅ" }, { "gye", "ぎぇ" }, { "gyo", "ぎょ" },
        // さ行
        { "sa", "さ" }, { "si", "し" }, { "shi", "し" }, { "su", "す" }, { "se", "せ" }, { "so", "そ" },
        { "sya", "しゃ" }, { "syi", "しぃ" }, { "syu", "しゅ" }, { "sye", "しぇ" }, { "syo", "しょ" },
        { "sha", "しゃ" }, { "shu", "しゅ" }, { "she", "しぇ" }, { "sho", "しょ" },
        // ざ行
        { "za", "ざ" }, { "zi", "じ" }, { "ji", "じ" }, { "zu", "ず" }, { "ze", "ぜ" }, { "zo", "ぞ" },
        { "ja", "じゃ" }, { "ju", "じゅ" }, { "je", "じぇ" }, { "jo", "じょ" },
        { "jya", "じゃ" }, { "jyi", "じぃ" }, { "jyu", "じゅ" }, { "jye", "じぇ" }, { "jyo", "じょ" },
        { "zya", "じゃ" }, { "zyi", "じぃ" }, { "zyu", "じゅ" }, { "zye", "じぇ" }, { "zyo", "じょ" },
        // た行
        { "ta", "た" }, { "ti", "ち" }, { "chi", "ち" }, { "tu", "つ" }, { "tsu", "つ" }, { "te", "て" }, { "to", "と" },
        { "tya", "ちゃ" }, { "tyi", "ちぃ" }, { "tyu", "ちゅ" }, { "tye", "ちぇ" }, { "tyo", "ちょ" },
        { "cha", "ちゃ" }, { "chu", "ちゅ" }, { "che", "ちぇ" }, { "cho", "ちょ" },
        { "cya", "ちゃ" }, { "cyi", "ちぃ" }, { "cyu", "ちゅ" }, { "cye", "ちぇ" }, { "cyo", "ちょ" },
        { "tsa", "つぁ" }, { "tsi", "つぃ" }, { "tse", "つぇ" }, { "tso", "つぉ" },
        { "tha", "てゃ" }, { "thi", "てぃ" }, { "thu", "てゅ" }, { "the", "てぇ" }, { "tho", "てょ" },
        { "twu", "とぅ" },
        // だ行
        { "da", "だ" }, { "di", "ぢ" }, { "du", "づ" }, { "de", "で" }, { "do", "ど" },
        { "dya", "ぢゃ" }, { "dyi", "ぢぃ" }, { "dyu", "ぢゅ" }, { "dye", "ぢぇ" }, { "dyo", "ぢょ" },
        { "dha", "でゃ" }, { "dhi", "でぃ" }, { "dhu", "でゅ" }, { "dhe", "でぇ" }, { "dho", "でょ" },
        { "dwu", "どぅ" },
        // な行
        { "na", "な" }, { "ni", "に" }, { "nu", "ぬ" }, { "ne", "ね" }, { "no", "の" },
        { "nya", "にゃ" }, { "nyi", "にぃ" }, { "nyu", "にゅ" }, { "nye", "にぇ" }, { "nyo", "にょ" },
        // は行
        { "ha", "は" }, { "hi", "ひ" }, { "hu", "ふ" }, { "fu", "ふ" }, { "he", "へ" }, { "ho", "ほ" },
        { "hya", "ひゃ" }, { "hyi", "ひぃ" }, { "hyu", "ひゅ" }, { "hye", "ひぇ" }, { "hyo", "ひょ" },
        { "fa", "ふぁ" }, { "fi", "ふぃ" }, { "fe", "ふぇ" }, { "fo", "ふぉ" },
        { "fya", "ふゃ" }, { "fyu", "ふゅ" }, { "fyo", "ふょ" },
        // ば行・ぱ行
        { "ba", "ば" }, { "bi", "び" }, { "bu", "ぶ" }, { "be", "べ" }, { "bo", "ぼ" },
        { "bya", "びゃ" }, { "byi", "びぃ" }, { "byu", "びゅ" }, { "bye", "びぇ" }, { "byo", "びょ" },
        { "pa", "ぱ" }, { "pi", "ぴ" }, { "pu", "ぷ" }, { "pe", "ぺ" }, { "po", "ぽ" },
        { "pya", "ぴゃ" }, { "pyi", "ぴぃ" }, { "pyu", "ぴゅ" }, { "pye", "ぴぇ" }, { "pyo", "ぴょ" },
        // ま行
        { "ma", "ま" }, { "mi", "み" }, { "mu", "む" }, { "me", "め" }, { "mo", "も" },
        { "mya", "みゃ" }, { "myi", "みぃ" }, { "myu", "みゅ" }, { "mye", "みぇ" }, { "myo", "みょ" },
        // や行・わ行
        { "ya", "や" }, { "yu", "ゆ" }, { "yo", "よ" },
        { "wa", "わ" }, { "wi", "うぃ" }, { "we", "うぇ" }, { "wo", "を" }, { "wu", "う" },
        { "wha", "うぁ" }, { "whi", "うぃ" }, { "whu", "う" }, { "whe", "うぇ" }, { "who", "うぉ" },
        // ら行
        { "ra", "ら" }, { "ri", "り" }, { "ru", "る" }, { "re", "れ" }, { "ro", "ろ" },
        { "rya", "りゃ" }, { "ryi", "りぃ" }, { "ryu", "りゅ" }, { "rye", "りぇ" }, { "ryo", "りょ" },
        // ゔ
        { "va", "ゔぁ" }, { "vi", "ゔぃ" }, { "vu", "ゔ" }, { "ve", "ゔぇ" }, { "vo", "ゔぉ" },
        // 小さい文字
        { "xa", "ぁ" }, { "xi", "ぃ" }, { "xu", "ぅ" }, { "xe", "ぇ" }, { "xo", "ぉ" },
        { "la", "ぁ" }, { "li", "ぃ" }, { "lu", "ぅ" }, { "le", "ぇ" }, { "lo", "ぉ" },
        { "xya", "ゃ" }, { "xyu", "ゅ" }, { "xyo", "ょ" }, { "lya", "ゃ" }, { "lyu", "ゅ" }, { "lyo", "ょ" },
        { "xtu", "っ" }, { "ltu", "っ" }, { "xtsu", "っ" }, { "ltsu", "っ" }, { "xwa", "ゎ" }, { "lwa", "ゎ" },
    };

    static bool IsVowelCh(char c) { return c == 'a' || c == 'i' || c == 'u' || c == 'e' || c == 'o'; }
    static bool IsAlphaCh(char c) { return c >= 'a' && c <= 'z'; }

    // ローマ字 (英字は大文字小文字を区別しない) → ひらがな。わからない文字はそのまま残す。
    //   kk, tt などの子音の連続 → っ、 nn / n' → ん、 - → ー、 , → 、 . → 。
    static std::string RomajiToHiragana(const std::string &in)
    {
        std::string s;
        for (char c : in)
            s += (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);

        std::string out;
        const size_t n = s.size();
        size_t i = 0;
        while (i < n)
        {
            const char c = s[i];

            // 促音 (同じ子音が2つ続く。n は除く)
            if (i + 1 < n && c == s[i + 1] && IsAlphaCh(c) && !IsVowelCh(c) && c != 'n')
            {
                out += "っ";
                ++i;
                continue;
            }
            if (c == 't' && i + 2 < n && s[i + 1] == 'c' && s[i + 2] == 'h')   // tch → っち
            {
                out += "っ";
                ++i;
                continue;
            }

            // ん
            if (c == 'n')
            {
                if (i + 1 >= n)
                {
                    out += "ん";
                    ++i;
                    continue;
                }
                const char d = s[i + 1];
                if (d == 'n')
                {
                    out += "ん";
                    // 「nna」は「んな」。「nn」のあとが母音・y なら、2つ目の n は次の音に使う
                    i += (i + 2 < n && (IsVowelCh(s[i + 2]) || s[i + 2] == 'y')) ? 1 : 2;
                    continue;
                }
                if (d == '\'')
                {
                    out += "ん";
                    i += 2;
                    continue;
                }
                if (!IsVowelCh(d) && d != 'y')
                {
                    out += "ん";
                    ++i;
                    continue;
                }
            }

            // 最長一致
            bool hit = false;
            for (size_t len = 4; len >= 1 && !hit; --len)
            {
                if (i + len > n)
                    continue;
                for (const RomajiRule &r : ROMAJI_RULES)
                {
                    if (std::strlen(r.r) == len && s.compare(i, len, r.r) == 0)
                    {
                        out += r.k;
                        i += len;
                        hit = true;
                        break;
                    }
                }
            }
            if (hit)
                continue;

            switch (c)
            {
                case '-': out += "ー"; break;
                case ',': out += "、"; break;
                case '.': out += "。"; break;
                case '?': out += "？"; break;
                case '!': out += "！"; break;
                case '~': out += "〜"; break;
                case '/': out += "・"; break;
                case '[': out += "「"; break;
                case ']': out += "」"; break;
                case '\'': break;           // 「n'」の区切りだけで余った ' は捨てる
                default: out += c; break;
            }
            ++i;
        }
        return out;
    }

    // ---------------- Google 日本語入力 (漢字変換) ----------------
    struct Segment
    {
        std::string              src;      // 変換前 (ひらがな)
        std::vector<std::string> cands;    // 変換候補
    };

#ifndef JPINPUT_NO_GOOGLE
    static std::string PercentEncode(const std::string &s)
    {
        static const char *HEX = "0123456789ABCDEF";
        std::string o;
        for (char ch : s)
        {
            const u8 c = (u8)ch;
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                c == '-' || c == '_' || c == '.' || c == '~')
                o += (char)c;
            else
            {
                o += '%';
                o += HEX[c >> 4];
                o += HEX[c & 15];
            }
        }
        return o;
    }

    // URL から本文を取る (最大 64KB)。リダイレクト (301/302) は4回まで追う。
    static bool HttpGet(std::string url, std::string &body, std::string &err)
    {
        Result rc = httpcInit(0);
        if (R_FAILED(rc))
        {
            err = Utils::Format("ネット接続の準備に失敗 (0x%08X)。\nこのゲームでは、ネットを使う権限がないかもしれません。", (u32)rc);
            return false;
        }

        bool ok = false;
        for (int hop = 0; hop < 5 && !ok; ++hop)
        {
            httpcContext ctx;
            rc = httpcOpenContext(&ctx, HTTPC_METHOD_GET, url.c_str(), 1);
            if (R_FAILED(rc))
            {
                err = Utils::Format("接続を開けません (0x%08X)", (u32)rc);
                break;
            }
            httpcSetSSLOpt(&ctx, SSLCOPT_DisableVerify);
            httpcAddRequestHeaderField(&ctx, "User-Agent", "Mozilla/5.0");

            rc = httpcBeginRequest(&ctx);
            if (R_FAILED(rc))
            {
                err = Utils::Format("接続できません (0x%08X)。Wi-Fiを確認してください。", (u32)rc);
                httpcCloseContext(&ctx);
                break;
            }
            u32 status = 0;
            rc = httpcGetResponseStatusCodeTimeout(&ctx, &status, 10ULL * 1000ULL * 1000ULL * 1000ULL);
            if (R_FAILED(rc))
            {
                err = Utils::Format("応答がありません (0x%08X)", (u32)rc);
                httpcCloseContext(&ctx);
                break;
            }
            if (status >= 300 && status < 400)
            {
                char loc[512];
                std::memset(loc, 0, sizeof(loc));
                rc = httpcGetResponseHeader(&ctx, "Location", loc, sizeof(loc) - 1);
                httpcCloseContext(&ctx);
                if (R_FAILED(rc) || loc[0] == 0)
                {
                    err = "リダイレクト先がわかりません。";
                    break;
                }
                url = loc;
                continue;
            }
            if (status != 200)
            {
                err = Utils::Format("サーバーの応答が %u でした。", status);
                httpcCloseContext(&ctx);
                break;
            }

            body.clear();
            std::vector<u8> buf(4096);
            u32 got = 0;
            do
            {
                got = 0;
                rc = httpcDownloadData(&ctx, buf.data(), (u32)buf.size(), &got);
                body.append((const char *)buf.data(), got);
                if (body.size() > 65536)
                    break;
            } while (rc == (Result)HTTPC_RESULTCODE_DOWNLOADPENDING);
            httpcCloseContext(&ctx);
            if (R_FAILED(rc) && rc != (Result)HTTPC_RESULTCODE_DOWNLOADPENDING)
            {
                err = Utils::Format("受信に失敗しました (0x%08X)", (u32)rc);
                break;
            }
            ok = true;
        }
        httpcExit();
        return ok;
    }

    // JSON の文字列を1つ読む (i は開き " の位置)。\uXXXX (サロゲートペア含む) にも対応
    static bool JsonString(const std::string &s, size_t &i, std::string &out)
    {
        out.clear();
        if (i >= s.size() || s[i] != '"')
            return false;
        ++i;
        while (i < s.size())
        {
            char c = s[i++];
            if (c == '"')
                return true;
            if (c != '\\')
            {
                out += c;
                continue;
            }
            if (i >= s.size())
                return false;
            c = s[i++];
            switch (c)
            {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'u':
                {
                    if (i + 4 > s.size())
                        return false;
                    u32 cp = (u32)std::strtoul(s.substr(i, 4).c_str(), nullptr, 16);
                    i += 4;
                    if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u')
                    {
                        const u32 lo = (u32)std::strtoul(s.substr(i + 2, 4).c_str(), nullptr, 16);
                        if (lo >= 0xDC00 && lo < 0xE000)
                        {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            i += 6;
                        }
                    }
                    Utf8Append(out, cp);
                    break;
                }
                default: out += c; break;   // \" \\ \/ など
            }
        }
        return false;
    }

    static void JsonSkipWs(const std::string &s, size_t &i)
    {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t'))
            ++i;
    }

    // [["よみ",["候補1","候補2",...]],["よみ2",[...]]] を読む
    static bool ParseGoogle(const std::string &s, std::vector<Segment> &segs, std::string &err)
    {
        segs.clear();
        size_t i = 0;
        JsonSkipWs(s, i);
        if (i >= s.size() || s[i] != '[')
        {
            err = "変換結果を読めませんでした。";
            return false;
        }
        ++i;
        for (;;)
        {
            JsonSkipWs(s, i);
            if (i >= s.size())
                break;
            if (s[i] == ']')
                break;
            if (s[i] == ',')
            {
                ++i;
                continue;
            }
            if (s[i] != '[')
            {
                err = "変換結果の形が想定と違います。";
                return false;
            }
            ++i;
            Segment seg;
            JsonSkipWs(s, i);
            if (!JsonString(s, i, seg.src))
            {
                err = "変換結果の形が想定と違います。";
                return false;
            }
            JsonSkipWs(s, i);
            if (i < s.size() && s[i] == ',')
                ++i;
            JsonSkipWs(s, i);
            if (i >= s.size() || s[i] != '[')
            {
                err = "変換結果の形が想定と違います。";
                return false;
            }
            ++i;
            for (;;)
            {
                JsonSkipWs(s, i);
                if (i >= s.size())
                    break;
                if (s[i] == ']')
                {
                    ++i;
                    break;
                }
                if (s[i] == ',')
                {
                    ++i;
                    continue;
                }
                std::string cand;
                if (!JsonString(s, i, cand))
                {
                    err = "変換結果の形が想定と違います。";
                    return false;
                }
                seg.cands.push_back(cand);
            }
            JsonSkipWs(s, i);
            if (i < s.size() && s[i] == ']')
                ++i;
            segs.push_back(seg);
        }
        if (segs.empty())
        {
            err = "変換候補がありませんでした。";
            return false;
        }
        return true;
    }

    // ひらがな → 漢字かな交じり (Google)。ネット接続が要る。
    static bool GoogleConvert(const std::string &hira, std::vector<Segment> &segs, std::string &err)
    {
        const std::string q = "www.google.com/transliterate?langpair=ja-Hira%7Cja&text=" + PercentEncode(hira);
        std::string body, e1;
        bool ok = HttpGet("https://" + q, body, e1);
        if (!ok)
        {
            std::string e2;
            body.clear();
            ok = HttpGet("http://" + q, body, e2);
            if (!ok)
            {
                err = e1;
                return false;
            }
        }
        return ParseGoogle(body, segs, err);
    }
#endif  // JPINPUT_NO_GOOGLE

    // ---------------- 入力の画面 ----------------
    // 日本語を1つ入力する。
    //   1) 画面キーボードで、ローマ字を打つ (例: yamada)
    //   2) ひらがな / カタカナ / 漢字に変換 (Google) / 英数字のまま から選ぶ
    // 決定したら true (out は UTF-8)。キャンセルなら false。
    static bool AskJapanese(const std::string &title, std::string &out)
    {
        for (;;)
        {
            std::string romaji;
            Keyboard ka(title + " (ローマ字で入力。例: yamada)");
            if (ka.Open(romaji) != 0)
                return false;
            if (romaji.empty())
                return false;

            const std::string hira = RomajiToHiragana(romaji);
            const std::string kata = HiraganaToKatakana(hira);

            std::vector<std::string> rows;
            rows.push_back("ひらがな: " + hira);
            rows.push_back("カタカナ: " + kata);
#ifndef JPINPUT_NO_GOOGLE
            rows.push_back("漢字に変換 (Google・ネット接続が必要)");
#else
            rows.push_back("(漢字変換は無効です)");
#endif
            rows.push_back("英数字のまま: " + romaji);
            rows.push_back("入力し直す");
            Keyboard kc(title + ": 「" + hira + "」", rows);
            const int sel = kc.Open();
            if (sel < 0)
                return false;
            if (sel == 0)
            {
                out = hira;
                return true;
            }
            if (sel == 1)
            {
                out = kata;
                return true;
            }
            if (sel == 3)
            {
                out = romaji;
                return true;
            }
            if (sel == 4)
                continue;

#ifndef JPINPUT_NO_GOOGLE
            // 漢字変換
            std::vector<Segment> segs;
            std::string err;
            if (!GoogleConvert(hira, segs, err))
            {
                MessageBox("漢字に変換できません", err)();
                continue;
            }
            std::string result;
            bool cancelled = false;
            for (const Segment &sg : segs)
            {
                std::vector<std::string> cs = sg.cands;
                bool hasSrc = false;
                for (const std::string &c : cs)
                    if (c == sg.src)
                        hasSrc = true;
                if (!hasSrc)
                    cs.push_back(sg.src);
                if (cs.size() > 60)
                    cs.resize(60);
                if (cs.size() == 1)
                {
                    result += cs[0];
                    continue;
                }
                Keyboard kd("「" + sg.src + "」の変換", cs);
                const int ci = kd.Open();
                if (ci < 0)
                {
                    cancelled = true;
                    break;
                }
                result += cs[ci];
            }
            if (cancelled)
                continue;
            out = result;
            return true;
#else
            continue;
#endif
        }
    }
}   // namespace Jp
}   // namespace CTRPluginFramework
