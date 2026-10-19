namespace NB.Core.Compression;

/// <summary>
/// LZX decoder (Microsoft LZX as used by Xbox 360 XMemCompress). Follows the structure of
/// libmspack's lzxd: 16-bit little-endian words read MSB first, frames of 32 KiB output,
/// verbatim / aligned-offset / uncompressed blocks, delta-coded tree lengths and E8 translation.
/// The caller feeds one XMem frame at a time (each frame is byte-aligned in the input).
/// </summary>
public sealed class LzxDecoder
{
    public const int FrameSize = 32768;
    const int NumChars = 256, NumPrimaryLengths = 7, MinMatch = 2, NumSecondaryLengths = 249;
    const int PretreeSyms = 20, AlignedSyms = 8, MaxBits = 16;
    const int BlockVerbatim = 1, BlockAligned = 2, BlockUncompressed = 3;

    static readonly byte[] ExtraBits = new byte[51];
    static readonly uint[] PositionBase = new uint[51];

    static LzxDecoder()
    {
        int j = 0;
        for (int i = 0; i < 50; i += 2)
        {
            ExtraBits[i] = ExtraBits[i + 1] = (byte)j;
            if (i != 0 && j < 17) j++;
        }
        ExtraBits[50] = 17;
        uint b = 0;
        for (int i = 0; i < 51; i++) { PositionBase[i] = b; b += 1u << ExtraBits[i]; }
    }

    readonly byte[] _window;
    readonly int _windowSize;
    readonly int _mainElements;
    int _windowPosn, _framePosn;
    uint _r0, _r1, _r2;
    int _blockType, _blockLength, _blockRemaining;
    bool _headerRead, _intelStarted;
    int _intelFileSize, _intelCurPos;

    readonly byte[] _mainLen, _lengthLen = new byte[NumSecondaryLengths + 16], _alignedLen = new byte[AlignedSyms], _preLen = new byte[PretreeSyms];
    readonly Huffman _main, _length = new(NumSecondaryLengths + 16, 12), _aligned = new(AlignedSyms, 7), _pre = new(PretreeSyms, 6);
    bool _lengthEmpty;

    // bit reader state (per frame)
    byte[] _in = Array.Empty<byte>();
    int _ip, _iend;
    uint _bitBuf;
    int _bitsLeft;

    public LzxDecoder(int windowBits)
    {
        if (windowBits < 15 || windowBits > 21) throw new ArgumentOutOfRangeException(nameof(windowBits));
        _windowSize = 1 << windowBits;
        _window = new byte[_windowSize];
        int posSlots = windowBits switch { 20 => 42, 21 => 50, _ => windowBits * 2 };
        _mainElements = NumChars + posSlots * 8;
        _mainLen = new byte[_mainElements + 16];
        _main = new Huffman(_mainElements, 12);
        Reset();
    }

    public void Reset()
    {
        _r0 = _r1 = _r2 = 1;
        _headerRead = false;
        _blockRemaining = 0; _blockType = 0; _blockLength = 0;
        _intelStarted = false; _intelCurPos = 0; _intelFileSize = 0;
        _windowPosn = 0; _framePosn = 0;
        Array.Clear(_mainLen); Array.Clear(_lengthLen);
    }

    // ---------------- bit reading ----------------
    void Ensure(int n)
    {
        while (_bitsLeft < n)
        {
            int b0 = _ip < _iend ? _in[_ip] : 0;
            int b1 = _ip + 1 < _iend ? _in[_ip + 1] : 0;
            _ip += 2;
            _bitBuf |= (uint)((b1 << 8) | b0) << (16 - _bitsLeft);
            _bitsLeft += 16;
        }
    }
    uint Peek(int n) => _bitBuf >> (32 - n);
    void Remove(int n) { _bitBuf <<= n; _bitsLeft -= n; }
    uint Bits(int n)
    {
        if (n == 0) return 0;
        if (n > 16) { uint hi = Bits(n - 16); return (hi << 16) | Bits(16); }
        Ensure(n); uint v = Peek(n); Remove(n); return v;
    }
    int Sym(Huffman h)
    {
        Ensure(MaxBits);
        int s = h.Decode(_bitBuf, out int len);
        if (s < 0) throw new InvalidDataException("LZX: invalid Huffman code");
        Remove(len);
        return s;
    }

