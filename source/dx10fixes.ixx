/*
  Fixes that only apply under D3D10, where HDR is forced on and the post effect chain the D3D9 path
  never runs is always live.

  Black squares flickering across the screen with HDR and bloom both on
  Most visible looking at water, which is where the scene is most likely to contain texels of exactly
  zero luminance.

  Bloom is a block reduction pyramid and its first pass is the bright pass downsample: four taps of
  the HDR scene, each scaled by the adapted luminance, each weighted by how far it sits above the
  bloom threshold, summed into rgb, with the four taps' unweighted luminance summed into alpha for the
  eye adaptation to read back. The weight is computed per tap as

      luma   = dot(tapColour * adaptedLuminance, float3(0.2989, 0.587, 0.114));
      weight = saturate((luma - BloomParams.y) / luma);

  so a texel whose weighted luminance is exactly zero makes that 0/0, which is a NaN, and the NaN
  multiplies straight into the tap. The saturate is supposed to flush it - the D3D functional spec
  says saturate of NaN is zero - but that is one of the few places hardware is allowed to disagree and
  does. From there the pyramid does what a pyramid does: one NaN texel becomes one texel of the level
  below it, and another of the level below that, and comes back through the blur and the combine as an
  axis aligned square the size of whichever level it reached. At 4K one texel of the 60x34 level is 64
  by 64 screen pixels. NaN resolves to black, so the square is black, and the pool hands the chain a
  different surface every frame, so it flickers.

  The shader does guard for this, immediately before it writes:

      and  r0.xy, r2.xzxx, l(0x7FFFFFFF, 0x7FFFFFFF, 0, 0)      drop the sign
      ieq  r0.xy, r0.xyxx, l(0x7F800000, 0x7F800000, 0, 0)      == infinity
      or   r0.x,  r0.y, r0.x
      movc r0.xyzw, r0.xxxx, l(0,0,0,0), r2.xyzw

  and the guard misses every NaN there is. With the sign masked off, 0x7F800000 is exactly infinity,
  and a NaN is strictly greater than it - an exponent of all ones with a mantissa that is not zero.

  What goes in its place is a per component clamp, which is what the community shader for this bug
  does:

      r0 = max(0.0, r0);

  In D3D, min and max return whichever operand is not NaN, so max against zero flushes a NaN component
  to zero and leaves every finite component exactly as it was. Per component matters. Everything
  downstream divides by the adapted luminance this pass feeds through alpha,

      r0.y = clamp(adaptedLuminance, LuminanceRange);
      r0.x = r0.y / adaptedLuminance;
      r0.x = clamp(r0.x, LuminanceAdaptationRange);

  so an edit that zeroes a whole pixel - which is what the guard does when it fires - drives that
  division to infinity and pins exposure at the maximum the game allows. Widening the comparison so it
  catches NaN does not darken the image, it blows the image out.

  Adding an instruction would mean rebuilding the container, and it is not necessary. The conditional
  move the guard ends in already holds every piece the max needs, in order, with room to spare: its
  destination is the destination, its second source is the literal zero, its third is the value, and
  its first - the condition - is two dwords that stop being needed. Rewriting the opcode from MOVC to
  MAX, sliding the last two operands down over the condition and filling the two dwords that fall off
  the end with NOP gives

      max r0.xyzw, l(0,0,0,0), r2.xyzw

  in the same number of dwords, so nothing after the instruction moves and the chunk keeps its length.
  The comparison above it becomes dead code and the driver drops it. What goes with it is the old
  infinity case: an infinite tap is no longer forced to zero, it stays infinite and is clamped by tone
  mapping like any other very bright value. Forcing a whole 4x4 block to black was itself a way to
  produce a black square, so that is the better way round.

  A DXBC container carries a sixteen byte digest at offset four covering every byte from offset twenty
  to the end, and the runtime checks it - an edited container without a corrected digest is refused
  outright by CreatePixelShader, which leaves the bright pass running with no pixel shader at all.
  ReviseChecksum below is MD5 over that range with a non standard final block: the length in bits
  leads the trailing partial block instead of following it, and the closing dword is that count
  shifted right by two with the low bit set. Where the tail leaves no room for both, it takes a block
  of its own and the two length words take another. It reproduces the digests the game's own shaders
  already carry, which is how it was confirmed rather than assumed.

  Two other shaders carry a similar guard and neither is this one. The eye adaptation pass compares
  against infinity with the literal on the left, which the same edit would silently invert, and
  selects the previous frame's adapted luminance rather than zero. So the edit is confined by what the
  bright pass demonstrably is: it weights four taps with the standard luminance vector, so the bit
  pattern of 0.2989 appears eight times in it, and requiring at least four identifies it and excludes
  anything that merely borrowed the idiom.

  Direct3D is resolved through LoadLibrary and GetProcAddress rather than imported, so the device is
  reached by hooking d3d10's creation entry points as the module arrives and the device's
  CreatePixelShader through its vtable once there is a device. Every pixel shader the game creates is
  scanned and handed on, patched if it is the bright pass and untouched if it is not. If the runtime
  rejects an edit anyway the original is created in its place, so a bad edit costs nothing.
*/

