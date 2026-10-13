using System.Text.Json.Serialization;

namespace DC2TYMesTextTool;

/// <summary>
/// Represents a text entry extracted from a MES script.
/// </summary>
public class TextEntry
{
    /// <summary>
    /// Offset in the bytecode section (used internally for reimport)
    /// </summary>
    [JsonIgnore]
    public int Offset { get; set; }
    
    /// <summary>
    /// Original byte length of the string (used for reimport)
    /// </summary>
    [JsonIgnore]
    public int OriginalLength { get; set; }
    
    /// <summary>
    /// The opcode that produced this text entry
    /// </summary>
    [JsonIgnore]
    public byte Opcode { get; set; }
    
    /// <summary>
    /// Whether the string was encrypted
    /// </summary>
    [JsonIgnore]
    public bool IsEncrypted { get; set; }
    
    /// <summary>
    /// Character name (for dialogue). Null for narration.
    /// </summary>
    [JsonPropertyName("name")]
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public string? Name { get; set; }
    
    /// <summary>
    /// The message text content.
    /// </summary>
    [JsonPropertyName("message")]
    public string Message { get; set; } = "";
    
    /// <summary>
    /// Whether this is a choice option text
    /// </summary>
    [JsonIgnore]
    public bool IsChoice { get; set; }
}

/// <summary>
/// Token representing a parsed bytecode instruction
/// </summary>
public struct Token
{
    public byte Opcode;
    public int Offset;
    public int Length;
    
    public Token(byte opcode, int offset, int length)
    {
        Opcode = opcode;
        Offset = offset;
        Length = length;
    }
}
