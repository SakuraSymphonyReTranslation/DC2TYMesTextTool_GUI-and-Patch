using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;

namespace DC2TYMesTextTool;

/// <summary>
/// MES script parser and text extractor.
/// </summary>
public class MesScript
{
    private byte[] _rawData = Array.Empty<byte>();
    private ScriptInfo _info = ScriptInfo.DC2TY;
    private List<Token> _tokens = new();
    private int _labelCount;
    private int _bytecodeOffset;
    private int[] _labels = Array.Empty<int>();
    
    // Regex pattern for detecting choice text
    private static readonly Regex ChoicePattern = new(@"戟ｅ訓.*|ワ[一-龯]", RegexOptions.Compiled);
    
    public byte[] RawData => _rawData;
    public ScriptInfo Info => _info;
    public IReadOnlyList<Token> Tokens => _tokens;
    public int LabelCount => _labelCount;
    public int BytecodeOffset => _bytecodeOffset;
    
    public void Load(byte[] data, ScriptInfo? info = null)
    {
        _rawData = data;
        _info = info ?? ScriptInfo.QueryFromData(data) ?? ScriptInfo.DC2TY;
        
        ParseStructure();
        TokenizeBytecode();
    }
    
    public void Load(string path, ScriptInfo? info = null)
    {
        Load(File.ReadAllBytes(path), info);
    }
    
    private void ParseStructure()
    {
        if (_rawData.Length < 8)
            throw new InvalidDataException("MES file too small");
            
        int rawCount = BitConverter.ToInt32(_rawData, 0);
        
        // DC2TY uses the Offset1 header layout: four bytes per label.
        _bytecodeOffset = rawCount * 4 + 4;
        
        if (_bytecodeOffset >= _rawData.Length)
            throw new InvalidDataException("Invalid MES structure");
            
        // Some original DC2TY files include a three-byte version marker.
        int labelStartOffset = 4;
        if (BitConverter.ToInt32(_rawData, 4) == 3)
        {
            labelStartOffset = 8;
        }

        // Calculate stride
        int stride = 4;
        
        // Use explicit count from header
        _labelCount = rawCount;
        
        _labels = new int[_labelCount];
        for (int i = 0; i < _labelCount; i++)
        {
            _labels[i] = BitConverter.ToInt32(_rawData, labelStartOffset + i * stride);
        }
    }
    
    private int CalcVersionLength()
    {
        // Check if header format implies version length.
        if ((int)_info.OffsetType == (int)OffsetType.Offset1)
        {
             // If header[1] == 3, version len is 3
             // But simpler to rely on labels[0] or raw check
             if (BitConverter.ToInt32(_rawData, 4) == 3) return 3;
        }
        return (_info.Version & 0xFF00) == 0 ? 1 : 2;
    }

    private void TokenizeBytecode()
    {
        _tokens.Clear();
        // Start after the version token
        int pos = _bytecodeOffset + CalcVersionLength();
        
        while (pos < _rawData.Length)
        {
            byte opcode = _rawData[pos];
            int tokenLength;
            
            // Determine token length based on opcode type
            if (_info.IsInSection(_info.Uint8x2, opcode))
            {
                // [op][arg1][arg2] = 3 bytes
                tokenLength = 3;
            }
            else if (_info.IsInSection(_info.Uint8Str, opcode))
            {
                // [op][arg1][string\0]
                tokenLength = 2 + GetStringLength(pos + 2) + 1;
            }
            else if (_info.IsInSection(_info.String, opcode))
            {
                // [op][string\0]
                tokenLength = 1 + GetStringLength(pos + 1) + 1;
            }
            else if (_info.IsInSection(_info.EncStr, opcode))
            {
                // [op][encstring\0]
                tokenLength = 1 + GetStringLength(pos + 1) + 1;
            }
            else if (_info.IsInSection(_info.Uint16x4, opcode))
            {
                // [op][4 x uint16] = 9 bytes
                tokenLength = 9;
            }
            else
            {
                // Unknown opcode, skip 1 byte
                tokenLength = 1;
            }
            
            // Bounds check
            if (pos + tokenLength > _rawData.Length)
                break;
                
            // Tokens are relative to the start of ACTUAL bytecode (after version token)
            _tokens.Add(new Token(opcode, pos - (_bytecodeOffset + CalcVersionLength()), tokenLength));
            pos += tokenLength;
        }
    }
    