module;

#include <common.hxx>
#include <cstdint>
#include <vector>

export module dx10fixes;

import common;
import dunia;

// ------------------------------------------------------------------------------------------------
// The bytecode container.

// Every compiled shader starts with this, followed by a 16 byte digest, a version, the total size,
// the chunk count, and then that many chunk offsets.
static constexpr uint32_t nContainerMagic = 0x43425844; // 'DXBC'
static constexpr ptrdiff_t nContainerDigest = 0x04;
static constexpr ptrdiff_t nContainerChunkCount = 0x1C;
static constexpr ptrdiff_t nContainerChunkOffsets = 0x20;

// The shader model 4 instruction chunk. Model 5 renamed it, and both are accepted so a build that
// compiled the same shader at a higher level is still covered.
static constexpr uint32_t nChunkShaderModel4 = 0x52444853; // 'SHDR'
static constexpr uint32_t nChunkShaderModel5 = 0x58454853; // 'SHEX'

// A chunk header is its tag and its byte length; the instruction chunk then carries a version and a
// dword length before the tokens themselves begin.
static constexpr ptrdiff_t nChunkLength = 0x04;
static constexpr ptrdiff_t nChunkBody = 0x08;
static constexpr ptrdiff_t nInstructionsBegin = 0x08;

// ------------------------------------------------------------------------------------------------
// Instruction tokens.

// The operation in the low eleven bits, the instruction's length in dwords in bits 24 to 30.
static constexpr uint32_t nOpcodeMask = 0x7FF;
static constexpr uint32_t nLengthShift = 24;
static constexpr uint32_t nLengthMask = 0x7F;

static constexpr uint32_t nOpcodeCustomData = 53;      // length lives in the token after it
static constexpr uint32_t nOpcodeMaximum = 52;         // MAX
static constexpr uint32_t nOpcodeConditionalMove = 55; // MOVC
static constexpr uint32_t nOpcodeNoOperation = 58;     // NOP

// The bit pattern of positive infinity, which is what the guard compares against.
static constexpr uint32_t nPositiveInfinityBits = 0x7F800000;

// The first component of the standard luminance vector, 0.2989. One per dot product, and the bright
// pass does eight of them - four taps, each dotted raw and again after scaling.
static constexpr uint32_t nLuminanceWeightBits = 0x3E99096C;
static constexpr uint32_t nMinLuminanceWeights = 4;

// How many instructions after the comparison the conditional move is still taken to belong to it. In
// the shader as shipped it is three; a permutation that schedules something between them has a little
// room, and anything past that is a different piece of code.
static constexpr uint32_t nGuardWindow = 8;

// More than this in one shader means the walk has found something that is not the guard.
static constexpr uint32_t nMaxCandidates = 4;

// ------------------------------------------------------------------------------------------------
// Operand tokens.

