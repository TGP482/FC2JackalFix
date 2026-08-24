/* Based on FoxAhead's Far Cry 2 Multi Fixer: https://github.com/FoxAhead/Far-Cry-2-Multi-Fixer */

module;

#include <common.hxx>

export module jackaltapes;

import common;
import dunia;

// Not exposed in the ini. Plain bug fix.
class JackalTapes
{
public:
    JackalTapes()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            // Lookup walks 0x78-byte records. With the first flag byte set it falls through to a second
            // comparison that accepts a wrong entry, so every southern-map tape gives the same recording.
            auto* pTapeLookup = dunia_find("8B 4C 24 0C 8B 15 ? ? ? ? 90 80 7E 74 00 75 0A 3B CA 75 0A 80 7E 75 00 75 16", 16);
            if (!pTapeLookup)
                return;

            injector::WriteMemory<uint8_t>(pTapeLookup, 0x14, true); // JNZ +0x0A -> JNZ +0x14
        };
    }
} JackalTapes;