    private int GetStringLength(int offset)
    {
        int length = 0;
        while (offset + length < _rawData.Length && _rawData[offset + length] != 0)
        {
            length++;
        }
        return length;
    }
    
    public List<TextEntry> ExtractText(Encoding encoding, bool verbose = false)
    {
        var entries = new List<TextEntry>();
        int emptyCount = 0;
        
        foreach (var token in _tokens)
        {
            string? text = null;
            bool isEncrypted = false;
            
            if (_info.IsInSection(_info.EncStr, token.Opcode))
            {
                text = DecryptString(token.Offset + 1, encoding);
                isEncrypted = true;
            }
            else if (_info.OutputOpcodes.Contains(token.Opcode))
            {
                text = ReadString(token.Offset + 1, encoding);
            }
            
            if (text == null) continue;
            
            if (string.IsNullOrEmpty(text))
            {
                emptyCount++;
                continue;
            }
                
            text = CleanText(text);
            if (IsChoiceLabel(text))
                text = RestoreChoiceLabelForJson(text);
            
            if (string.IsNullOrEmpty(text))
            {
                emptyCount++;
                continue;
            }
            
            bool isChoice = ChoicePattern.IsMatch(text);
            
            var entry = new TextEntry
            {
                Offset = token.Offset,
                OriginalLength = token.Length,
                Opcode = token.Opcode,
                IsEncrypted = isEncrypted,
                Message = text,
                IsChoice = isChoice
            };
            
            entries.Add(entry);
        }

        AddMissedEncryptedTextEntries(entries, encoding);
        
        return entries;
    }

    private void AddMissedEncryptedTextEntries(List<TextEntry> entries, Encoding encoding)
    {
        var knownOffsets = new HashSet<int>(entries.Select(e => e.Offset));
        int baseOffset = _bytecodeOffset + CalcVersionLength();
        var coveredOffsets = new bool[Math.Max(0, _rawData.Length - baseOffset)];
        foreach (var token in _tokens)
        {
            int start = Math.Max(0, token.Offset);
            int end = Math.Min(coveredOffsets.Length, token.Offset + token.Length);
            for (int i = start; i < end; i++)
                coveredOffsets[i] = true;
        }

        int pos = baseOffset;

        while (pos + 2 < _rawData.Length)
        {
            int rel = pos - baseOffset;
            if (rel >= 0 && rel < coveredOffsets.Length && coveredOffsets[rel])
            {
                pos++;
                continue;
            }

            byte opcode = _rawData[pos];
            if (!_info.IsInSection(_info.EncStr, opcode) ||
                knownOffsets.Contains(rel))
            {
                pos++;
                continue;
            }

            int textLen = GetStringLength(pos + 1);
            if (textLen <= 0 || textLen > 512 || pos + 1 + textLen >= _rawData.Length)
            {
                pos++;
                continue;
            }

            string text = DecryptBytes(pos + 1, textLen, encoding);
            text = CleanText(text);
            if (IsChoiceLabel(text))
                text = RestoreChoiceLabelForJson(text);

            if (IsLikelyMissedText(text))
            {
                entries.Add(new TextEntry
                {
                    Offset = rel,
                    OriginalLength = 1 + textLen + 1,
                    Opcode = opcode,
                    IsEncrypted = true,
                    Message = text,
                    IsChoice = ChoicePattern.IsMatch(text)
                });
                knownOffsets.Add(rel);
                pos += textLen + 2;
                continue;
            }

            pos++;
        }

        entries.Sort((a, b) => a.Offset.CompareTo(b.Offset));
    }

