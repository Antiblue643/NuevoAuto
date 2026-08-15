from PIL import Image

def image_to_c_header(image_path, output_name="spritesheet"):
    img = Image.open(image_path).convert("1")
    width, height = img.size

    if width % 8 != 0 or height % 8 != 0:
        raise ValueError("Image dimensions must be multiples of 8.")

    tiles_x = width // 8
    tiles_y = height // 8
    c_data = []

    for ty in range(tiles_y):
        for tx in range(tiles_x):
            for row in range(8):
                byte_val = 0
                for col in range(8):
                    px = img.getpixel((tx * 8 + col, ty * 8 + row))
                    # px is 0 (black) or 255 (white). Treat > 0 as 1.
                    bit = 1 if px > 0 else 0
                    # bit 7 is leftmost pixel (col 0), bit 0 is rightmost (col 7)
                    byte_val |= (bit << (7 - col))
                c_data.append(byte_val)

    # Format as C array
    bin_vals = [f"0x{b:02X}" for b in c_data]
    array_lines = [", ".join(bin_vals[i:i+8]) for i in range(0, len(bin_vals), 8)]

    header = f"static const int {output_name}[] = {{\n"
    header += ",\n".join(f"    {line}" for line in array_lines)
    header += f"\n}};\n\nstatic const unsigned int {output_name}_len = {len(c_data)};\n"
    return header

print(image_to_c_header("emures/font_std.png", "font_std"))