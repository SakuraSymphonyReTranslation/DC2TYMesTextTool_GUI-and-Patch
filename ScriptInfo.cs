namespace DC2TYMesTextTool;

/// <summary>
/// Script configuration dedicated to D.C.II To You.
/// </summary>
public class ScriptInfo
{
    public string Name { get; init; } = "";
    public ushort Version { get; init; }
    public OffsetType OffsetType { get; init; }
    
    // Opcode ranges for different instruction types
    public (byte Begin, byte End) Uint8x2 { get; init; }   // [op][arg1:byte][arg2:byte]
    public (byte Begin, byte End) Uint8Str { get; init; }  // [op][arg1:byte][string]
    public (byte Begin, byte End) String { get; init; }    // [op][string] - unencrypted
    public (byte Begin, byte End) EncStr { get; init; }    // [op][encstring] - encrypted
    public (byte Begin, byte End) Uint16x4 { get; init; }  // [op][4 x uint16]
    
    public byte EncryptionKey { get; init; }
    public byte[] OutputOpcodes { get; init; } = Array.Empty<byte>();
    
    /// <summary>
    /// Check if an opcode falls within a section range
    /// </summary>
    public bool IsInSection((byte Begin, byte End) section, byte opcode)
    {
        if (section.Begin == 0xFF && section.End == 0xFF)
            return false;
        return opcode >= section.Begin && opcode <= section.End;
    }
    
    /// <summary>
    /// D.C.II To You ~Da Capo II~ Side Episodes configuration
    /// From original C++ MesTextTool script_info.cpp
    /// </summary>
    public static readonly ScriptInfo DC2TY = new()
    {
        Name = "dc2ty",
        Version = 0x5F69,
        OffsetType = OffsetType.Offset1,
        Uint8x2 = (0x00, 0x28),
        Uint8Str = (0x29, 0x2E),
        String = (0x2F, 0x49),
        EncStr = (0x4A, 0x4D),
        Uint16x4 = (0x4E, 0xFF),
        EncryptionKey = 0x20,
        OutputOpcodes = new byte[] { 0x45 }
    };

    /// <summary>
    /// Query script info by name
    /// </summary>
    public static ScriptInfo? QueryByName(string name)
    {
        return name.Equals(DC2TY.Name, StringComparison.OrdinalIgnoreCase)
            ? DC2TY
            : null;
    }
    
    /// <summary>
    /// Query script info from MES file data
    /// </summary>
    public static ScriptInfo? QueryFromData(byte[] data)
    {
        if (data.Length < 8)
            return null;
            
        int labelCount = BitConverter.ToInt32(data, 0);
        int offset1 = labelCount * 4 + 4;
        if (data.Length > offset1 + 2)
        {
            ushort version1 = BitConverter.ToUInt16(data, offset1);
            if (version1 == DC2TY.Version)
                return DC2TY;
        }
        
        // Default to this tool's target game if detection fails.
        return DC2TY;
    }
}

public enum OffsetType
{
    Offset1  // head[0] * 4 + 4
}