    private bool IsLikelyMissedText(string text)
    {
        if (string.IsNullOrWhiteSpace(text))
            return false;

        string t = text.Trim();
        if (t.Length <= 1)
            return false;
        if (t.StartsWith("VOICE\\", StringComparison.OrdinalIgnoreCase) ||
            t.StartsWith("voice\\", StringComparison.OrdinalIgnoreCase) ||
            t.StartsWith("bg\\", StringComparison.OrdinalIgnoreCase) ||
            t.StartsWith("bst\\", StringComparison.OrdinalIgnoreCase))
            return false;

        return t.StartsWith("「", StringComparison.Ordinal) ||
               t.StartsWith("『", StringComparison.Ordinal) ||
               t.StartsWith("\"", StringComparison.Ordinal) ||
               t.StartsWith("\u201c", StringComparison.Ordinal) ||
               t.IndexOfAny(new[] { '。', '、', '，', ',', '.', '！', '!', '？', '?', '\n', '\r' }) >= 0 ||
               t.Any(c => (c >= '\u3040' && c <= '\u30FF') || (c >= '\u4E00' && c <= '\u9FFF'));
    }
    
    public List<TextEntry> ExtractTextPatternBased(Encoding encoding)
    {
        // DC2TY extraction is opcode-based; an empty result indicates an invalid or unsupported file.
        return new List<TextEntry>();
    }

    public List<TextEntry> FindUnextractedJapaneseTextCandidates(
        Encoding encoding, IEnumerable<int> extractedOffsets)
    {
        var extracted = new HashSet<int>(extractedOffsets);
        var candidates = new List<TextEntry>();

        foreach (var token in _tokens)
        {
            if (extracted.Contains(token.Offset))
                continue;

            string? text = null;
            bool encrypted = false;
            int stringRelativeOffset;

            if (_info.IsInSection(_info.EncStr, token.Opcode))
            {
                stringRelativeOffset = token.Offset + 1;
                text = DecryptString(stringRelativeOffset, encoding);
                encrypted = true;
            }
            else if (_info.IsInSection(_info.String, token.Opcode))
            {
                stringRelativeOffset = token.Offset + 1;
                text = ReadString(stringRelativeOffset, encoding);
            }
            else if (_info.IsInSection(_info.Uint8Str, token.Opcode))
            {
                stringRelativeOffset = token.Offset + 2;
                text = ReadString(stringRelativeOffset, encoding);
            }
            else
            {
                continue;
            }

            text = CleanText(text ?? "");
            if (!IsLikelyJapaneseUserText(text))
                continue;

            candidates.Add(new TextEntry
            {
                Offset = token.Offset,
                OriginalLength = token.Length,
                Opcode = token.Opcode,
                IsEncrypted = encrypted,
                Message = text,
                IsChoice = ChoicePattern.IsMatch(text)
            });
        }

        return candidates;
    }

    private static bool IsLikelyJapaneseUserText(string text)
    {
        if (string.IsNullOrWhiteSpace(text))
            return false;

        string t = text.Trim();
        if (t.Length <= 1 || t.Length > 512)
            return false;
        if (t.StartsWith("VOICE\\", StringComparison.OrdinalIgnoreCase) ||
            t.StartsWith("voice\\", StringComparison.OrdinalIgnoreCase) ||
            t.StartsWith("bg\\", StringComparison.OrdinalIgnoreCase) ||
            t.StartsWith("bst\\", StringComparison.OrdinalIgnoreCase) ||
            t.StartsWith("system\\", StringComparison.OrdinalIgnoreCase) ||
            t.StartsWith("SYSTEM\\", StringComparison.OrdinalIgnoreCase) ||
            t.Contains('\\'))
            return false;

        int jpCount = 0;
        int controlCount = 0;
        foreach (char c in t)
        {
            if (char.IsControl(c) && c != '\r' && c != '\n' && c != '\t')
                controlCount++;
            if ((c >= '\u3040' && c <= '\u30FF') ||
                (c >= '\u3400' && c <= '\u9FFF') ||
                "「」。、！？・ー～♪♫♬（）".IndexOf(c) >= 0)
                jpCount++;
        }

        if (controlCount > 0)
            return false;
        return jpCount >= 2 || t.StartsWith("「", StringComparison.Ordinal) ||
               t.StartsWith("『", StringComparison.Ordinal);
    }