static constexpr uint32_t nOperandExtendedBit = 0x80000000;
static constexpr uint32_t nOperandComponentMask = 0x3;
static constexpr uint32_t nOperandTypeShift = 12;
static constexpr uint32_t nOperandTypeMask = 0xFF;
static constexpr uint32_t nOperandDimensionShift = 20;
static constexpr uint32_t nOperandDimensionMask = 0x3;
static constexpr uint32_t nOperandRepresentationShift = 22;
static constexpr uint32_t nOperandRepresentationBits = 3;
static constexpr uint32_t nOperandRepresentationMask = 0x7;

static constexpr uint32_t nOperandTypeImmediate32 = 4;
static constexpr uint32_t nOperandTypeImmediate64 = 5;

static constexpr uint32_t nOperandOneComponent = 1;
static constexpr uint32_t nOperandFourComponents = 2;

// How an index is written: a literal, a wide literal, a register, or a literal plus a register.
static constexpr uint32_t nIndexImmediate32 = 0;
static constexpr uint32_t nIndexImmediate64 = 1;
static constexpr uint32_t nIndexRelative = 2;
static constexpr uint32_t nIndexImmediate32Relative = 3;
static constexpr uint32_t nIndexImmediate64Relative = 4;

// A relative index is itself an operand. One level of that is all the language produces; the limit is
// there so a corrupt token cannot recurse.
static constexpr uint32_t nMaxOperandDepth = 2;

// The conditional move takes four.
static constexpr uint32_t nConditionalMoveOperands = 4;
static constexpr uint32_t nOperandCondition = 1;
static constexpr uint32_t nOperandTrueValue = 2;
static constexpr uint32_t nOperandFalseValue = 3;

// ------------------------------------------------------------------------------------------------
// The device, and the slot its pixel shader creation sits in.

static constexpr size_t nCreatePixelShaderSlot = 82;

static SafetyHookInline CreateDeviceHook{};
static SafetyHookInline CreateDeviceAndSwapChainHook{};
static SafetyHookInline CreatePixelShaderHook{};

// ------------------------------------------------------------------------------------------------
// The container's digest, without which nothing above this line reaches the screen.

// MD5's per round additive constants and rotations, written out rather than generated: they are the
// standard ones, and a table is easier to check against the specification than a sine call is.
static constexpr uint32_t nDigestConstants[64] =
{
    0xD76AA478, 0xE8C7B756, 0x242070DB, 0xC1BDCEEE,
    0xF57C0FAF, 0x4787C62A, 0xA8304613, 0xFD469501,
    0x698098D8, 0x8B44F7AF, 0xFFFF5BB1, 0x895CD7BE,
    0x6B901122, 0xFD987193, 0xA679438E, 0x49B40821,
    0xF61E2562, 0xC040B340, 0x265E5A51, 0xE9B6C7AA,
    0xD62F105D, 0x02441453, 0xD8A1E681, 0xE7D3FBC8,
    0x21E1CDE6, 0xC33707D6, 0xF4D50D87, 0x455A14ED,
    0xA9E3E905, 0xFCEFA3F8, 0x676F02D9, 0x8D2A4C8A,
    0xFFFA3942, 0x8771F681, 0x6D9D6122, 0xFDE5380C,
    0xA4BEEA44, 0x4BDECFA9, 0xF6BB4B60, 0xBEBFBC70,
    0x289B7EC6, 0xEAA127FA, 0xD4EF3085, 0x04881D05,
    0xD9D4D039, 0xE6DB99E5, 0x1FA27CF8, 0xC4AC5665,
    0xF4292244, 0x432AFF97, 0xAB9423A7, 0xFC93A039,
    0x655B59C3, 0x8F0CCC92, 0xFFEFF47D, 0x85845DD1,
    0x6FA87E4F, 0xFE2CE6E0, 0xA3014314, 0x4E0811A1,
    0xF7537E82, 0xBD3AF235, 0x2AD7D2BB, 0xEB86D391,
};

static constexpr uint32_t nDigestRotations[64] =
{
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

static constexpr uint32_t nDigestState[4] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476 };

static constexpr size_t nDigestBlock = 64;
static constexpr size_t nDigestTailLimit = 56;

// The message starts here, which is immediately after the digest itself and the version dword.
static constexpr size_t nDigestSkip = 20;

