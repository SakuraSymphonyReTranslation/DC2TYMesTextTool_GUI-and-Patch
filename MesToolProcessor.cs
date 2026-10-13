using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;

namespace DC2TYMesTextTool
{
    public class MesToolProcessor
    {
        private static readonly Encoding ShiftJIS;
        private static readonly JsonSerializerOptions JsonOptions = new()
        {
            WriteIndented = true,
            Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping
        };

        static MesToolProcessor()
        {
            Encoding.RegisterProvider(CodePagesEncodingProvider.Instance);
            ShiftJIS = Encoding.GetEncoding(932);
        }

        public event Action<string>? Log;

        private void LogMessage(string message)
        {
            Log?.Invoke(message);
        }

        private void LogError(string message)
        {
            Log?.Invoke($"Error: {message}");
        }

        public int ExportMesToJson(string inputPath, string outputPath)
        {
            LogMessage($"Exporting {Path.GetFileName(inputPath)}... ");
            
            try
            {
                var script = new MesScript();
                script.Load(inputPath);
                
                var entries = script.ExtractText(ShiftJIS);
                
                if (entries.Count < 5)
                {
                    var patternEntries = script.ExtractTextPatternBased(ShiftJIS);
                    if (patternEntries.Count > entries.Count)
                        entries = patternEntries;
                }

                var missedJapanese = script.FindUnextractedJapaneseTextCandidates(
                    ShiftJIS, entries.Select(e => e.Offset));
                if (missedJapanese.Count > 0)
                {
                    LogMessage($"Warning: {missedJapanese.Count} likely Japanese text candidate(s) were not exported.");
                    foreach (var missed in missedJapanese.Take(5))
                        LogMessage($"  offset 0x{missed.Offset:X}: {missed.Message}");
                    if (missedJapanese.Count > 5)
                        LogMessage("  ...");
                }
                
                var processedEntries = ProcessEntries(entries);
                
                var outputList = new List<object>();
                foreach (var entry in processedEntries)
                {
                    if (!string.IsNullOrEmpty(entry.Name))
                    {
                        outputList.Add(new { name = entry.Name, message = entry.Message });
                    }
                    else
                    {
                        outputList.Add(new { message = entry.Message });
                    }
                }
                
                string json = JsonSerializer.Serialize(outputList, JsonOptions);
                File.WriteAllText(outputPath, json, Encoding.UTF8);
                
                string auditSuffix = outputList.Count == 0 && missedJapanese.Count == 0
                    ? ", no JP text candidates"
                    : "";
                LogMessage($"OK ({outputList.Count} entries{auditSuffix})");
                return 0;
            }
            catch (Exception ex)
            {
                LogError($"Failed to export {inputPath}: {ex.Message}");
                return 1;
            }
        }

        private List<TextEntry> ProcessEntries(List<TextEntry> entries)
        {
            var processed = new List<TextEntry>();

            for (int i = 0; i < entries.Count; i++)
            {
                var current = entries[i];
                if (i + 1 < entries.Count && IsLikelySpeakerName(current) &&
                    IsDialogueMessage(entries[i + 1].Message))
                {
                    var dialogue = entries[i + 1];
                    processed.Add(new TextEntry
                    {
                        Offset = dialogue.Offset,
                        OriginalLength = dialogue.OriginalLength,
                        Opcode = dialogue.Opcode,
                        IsEncrypted = dialogue.IsEncrypted,
                        IsChoice = dialogue.IsChoice,
                        Name = NormalizeJsonAsciiWidth(current.Message),
                        Message = NormalizeJsonAsciiWidth(dialogue.Message)
                    });
                    i++;
                    continue;
                }

                processed.Add(new TextEntry
                {
                    Offset = current.Offset,
                    OriginalLength = current.OriginalLength,
                    Opcode = current.Opcode,
                    IsEncrypted = current.IsEncrypted,
                    IsChoice = current.IsChoice,
                    Name = string.IsNullOrEmpty(current.Name) ? null : NormalizeJsonAsciiWidth(current.Name),
                    Message = NormalizeJsonAsciiWidth(current.Message)
                });
            }

            return processed;
        }