    void ReadLens(byte[] lens, int first, int last)
    {
        for (int x = 0; x < PretreeSyms; x++) _preLen[x] = (byte)Bits(4);
        if (!_pre.Build(_preLen, PretreeSyms)) throw new InvalidDataException("LZX: bad pretree");
        for (int x = first; x < last;)
        {
            int z = Sym(_pre);
            if (z == 17) { int y = (int)Bits(4) + 4; while (y-- > 0 && x < last) lens[x++] = 0; }
            else if (z == 18) { int y = (int)Bits(5) + 20; while (y-- > 0 && x < last) lens[x++] = 0; }
            else if (z == 19)
            {
                int y = (int)Bits(1) + 4;
                z = Sym(_pre);
                z = lens[x] - z; if (z < 0) z += 17;
                while (y-- > 0 && x < last) lens[x++] = (byte)z;
            }
            else { z = lens[x] - z; if (z < 0) z += 17; lens[x++] = (byte)z; }
        }
    }

    /// <summary>Decodes one frame. <paramref name="input"/> holds exactly that frame's compressed bytes.</summary>
    public void DecodeFrame(byte[] input, int inOff, int inLen, Span<byte> output, int frameSize)
    {
        _in = input; _ip = inOff; _iend = inOff + inLen; _bitBuf = 0; _bitsLeft = 0;
        DecodeNextFrame(output, frameSize);
    }

    /// <summary>
    /// Decodes a continuous LZX stream (no per-frame size prefixes, as used by XEX "normal" compression):
    /// the bit reader carries across 32 KiB frames and realigns to 16 bits after each frame.
    /// </summary>
    public void DecodeStream(byte[] input, int inOff, int inLen, byte[] output)
    {
        _in = input; _ip = inOff; _iend = inOff + inLen; _bitBuf = 0; _bitsLeft = 0;
        for (int pos = 0; pos < output.Length; pos += FrameSize)
        {
            int n = Math.Min(FrameSize, output.Length - pos);
            DecodeNextFrame(output.AsSpan(pos, n), n);
            if (_bitsLeft > 0) Ensure(16);
            if ((_bitsLeft & 15) != 0) Remove(_bitsLeft & 15);
        }
    }

