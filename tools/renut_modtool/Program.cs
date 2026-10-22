// renut-modtool: builds reNut part mods (.nbpatch) from a mod folder. See docs/modding/custom-parts.md.
//
//   renut-modtool build <mod folder> <game folder> [-o <file.nbpatch>]
//   renut-modtool materials <game folder> <template model> [--turning]
//
// A mod folder holds mod.json, a parts/ folder (part files, scripts) and optionally models.json with the OBJ and image
// files it names. The game folder is the one with default.xex and Bundle/.
//
// Parts of NB Studio's NB.Core (NBCore/, MIT, Copyright (c) 2026 weighta, see NBCore/LICENSE) read and write the game's
// bundles and import the models.
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Nodes;
using Renut.ModTool;

try
{
    if (args.Length >= 3 && args[0] == "build")
        return Build(args[1], args[2], Option("-o"));
    if (args.Length >= 3 && args[0] == "materials")
    {
        var b = new ModBuilder(GameFolder(args[1]));
        foreach (var m in b.TemplateMaterials(Strip(args[2], ModBuilder.ModelPrefix), args.Contains("--turning"))) Console.WriteLine(m);
        return 0;
    }
    Console.WriteLine("""
        renut-modtool: builds reNut part mods (.nbpatch).

          renut-modtool build <mod folder> <game folder> [-o <file.nbpatch>]
              Checks the mod's part files, builds its models (models.json) and writes the .nbpatch.
          renut-modtool materials <game folder> <template model> [--turning]
              Lists a shipped part model's colour textures, for models.json "materials" (e.g. gadgets_horn);
              --turning: only those on a wheel's turning part (for "spins").

        See docs/modding/custom-parts.md.
        """);
    return 1;
}
catch (Exception e) when (e is ArgumentException or InvalidDataException or IOException or JsonException or UnauthorizedAccessException)
{
    Console.Error.WriteLine("error: " + e.Message);
    return 2;
}

string? Option(string name)
{
    int i = Array.IndexOf(args, name);
    return i >= 0 && i + 1 < args.Length ? args[i + 1] : null;
}

static string GameFolder(string path)
{
    var full = Path.GetFullPath(path);
    if (!File.Exists(Path.Combine(full, "Bundle", "50", "685374")))
        throw new ArgumentException($"{full} is not the game folder (it has no Bundle/50/685374)");
    return full;
}

static string Strip(string name, string prefix)
{
    name = name.Trim().ToLowerInvariant();
    return name.StartsWith(prefix) ? name[prefix.Length..] : name;
}

static string Slug(string s)
{
    var sb = new System.Text.StringBuilder();
    foreach (var c in s.ToLowerInvariant())
        if (char.IsAsciiLetterOrDigit(c)) sb.Append(c);
        else if (sb.Length > 0 && sb[^1] != '-') sb.Append('-');
    return sb.ToString().Trim('-');
}