        private static bool IsDialogueMessage(string? text)
        {
            if (string.IsNullOrWhiteSpace(text))
                return false;

            text = text.Trim();
            return text.StartsWith("「") || text.StartsWith("『") ||
                   text.StartsWith("\"") || text.StartsWith("\u201c");
        }

        private static bool IsLikelySpeakerName(TextEntry entry)
        {
            string text = entry.Message?.Trim() ?? "";
            if (text.Length == 0 || text.Length > 32 || entry.IsChoice)
                return false;
            if (IsDialogueMessage(text))
                return false;
            if (text.Contains('\n') || text.Contains('\r'))
                return false;

            char[] sentencePunctuation =
            {
                '。', '、', '，', ',', '.', '！', '!', '？', '?', '「', '」',
                '『', '』', '（', '）', '(', ')', ':', ';'
            };
            if (text.IndexOfAny(sentencePunctuation) >= 0)
                return false;

            return true;
        }

        private static string NormalizeJsonAsciiWidth(string text)
        {
            if (string.IsNullOrEmpty(text))
                return text;

            var sb = new StringBuilder(text.Length);
            foreach (char c in text)
            {
                if (c >= '\uFF01' && c <= '\uFF5E')
                    sb.Append((char)(c - 0xFEE0));
                else if (c == '\u3000')
                    sb.Append(' ');
                else
                    sb.Append(c);
            }
            return sb.ToString();
        }

        private static string NormalizeImportedMessageText(string text)
        {
            return NormalizeJsonAsciiWidth(text);
        }

        public int ImportJsonToMes(string jsonPath, string originalMesPath, string outputPath, int wrapWidth = 70)
        {
            LogMessage($"Importing {Path.GetFileName(jsonPath)}... ");
            
            try
            {
                string baseName = Path.GetFileNameWithoutExtension(jsonPath);
                bool isSceneReplay = IsSceneReplayListFile(baseName);
                bool preserveUiLatin = IsPopupUiTextFile(baseName);
                string json = File.ReadAllText(jsonPath, Encoding.UTF8);
                var entries = JsonSerializer.Deserialize<List<TextEntry>>(json) 
                    ?? new List<TextEntry>();

                if (isSceneReplay)
                {
                    var sceneScript = new MesScript();
                    sceneScript.Load(originalMesPath);
                    UpdateSceneReplayTitleMap(sceneScript, entries, outputPath, baseName);

                    var replayEntries = BuildSceneReplayImportEntries(sceneScript, entries, baseName);
                    UpdateSceneReplayDisplayMap(entries, replayEntries, outputPath, baseName);
                    byte[] sceneReplayMes = sceneScript.ImportText(replayEntries, ShiftJIS,
                        formatter: null,
                        preserveLatinAscii: true);
                    File.WriteAllBytes(outputPath, sceneReplayMes);

                    LogMessage("Using scene replay full-title mode: imported full Indonesian titles with Latin spaces.");
                    LogMessage($"OK ({entries.Count} replay entries -> {replayEntries.Count} full-title entries)");
                    return 0;
                }
                
                var expandedEntries = new List<TextEntry>();
                for (int i = 0; i < entries.Count; i++)
                {
                    var entry = entries[i];
                    if (IsChoiceBranchId(entry.Message) &&
                        i + 1 < entries.Count &&
                        IsChoiceDisplayText(entries[i + 1].Message))
                    {
                        continue;
                    }

                    if (!string.IsNullOrEmpty(entry.Name))
                    {
                        expandedEntries.Add(new TextEntry { Message = NormalizeJsonAsciiWidth(entry.Name) });
                    }
                    expandedEntries.Add(new TextEntry { Message = NormalizeImportedMessageText(entry.Message) });
                }
                
                var script = new MesScript();
                script.Load(originalMesPath);
                
                TextFormatter? formatter = new() { MaxLineLength = wrapWidth, MinLineLength = Math.Max(18, wrapWidth - 10) };

                byte[] newMes = script.ImportText(expandedEntries, ShiftJIS,
                    formatter, preserveLatinAscii: preserveUiLatin);
                File.WriteAllBytes(outputPath, newMes);
                
                LogMessage($"OK ({entries.Count} entries -> {expandedEntries.Count} expanded)");
                return 0;
            }
            catch (Exception ex)
            {
                LogError($"Failed to import {jsonPath}: {ex.Message}");
                return 1;
            }
        }

