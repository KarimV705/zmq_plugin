import os

svg_path = "res/ZeroMQSocket.svg"

with open(svg_path, "r", encoding="utf-8") as f:
    original_svg = f.read()

# 1. Dark Theme (just copy the original)
with open("res/ZeroMQSocket_dark.svg", "w", encoding="utf-8") as f:
    f.write(original_svg)
print("Generated res/ZeroMQSocket_dark.svg")

# 2. Light Theme Replacement Map
# Original background rect: fill="#1b1c1e"
# Original border rect: stroke="#2c2d30"
# Original display placeholder: stroke="#3a3c40"
# Original ØMQ SOCKET text: fill="#707175"
# Original labels (PORT, PROTO, BP, headers): fill="#a0a1a5"
# Original numbers / sub-labels: fill="#606165"
# Original footer (discotemple): fill="#404145"
# Original screw placement: fill="#505154"
light_replacements = {
    'fill="#1b1c1e"': 'fill="#e6e6e6"',
    'stroke="#2c2d30"': 'stroke="#b0b0b0"',
    'stroke="#3a3c40"': 'stroke="#909090"',
    'fill="#707175"': 'fill="#202020"',
    'fill="#a0a1a5"': 'fill="#404040"',
    'fill="#606165"': 'fill="#505050"',
    'fill="#404145"': 'fill="#202020"',
    'fill="#505154"': 'fill="#808080"'
}

light_svg = original_svg
for orig, rep in light_replacements.items():
    light_svg = light_svg.replace(orig, rep)

with open("res/ZeroMQSocket_light.svg", "w", encoding="utf-8") as f:
    f.write(light_svg)
print("Generated res/ZeroMQSocket_light.svg")

# 3. Jungle Theme Replacement Map
jungle_replacements = {
    'fill="#1b1c1e"': 'fill="#122b14"',
    'stroke="#2c2d30"': 'stroke="#4a5d30"',
    'stroke="#3a3c40"': 'stroke="#5a6e3b"',
    'fill="#707175"': 'fill="#39ff14"',     # Lime Green title
    'fill="#a0a1a5"': 'fill="#ffd700"',     # Yellow/banana headers
    'fill="#606165"': 'fill="#8b8e36"',     # Mossy sub-labels
    'fill="#404145"': 'fill="#39ff14"',     # Lime footer
    'fill="#505154"': 'fill="#40513b"'
}

jungle_svg = original_svg
for orig, rep in jungle_replacements.items():
    jungle_svg = jungle_svg.replace(orig, rep)

with open("res/ZeroMQSocket_jungle.svg", "w", encoding="utf-8") as f:
    f.write(jungle_svg)
print("Generated res/ZeroMQSocket_jungle.svg")

# 4. Vaporwave Theme Replacement Map
vapor_replacements = {
    'fill="#1b1c1e"': 'fill="#2b0f54"',     # Deep synthwave purple
    'stroke="#2c2d30"': 'stroke="#ff007f"',   # Neon hotline pink border
    'stroke="#3a3c40"': 'stroke="#b967ff"',   # Light violet display border
    'fill="#707175"': 'fill="#01cdfe"',     # Neon cyan title
    'fill="#a0a1a5"': 'fill="#ff71ce"',     # Pastel pink headers
    'fill="#606165"': 'fill="#01cdfe"',     # Neon cyan sub-labels
    'fill="#404145"': 'fill="#fffb96"',     # Neon yellow footer
    'fill="#505154"': 'fill="#4d0080"'
}

vapor_svg = original_svg
for orig, rep in vapor_replacements.items():
    vapor_svg = vapor_svg.replace(orig, rep)

with open("res/ZeroMQSocket_vaporwave.svg", "w", encoding="utf-8") as f:
    f.write(vapor_svg)
print("Generated res/ZeroMQSocket_vaporwave.svg")
