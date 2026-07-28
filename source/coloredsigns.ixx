/*
  "Removing coloured road signs" from An Almost Complete Guide to Far Cry 2 Modding, in code.

  The guide's version is a data edit: every MissionObjectiveSigns.* prototype in
  entitylibrarypatchoverride.fcb carries a "Colors" block (hash C512C6A9) with four Vector4s -
  None (DFA2AFF1), Main (1F1A625A), Subvert (7D65CCD1) and a fourth the guide leaves unnamed
  (590E69F7, "Underground" in the engine) - and the fix rewrites Main, Subvert and Underground to
  match None, 0000803F x4, so no mission state tints the sign. That needs a repacked patch.dat,
  which collides with every other mod that ships one.

  Those four values are the "Colors" property group of the CRoadSign entity component, registered
  in FUN_10667f30:

    FUN_1004ea20("Colors")                      ; group
      child 0 "None"        offset 0x20, index 0
      child 1 "Main"        offset 0x20, index 1
      child 2 "Subvert"     offset 0x20, index 2
      child 3 "Underground" offset 0x20, index 3

  Their shared accessor (FUN_10667860) computes object + [desc+0xC] + [desc+0x10] * 0x10, so the
  block is a flat Vector4[4] living at CRoadSign+0x20 through +0x5F. The component's constructor
  (FUN_106679d0, 0x80 bytes, vtable 0x10E8D988) seeds them: Colors[0] white, Colors[1..3] red, and
  the entity library then overwrites all four from the archive.

  Only one function reads them - FUN_10667b50(CRoadSign* this, int nTagIndex), reached from
  FUN_10667e10 when the sign's active objective tag changes and from the refresh thunk at
  0x10667C80. It walks the sign's materials and, for each one, pushes the chosen Vector4 into
  material parameter 0x0E44F777:

    MOV  ESI, [ESP+0x18]        ; nTagIndex
    TEST ESI, ESI
    JL   colour_none            ; no objective       -> Colors[0]
    CMP  ESI, [EBP+0x70]        ; MissionColors count
    JGE  colour_main            ; tag not in list    -> Colors[1]
    MOV  EAX, [EBP+0x6C]        ; MissionColors data
    MOV  ECX, [EAX+ESI*4]       ; enumColor for this tag
    ADD  ECX, 2
    SHL  ECX, 4                 ; -> this + 0x20 + enumColor * 0x10
    ADD  ECX, EBP
    PUSH ECX
    JMP  apply
  colour_main:
    LEA  EDX, [EBP+0x30]        ; Colors[1] Main
    PUSH EDX
    JMP  apply
  colour_none:
    LEA  EAX, [EBP+0x20]        ; Colors[0] None
    PUSH EAX
  apply:
    MOV  ECX, [EDI+0x48]
    PUSH 0x11649EEC             ; parameter name hash 0x0E44F777
    CALL FUN_10409E60           ; CMaterialInstance::SetVector4

  So the whole feature is one three-way select over a Vector4[4]. Turning the JL into an
  unconditional JMP - one byte, 0x7C -> 0xEB, same length, same target - sends every path to
  Colors[0], which is the untinted white entry both by the constructor's default and in the shipped
  data. That is the exact end state the guide's rewrite produces, with no archive involved, nothing
  else in the engine touched, and the branch restorable at any time.

  ESI is deliberately left alone: the store two instructions past the call, MOV [EBP+0x10], ESI,
  is the component's CurrentTag bookkeeping. Clobbering it to -1 to force the existing JL would
  make FUN_10667e10 think the tag never settles and re-run this for every sign, every tick.

  Toggling live works, with one wrinkle: signs already tinted keep their tint until their objective
  tag next changes, since nothing re-runs the select on its own. Starting or finishing any mission
  clears them.
*/

module;

#include <common.hxx>

export module coloredsigns;

import common;
import dunia;
import settings;

class ColoredSigns
{
public:
    ColoredSigns()
    {
        JackalFix::onDuniaInitEvent() += []()
        {
            // Matched from the tag load through the last of the three colour branches. No
            // relocated bytes in that span - the parameter hash push that follows is the first
            // absolute address, and it is left out.
            auto pattern = dunia_pattern("8B 74 24 18 85 F6 7C 1C 3B 75 70 7D 11 8B 45 6C 8B 0C B0 83 C1 02 C1 E1 04 03 CD 51 EB 0A 8D 55 30 52 EB 04 8D 45 20 50");
            if (pattern.empty())
                return;

            static raw_mem fnSignColorSelect(pattern.get_first(6), { 0xEB }); // JL colour_none -> JMP colour_none

            static auto ColoredSignsCB = []()
            {
                if (JackalFixSettings.GetInt(PREF_NOCOLOREDSIGNS) != 0)
                    fnSignColorSelect.Write();
                else
                    fnSignColorSelect.Restore();
            };

            ColoredSignsCB();

            JackalFix::onIniFileChange() += []()
            {
                ColoredSignsCB();
            };
        };
    }
} ColoredSigns;
