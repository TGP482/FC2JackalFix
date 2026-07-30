/*
  Credit to https://github.com/FoxAhead/Far-Cry-2-Multi-Fixer
  These fixes were implemented based on the Far Cry 2 Multi Fixer by FoxAhead.
*/

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
            // The tape lookup walks an array of 0x78 byte records. With the first flag byte set it
            // falls through into a second comparison that accepts a wrong entry, so every tape in
            // the southern map resolves to the same recording. Retarget the branch at the loop
            // increment to skip that block.
            auto pattern = dunia_pattern("8B 4C 24 0C 8B 15 ? ? ? ? 90 80 7E 74 00 75 0A 3B CA 75 0A 80 7E 75 00 75 16");
            if (pattern.empty())
                return;

            injector::WriteMemory<uint8_t>(pattern.get_first(16), 0x14, true); // JNZ +0x0A -> JNZ +0x14
        };
    }
} JackalTapes;
