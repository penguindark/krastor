#!/usr/bin/env python3
"""Krastor - color emoji atlas generator (output_format_plan.md Step 4).

Renders a curated set of common emoji from Noto Color Emoji (CBDT color
bitmaps) into one RGBA sprite-sheet atlas + a JSON manifest that
src/emoji.cpp loads at runtime.

    pip install pillow
    curl -L -o data/fonts/NotoColorEmoji.ttf \
      https://fuchsia.googlesource.com/third_party/github.com/googlefonts/noto-emoji/+/refs/heads/main/fonts/NotoColorEmoji.ttf?format=TEXT \
      | base64 -d > data/fonts/NotoColorEmoji.ttf   (or the PowerShell equivalent)
    python tools/gen_emoji_atlas.py

The source .ttf is git-ignored; the generated atlas + manifest are committed.
Lookup keys are single codepoints (U+FE0F stripped at runtime); ZWJ sequences
and flag pairs are intentionally not included (documented limit).
"""
import json
import os
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT = os.path.join(ROOT, "data", "fonts", "NotoColorEmoji.ttf")
OUT_PNG = os.path.join(ROOT, "data", "fonts", "emoji_atlas.png")
OUT_JSON = os.path.join(ROOT, "data", "fonts", "emoji_atlas.json")

CELL = 64          # atlas cell size in px
COLS = 16          # atlas columns

# Curated common emoji (faces, gestures, hearts, objects, nature, food,
# animals, symbols). Keep this list free of ZWJ sequences and flags.
EMOJI = list(
    "😀😁😂🤣😃😄😅😆😉😊😋😎😍😘🥰😗😙😚🙂🤗🤩🤔🤨😐😑😶🙄😏😣😥😮🤐😯"
    "😪😫😴😌😛😜😝🤤😒😓😔😕🙃🤑😲🙁😖😞😟😤😢😭😦😧😨😩🤯😬😰😱😳🤪😵"
    "😡😠🤬😷🤒🤕🤢🤮🤧😇🤠🤡🤥🤫🤭🧐🤓😈👿👹👺💀👻👽🤖💩😺😸😹😻😼😽🙀😿😾"
    "👋🤚🖐✋🖖👌🤌🤏✌🤞🤟🤘🤙👈👉👆👇☝👍👎✊👊🤛🤜👏🙌👐🤲🤝🙏💪🦵🦶👂👃👀👁"
    "❤🧡💛💚💙💜🖤🤍🤎💔❣💕💞💓💗💖💘💝💟☮✝☪🕉☸✡✯☯"
    "🔥✨⭐🌟💫💥💦💨💡🔔📌📎📐📏🔗📁📂📄📝📊📈📉💹💻🖥⌨🖱💾💿📀📱☎📞📟📠"
    "🎉🎊🥳🎁🎈🎀🏆🏅🎖🎪🎡🎢"
    "🔍🔎🔒🔓🔑🗝🛠⚒⚙⚖🧲💰💵💎🚀🛸🎯🎲🎮🎨🎬🎭🎵🎶🎼🎧📚📖📑🗂📅📆⏰⏳⌛"
    "☀🌤⛅🌧⛈❄⛄🌈☁💧🌊🌍🌙⭐🌌🌲🌳🌴🌱🌿☘🍀🍁🍂🍃🍄🌸🌹🌺🌻🌼🌷🌻"
    "🍇🍈🍉🍊🍋🍌🍍🍎🍏🍐🍑🍒🍓🥝🍅🥑🍕🍔🍟🌭🌮🌯🍿🍩🍪🎂🍰☕🍵🍶🍺🍻🍷🥂🥃"
    "🐶🐱🐭🐹🐰🦊🐻🐼🐨🐯🦁🐮🐷🐸🐵🐔🐧🐦🐤🦆🦅🦉🐴🦄🐝🐛🦋🐌🐞🐜🕷🐢🐍🐙🦑🦀🐬🐳🐋🐠🐟"
    "🚗🚕🚌🏎🚓🚑🚒🚲✈🚀🚁🛳⛵🚤🚢🚥🚦🏁🚧🏠🏢🏥🏦🏫"
    "✅❌⚠✔✖‼⁉™©®🆗🆕🔴🟠🟡🟢🔵🟣⚫⚪⬛⬜🔺🔻🔸🔹"
)

# De-duplicate while preserving order.
seen = set()
EMOJI = [e for e in EMOJI if not (e in seen or seen.add(e))]


def main():
    if not os.path.exists(FONT):
        sys.exit(f"missing font: {FONT} (see this script's docstring)")
    font = ImageFont.truetype(FONT, 109)  # Noto CBDT native pixel size

    rows = (len(EMOJI) + COLS - 1) // COLS
    atlas = Image.new("RGBA", (COLS * CELL, rows * CELL), (0, 0, 0, 0))
    manifest = {"_cell": CELL, "_cols": COLS, "_px": rows * CELL}
    skipped = []

    for idx, e in enumerate(EMOJI):
        img = Image.new("RGBA", (160, 160), (0, 0, 0, 0))
        d = ImageDraw.Draw(img)
        d.text((16, 16), e, font=font, embedded_color=True)
        bbox = img.getbbox()
        if not bbox:
            skipped.append(e)
            continue
        glyph = img.crop(bbox)
        side = max(glyph.size)
        canvas = Image.new("RGBA", (side, side), (0, 0, 0, 0))
        canvas.paste(glyph, ((side - glyph.width) // 2,
                             (side - glyph.height) // 2))
        canvas = canvas.resize((CELL, CELL), Image.LANCZOS)
        r, c = divmod(idx, COLS)
        atlas.paste(canvas, (c * CELL, r * CELL))
        cps = [ord(ch) for ch in e if ord(ch) not in (0xFE0F, 0x200D)]
        if len(cps) != 1:
            skipped.append(e)
            continue
        manifest["U+%04X" % cps[0]] = [c, r]

    atlas.save(OUT_PNG)
    with open(OUT_JSON, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=0)
    n = len([k for k in manifest if not k.startswith("_")])
    info = f"{OUT_PNG}: {atlas.width}x{atlas.height}, {n} emoji"
    if skipped:
        info += ", skipped " + " ".join(
            "+".join("U+%04X" % ord(c) for c in e) for e in skipped)
    print(info.encode("ascii", "replace").decode())


if __name__ == "__main__":
    main()