    void DecodeNextFrame(Span<byte> output, int frameSize)
    {
        if (frameSize > FrameSize || frameSize <= 0) throw new ArgumentOutOfRangeException(nameof(frameSize));

        if (!_headerRead)
        {
            uint i = Bits(1), j = 0;
            if (i != 0) { i = Bits(16); j = Bits(16); }
            _intelFileSize = (int)((i << 16) | j);
            _headerRead = true;
        }

        byte[] window = _window;
        int wpos = _windowPosn;
        int bytesTodo = _framePosn + frameSize - wpos;
        while (bytesTodo > 0)
        {
            if (_blockRemaining == 0)
            {
                _blockType = (int)Bits(3);
                uint hi = Bits(16), lo = Bits(8);
                _blockRemaining = _blockLength = (int)((hi << 8) | lo);
                switch (_blockType)
                {
                    case BlockAligned:
                        for (int k = 0; k < 8; k++) _alignedLen[k] = (byte)Bits(3);
                        if (!_aligned.Build(_alignedLen, AlignedSyms)) throw new InvalidDataException("LZX: bad aligned tree");
                        goto case BlockVerbatim;
                    case BlockVerbatim:
                        ReadLens(_mainLen, 0, 256);
                        ReadLens(_mainLen, 256, _mainElements);
                        if (!_main.Build(_mainLen, _mainElements)) throw new InvalidDataException("LZX: bad main tree");
                        if (_mainLen[0xE8] != 0) _intelStarted = true;
                        ReadLens(_lengthLen, 0, NumSecondaryLengths);
                        _lengthEmpty = !_length.Build(_lengthLen, NumSecondaryLengths);
                        break;
                    case BlockUncompressed:
                        _intelStarted = true;
                        if (_bitsLeft == 0) Ensure(16);
                        // Any bits still buffered belong to the word(s) already consumed; drop them.
                        if (_bitsLeft > 16) _ip -= 2;
                        _bitsLeft = 0; _bitBuf = 0;
                        _r0 = RdLE32(); _r1 = RdLE32(); _r2 = RdLE32();
                        break;
                    default:
                        throw new InvalidDataException($"LZX: bad block type {_blockType}");
                }
            }

            int thisRun = Math.Min(_blockRemaining, bytesTodo);
            bytesTodo -= thisRun;
            _blockRemaining -= thisRun;

            if (_blockType == BlockUncompressed)
            {
                if (_ip + thisRun > _iend) throw new InvalidDataException("LZX: uncompressed block overruns input");
                Buffer.BlockCopy(_in, _ip, window, wpos, thisRun);
                _ip += thisRun; wpos += thisRun; thisRun = 0;
                // Odd-length stored blocks are padded to 16 bits. With XMem framing the pad byte
                // belongs to the frame in which the block ends, so consume it right away.
                if (_blockRemaining == 0 && (_blockLength & 1) != 0) _ip++;
            }
            else
            {
                bool aligned = _blockType == BlockAligned;
                while (thisRun > 0)
                {
                    int mainEl = Sym(_main);
                    if (mainEl < NumChars) { window[wpos++] = (byte)mainEl; thisRun--; continue; }
                    mainEl -= NumChars;
                    int matchLen = mainEl & NumPrimaryLengths;
                    if (matchLen == NumPrimaryLengths)
                    {
                        if (_lengthEmpty) throw new InvalidDataException("LZX: length tree empty");
                        matchLen += Sym(_length);
                    }
                    matchLen += MinMatch;
                    uint matchOff;
                    int slot = mainEl >> 3;
                    switch (slot)
                    {
                        case 0: matchOff = _r0; break;
                        case 1: matchOff = _r1; _r1 = _r0; _r0 = matchOff; break;
                        case 2: matchOff = _r2; _r2 = _r0; _r0 = matchOff; break;
                        default:
                            int extra = slot >= 36 ? 17 : ExtraBits[slot];
                            if (!aligned)
                            {
                                matchOff = PositionBase[slot] - 2 + Bits(extra);
                            }
                            else
                            {
                                matchOff = PositionBase[slot] - 2;
                                if (extra > 3) { matchOff += Bits(extra - 3) << 3; matchOff += (uint)Sym(_aligned); }
                                else if (extra == 3) matchOff += (uint)Sym(_aligned);
                                else if (extra > 0) matchOff += Bits(extra);
                                else matchOff = 1;
                            }
                            _r2 = _r1; _r1 = _r0; _r0 = matchOff;
                            break;
                    }
                    if (wpos + matchLen > _windowSize) throw new InvalidDataException("LZX: match runs over window end");
                    int off = (int)matchOff;
                    if (off > wpos)
                    {
                        int j = off - wpos;
                        if (j > _windowSize) throw new InvalidDataException("LZX: match offset beyond window");
                        int src = _windowSize - j, n = matchLen;
                        if (j < n) { n -= j; while (j-- > 0) window[wpos++] = window[src++]; src = 0; }
                        while (n-- > 0) window[wpos++] = window[src++];
                    }
                    else
                    {
                        int src = wpos - off;
                        for (int n = 0; n < matchLen; n++) window[wpos + n] = window[src + n];
                        wpos += matchLen;
                    }
                    thisRun -= matchLen;
                }
            }
            if (thisRun < 0)
            {
                if (-thisRun > _blockRemaining) throw new InvalidDataException("LZX: overrun past block end");
                _blockRemaining -= -thisRun;
            }
        }
        if (wpos - _framePosn != frameSize) throw new InvalidDataException("LZX: frame size mismatch");

        var frame = window.AsSpan(_framePosn, frameSize);
        if (_intelStarted && _intelFileSize != 0 && frameSize > 10)
        {
            frame.CopyTo(output);
            E8Translate(output[..frameSize]);
        }
        else frame.CopyTo(output);
        _intelCurPos += frameSize;

        _framePosn += frameSize;
        if (wpos == _windowSize) wpos = 0;
        if (_framePosn == _windowSize) _framePosn = 0;
        _windowPosn = wpos;
    }