        private static bool IsPopupUiTextFile(string baseName)
        {
            return baseName.Equals("main", StringComparison.OrdinalIgnoreCase) ||
                   baseName.Equals("LOAD", StringComparison.OrdinalIgnoreCase) ||
                   baseName.Equals("SAVE", StringComparison.OrdinalIgnoreCase) ||
                   baseName.Equals("advmenu", StringComparison.OrdinalIgnoreCase) ||
                   baseName.Equals("option", StringComparison.OrdinalIgnoreCase) ||
                   baseName.Equals("title_00", StringComparison.OrdinalIgnoreCase) ||
                   baseName.Equals("title_01", StringComparison.OrdinalIgnoreCase) ||
                   baseName.Equals("title_02", StringComparison.OrdinalIgnoreCase) ||
                   baseName.Equals("title_03", StringComparison.OrdinalIgnoreCase);
        }

        private static bool IsChoiceBranchId(string? text)
        {
            if (string.IsNullOrWhiteSpace(text))
                return false;

            text = text.Trim();
            if (text.Length < 1 || text.Length > 3)
                return false;

            return text.All(c => c >= '0' && c <= '9');
        }

        private static bool IsChoiceDisplayText(string? text)
        {
            return !string.IsNullOrWhiteSpace(text) &&
                   text.TrimStart().StartsWith("○", StringComparison.Ordinal);
        }

        private void UpdateSceneReplayTitleMap(MesScript script, List<TextEntry> entries,
            string outputPath, string baseName)
        {
            string mapPath = GetReplayTitleMapPath(outputPath);
            Directory.CreateDirectory(Path.GetDirectoryName(mapPath) ?? ".");

            var rows = new Dictionary<string, (string Id, string File)>();
            if (File.Exists(mapPath))
            {
                foreach (string line in File.ReadAllLines(mapPath, Encoding.UTF8))
                {
                    if (string.IsNullOrWhiteSpace(line) || line.StartsWith("#"))
                        continue;

                    string[] parts = line.Split('\t');
                    if (parts.Length >= 2 && !string.IsNullOrWhiteSpace(parts[0]))
                        rows[parts[0]] = (parts[1], parts.Length >= 3 ? parts[2] : "");
                }
            }

            var originalEntries = script.ExtractText(ShiftJIS);
            int count = Math.Min(originalEntries.Count, entries.Count);
            for (int i = 0; i < count; i++)
            {
                string jp = originalEntries[i].Message?.Trim() ?? "";
                string id = NormalizeJsonAsciiWidth(entries[i].Message ?? "").Trim();
                if (string.IsNullOrEmpty(jp) || string.IsNullOrEmpty(id) ||
                    IsQuestionPlaceholder(jp) || IsQuestionPlaceholder(id) || jp == id)
                {
                    continue;
                }

                rows[jp] = (id, baseName + ".json");
            }

            using var writer = new StreamWriter(mapPath, false, new UTF8Encoding(false));
            writer.WriteLine("# JP\tID\tfile");
            foreach (var row in rows.OrderBy(r => r.Value.File).ThenBy(r => r.Key, StringComparer.Ordinal))
                writer.WriteLine($"{row.Key}\t{row.Value.Id}\t{row.Value.File}");
        }