static uint32_t RotateLeft(uint32_t nValue, uint32_t nCount)
{
    return (nValue << nCount) | (nValue >> (32 - nCount));
}

// One MD5 compression, standard in every respect. Only what gets fed to it is unusual.
static void DigestBlock(uint32_t State[4], const uint8_t* pBlock)
{
    uint32_t nMessage[16]{};
    for (uint32_t nWord = 0; nWord < 16; nWord++)
    {
        nMessage[nWord] = static_cast<uint32_t>(pBlock[nWord * 4])
            | (static_cast<uint32_t>(pBlock[nWord * 4 + 1]) << 8)
            | (static_cast<uint32_t>(pBlock[nWord * 4 + 2]) << 16)
            | (static_cast<uint32_t>(pBlock[nWord * 4 + 3]) << 24);
    }

    auto nA = State[0], nB = State[1], nC = State[2], nD = State[3];

    for (uint32_t nRound = 0; nRound < 64; nRound++)
    {
        uint32_t nMix = 0;
        uint32_t nWord = 0;

        if (nRound < 16)
        {
            nMix = (nB & nC) | (~nB & nD);
            nWord = nRound;
        }
        else if (nRound < 32)
        {
            nMix = (nD & nB) | (~nD & nC);
            nWord = (5 * nRound + 1) % 16;
        }
        else if (nRound < 48)
        {
            nMix = nB ^ nC ^ nD;
            nWord = (3 * nRound + 5) % 16;
        }
        else
        {
            nMix = nC ^ (nB | ~nD);
            nWord = (7 * nRound) % 16;
        }

        nMix += nA + nDigestConstants[nRound] + nMessage[nWord];

        nA = nD;
        nD = nC;
        nC = nB;
        nB += RotateLeft(nMix, nDigestRotations[nRound]);
    }

    State[0] += nA;
    State[1] += nB;
    State[2] += nC;
    State[3] += nD;
}

// Recomputes the digest over everything past the header and writes it back into the header, which is
// what makes an edited container acceptable to the runtime.
static void ReviseChecksum(uint8_t* pBytecode, size_t nBytecodeSize)
{
    if (pBytecode == nullptr || nBytecodeSize <= nDigestSkip)
        return;

    auto* pMessage = pBytecode + nDigestSkip;
    auto nMessageSize = nBytecodeSize - nDigestSkip;

    auto nBits = static_cast<uint32_t>(nMessageSize * 8);
    auto nClosing = (nBits >> 2) | 1u;

    uint32_t State[4] = { nDigestState[0], nDigestState[1], nDigestState[2], nDigestState[3] };

    auto nTail = nMessageSize % nDigestBlock;
    auto nWhole = nMessageSize - nTail;

    for (size_t nOffset = 0; nOffset < nWhole; nOffset += nDigestBlock)
        DigestBlock(State, pMessage + nOffset);

    uint8_t Block[nDigestBlock]{};

    if (nTail >= nDigestTailLimit)
    {
        // The tail and its terminator fill one block on their own.
        for (size_t nByte = 0; nByte < nTail; nByte++)
            Block[nByte] = pMessage[nWhole + nByte];

        Block[nTail] = 0x80;
        for (size_t nByte = nTail + 1; nByte < nDigestBlock; nByte++)
            Block[nByte] = 0;

        DigestBlock(State, Block);

        // And the two length words take the next one between them.
        for (size_t nByte = 0; nByte < nDigestBlock; nByte++)
            Block[nByte] = 0;

        for (uint32_t nByte = 0; nByte < 4; nByte++)
        {
            Block[nByte] = static_cast<uint8_t>(nBits >> (nByte * 8));
            Block[nDigestBlock - 4 + nByte] = static_cast<uint8_t>(nClosing >> (nByte * 8));
        }

        DigestBlock(State, Block);
    }
    else
    {
        // Bit count, tail, terminator, zeroes, closing word - exactly one block.
        for (uint32_t nByte = 0; nByte < 4; nByte++)
        {
            Block[nByte] = static_cast<uint8_t>(nBits >> (nByte * 8));
            Block[nDigestBlock - 4 + nByte] = static_cast<uint8_t>(nClosing >> (nByte * 8));
        }

        for (size_t nByte = 0; nByte < nTail; nByte++)
            Block[4 + nByte] = pMessage[nWhole + nByte];

        Block[4 + nTail] = 0x80;

        for (size_t nByte = 4 + nTail + 1; nByte < nDigestBlock - 4; nByte++)
            Block[nByte] = 0;

        DigestBlock(State, Block);
    }

    for (uint32_t nWord = 0; nWord < 4; nWord++)
        for (uint32_t nByte = 0; nByte < 4; nByte++)
            pBytecode[nContainerDigest + nWord * 4 + nByte] = static_cast<uint8_t>(State[nWord] >> (nByte * 8));
}