    uint RdLE32()
    {
        if (_ip + 4 > _iend) throw new InvalidDataException("LZX: truncated R0-R2");
        uint v = (uint)(_in[_ip] | _in[_ip + 1] << 8 | _in[_ip + 2] << 16 | _in[_ip + 3] << 24);
        _ip += 4; return v;
    }

    void E8Translate(Span<byte> d)
    {
        int curpos = _intelCurPos, end = d.Length - 10, i = 0;
        while (i < end)
        {
            if (d[i++] != 0xE8) { curpos++; continue; }
            int abs = d[i] | d[i + 1] << 8 | d[i + 2] << 16 | d[i + 3] << 24;
            if (abs >= -curpos && abs < _intelFileSize)
            {
                int rel = abs >= 0 ? abs - curpos : abs + _intelFileSize;
                d[i] = (byte)rel; d[i + 1] = (byte)(rel >> 8); d[i + 2] = (byte)(rel >> 16); d[i + 3] = (byte)(rel >> 24);
            }
            i += 4; curpos += 5;
        }
    }

    /// <summary>Canonical Huffman decoder: fast table for short codes, canonical walk for long ones.</summary>
    sealed class Huffman
    {
        readonly int _tableBits;
        readonly ushort[] _table;
        readonly ushort[] _count = new ushort[MaxBits + 1];
        readonly ushort[] _symbols;
        public Huffman(int maxSyms, int tableBits)
        {
            _tableBits = tableBits;
            _table = new ushort[1 << tableBits];
            _symbols = new ushort[maxSyms];
        }

        /// <summary>Returns false if the tree is empty (all lengths zero).</summary>
        public bool Build(byte[] lens, int n)
        {
            Array.Clear(_count);
            for (int s = 0; s < n; s++) _count[lens[s]]++;
            _count[0] = 0;
            Span<ushort> offs = stackalloc ushort[MaxBits + 2];
            int total = 0;
            for (int l = 1; l <= MaxBits; l++) { offs[l] = (ushort)total; total += _count[l]; }
            if (total == 0) { Array.Fill(_table, (ushort)0xFFFF); return false; }
            for (int s = 0; s < n; s++) if (lens[s] != 0) _symbols[offs[lens[s]]++] = (ushort)s;

            Array.Fill(_table, (ushort)0xFFFF);
            int code = 0, idx = 0;
            for (int l = 1; l <= MaxBits; l++)
            {
                for (int k = 0; k < _count[l]; k++, code++, idx++)
                {
                    if (l <= _tableBits)
                    {
                        int start = code << (_tableBits - l), cnt = 1 << (_tableBits - l);
                        ushort e = (ushort)((_symbols[idx] << 5) | l);
                        for (int t = 0; t < cnt; t++) _table[start + t] = e;
                    }
                }
                code <<= 1;
            }
            return true;
        }

        public int Decode(uint bitBuf, out int len)
        {
            ushort e = _table[bitBuf >> (32 - _tableBits)];
            if (e != 0xFFFF) { len = e & 31; return e >> 5; }
            int code = 0, first = 0, index = 0;
            for (int l = 1; l <= MaxBits; l++)
            {
                code |= (int)((bitBuf >> (32 - l)) & 1);
                int count = _count[l];
                if (code - count < first) { len = l; return _symbols[index + (code - first)]; }
                index += count; first += count; first <<= 1; code <<= 1;
            }
            len = 0; return -1;
        }
    }
}