    public byte[] ImportText(List<TextEntry> translatedEntries, Encoding encoding,
        TextFormatter? formatter = null, bool preserveLatinAscii = false)
    {
         // Re-extract original text to get offsets
        var originalEntries = ExtractText(encoding, false).ToList();
        
        var translationMap = new Dictionary<int, string>();
        for (int i = 0; i < Math.Min(originalEntries.Count, translatedEntries.Count); i++)
        {
            var origEntry = originalEntries[i];
            var text = translatedEntries[i].Message;
            
            if (formatter != null && !string.IsNullOrEmpty(text))
            {
                int originalMax = formatter.MaxLineLength;
                if (origEntry.Opcode == 0x6A) formatter.MaxLineLength = 1000;
                text = formatter.Format(text);
                formatter.MaxLineLength = originalMax;
            }

            if (preserveLatinAscii && IsQuestionPlaceholder(text))
                continue;
            
            translationMap[origEntry.Offset] = text;
        }
        
        int versionLen = CalcVersionLength();
        
        // Clone raw data for the header
        byte[] newHeader = new byte[_bytecodeOffset];
        Array.Copy(_rawData, newHeader, _bytecodeOffset);
        
        int labelStartOffset = BitConverter.ToInt32(_rawData, 4) == 3 ? 8 : 4;
        
        // Note: _labelCount is now derived from available space
        
        // Determine stride
        int stride = 4;
        int firstTokenLength = labelStartOffset == 8 ? 3 : (((_info.Version & 0xFF00) == 0) ? 1 : 2);
        
        using var bytecodeOutput = new MemoryStream();
        int labelIndex = 0;

        foreach (var token in _tokens)
        {
            long currentPos = bytecodeOutput.Position; // Position relative to start of bytecode
            int newTokenLength = GetNewTokenLength(token, translationMap, encoding, preserveLatinAscii);

            if ((int)_info.OffsetType == (int)OffsetType.Offset1 && labelStartOffset == 4 && labelIndex < _labelCount)
            {
                int oldLabel = BitConverter.ToInt32(newHeader, labelStartOffset + labelIndex * stride);
                if (token.Offset + firstTokenLength == oldLabel)
                {
                    int newLabel = (int)currentPos + firstTokenLength;
                    byte[] labelBytes = BitConverter.GetBytes(newLabel);
                    Array.Copy(labelBytes, 0, newHeader, labelStartOffset + labelIndex * stride, 4);
                    labelIndex++;
                }
            }
            
            // Write Token
            bool isEncStr = _info.IsInSection(_info.EncStr, token.Opcode);
            bool isOutputOp = _info.OutputOpcodes.Contains(token.Opcode);
            
            if ((isEncStr || isOutputOp) && translationMap.TryGetValue(token.Offset, out var text))
            {
                string originalText = isEncStr
                    ? DecryptString(token.Offset + 1, encoding)
                    : ReadString(token.Offset + 1, encoding);
                text = PrepareTextForMesWrite(text, originalText, preserveLatinAscii, isEncStr);
                byte[] textBytes = encoding.GetBytes(text);
                if (isEncStr)
                {
                    for (int i = 0; i < textBytes.Length; i++)
                    {
                        textBytes[i] = (byte)((textBytes[i] - _info.EncryptionKey) & 0xFF);
                    }
                }
                bytecodeOutput.WriteByte(token.Opcode);
                bytecodeOutput.Write(textBytes, 0, textBytes.Length);
                bytecodeOutput.WriteByte(0);
            }
            else
            {
                bytecodeOutput.Write(_rawData, _bytecodeOffset + versionLen + token.Offset, token.Length);
            }
        }
        
        // Construct Final File
        using var output = new MemoryStream();
        
        output.Write(newHeader, 0, newHeader.Length);
        output.Write(_rawData, _bytecodeOffset, versionLen);
        bytecodeOutput.Position = 0;
        bytecodeOutput.CopyTo(output);
        
        return output.ToArray();
    }

