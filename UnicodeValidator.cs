using System.Globalization;

namespace DC2TYMesTextTool;

/// <summary>
/// Unicode range validation for filtering valid game text.
/// Based on CircusEditor's UnicodeRange.cs
/// </summary>
public static class UnicodeValidator
{
    private static readonly UnicodeCategory[] UnprintableCategories =
    {
        UnicodeCategory.Control,
        UnicodeCategory.OtherNotAssigned,
        UnicodeCategory.Surrogate
    };
    
    /// <summary>
    /// Unicode ranges commonly found in Japanese game text
    /// </summary>
    private static readonly (int Min, int Max, UnicodeRangeType Type)[] Ranges =
    {
        // Basic Latin (ASCII)
        (0x0020, 0x007F, UnicodeRangeType.BasicLatin),
        // Latin-1 Supplement
        (0x00A0, 0x00FF, UnicodeRangeType.Latin1Supplement),
        // Latin Extended-A
        (0x0100, 0x017F, UnicodeRangeType.LatinExtendedA),
        // General Punctuation
        (0x2000, 0x206F, UnicodeRangeType.GeneralPunctuation),
        // CJK Symbols and Punctuation
        (0x3000, 0x303F, UnicodeRangeType.CJKSymbolsAndPunctuation),
        // Hiragana
        (0x3040, 0x309F, UnicodeRangeType.Hiragana),
        // Katakana
        (0x30A0, 0x30FF, UnicodeRangeType.Katakana),
        // Katakana Phonetic Extensions
        (0x31F0, 0x31FF, UnicodeRangeType.KatakanaPhoneticExtensions),
        // CJK Unified Ideographs
        (0x4E00, 0x9FFF, UnicodeRangeType.CJKUnifiedIdeographs),
        // Halfwidth and Fullwidth Forms
        (0xFF00, 0xFFEF, UnicodeRangeType.HalfwidthAndFullwidthForms),
        // Private Use Area (game-specific characters)
        (0xE000, 0xF8FF, UnicodeRangeType.PrivateUseArea),
    };
    
    private static readonly HashSet<UnicodeRangeType> AllowedJapaneseRanges = new()
    {
        UnicodeRangeType.Hiragana,
        UnicodeRangeType.Katakana,
        UnicodeRangeType.KatakanaPhoneticExtensions,
        UnicodeRangeType.CJKUnifiedIdeographs,
        UnicodeRangeType.GeneralPunctuation,
        UnicodeRangeType.CJKSymbolsAndPunctuation,
        UnicodeRangeType.HalfwidthAndFullwidthForms,
        UnicodeRangeType.PrivateUseArea,
    };
    
    private static readonly HashSet<UnicodeRangeType> AllowedLatinRanges = new()
    {
        UnicodeRangeType.BasicLatin,
        UnicodeRangeType.Latin1Supplement,
        UnicodeRangeType.LatinExtendedA,
    };
    
    public static bool IsUnprintable(char c)
    {
        return UnprintableCategories.Contains(char.GetUnicodeCategory(c));
    }
    
    public static UnicodeRangeType GetRange(char c)
    {
        int code = c;
        foreach (var (min, max, type) in Ranges)
        {
            if (code >= min && code <= max)
                return type;
        }
        return UnicodeRangeType.Unknown;
    }
    
    /// <summary>
    /// Validates if a string contains valid Japanese game text
    /// </summary>
    public static bool IsValidString(string text)
    {
        if (string.IsNullOrEmpty(text))
            return false;
        
        // Allow single-character strings if they're valid Japanese characters
        if (text.Length == 1)
        {
            var range = GetRange(text[0]);
            return AllowedJapaneseRanges.Contains(range);
        }
            
        // Check for corruption markers
        if (text.Contains('�'))
            return false;
            
        int mismatchCount = 0;
        int japaneseCharCount = 0;
        
        foreach (char c in text)
        {
            if (IsUnprintable(c))
                return false;
                
            var range = GetRange(c);
            if (AllowedJapaneseRanges.Contains(range))
                japaneseCharCount++;
            else if (!AllowedLatinRanges.Contains(range))
                mismatchCount++;
        }
        
        // For short strings (names), require at least one Japanese char and mostly valid chars
        if (text.Length <= 10)
        {
            // Accept if has any Japanese text and no major corruption
            return japaneseCharCount > 0 && mismatchCount <= 1;
        }
        
        // For longer strings, allow up to 1/3 mismatches
        return mismatchCount < text.Length / 3;
    }
    
    /// <summary>
    /// Check if string is primarily Latin characters
    /// </summary>
    public static bool IsPrimarylyLatin(string text)
    {
        int latinCount = 0;
        foreach (char c in text)
        {
            var range = GetRange(c);
            if (AllowedLatinRanges.Contains(range))
                latinCount++;
        }
        return latinCount > text.Length / 2;
    }
}

public enum UnicodeRangeType
{
    Unknown,
    BasicLatin,
    Latin1Supplement,
    LatinExtendedA,
    GeneralPunctuation,
    CJKSymbolsAndPunctuation,
    Hiragana,
    Katakana,
    KatakanaPhoneticExtensions,
    CJKUnifiedIdeographs,
    HalfwidthAndFullwidthForms,
    PrivateUseArea,
}
