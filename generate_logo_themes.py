import re

with open("res/discotemple.svg", "r", encoding="utf-8") as f:
    svg_content = f.read()

# Находим тег <g>...</g> который содержит буквы логотипа
# Игнорируем пути вне этого тега (которые рисуют деформированные линии подчёркивания)
g_match = re.search(r'(<g>.*?</g>)', svg_content, re.DOTALL)
if not g_match:
    print("Error: <g> group not found in SVG!")
    exit(1)

g_group = g_match.group(1)

# Базовый заголовок и хвост SVG
svg_header = '<?xml version="1.0" encoding="UTF-8" standalone="no"?><!DOCTYPE svg PUBLIC "-//W3C//DTD SVG 1.1//EN" "http://www.w3.org/Graphics/SVG/1.1/DTD/svg11.dtd"><svg width="100%" height="100%" viewBox="0 0 5334 1471" version="1.1" xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" xml:space="preserve" xmlns:serif="http://www.serif.com/" style="fill-rule:evenodd;clip-rule:evenodd;stroke-linejoin:round;stroke-miterlimit:2;">'
svg_footer = '</svg>'

themes = {
    "light": "#28282b",      # Тёмно-серый логотип для светлой темы
    "dark": "#ffffff",       # Белый логотип для тёмной темы
    "jungle": "#ffd700",     # Золотой логотип для джунглей
    "vaporwave": "#01cdfe"   # Лазурный логотип для вейпорвейва
}

for name, color in themes.items():
    # Добавляем fill к тегу <g> и заменяем стиль в путях для надёжности
    themed_g = g_group.replace('<g>', f'<g fill="{color}">')
    # Добавляем fill к каждому path внутри группы
    themed_g = re.sub(r'<path ', f'<path fill="{color}" ', themed_g)
    
    # Собираем полный SVG
    themed_svg = svg_header + themed_g + svg_footer
    
    output_path = f"res/discotemple_{name}.svg"
    with open(output_path, "w", encoding="utf-8") as f:
        f.write(themed_svg)
    print(f"Generated {output_path} with color {color}")