// ------------------------------------------------------------------------------------------------
// The bright pass guard.

// Walks the container's chunk table for the instruction chunk, and returns it along with how many
// dwords of tokens it holds. Everything is bounds checked against the length the caller was given,
// since this is reading a buffer the game owns.
static uint32_t* FindInstructions(uint8_t* pBytecode, size_t nBytecodeSize, uint32_t& nInstructionDwords)
{
    nInstructionDwords = 0;

    if (pBytecode == nullptr || nBytecodeSize < nContainerChunkOffsets)
        return nullptr;

    if (*reinterpret_cast<uint32_t*>(pBytecode) != nContainerMagic)
        return nullptr;

    auto nChunks = *reinterpret_cast<uint32_t*>(pBytecode + nContainerChunkCount);
    if (nChunks == 0 || nBytecodeSize < nContainerChunkOffsets + static_cast<size_t>(nChunks) * sizeof(uint32_t))
        return nullptr;

    auto* pOffsets = reinterpret_cast<uint32_t*>(pBytecode + nContainerChunkOffsets);

    for (uint32_t nChunk = 0; nChunk < nChunks; nChunk++)
    {
        auto nOffset = pOffsets[nChunk];
        if (nOffset + nChunkBody > nBytecodeSize)
            continue;

        auto* pChunk = pBytecode + nOffset;
        auto nTag = *reinterpret_cast<uint32_t*>(pChunk);
        if (nTag != nChunkShaderModel4 && nTag != nChunkShaderModel5)
            continue;

        auto nLength = *reinterpret_cast<uint32_t*>(pChunk + nChunkLength);
        if (nLength < nInstructionsBegin || nOffset + nChunkBody + nLength > nBytecodeSize)
            continue;

        nInstructionDwords = (nLength - nInstructionsBegin) / sizeof(uint32_t);

        return reinterpret_cast<uint32_t*>(pChunk + nChunkBody + nInstructionsBegin);
    }

    return nullptr;
}