    public byte[] ImportTextFixedLength(List<TextEntry> translatedEntries,
        Encoding encoding, bool preserveLatinAscii = false,
        bool useInvisibleSceneReplayPadding = false)
    {
        var originalEntries = ExtractText(encoding, false).ToList();
        var output = new byte[_rawData.Length];
        Array.Copy(_rawData, output, _rawData.Length);

        for (int i = 0; i < Math.Min(originalEntries.Count, translatedEntries.Count); i++)
        {
            var origEntry = originalEntries[i];
            string text = translatedEntries[i].Message ?? "";
            if (IsQuestionPlaceholder(text))
                continue;

            int tokenIndex = _tokens.FindIndex(t => t.Offset == origEntry.Offset);
            if (tokenIndex < 0)
                continue;
            var token = _tokens[tokenIndex];

            bool isEncStr = _info.IsInSection(_info.EncStr, token.Opcode);
            bool isOutputOp = _info.OutputOpcodes.Contains(token.Opcode);
            if (!isEncStr && !isOutputOp)
                continue;

            string originalText = isEncStr
                ? DecryptString(token.Offset + 1, encoding)
                : ReadString(token.Offset + 1, encoding);
            text = PrepareTextForMesWrite(text, originalText, preserveLatinAscii, isEncStr);
            byte[] textBytes = encoding.GetBytes(text);

            int maxBytes = Math.Max(0, token.Length - 2);
            if (textBytes.Length > maxBytes)
                textBytes = TruncateEncodedText(textBytes, maxBytes);

            if (isEncStr)
            {
                for (int j = 0; j < textBytes.Length; j++)
                    textBytes[j] = (byte)((textBytes[j] - _info.EncryptionKey) & 0xFF);
            }

            int absolute = _bytecodeOffset + CalcVersionLength() + token.Offset + 1;
            if (absolute < 0 || absolute >= output.Length)
                continue;

            int writable = Math.Min(maxBytes, output.Length - absolute - 1);
            if (writable < 0)
                continue;

            if (useInvisibleSceneReplayPadding)
            {
                FillSceneReplayPadding(output, absolute, writable, isEncStr);
            }
            else
            {
                byte padByte = (byte)'^';
                if (isEncStr)
                    padByte = (byte)((padByte - _info.EncryptionKey) & 0xFF);

                for (int j = 0; j < writable; j++)
                    output[absolute + j] = padByte;
            }

            Array.Copy(textBytes, 0, output, absolute,
                Math.Min(textBytes.Length, writable));
        }

        return output;
    }

    private void FillSceneReplayPadding(byte[] output, int absolute, int writable,
        bool encrypted)
    {
        byte Encode(byte b) => encrypted
            ? (byte)((b - _info.EncryptionKey) & 0xFF)
            : b;

        for (int j = 0; j < writable; j++)
            output[absolute + j] = Encode((byte)' ');
    }

    private static byte[] TruncateEncodedText(byte[] bytes, int maxBytes)
    {
        if (maxBytes <= 0)
            return Array.Empty<byte>();
        if (bytes.Length <= maxBytes)
            return bytes;

        int len = maxBytes;
        if (len > 0 && IsShiftJisLeadByte(bytes[len - 1]))
            len--;
        var truncated = new byte[len];
        Array.Copy(bytes, truncated, len);
        return truncated;
    }

    private static bool IsShiftJisLeadByte(byte b)
    {
        return (b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC);
    }
    
    private int GetNewTokenLength(Token token, Dictionary<int, string> translationMap,
        Encoding encoding, bool preserveLatinAscii = false)
    {
        bool isEncStr = _info.IsInSection(_info.EncStr, token.Opcode);
        bool isOutputOp = _info.OutputOpcodes.Contains(token.Opcode);
        
        if ((isEncStr || isOutputOp) && translationMap.TryGetValue(token.Offset, out var text))
        {
            string originalText = isEncStr
                ? DecryptString(token.Offset + 1, encoding)
                : ReadString(token.Offset + 1, encoding);
            text = PrepareTextForMesWrite(text, originalText, preserveLatinAscii);
            byte[] textBytes = encoding.GetBytes(text);
            return 1 + textBytes.Length + 1; // opcode + text + null
        }
        return token.Length;
    }

    private static string PrepareTextForMesWrite(string text, string originalText,
        bool preserveLatinAscii = false, bool encryptedString = false)
    {
        text = NormalizeMesInputText(text);
        if (IsAsciiChoiceId(originalText) && IsAsciiChoiceId(text))
            return text.Trim();
        if (IsChoiceLabel(text))
            return NormalizeChoiceLabelForMes(text, preserveLatinAscii);
        if (preserveLatinAscii)
            return NormalizeAsciiWidthForMesLatin(text, encryptedString);
        return ConvertAsciiToFullWidthForMes(text);
    }

