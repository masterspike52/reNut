using System.Numerics;
using NB.Core.IO;

namespace NB.Core.Havok;

/// <summary>
/// Havok 5.5 binary packfile (32-bit pointers, big-endian) as stored in aid_havok_* assets: the asset's .data starts
/// with a small wrapper (+0x20 offset and +0x24 size of the packfile). Sections: __classnames__, __types__ (full
/// hkClass reflection), __data__. Pointers inside a section are resolved through its local/global fixup tables; every
/// object's class comes from the virtual fixups.
/// </summary>
public sealed class HkPackfile
{
    public sealed class Section
    {
        public string Tag = "";
        public int Start, Local, Global, Virtual, Exports, Imports, End;
        public byte[] Bytes = Array.Empty<byte>();
        public readonly Dictionary<int, (int Section, int Offset)> Pointers = new();
        public readonly List<(int Offset, string Class)> Objects = new();
    }

    public string Version = "";
    public readonly List<Section> Sections = new();
    public Section Data => Sections.First(s => s.Tag == "__data__");
    public Section? Types => Sections.FirstOrDefault(s => s.Tag == "__types__");
    public readonly Dictionary<string, HkClass> Classes = new();

    public static bool IsPackfileAsset(byte[] data) =>
        data.Length >= 0x28 && BE.U32(data, 0x20) + 8 <= data.Length && BE.U32(data, (int)BE.U32(data, 0x20)) == 0x57E0E057;

    public static HkPackfile FromAsset(byte[] data)
    {
        int off = BE.S32(data, 0x20), size = BE.S32(data, 0x24);
        return Parse(data.AsSpan(off, Math.Min(size, data.Length - off)).ToArray());
    }

    public static HkPackfile Parse(byte[] p)
    {
        if (BE.U32(p, 0) != 0x57E0E057 || BE.U32(p, 4) != 0x10C0C010) throw new InvalidDataException("not a Havok packfile");
        if (p[16] != 4 || p[17] != 0) throw new InvalidDataException("only 32-bit big-endian packfiles are supported");
        var pf = new HkPackfile { Version = BE.CStr(p, 40, 16) };
        int n = BE.S32(p, 20);
        for (int i = 0; i < n; i++)
        {
            int o = 0x40 + 0x30 * i;
            var s = new Section
            {
                Tag = BE.CStr(p, o, 19), Start = BE.S32(p, o + 20), Local = BE.S32(p, o + 24), Global = BE.S32(p, o + 28),
                Virtual = BE.S32(p, o + 32), Exports = BE.S32(p, o + 36), Imports = BE.S32(p, o + 40), End = BE.S32(p, o + 44),
            };
            s.Bytes = p.AsSpan(s.Start, s.End).ToArray();
            pf.Sections.Add(s);
        }
        var names = pf.Sections[0];
        foreach (var (idx, s) in pf.Sections.Select((s, i) => (i, s)))
        {
            for (int k = s.Local; k + 8 <= s.Global; k += 8)
            {
                int src = BE.S32(s.Bytes, k); if (src == -1) break;
                s.Pointers[src] = (idx, BE.S32(s.Bytes, k + 4));
            }
            for (int k = s.Global; k + 12 <= s.Virtual; k += 12)
            {
                int src = BE.S32(s.Bytes, k); if (src == -1) break;
                s.Pointers[src] = (BE.S32(s.Bytes, k + 4), BE.S32(s.Bytes, k + 8));
            }
            for (int k = s.Virtual; k + 12 <= s.Exports; k += 12)
            {
                int obj = BE.S32(s.Bytes, k); if (obj == -1) break;
                s.Objects.Add((obj, BE.CStr(names.Bytes, BE.S32(s.Bytes, k + 8), 128)));
            }
            s.Objects.Sort((a, b) => a.Offset.CompareTo(b.Offset));
        }
        pf.ReadClasses();
        return pf;
    }

    public byte[] SectionBytes(int index) => Sections[index].Bytes;

