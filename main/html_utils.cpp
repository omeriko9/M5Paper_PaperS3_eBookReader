#include "html_utils.h"
#include <string>
#include <cstdlib>
#include <cstdint>
#include <cstring>

static inline bool isDecDigit(unsigned char c) { return c >= '0' && c <= '9'; }
static inline bool isHexDigit(unsigned char c)
{
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}
static inline uint32_t hexVal(unsigned char c)
{
    if (c <= '9')
        return (uint32_t)(c - '0');
    if (c <= 'F')
        return (uint32_t)(c - 'A' + 10);
    return (uint32_t)(c - 'a' + 10);
}

static inline void appendUtf8(std::string &out, uint32_t cp)
{
    // Clamp invalid Unicode
    if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu))
    {
        out.push_back('?');
        return;
    }

    if (cp <= 0x7Fu)
    {
        out.push_back((char)cp);
    }
    else if (cp <= 0x7FFu)
    {
        out.push_back((char)(0xC0u | (cp >> 6)));
        out.push_back((char)(0x80u | (cp & 0x3Fu)));
    }
    else if (cp <= 0xFFFFu)
    {
        out.push_back((char)(0xE0u | (cp >> 12)));
        out.push_back((char)(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back((char)(0x80u | (cp & 0x3Fu)));
    }
    else
    {
        out.push_back((char)(0xF0u | (cp >> 18)));
        out.push_back((char)(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back((char)(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back((char)(0x80u | (cp & 0x3Fu)));
    }
}

static inline bool matchEntity(const char *p, size_t len, const char *lit)
{
    const size_t litLen = std::strlen(lit);
    return len == litLen && std::memcmp(p, lit, litLen) == 0;
}

std::string decodeHtmlEntities(const std::string &str)
{
    const char *s = str.data();
    const size_t n = str.size();

    std::string out;
    out.reserve(n);

    for (size_t i = 0; i < n;)
    {
        if (s[i] != '&')
        {
            out.push_back(s[i++]);
            continue;
        }

        // Try parse entity starting at '&'
        size_t j = i + 1;

        // Allow optional whitespace: "&   #160 ;"
        while (j < n && (s[j] == ' ' || s[j] == '\t' || s[j] == '\r' || s[j] == '\n'))
            ++j;

        if (j >= n)
        {
            out.push_back(s[i++]);
            continue;
        }

        // Scan until ';' with a reasonable cap to avoid runaway
        // (Most entities are short; cap helps on garbage input.)
        const size_t maxScan = 32;
        size_t k = j;
        size_t scanned = 0;
        while (k < n && s[k] != ';' && scanned < maxScan)
        {
            ++k;
            ++scanned;
        }

        if (k >= n || s[k] != ';')
        {
            // No ';' found soon. Treat '&' as literal.
            out.push_back('&');
            ++i;
            continue;
        }

        // Now entity text is [j, k)
        const char *p = s + j;
        size_t len = k - j;

        // Trim trailing whitespace before ';'
        while (len > 0)
        {
            char c = p[len - 1];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
                --len;
            else
                break;
        }

        bool decoded = false;

        if (len > 0 && p[0] == '#')
        {
            // Numeric entity: "&#123;" or "&#x1F60A;"
            size_t t = 1;
            int base = 10;

            if (t < len && (p[t] == 'x' || p[t] == 'X'))
            {
                base = 16;
                ++t;
            }

            uint32_t cp = 0;
            bool any = false;

            if (base == 10)
            {
                while (t < len && isDecDigit((unsigned char)p[t]))
                {
                    any = true;
                    cp = cp * 10u + (uint32_t)(p[t] - '0');
                    ++t;
                }
            }
            else
            {
                while (t < len && isHexDigit((unsigned char)p[t]))
                {
                    any = true;
                    cp = (cp << 4) + hexVal((unsigned char)p[t]);
                    ++t;
                }
            }

            // Allow whitespace after number (we already trimmed at end, so this is mostly academic)
            while (t < len && (p[t] == ' ' || p[t] == '\t' || p[t] == '\r' || p[t] == '\n'))
                ++t;

            if (any && t == len)
            {
                // Map NBSP numeric (160 / 0xA0) to regular space to match your "&nbsp" behavior
                if (cp == 0xA0u)
                    out.push_back(' ');
                else
                    appendUtf8(out, cp);
                decoded = true;
            }
        }
        else
        {
            // Named entities (fast exact matches, no allocations)
            // Basic
            if (matchEntity(p, len, "amp"))
            {
                out.push_back('&');
                decoded = true;
            }
            else if (matchEntity(p, len, "lt"))
            {
                out.push_back('<');
                decoded = true;
            }
            else if (matchEntity(p, len, "gt"))
            {
                out.push_back('>');
                decoded = true;
            }
            else if (matchEntity(p, len, "quot"))
            {
                out.push_back('"');
                decoded = true;
            }
            else if (matchEntity(p, len, "apos"))
            {
                out.push_back('\'');
                decoded = true;
            }
            else if (matchEntity(p, len, "nbsp"))
            {
                out.push_back(' ');
                decoded = true;
            }

            // Typography
            else if (matchEntity(p, len, "hellip"))
            {
                out.append("\xE2\x80\xA6");
                decoded = true;
            } // …
            else if (matchEntity(p, len, "mdash"))
            {
                out.append("\xE2\x80\x94");
                decoded = true;
            } // —
            else if (matchEntity(p, len, "ndash"))
            {
                out.append("\xE2\x80\x93");
                decoded = true;
            } // –
            else if (matchEntity(p, len, "lsquo"))
            {
                out.push_back('\'');
                decoded = true;
            }
            else if (matchEntity(p, len, "rsquo"))
            {
                out.push_back('\'');
                decoded = true;
            }
            else if (matchEntity(p, len, "ldquo"))
            {
                out.push_back('"');
                decoded = true;
            }
            else if (matchEntity(p, len, "rdquo"))
            {
                out.push_back('"');
                decoded = true;
            }

            // Additional
            else if (matchEntity(p, len, "bull"))
            {
                out.append("\xE2\x80\xA2");
                decoded = true;
            } // •
            else if (matchEntity(p, len, "copy"))
            {
                out.append("\xC2\xA9");
                decoded = true;
            } // ©
            else if (matchEntity(p, len, "reg"))
            {
                out.append("\xC2\xAE");
                decoded = true;
            } // ®
            else if (matchEntity(p, len, "trade"))
            {
                out.append("\xE2\x84\xA2");
                decoded = true;
            } // ™
            else if (matchEntity(p, len, "deg"))
            {
                out.append("\xC2\xB0");
                decoded = true;
            } // °
            else if (matchEntity(p, len, "frac12"))
            {
                out.append("\xC2\xBD");
                decoded = true;
            } // ½
            else if (matchEntity(p, len, "frac14"))
            {
                out.append("\xC2\xBC");
                decoded = true;
            } // ¼
            else if (matchEntity(p, len, "frac34"))
            {
                out.append("\xC2\xBE");
                decoded = true;
            } // ¾
        }

        if (decoded)
        {
            i = k + 1; // skip past ';'
        }
        else
        {
            // Unknown: keep original "&...;" as-is, without allocating substr()
            out.append(s + i, (k - i) + 1);
            i = k + 1;
        }
    }

    return out;
}