// How many dwords an operand occupies. Zero if it does not fit in what is left, or if it is written
// in a way this does not understand, which is the same thing as far as the caller is concerned - it
// gives up on the instruction rather than guessing where the next one starts.
static uint32_t OperandLength(const uint32_t* pTokens, uint32_t nAvailable, uint32_t nDepth)
{
    if (nAvailable == 0 || nDepth > nMaxOperandDepth)
        return 0;

    auto nToken = pTokens[0];
    uint32_t nLength = 1;

    // Extended tokens chain on the same bit. The language only ever emits one.
    while (pTokens[nLength - 1] & nOperandExtendedBit)
    {
        if (nLength >= nAvailable || nLength > nMaxOperandDepth)
            return 0;

        nLength++;
    }

    auto nComponents = nToken & nOperandComponentMask;
    auto nType = (nToken >> nOperandTypeShift) & nOperandTypeMask;

    // A literal carries its value inline and has no indices at all.
    if (nType == nOperandTypeImmediate32 || nType == nOperandTypeImmediate64)
    {
        uint32_t nValues = 0;
        if (nComponents == nOperandOneComponent)
            nValues = 1;
        else if (nComponents == nOperandFourComponents)
            nValues = 4;
        else
            return 0;

        if (nType == nOperandTypeImmediate64)
            nValues *= 2;

        nLength += nValues;

        return nLength <= nAvailable ? nLength : 0;
    }

    auto nDimensions = (nToken >> nOperandDimensionShift) & nOperandDimensionMask;

    for (uint32_t nDimension = 0; nDimension < nDimensions; nDimension++)
    {
        auto nRepresentation = (nToken >> (nOperandRepresentationShift + nDimension * nOperandRepresentationBits))
            & nOperandRepresentationMask;

        switch (nRepresentation)
        {
        case nIndexImmediate32:
            nLength += 1;
            break;

        case nIndexImmediate64:
            nLength += 2;
            break;

        case nIndexImmediate32Relative:
            nLength += 1;
            [[fallthrough]];

        case nIndexRelative:
        {
            if (nLength >= nAvailable)
                return 0;

            auto nRelative = OperandLength(pTokens + nLength, nAvailable - nLength, nDepth + 1);
            if (nRelative == 0)
                return 0;

            nLength += nRelative;
            break;
        }

        case nIndexImmediate64Relative:
        {
            nLength += 2;
            if (nLength >= nAvailable)
                return 0;

            auto nRelative = OperandLength(pTokens + nLength, nAvailable - nLength, nDepth + 1);
            if (nRelative == 0)
                return 0;

            nLength += nRelative;
            break;
        }

        default:
            return 0;
        }

        if (nLength > nAvailable)
            return 0;
    }

    return nLength;
}

// True if the operand is a four component literal whose four values are all zero, which is the
// float4(0,0,0,0) the guard selects when it fires.
static bool IsZeroLiteral(const uint32_t* pOperand, uint32_t nLength)
{
    if (nLength != 5)
        return false;

    if ((pOperand[0] & nOperandComponentMask) != nOperandFourComponents)
        return false;

    if (((pOperand[0] >> nOperandTypeShift) & nOperandTypeMask) != nOperandTypeImmediate32)
        return false;

    return pOperand[1] == 0 && pOperand[2] == 0 && pOperand[3] == 0 && pOperand[4] == 0;
}

// Rewrites  movc dest, condition, l(0,0,0,0), value  into  max dest, l(0,0,0,0), value  in place. The
// condition's two dwords come out of the middle and two NOPs go on the end, so the instruction keeps
// its length and nothing after it moves. False if the instruction is not shaped as expected, in which
// case it is left exactly as it was.
static bool RewriteAsMaximum(uint32_t* pInstruction, uint32_t nInstructionDwords)
{
    uint32_t nOperandOffsets[nConditionalMoveOperands]{};
    uint32_t nOperandLengths[nConditionalMoveOperands]{};

    uint32_t nOffset = 1;

    for (uint32_t nOperand = 0; nOperand < nConditionalMoveOperands; nOperand++)
    {
        auto nLength = OperandLength(pInstruction + nOffset, nInstructionDwords - nOffset, 0);
        if (nLength == 0)
            return false;

        nOperandOffsets[nOperand] = nOffset;
        nOperandLengths[nOperand] = nLength;
        nOffset += nLength;
    }

    // Anything left over means the instruction holds something this did not account for.
    if (nOffset != nInstructionDwords)
        return false;

    if (!IsZeroLiteral(pInstruction + nOperandOffsets[nOperandTrueValue], nOperandLengths[nOperandTrueValue]))
        return false;

    auto nCondition = nOperandLengths[nOperandCondition];
    auto nKept = nOperandLengths[nOperandTrueValue] + nOperandLengths[nOperandFalseValue];
    auto nNewLength = nInstructionDwords - nCondition;

    // Slide the literal and the value down over the condition.
    for (uint32_t nDword = 0; nDword < nKept; nDword++)
        pInstruction[nOperandOffsets[nOperandCondition] + nDword] = pInstruction[nOperandOffsets[nOperandTrueValue] + nDword];

    // And fill what falls off the end, so the instruction still occupies the dwords it did.
    for (uint32_t nDword = nNewLength; nDword < nInstructionDwords; nDword++)
        pInstruction[nDword] = nOpcodeNoOperation | (1u << nLengthShift);

    // Everything else in the opcode token - the saturate flag above all - is left as it was, since it
    // means the same thing on both instructions.
    pInstruction[0] = (pInstruction[0] & ~(nOpcodeMask | (nLengthMask << nLengthShift)))
        | nOpcodeMaximum
        | (nNewLength << nLengthShift);

    return true;
}