        private void UpdateSceneReplayDisplayMap(List<TextEntry> fullEntries,
            List<TextEntry> displayEntries, string outputPath, string baseName)
        {
            string mapPath = GetReplayDisplayMapPath(outputPath);
            Directory.CreateDirectory(Path.GetDirectoryName(mapPath) ?? ".");

            var rows = new Dictionary<string, (string Full, string File)>();
            if (File.Exists(mapPath))
            {
                foreach (string line in File.ReadAllLines(mapPath, Encoding.UTF8))
                {
                    if (string.IsNullOrWhiteSpace(line) || line.StartsWith("#"))
                        continue;

                    string[] parts = line.Split('\t');
                    if (parts.Length >= 2 && !string.IsNullOrWhiteSpace(parts[0]))
                        rows[parts[0]] = (parts[1], parts.Length >= 3 ? parts[2] : "");
                }
            }

            int count = Math.Min(fullEntries.Count, displayEntries.Count);
            for (int i = 0; i < count; i++)
            {
                string full = NormalizeJsonAsciiWidth(fullEntries[i].Message ?? "").Trim();
                string display = NormalizeJsonAsciiWidth(displayEntries[i].Message ?? "").Trim();
                if (string.IsNullOrEmpty(full) || string.IsNullOrEmpty(display) ||
                    IsQuestionPlaceholder(full) || IsQuestionPlaceholder(display))
                {
                    continue;
                }

                rows[display] = (full, baseName + ".json");
            }

            using var writer = new StreamWriter(mapPath, false, new UTF8Encoding(false));
            writer.WriteLine("# DISPLAY\tFULL\tfile");
            foreach (var row in rows.OrderBy(r => r.Value.File).ThenBy(r => r.Key, StringComparer.Ordinal))
                writer.WriteLine($"{row.Key}\t{row.Value.Full}\t{row.Value.File}");
        }

        private static string GetReplayTitleMapPath(string outputPath)
        {
            string? mesDir = Path.GetDirectoryName(outputPath);
            string? idDataDir = Path.GetDirectoryName(mesDir ?? "");
            if (!string.IsNullOrEmpty(idDataDir) &&
                string.Equals(Path.GetFileName(idDataDir), "id_Data", StringComparison.OrdinalIgnoreCase))
            {
                return Path.Combine(idDataDir, "replay_titles.tsv");
            }

            return Path.Combine(mesDir ?? ".", "replay_titles.tsv");
        }

        private static string GetReplayDisplayMapPath(string outputPath)
        {
            string? mesDir = Path.GetDirectoryName(outputPath);
            string? idDataDir = Path.GetDirectoryName(mesDir ?? "");
            if (!string.IsNullOrEmpty(idDataDir) &&
                string.Equals(Path.GetFileName(idDataDir), "id_Data", StringComparison.OrdinalIgnoreCase))
            {
                return Path.Combine(idDataDir, "replay_display_titles.tsv");
            }

            return Path.Combine(mesDir ?? ".", "replay_display_titles.tsv");
        }

        private static bool IsQuestionPlaceholder(string? text)
        {
            if (string.IsNullOrWhiteSpace(text))
                return false;

            text = text.Trim();
            return text.Length >= 3 && text.All(c => c == '?' || c == '？');
        }

        private static List<TextEntry> BuildSceneReplayImportEntries(MesScript script, List<TextEntry> entries,
            string baseName)
        {
            var originalEntries = script.ExtractText(ShiftJIS);
            int count = Math.Min(originalEntries.Count, entries.Count);
            var result = new List<TextEntry>(originalEntries.Count);

            for (int i = 0; i < count; i++)
            {
                string original = originalEntries[i].Message ?? "";
                string translated = NormalizeJsonAsciiWidth(entries[i].Message ?? "").Trim();
                string message = IsQuestionPlaceholder(translated)
                    ? original
                    : FormatSceneReplayTitle(translated);
                if (IsErikaSceneReplay(baseName))
                    message = MakeErikaReplayTitleEngineSafe(message);

                result.Add(new TextEntry { Message = message });
            }

            for (int i = count; i < originalEntries.Count; i++)
                result.Add(new TextEntry { Message = originalEntries[i].Message ?? "" });

            return result;
        }

