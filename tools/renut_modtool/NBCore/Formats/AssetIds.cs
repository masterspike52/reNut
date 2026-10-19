using System.IO.Hashing;
using System.Text;
using System.Text.RegularExpressions;

namespace NB.Core.Formats;

/// <summary>
/// Asset / bundle id scheme: <c>typeByte &lt;&lt; 24 | (CRC32-without-final-xor(name) &amp; 0xFFFFFF)</c>,
/// where the hashed name drops the <c>aid_&lt;type&gt;_</c> (or <c>aid_bundle_</c>) prefix.
/// </summary>
public static partial class AssetIds
{
    public static readonly Dictionary<string, byte> TypeBytes = new()
    {
        ["vehicle"] = 0x00, ["texture"] = 0x01, ["anim"] = 0x02, ["model"] = 0x04, ["havok"] = 0x05, ["animevents"] = 0x06,
        ["cutscene"] = 0x08, ["cutsceneevents"] = 0x09, ["misc"] = 0x0B, ["actorgoals"] = 0x0C, ["marker"] = 0x0D,
        ["callout"] = 0x0E, ["aidlist"] = 0x0F, ["loctext"] = 0x11, ["avatarhavokdata"] = 0x14, ["xcuelist"] = 0x15,
        ["font"] = 0x16, ["script"] = 0x19, ["fxemitter"] = 0x1B, ["fxparticle"] = 0x1C, ["fxrumble"] = 0x1D,
        ["fxcamshake"] = 0x1E, ["objparams"] = 0x1F, ["animtable"] = 0x20, ["scripttable"] = 0x23, ["statetable"] = 0x24,
        ["xuipackage"] = 0x2A, ["xuicachefile"] = 0x2B, ["xuiloadlist"] = 0x2F, ["xlsdata"] = 0x35, ["video"] = 0x36,
        ["challenge"] = 0x3D, ["vertexshader"] = 0x41, ["pixelshader"] = 0x42, ["dialog"] = 0x43, ["chardata"] = 0x44,
        ["gpuparticleeffect"] = 0x45, ["cutcam"] = 0x46, ["blobsdropleteffect"] = 0x47, ["explosioneffect"] = 0x48,
        ["garagetutorial"] = 0x49, ["3dgpuparticleeffect"] = 0x4A, ["compositeeffect"] = 0x4B, ["ddstexture"] = 0x4D,
        ["pathenginepreprocess"] = 0x4E, ["bundle"] = 0x4F, ["streambundle"] = 0x50,
    };
    public static readonly Dictionary<byte, string> TypeNames = TypeBytes.ToDictionary(kv => kv.Value, kv => kv.Key);

    public static uint Hash24(string s)
    {
        uint crc = Crc32.HashToUInt32(Encoding.Latin1.GetBytes(s));
        return ~crc & 0xFFFFFF;
    }

    public static uint Make(byte type, string hashedName) => (uint)type << 24 | Hash24(hashedName);

    [GeneratedRegex(@"^(?:D:\\LocalLibrary\\BanjoX\\)?aid_([a-z0-9]+)_([^\\,]+)")]
    private static partial Regex AidRegex();

    /// <summary>Splits a CAFF symbol into (type, short name) e.g. ("texture", "banjox_…_0x…mip").</summary>
    public static (string Type, string Name)? Parse(string symbol)
    {
        var m = AidRegex().Match(symbol);
        return m.Success ? (m.Groups[1].Value, m.Groups[2].Value) : null;
    }

    /// <summary>Clean display name ("aid_texture_banjox_…mip") without the library path, ",timestamp,version" suffix or "(n)".</summary>
    public static string DisplayName(string symbol)
    {
        var s = symbol.StartsWith(@"D:\LocalLibrary\BanjoX\") ? symbol[@"D:\LocalLibrary\BanjoX\".Length..] : symbol;
        int bs = s.IndexOf('\\'); if (bs >= 0) s = s[..bs];
        int comma = s.IndexOf(','); if (comma >= 0) s = s[..comma];
        return s;
    }

    public static uint? IdOf(string symbol)
    {
        var p = Parse(symbol);
        if (p == null || !TypeBytes.TryGetValue(p.Value.Type, out var tb)) return null;
        return Make(tb, p.Value.Name);
    }

    public static uint BundleId(string bundleName) => Make(0x4F, bundleName.StartsWith("aid_bundle_") ? bundleName["aid_bundle_".Length..] : bundleName);
}
