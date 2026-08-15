with open(".\\emures\\charmap.txt", "r", encoding="utf-8") as f:
    lines = f.readlines()

with open(".\\emures\\charmap.txt", "w", encoding="utf-8") as f:
    for i, line in enumerate(lines):
        f.write(f"{line.strip()} {i}\n")