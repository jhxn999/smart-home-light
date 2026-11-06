#!/usr/bin/env python3
"""Embed dashboard.html as a PROGMEM raw string for the Arduino sketch."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "dashboard.html"
TARGET = ROOT / "firmware" / "NovaLight" / "dashboard_html.h"
DELIMITER = "NOVALIGHT_HTML"

html = SOURCE.read_text(encoding="utf-8")
if f"){DELIMITER}\"" in html:
    raise SystemExit(f"The dashboard contains the reserved C++ delimiter {DELIMITER!r}.")

header = (
    "// Generated from ../dashboard.html by tools/embed_dashboard.py. Do not edit by hand.\n"
    "#pragma once\n"
    "#include <pgmspace.h>\n\n"
    f"static const char NOVALIGHT_HTML[] PROGMEM = R\"{DELIMITER}(\n"
    f"{html}\n"
    f"){DELIMITER}\";\n"
)
TARGET.write_text(header, encoding="utf-8", newline="\n")
print(f"Wrote {TARGET.relative_to(ROOT)} from {SOURCE.name}")