    private static bool IsQuestionPlaceholder(string? text)
    {
        if (string.IsNullOrWhiteSpace(text))
            return false;

        text = text.Trim();
        return text.Length >= 3 && text.All(c => c == '?');
    }

    private static string NormalizeMesInputText(string text)
    {
        if (string.IsNullOrEmpty(text))
            return text;

        // ADV32's SJIS text path cannot safely carry several Unicode dash and
        // quote variants from JSON. Normalize them before width conversion so
        // re-imports do not leave hidden JP/garbled text in-game.
        text = text
            .Replace("\u2010", "-")
            .Replace("\u2011", "-")
            .Replace("\u2012", "-")
            .Replace("\u2013", " - ")
            .Replace("\u2014", " - ")
            .Replace("\u2015", " - ")
            .Replace("\u2212", "-")
            .Replace('\u2018', '\'')
            .Replace('\u2019', '\'')
            .Replace('\u201C', '"')
            .Replace('\u201D', '"');

        text = NormalizePauseMarksForLatinMes(text);
        text = NormalizeStandaloneEllipsis(text);

        while (text.Contains("  ", StringComparison.Ordinal))
            text = text.Replace("  ", " ", StringComparison.Ordinal);
        return text;
    }

    private static string NormalizePauseMarksForLatinMes(string text)
    {
        if (string.IsNullOrEmpty(text))
            return text;

        var sb = new StringBuilder(text.Length);
        bool inPauseRun = false;

        void FlushPauseRun()
        {
            if (!inPauseRun)
                return;
            sb.Append("...");
            inPauseRun = false;
        }

        foreach (char c in text)
        {
            if (c == '…' || c == '─')
            {
                inPauseRun = true;
                continue;
            }

            FlushPauseRun();
            sb.Append(c);
        }

        FlushPauseRun();
        return sb.ToString();
    }

    private static string NormalizeStandaloneEllipsis(string text)
    {
        string core = text.Trim();
        if (core.Length >= 2 &&
            ((core[0] == '「' && core[^1] == '」') ||
             (core[0] == '『' && core[^1] == '』') ||
             (core[0] == '"' && core[^1] == '"')))
        {
            core = core[1..^1].Trim();
        }

        if (core.Length == 0)
            return text;

        foreach (char c in core)
        {
            if (c != '.' && c != '…' && c != ' ' && c != '\u3000')
                return text;
        }

        var sb = new StringBuilder(text.Length);
        int dotRun = 0;

        void FlushDots()
        {
            if (dotRun == 0)
                return;

            if (dotRun >= 3)
            {
                int ellipsisCount = (dotRun + 2) / 3;
                sb.Append('…', ellipsisCount);
            }
            else
            {
                sb.Append('.', dotRun);
            }

            dotRun = 0;
        }

        foreach (char c in text)
        {
            if (c == '.')
            {
                dotRun++;
                continue;
            }

            FlushDots();
            sb.Append(c);
        }

        FlushDots();
        return sb.ToString();
    }

    private static bool IsChoiceLabel(string text)
    {
        if (string.IsNullOrWhiteSpace(text))
            return false;

        return text.TrimStart().StartsWith("○", StringComparison.Ordinal);
    }

    private static string NormalizeChoiceLabelForMes(string text,
        bool preserveLatinAscii = false)
    {
        if (string.IsNullOrEmpty(text))
            return text;

        // Choice labels are encrypted strings. ASCII space (0x20) would become
        // a NUL byte after the engine's -0x20 encryption. Keep the visible
        // letters Latin when requested, but store word gaps as full-width
        // spaces so the encrypted string is not truncated.
        string normalized = NormalizeAsciiWidthForMesLatin(text.Trim());
        var sb = new StringBuilder(text.Length);
        foreach (char c in normalized)
        {
            sb.Append(c == ' ' ? '\u3000' : c);
        }
        return sb.ToString();
    }

    private static string RestoreChoiceLabelForJson(string text)
    {
        // Keep the internal MES placeholder invisible to JSON users.
        return text.Replace('~', ' ').Replace('\t', ' ').Replace('\u3000', ' ');
    }