static int Build(string modDir, string gameDir, string? outPath)
{
    modDir = Path.GetFullPath(modDir);
    var game = GameFolder(gameDir);
    var infoPath = Path.Combine(modDir, "mod.json");
    if (!File.Exists(infoPath)) throw new ArgumentException($"{modDir} has no mod.json");
    var info = JsonSerializer.Deserialize<ModInfo>(File.ReadAllText(infoPath), ModBuilder.Json) ?? throw new InvalidDataException("mod.json is empty");
    if (info.Name.Length == 0) throw new ArgumentException("mod.json needs a \"name\"");
    if (info.Id.Length == 0) info.Id = Slug(info.Name);
    var partsDir = Path.Combine(modDir, "parts");
    if (!Directory.Exists(partsDir)) throw new ArgumentException($"{modDir} has no parts folder");

    var builder = new ModBuilder(game);
    var modelsPath = Path.Combine(modDir, "models.json");
    var spec = File.Exists(modelsPath) ? JsonSerializer.Deserialize<ModelsSpec>(File.ReadAllText(modelsPath), ModBuilder.Json) ?? new ModelsSpec() : null;

    // the part files
    var problems = new List<string>();
    var ids = new HashSet<string>();
    int partCount = 0;
    foreach (var file in Directory.GetFiles(partsDir, "*.json", SearchOption.AllDirectories).Order())
    {
        string rel = "parts/" + Path.GetRelativePath(partsDir, file).Replace('\\', '/');
        void Problem(string p) => problems.Add($"{rel}: {p}");
        JsonObject part;
        try { part = JsonNode.Parse(File.ReadAllText(file), documentOptions: new JsonDocumentOptions { CommentHandling = JsonCommentHandling.Skip, AllowTrailingCommas = true }) as JsonObject ?? throw new JsonException("not a JSON object"); }
        catch (JsonException e) { Problem(e.Message); continue; }
        partCount++;
        string Str(string key) => part[key]?.GetValueKind() == JsonValueKind.String ? part[key]!.GetValue<string>() : "";
        string id = Str("id").ToLowerInvariant();
        if (id.Length == 0 || id.Length > 40 || !id.All(c => c is >= 'a' and <= 'z' or >= '0' and <= '9' or '_')) Problem("\"id\" must be 1-40 characters of a-z, 0-9 and _");
        else if (!ids.Add(id)) Problem($"another part file has the id '{id}'");
        string baseName = Str("base"), objparams = Str("objparams"), script = Str("script");
        if (baseName.Length == 0 && objparams.Length == 0) Problem("needs \"base\" (a shipped part) or \"objparams\"");
        if (baseName.Length > 0)
        {
            var b = Strip(baseName, "aid_objparams_");
            if (!b.StartsWith("banjox_vehicleblock_")) b = "banjox_vehicleblock_" + b;
            if (!builder.ResidentHas("aid_objparams_" + b)) Problem($"base '{baseName}' is not a part of the game");
        }
        foreach (var (key, path) in new[] { ("script", script), ("objparams", objparams) })
            if (path.Length > 0 && !File.Exists(Path.Combine(modDir, path.Replace('/', Path.DirectorySeparatorChar))))
                Problem($"\"{key}\": {path} isn't in the mod folder (paths start with parts/)");
        string model = Strip(Str("model"), ModBuilder.ModelPrefix), attach = Strip(Str("attach"), ModBuilder.AttachPrefix);
        if (model.StartsWith("banjox_vehicleparts_")) model = model["banjox_vehicleparts_".Length..];
        if (attach.StartsWith("banjox_vehicleblock_")) attach = attach["banjox_vehicleblock_".Length..];
        if (model.Length > 0 && spec?.Models.Any(m => m.Id == model) != true && !builder.StreamHas(ModBuilder.ModelPrefix + model))
            Problem($"\"model\": '{model}' is neither in models.json nor a part model of the game");
        if (attach.Length > 0 && spec?.Attach.Any(a => a.Id == attach) != true && !builder.StreamHas(ModBuilder.AttachPrefix + attach))
            Problem($"\"attach\": '{attach}' is neither in models.json nor a part footprint of the game");
    }
    foreach (var f in Directory.GetFiles(modDir, "*.json"))
        if (Path.GetFileName(f) is var n && !n.Equals("mod.json", StringComparison.OrdinalIgnoreCase) && !n.Equals("models.json", StringComparison.OrdinalIgnoreCase))
            problems.Add($"{n}: part files go in parts/; this one is next to mod.json, so it isn't packed");
    if (partCount == 0) problems.Add("parts/: no part files (*.json)");
    if (problems.Count > 0)
    {
        foreach (var p in problems) Console.Error.WriteLine("error: " + p);
        return 2;
    }

    // the models
    var files = new List<(string Path, byte[] Data)>();
    if (spec != null && (spec.Models.Count > 0 || spec.Attach.Count > 0))
    {
        string bundle = (spec.Bundle ?? info.Id).ToLowerInvariant().Replace('-', '_');
        if (bundle.Length == 0 || bundle.Length > 40 || !bundle.All(c => c is >= 'a' and <= 'z' or >= '0' and <= '9' or '_'))
            throw new ArgumentException("models.json \"bundle\" must be 1-40 characters of a-z, 0-9 and _");
        files = builder.Build(spec, bundle, modDir);
        foreach (var l in builder.Log) Console.WriteLine("  " + l);
    }

    // the patch
    outPath = Path.GetFullPath(outPath ?? info.Id + ".nbpatch");
    var manifest = new JsonObject
    {
        ["Format"] = 2, ["Id"] = info.Id, ["Name"] = info.Name, ["Version"] = info.Version, ["Author"] = info.Author,
        ["Description"] = info.Description, ["Created"] = DateTime.Now, ["Category"] = "Parts",
        ["Files"] = new JsonArray(files.Select((f, i) => (JsonNode)new JsonObject
        {
            ["Path"] = f.Path, ["Kind"] = "new", ["SourceSha256"] = "", ["SourceSize"] = 0,
            ["TargetSha256"] = Convert.ToHexString(SHA256.HashData(f.Data)).ToLowerInvariant(), ["TargetSize"] = f.Data.Length,
            ["Entry"] = $"d/{i:D4}.bin", ["CopiedBytes"] = 0, ["LiteralBytes"] = f.Data.Length,
        }).ToArray()),
        ["ExeMods"] = new JsonArray(), ["Multiplayer"] = "", ["Requires"] = new JsonArray(), ["Conflicts"] = new JsonArray(),
        ["Tags"] = new JsonArray(), ["Ops"] = new JsonArray(),
    };
    var tmp = outPath + ".tmp";
    using (var zip = ZipFile.Open(tmp, ZipArchiveMode.Create))
    {
        using (var w = new StreamWriter(zip.CreateEntry("patch.json", CompressionLevel.Optimal).Open()))
            w.Write(manifest.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
        for (int i = 0; i < files.Count; i++)
            using (var s = zip.CreateEntry($"d/{i:D4}.bin", CompressionLevel.SmallestSize).Open()) s.Write(files[i].Data);
        foreach (var f in Directory.GetFiles(partsDir, "*", SearchOption.AllDirectories).Order(StringComparer.Ordinal))
            zip.CreateEntryFromFile(f, "parts/" + Path.GetRelativePath(partsDir, f).Replace('\\', '/'), CompressionLevel.Optimal);
    }
    File.Move(tmp, outPath, true);
    Console.WriteLine($"wrote {outPath}: {partCount} part(s), {files.Count} bundle file(s)");
    return 0;
}