        private static string FormatSceneReplayTitle(string text)
        {
            if (string.IsNullOrWhiteSpace(text))
                return text;

            text = text.Trim()
                       .Replace("\r", " ", StringComparison.Ordinal)
                       .Replace("\n", " ", StringComparison.Ordinal);
            while (text.Contains("  ", StringComparison.Ordinal))
                text = text.Replace("  ", " ", StringComparison.Ordinal);

            return text;
        }

        private static bool IsErikaSceneReplay(string baseName)
        {
            return baseName.Equals("sce_erika", StringComparison.OrdinalIgnoreCase);
        }

        private static string MakeErikaReplayTitleEngineSafe(string text)
        {
            if (string.IsNullOrWhiteSpace(text))
                return text;

            const int safeLimit = 45;
            if (ShiftJIS.GetByteCount(text) <= safeLimit)
                return text;

            string normalized = text.Trim();
            int tilde = normalized.IndexOf('~');
            if (tilde >= 0 && tilde + 1 < normalized.Length)
            {
                string right = normalized[(tilde + 1)..].Trim();
                if (!string.IsNullOrWhiteSpace(right) &&
                    ShiftJIS.GetByteCount(right) <= safeLimit)
                {
                    return right;
                }
            }

            string[] prefixes =
            [
                "Pertempuran Jarak Dekat yang Kacau",
                "Pertempuran Jarak Dekat",
                "Pertempuran Penentu"
            ];

            foreach (string prefix in prefixes)
            {
                if (normalized.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
                {
                    string rest = normalized[prefix.Length..].Trim(' ', '-', '~');
                    if (!string.IsNullOrWhiteSpace(rest) &&
                        ShiftJIS.GetByteCount(rest) <= safeLimit)
                    {
                        return rest;
                    }
                }
            }

            var words = normalized.Split(' ', StringSplitOptions.RemoveEmptyEntries);
            var kept = new List<string>();
            foreach (string word in words)
            {
                string candidate = kept.Count == 0 ? word : string.Join(" ", kept) + " " + word;
                if (ShiftJIS.GetByteCount(candidate) > safeLimit)
                    break;
                kept.Add(word);
            }

            return kept.Count > 0 ? string.Join(" ", kept) : normalized;
        }

        private static string FitSceneReplayTitle(string text, int maxBytes)
        {
            if (maxBytes <= 0 || ShiftJIS.GetByteCount(text) <= maxBytes)
                return text;

            string[] removeWords =
            [
                "yang", "di", "dan", "dengan", "untuk", "ke", "dalam",
                "adalah", "sebuah", "seorang", "para"
            ];

            var words = text.Split(' ', StringSplitOptions.RemoveEmptyEntries).ToList();
            foreach (string removable in removeWords)
            {
                for (int i = words.Count - 1; i >= 0; i--)
                {
                    if (string.Equals(words[i], removable, StringComparison.OrdinalIgnoreCase))
                    {
                        words.RemoveAt(i);
                        string candidate = string.Join(" ", words);
                        if (ShiftJIS.GetByteCount(candidate) <= maxBytes)
                            return candidate;
                    }
                }
            }

            string compact = string.Join(" ", words)
                .Replace("Pertempuran", "Tempur", StringComparison.OrdinalIgnoreCase)
                .Replace("Permainan", "Main", StringComparison.OrdinalIgnoreCase)
                .Replace("Peringatan", "Ingat", StringComparison.OrdinalIgnoreCase)
                .Replace("Pengumuman", "Umumkan", StringComparison.OrdinalIgnoreCase)
                .Replace("Pembicaraan", "Obrolan", StringComparison.OrdinalIgnoreCase)
                .Replace("Kesalahpahaman", "Salah Paham", StringComparison.OrdinalIgnoreCase)
                .Replace("Perjalanan", "Jalan", StringComparison.OrdinalIgnoreCase)
                .Replace("Kembali", "Balik", StringComparison.OrdinalIgnoreCase)
                .Replace("Ketua Kelas", "Ketua", StringComparison.OrdinalIgnoreCase);

            while (compact.Contains("  ", StringComparison.Ordinal))
                compact = compact.Replace("  ", " ", StringComparison.Ordinal);
            compact = compact.Trim();

            if (ShiftJIS.GetByteCount(compact) <= maxBytes)
                return compact;

            var sb = new StringBuilder();
            foreach (char c in compact)
            {
                string candidate = sb.ToString() + c;
                if (ShiftJIS.GetByteCount(candidate) > maxBytes)
                    break;
                sb.Append(c);
            }

            return sb.ToString().TrimEnd();
        }

        private static string WrapSceneReplayTitle(string text)
        {
            const int maxFirstLine = 22;
            if (text.Length <= maxFirstLine)
                return text;

            int split = -1;
            for (int i = Math.Min(maxFirstLine, text.Length - 1); i >= 10; i--)
            {
                char c = text[i];
                if (c == ' ' || c == '-' || c == '/' || c == ':' || c == ',')
                {
                    split = i + (c == ' ' ? 0 : 1);
                    break;
                }
            }

            if (split < 0)
                split = Math.Min(maxFirstLine, text.Length - 1);

            string first = text[..split].Trim();
            string second = text[split..].Trim();
            if (string.IsNullOrEmpty(first) || string.IsNullOrEmpty(second))
                return text;

            return first + "\n" + second;
        }

        public int BatchExport(string inputFolder, string outputFolder)
        {
            if (!Directory.Exists(inputFolder))
            {
                LogError($"Folder not found: {inputFolder}");
                return 1;
            }

            Directory.CreateDirectory(outputFolder);
            var mesFiles = Directory.GetFiles(inputFolder, "*.mes");
            LogMessage($"Found {mesFiles.Length} MES files in {inputFolder}");

            int success = 0;
            int failed = 0;

            foreach (var mesFile in mesFiles)
            {
                string outputPath = Path.Combine(outputFolder, Path.GetFileNameWithoutExtension(mesFile) + ".json");
                if (ExportMesToJson(mesFile, outputPath) == 0)
                    success++;
                else
                    failed++;
            }
            
            LogMessage($"Batch Export Completed: {success} success, {failed} failed");
            return failed > 0 ? 1 : 0;
        }

        public int BatchImport(string jsonFolder, string mesFolder, string outputFolder, int wrapWidth)
        {
            if (!Directory.Exists(jsonFolder))
            {
                LogError($"JSON folder not found: {jsonFolder}");
                return 1;
            }
            
            if (!Directory.Exists(mesFolder))
            {
                LogError($"MES folder not found: {mesFolder}");
                return 1;
            }

            Directory.CreateDirectory(outputFolder);
            var jsonFiles = Directory.GetFiles(jsonFolder, "*.json");
            LogMessage($"Found {jsonFiles.Length} JSON files (wrap width: {wrapWidth})");

            int success = 0;
            int failed = 0;

            foreach (var jsonFile in jsonFiles)
            {
                string baseName = Path.GetFileNameWithoutExtension(jsonFile);
                string mesFile = Path.Combine(mesFolder, baseName + ".mes");
                string outputPath = Path.Combine(outputFolder, baseName + ".mes");
                
                if (!File.Exists(mesFile))
                {
                    LogMessage($"Skipping {baseName} - original MES not found");
                    continue;
                }
                
                if (ImportJsonToMes(jsonFile, mesFile, outputPath, wrapWidth) == 0)
                    success++;
                else
                    failed++;
            }

            LogMessage($"Batch Import Completed: {success} success, {failed} failed");
            return failed > 0 ? 1 : 0;
        }

        private static bool IsSceneReplayListFile(string baseName)
        {
            return baseName.StartsWith("sce_", StringComparison.OrdinalIgnoreCase);
        }
    }
}
