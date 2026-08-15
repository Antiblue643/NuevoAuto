def generate_comment(mnemonic, original, rendered):
    """Generates a human-readable comment explaining the instruction."""
    def desc(orig, rend):
        if orig[0] == 'reg': return rend
        if orig[0] == 'imm': return f"0x{rend}"
        if orig[0] == 'label': return f"address {rend}"
        return rend

    o = [desc(orig, rend) for orig, rend in zip(original, rendered)]
    while len(o) < 3:
        o.append("")

    m = mnemonic.strip()

    if m == "NOP": return "No operation."
    if m == "MOV": return f"Move {o[0]} to memory address {o[1]}."
    if m == "SMV": return f"Move {o[0]} to DISK address {o[1]}."
    if m == "VMV": return f"Move {o[0]} to VRAM address {o[1]}."
    if m == "LDM": return f"Load from memory address {o[1]} into {o[0]}."
    if m == "LOD": return f"Load {o[1]} into {o[0]}."
    if m == "SWP": return f"Swap {o[0]} and {o[1]}."

    if m == "ADD": return f"Add {o[1]} and {o[2]} and store to {o[0]}."
    if m == "ADC": return f"Add with carry {o[1]} and {o[2]} and store to {o[0]}."
    if m == "SUB": return f"Subtract {o[2]} from {o[1]} and store to {o[0]}."
    if m == "SBB": return f"Subtract with borrow {o[2]} from {o[1]} and store to {o[0]}."
    if m == "INC": return f"Increment {o[0]}."
    if m == "DEC": return f"Decrement {o[0]}."
    if m == "MUL": return f"Multiply {o[1]} and {o[2]} and store to {o[0]}."
    if m == "DIV": return f"Divide {o[1]} by {o[2]} and store to {o[0]}."

    if m == "AND": return f"Bitwise AND {o[1]} and {o[2]} and store to {o[0]}."
    if m == "OR":  return f"Bitwise OR {o[1]} and {o[2]} and store to {o[0]}."
    if m == "XOR": return f"Bitwise XOR {o[1]} and {o[2]} and store to {o[0]}."
    if m == "NOT": return f"Bitwise NOT {o[1]} and store to {o[0]}."
    if m == "NEG": return f"Negate {o[1]} and store to {o[0]}."
    if m == "SHL": return f"Shift {o[1]} left by {o[2]} and store to {o[0]}."
    if m == "SAL": return f"Arithmetic shift {o[1]} left by {o[2]} and store to {o[0]}."
    if m == "SHR": return f"Shift {o[1]} right by {o[2]} and store to {o[0]}."
    if m == "SAR": return f"Arithmetic shift {o[1]} right by {o[2]} and store to {o[0]}."

    if m == "JMP": return f"Jump to {o[0]}."
    if m == "JE":  return f"Jump to {o[2]} if {o[0]} equals {o[1]}."
    if m == "JNE": return f"Jump to {o[2]} if {o[0]} not equal to {o[1]}."
    if m == "JZ":  return f"Jump to {o[1]} if {o[0]} is zero."
    if m == "JNZ": return f"Jump to {o[1]} if {o[0]} is not zero."
    if m == "JL":  return f"Jump to {o[2]} if {o[0]} less than {o[1]}."
    if m == "JLE": return f"Jump to {o[2]} if {o[0]} less than or equal to {o[1]}."
    if m == "JG":  return f"Jump to {o[2]} if {o[0]} greater than {o[1]}."
    if m == "JGE": return f"Jump to {o[2]} if {o[0]} greater than or equal to {o[1]}."
    if m == "JB":  return f"Jump to {o[2]} if {o[0]} below {o[1]}."
    if m == "JBE": return f"Jump to {o[2]} if {o[0]} below or equal to {o[1]}."
    if m == "JA":  return f"Jump to {o[2]} if {o[0]} above {o[1]}."
    if m == "JAE": return f"Jump to {o[2]} if {o[0]} above or equal to {o[1]}."

    if m == "CAL": return f"Move pointer to address {o[0]}."
    if m == "RET": return "Return from subroutine."
    if m == "FOR": return f"Loop: if {o[0]} < {o[1]}, jump to {o[2]}."
    if m == "BRK": return "Break / Halt execution."
    if m == "PRT": return f"Print {o[0]}."

    if m == "LSM": return f"Load from DISK address {o[1]} into {o[0]}."
    if m == "LVM": return f"Load from VRAM address {o[1]} into {o[0]}."
    if m == "LNP": return f"Load from NPROG address {o[1]} into {o[0]}."
    if m == "SNP": return f"Store {o[0]} to NPROG address {o[1]}."
    if m == "LBM": return f"Load from SYSROM address {o[1]} into {o[0]}."

    if m == "FLP": return "Flip frame buffer."

    if m == "MOVI": return f"Move (immediate) {o[0]} to memory address in {o[1]}."
    if m == "SMVI": return f"Move (immediate) {o[0]} to DISK address in {o[1]}."
    if m == "VMVI": return f"Move (immediate) {o[0]} to VRAM address in {o[1]}."

    if m == "LDMI": return f"Load (immediate) from memory address in {o[1]} into {o[0]}."
    if m == "LSMI": return f"Load (immediate) from DISK address in {o[1]} into {o[0]}."
    if m == "LVMI": return f"Load (immediate) from VRAM address in {o[1]} into {o[0]}."
    if m == "LNPI": return f"Load (immediate) from NPROG address in {o[1]} into {o[0]}."
    if m == "SNPI": return f"Store (immediate) {o[0]} to NPROG address in {o[1]}."
    if m == "LBMI": return f"Load (immediate) from SYSROM address in {o[1]} into {o[0]}."

    if m == "PSH": return f"Push {o[0]} to the stack."
    if m == "POP": return f"Pop {o[0]} from the stack."
    if m == "LSP": return f"Load stack pointer from {o[0]}."
    if m == "SSP": return f"Store stack pointer to {o[0]}."
    if m == "SEI": return f"Set interrupt-enable flag."
    if m == "CLI": return f"Clear interrupt-enable flag."
    if m == "RTI": return f"Return from interrupt."
    if m == "SWI": return f"Software interrupt."

    return "Unknown instruction."