    void ReadClasses()
    {
        var t = Types;
        if (t == null) return;
        int ti = Sections.IndexOf(t);
        string Str(int off) => BE.CStr(t.Bytes, off, 256);
        var byOffset = new Dictionary<int, HkClass>();
        foreach (var (off, cls) in t.Objects)
        {
            if (cls != "hkClass") continue;
            var c = new HkClass { Name = t.Pointers.TryGetValue(off, out var np) ? Str(np.Offset) : "?", Size = BE.S32(t.Bytes, off + 8) };
            if (t.Pointers.TryGetValue(off + 4, out var pp)) c.ParentOffset = pp.Offset;
            if (t.Pointers.TryGetValue(off + 24, out var mp))
            {
                int nm = BE.S32(t.Bytes, off + 28);
                for (int m = 0; m < nm; m++)
                {
                    int mo = mp.Offset + 0x18 * m;
                    c.Members.Add(new HkMember
                    {
                        Name = t.Pointers.TryGetValue(mo, out var mn) ? Str(mn.Offset) : "?",
                        ClassOffset = t.Pointers.TryGetValue(mo + 4, out var mc) ? mc.Offset : -1,
                        Type = (HkType)t.Bytes[mo + 12], SubType = (HkType)t.Bytes[mo + 13],
                        CArraySize = BE.S16(t.Bytes, mo + 14), Offset = BE.U16(t.Bytes, mo + 18),
                    });
                }
            }
            byOffset[off] = c;
            Classes[c.Name] = c;
        }
        foreach (var c in byOffset.Values)
        {
            if (c.ParentOffset >= 0 && byOffset.TryGetValue(c.ParentOffset, out var parent)) c.Parent = parent;
            foreach (var m in c.Members) if (m.ClassOffset >= 0 && byOffset.TryGetValue(m.ClassOffset, out var mc)) m.Class = mc;
        }
    }

    public HkObject Object(int offset, string cls) => new(this, Sections.IndexOf(Data), offset, Classes.GetValueOrDefault(cls));
    public IEnumerable<HkObject> ObjectsOf(string cls) => Data.Objects.Where(o => o.Class == cls).Select(o => Object(o.Offset, cls));
}

public enum HkType : byte
{
    Void, Bool, Char, Int8, UInt8, Int16, UInt16, Int32, UInt32, Int64, UInt64, Real, Vector4, Quaternion, Matrix3, Rotation,
    QsTransform, Matrix4, Transform, Zero, Pointer, FunctionPointer, Array, InplaceArray, Enum, Struct, SimpleArray,
    HomogeneousArray, Variant, CString, ULong, Flags,
}

public sealed class HkMember
{
    public string Name = "";
    public HkType Type, SubType;
    public int Offset, CArraySize, ClassOffset = -1;
    public HkClass? Class;
}

public sealed class HkClass
{
    public string Name = "";
    public int Size, ParentOffset = -1;
    public HkClass? Parent;
    public readonly List<HkMember> Members = new();
    public HkMember? Find(string name) => Members.FirstOrDefault(m => m.Name == name) ?? Parent?.Find(name);
}

/// <summary>A located object with reflection-based field access.</summary>
public sealed class HkObject
{
    public readonly HkPackfile File; public readonly int Section, Offset; public readonly HkClass? Class;
    public HkObject(HkPackfile f, int section, int offset, HkClass? cls) { File = f; Section = section; Offset = offset; Class = cls; }
    byte[] B => File.SectionBytes(Section);
    int At(string field) => Offset + (Class?.Find(field)?.Offset ?? throw new KeyNotFoundException($"{Class?.Name}.{field}"));
    public bool Has(string field) => Class?.Find(field) != null;
    public float F(string field) => BE.F32(B, At(field));
    public int I(string field) => BE.S32(B, At(field));
    public byte U8(string field) => B[At(field)];
    public Vector4 V4(string field) { int o = At(field); return new(BE.F32(B, o), BE.F32(B, o + 4), BE.F32(B, o + 8), BE.F32(B, o + 12)); }
    public (int Section, int Offset)? Ptr(string field) => PtrAt(At(field));
    public (int Section, int Offset)? PtrAt(int absolute) =>
        File.Sections[Section].Pointers.TryGetValue(absolute, out var p) ? p : null;
    /// <summary>Pointer to another object in the data section, typed by its virtual fixup.</summary>
    public HkObject? Ref(string field) => RefAt(At(field));
    public HkObject? RefAt(int absolute)
    {
        if (PtrAt(absolute) is not { } p) return null;
        var cls = File.Sections[p.Section].Objects.FirstOrDefault(o => o.Offset == p.Offset).Class;
        return new HkObject(File, p.Section, p.Offset, cls == null ? null : File.Classes.GetValueOrDefault(cls));
    }
    /// <summary>hkArray / hkSimpleArray: (data pointer, count).</summary>
    public ((int Section, int Offset)? Data, int Count) Array(string field) { int o = At(field); return (PtrAt(o), BE.S32(B, o + 4)); }
    /// <summary>Embedded struct member.</summary>
    public HkObject Struct(string field) { var m = Class!.Find(field)!; return new HkObject(File, Section, Offset + m.Offset, m.Class); }
    public HkObject At(int section, int offset, string cls) => new(File, section, offset, File.Classes.GetValueOrDefault(cls));
}