    private static bool IsAsciiChoiceId(string text)
    {
        if (string.IsNullOrWhiteSpace(text))
            return false;

        text = text.Trim();
        if (text.Length < 1 || text.Length > 3)
            return false;

        foreach (char c in text)
        {
            if (c < '0' || c > '9')
                return false;
        }

        return true;
    }
    
    private string DecryptString(int relativeOffset, Encoding encoding)
    {
        int absoluteOffset = _bytecodeOffset + CalcVersionLength() + relativeOffset;
        var bytes = new List<byte>();
        
        while (absoluteOffset < _rawData.Length && _rawData[absoluteOffset] != 0)
        {
            byte decrypted = (byte)((_rawData[absoluteOffset] + _info.EncryptionKey) & 0xFF);
            bytes.Add(decrypted);
            absoluteOffset++;
        }
        
        return encoding.GetString(bytes.ToArray());
    }

    private string DecryptBytes(int absoluteOffset, int length, Encoding encoding)
    {
        var bytes = new byte[length];
        for (int i = 0; i < length; i++)
            bytes[i] = (byte)((_rawData[absoluteOffset + i] + _info.EncryptionKey) & 0xFF);
        return encoding.GetString(bytes);
    }
    
    private string ReadString(int relativeOffset, Encoding encoding)
    {
        int absoluteOffset = _bytecodeOffset + CalcVersionLength() + relativeOffset;
        var bytes = new List<byte>();
        
        while (absoluteOffset < _rawData.Length && _rawData[absoluteOffset] != 0)
        {
            bytes.Add(_rawData[absoluteOffset]);
            absoluteOffset++;
        }
        
        return encoding.GetString(bytes.ToArray());
    }
    
    private static string CleanText(string text)
    {
        text = text.TrimStart('$');  
        text = text.TrimEnd('$', '\u3000', ' '); 
        text = text.Replace("$", " ");
        return text;
    }

    private static string NormalizeAsciiWidth(string text)
    {
        if (string.IsNullOrEmpty(text))
            return text;

        var sb = new StringBuilder(text.Length);
        foreach (char c in text)
        {
            if (c >= '\uFF01' && c <= '\uFF5E')
            {
                sb.Append((char)(c - 0xFEE0));
            }
            else if (c == '\u3000')
            {
                sb.Append(' ');
            }
            else
            {
                sb.Append(c);
            }
        }
        return sb.ToString();
    }

    private static string NormalizeAsciiWidthForMesLatin(string text,
        bool encryptedString = false)
    {
        if (string.IsNullOrEmpty(text))
            return text;

        var sb = new StringBuilder(text.Length);
        foreach (char c in text)
        {
            if (c == ' ' || c == '\u3000')
            {
                // Encrypted MES strings cannot safely store ASCII space:
                // 0x20 - encryption key 0x20 becomes a NUL terminator.
                // Unencrypted UI/list strings should keep Latin spacing.
                sb.Append(encryptedString ? '\u3000' : ' ');
            }
            else if (c >= '\uFF01' && c <= '\uFF5E')
            {
                sb.Append((char)(c - 0xFEE0));
            }
            else
            {
                sb.Append(c);
            }
        }
        return sb.ToString();
    }

    private static string ConvertAsciiToFullWidthForMes(string text)
    {
        if (string.IsNullOrEmpty(text))
            return text;

        var sb = new StringBuilder(text.Length);
        int i = 0;
        while (i < text.Length)
        {
            // Preserve control codes such as @t90@d1 exactly.
            if (text[i] == '@')
            {
                int j = i + 1;
                int len = 1;
                while (j < text.Length && len <= 10 &&
                       (char.IsLetterOrDigit(text[j]) || text[j] == '@'))
                {
                    j++;
                    len++;
                }

                if (j > i + 1)
                {
                    sb.Append(text, i, j - i);
                    i = j;
                    continue;
                }
            }

            char c = text[i];
            if (c == ' ')
            {
                sb.Append('\u3000');
            }
            else if (c >= '!' && c <= '~')
            {
                sb.Append((char)(c + 0xFEE0));
            }
            else
            {
                sb.Append(c);
            }

            i++;
        }

        return sb.ToString();
    }
}

// Token, TextEntry, TextFormatter are defined in separate files.