// True if this shader was the bright pass and its guard is now a clamp.
static bool ClampNaNs(uint8_t* pBytecode, size_t nBytecodeSize)
{
    uint32_t nDwords = 0;
    auto* pTokens = FindInstructions(pBytecode, nBytecodeSize, nDwords);
    if (pTokens == nullptr)
        return false;

    uint32_t nCandidateOffsets[nMaxCandidates]{};
    uint32_t nCandidateLengths[nMaxCandidates]{};
    uint32_t nCandidates = 0;
    uint32_t nLuminanceWeights = 0;
    uint32_t nWindow = 0;

    // One walk to find out what the shader is. Nothing is written during it, because the decision
    // depends on a count that is only complete at the end.
    for (uint32_t nToken = 0; nToken < nDwords; )
    {
        auto nOpcode = pTokens[nToken] & nOpcodeMask;
        uint32_t nLength = 0;

        // The one instruction whose length is not in its opcode token.
        if (nOpcode == nOpcodeCustomData)
        {
            if (nToken + 1 >= nDwords)
                break;

            nLength = pTokens[nToken + 1];
        }
        else
        {
            nLength = (pTokens[nToken] >> nLengthShift) & nLengthMask;
        }

        // A length that does not fit means the walk has lost its place, and every offset past here
        // would be a guess. Stop rather than write anything on the strength of one.
        if (nLength < 1 || nToken + nLength > nDwords)
            break;

        bool bGuard = false;

        for (uint32_t nDword = 1; nDword < nLength; nDword++)
        {
            if (pTokens[nToken + nDword] == nPositiveInfinityBits)
                bGuard = true;
            else if (pTokens[nToken + nDword] == nLuminanceWeightBits)
                nLuminanceWeights++;
        }

        if (nOpcode == nOpcodeConditionalMove && nWindow > 0 && nCandidates < nMaxCandidates)
        {
            // Shape is checked properly at rewrite time; here it only has to be a conditional move
            // sitting inside a guard's window.
            nCandidateOffsets[nCandidates] = nToken;
            nCandidateLengths[nCandidates] = nLength;
            nCandidates++;
            nWindow = 0;
        }
        else if (bGuard)
        {
            nWindow = nGuardWindow;
        }
        else if (nWindow > 0)
        {
            nWindow--;
        }

        nToken += nLength;
    }

    // The bright pass weights four taps by the standard luminance vector. Anything that merely
    // borrowed the same guard idiom does not, and is left exactly as the game compiled it.
    if (nLuminanceWeights < nMinLuminanceWeights)
        return false;

    uint32_t nPatched = 0;

    for (uint32_t nCandidate = 0; nCandidate < nCandidates; nCandidate++)
    {
        if (RewriteAsMaximum(pTokens + nCandidateOffsets[nCandidate], nCandidateLengths[nCandidate]))
            nPatched++;
    }

    if (nPatched == 0)
        return false;

    ReviseChecksum(pBytecode, nBytecodeSize);

    return true;
}

// ------------------------------------------------------------------------------------------------

static HRESULT __stdcall CreatePixelShader(void* pDevice, const void* pBytecode, size_t nBytecodeSize, void** ppShader)
{
    std::vector<uint8_t> Patched(static_cast<const uint8_t*>(pBytecode), static_cast<const uint8_t*>(pBytecode) + nBytecodeSize);

    if (!ClampNaNs(Patched.data(), Patched.size()))
        return CreatePixelShaderHook.stdcall<HRESULT>(pDevice, pBytecode, nBytecodeSize, ppShader);

    auto nResult = CreatePixelShaderHook.stdcall<HRESULT>(pDevice, Patched.data(), nBytecodeSize, ppShader);

    // If the runtime will not take the edit, the original still has to be created or the pass draws
    // with no pixel shader at all, which looks far worse than the bug being fixed.
    if (nResult < 0)
        return CreatePixelShaderHook.stdcall<HRESULT>(pDevice, pBytecode, nBytecodeSize, ppShader);

    return nResult;
}

// The slot index comes from the interface's declaration order, which is fixed, but a wrong one would
// be a jump into whatever happened to be there. Requiring it to land in code some module owns catches
// that without assuming which module: d3d10 is only the entry point, and on anything past Vista the
// device behind it is serviced by the d3d11 runtime - or by whatever has wrapped it, since overlays
// and capture tools substitute their own device and are just as valid a thing to hook.
static bool BelongsToModule(void* pAddress)
{
    HMODULE hModule = nullptr;

    return GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        static_cast<LPCSTR>(pAddress), &hModule) && hModule != nullptr;
}

// Only the first device is hooked, since the game creates exactly one.
static void HookDevice(void* pDevice)
{
    if (pDevice == nullptr || CreatePixelShaderHook)
        return;

    auto** ppVTable = *reinterpret_cast<void***>(pDevice);
    if (ppVTable == nullptr)
        return;

    auto* pCreatePixelShader = ppVTable[nCreatePixelShaderSlot];
    if (!BelongsToModule(pCreatePixelShader))
        return;

    CreatePixelShaderHook = safetyhook::create_inline(pCreatePixelShader, CreatePixelShader);
}

static HRESULT __stdcall CreateDevice(void* pAdapter, uint32_t nDriverType, void* hSoftware, uint32_t nFlags,
    uint32_t nSDKVersion, void** ppDevice)
{
    auto nResult = CreateDeviceHook.stdcall<HRESULT>(pAdapter, nDriverType, hSoftware, nFlags, nSDKVersion, ppDevice);

    if (nResult >= 0 && ppDevice != nullptr)
        HookDevice(*ppDevice);

    return nResult;
}

static HRESULT __stdcall CreateDeviceAndSwapChain(void* pAdapter, uint32_t nDriverType, void* hSoftware, uint32_t nFlags,
    uint32_t nSDKVersion, void* pSwapChainDesc, void** ppSwapChain, void** ppDevice)
{
    auto nResult = CreateDeviceAndSwapChainHook.stdcall<HRESULT>(pAdapter, nDriverType, hSoftware, nFlags, nSDKVersion,
        pSwapChainDesc, ppSwapChain, ppDevice);

    if (nResult >= 0 && ppDevice != nullptr)
        HookDevice(*ppDevice);

    return nResult;
}

// A named function rather than a lambda inside a lambda: the plugin is built as modules, and Visual
// Studio's IntelliSense loses imported names through two levels of nesting even where the compiler is
// perfectly happy with them.
static void HookD3D10()
{
    auto hD3D10 = GetModuleHandleW(L"d3d10.dll");
    if (hD3D10 == nullptr)
        return;

    auto* pCreateDevice = GetProcAddress(hD3D10, "D3D10CreateDevice");
    auto* pCreateDeviceAndSwapChain = GetProcAddress(hD3D10, "D3D10CreateDeviceAndSwapChain");

    // Both, since which one the game uses is its own business and the second is only a wrapper around
    // the first on some runtimes.
    if (pCreateDevice != nullptr)
        CreateDeviceHook = safetyhook::create_inline(pCreateDevice, CreateDevice);

    if (pCreateDeviceAndSwapChain != nullptr)
        CreateDeviceAndSwapChainHook = safetyhook::create_inline(pCreateDeviceAndSwapChain, CreateDeviceAndSwapChain);
}

static void RegisterD3D10()
{
    CallbackHandler::RegisterCallback(L"d3d10.dll", HookD3D10);
}

class Dx10Fixes
{
public:
    Dx10Fixes()
    {
        // Direct3D is resolved at runtime rather than imported, so this waits for the module instead
        // of riding the Dunia init the rest of the plugin uses.
        JackalFix::onInitEvent() += RegisterD3D10;
    }
} Dx10Fixes;
