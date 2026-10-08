#!/usr/bin/env python3
"""Generate RayoMD's native math tables from the Adobe Core 14 AFM files.

Writes two files (default: src/core):

  math_symbols.inc       parser tables: control words, Unicode characters, environments,
                         ASCII characters, font variants, delimiters
  math_font_metrics.inc  layout tables: glyph metrics of the five math fonts, constructed
                         symbols, text-matrix operators, accent codes, exact Helvetica widths

The symbol data lives in this script and names every glyph by its AFM glyph name. Character
codes, advance widths and bounding boxes are read from the AFM files at run time, and the
script stops when a name does not resolve to an encoded glyph of its font.

Inputs: eight AFM files and the readme.txt that accompanies them, pinned by SHA-256 below.
They are Matplotlib's copy of the Adobe files (the directory mpl-data/fonts/pdfcorefonts of
the package, identical in Matplotlib 3.10.8) and are not part of this repository. Pass their
directory with --afm-dir, or install Matplotlib and leave the option out.

This is a developer tool: the two generated files are committed and no build step runs it.
Python 3 standard library only.

Exit status: 0 success; 1 --check found a file that differs; 2 an input was rejected or a
verification failed (nothing is written then).
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
SCRIPT_NAME = "scripts/generate_math_tables.py"
SYMBOLS_FILE = "math_symbols.inc"
METRICS_FILE = "math_font_metrics.inc"


class Failure(Exception):
    """An input was rejected or a verification failed."""


# =================================================================================================
# 1. Inputs
# =================================================================================================

# (file name, size in bytes, SHA-256)
PINNED_INPUTS = (
    ("Times-Roman.afm", 60460, "86136b527a5acee0c3283d56a6b68fc6a3d60982eb3bb0fb6cc54e83ecc8d77a"),
    ("Times-Italic.afm", 66328, "6cae69b92329193fd85082f0c89ad7a318df8fb5c939d7efe5343f88ab09473c"),
    ("Times-Bold.afm", 64251, "7104e6af62c53f029013fb0641a81e31c98589c8f27b5ea6289b25b092c74321"),
    ("Times-BoldItalic.afm", 59642, "a7358e772726e91aa87015a001563926ad33dc5a7b2eea9680b34cc78aba385c"),
    ("Symbol.afm", 9740, "3f951aa17af8cb4aa1e1288c6b9baa8a30d3e990ebdbe8cf9a3d5acabcb9f771"),
    ("Helvetica.afm", 74292, "db772f2830fb6d000907791d8d26a12524d96943a9a739e520ee855c6b25c96f"),
    ("Helvetica-Bold.afm", 69269, "8b697881c8ee617a177f6f2e2bbc88570b9fd2b9f0e89ecfc0f5507878f988fc"),
    ("Courier.afm", 15335, "31d72adad79910126b22a5579ec9e179eee86674ecebe3fcff6f76916193af0e"),
    ("readme.txt", 828, "311bfca694884d86006fb96de443b0f4359635921b95fc6c161bb94d013b7292"),
)
INPUT_ORIGIN = ("Matplotlib's copy of the Adobe files: the directory mpl-data/fonts/pdfcorefonts of the",
                "package, identical in Matplotlib 3.10.8.")
README_FILE = "readme.txt"
LICENCE_FIRST_WORDS = "This file and the 14 PostScript(R) AFM files it accompanies may be used"
LICENCE_LAST_WORDS = "to support the use of the AFM files."

# short id -> AFM base name. The first five are the math fonts /M1 .. /M5, in this order.
FONT_NAMES = {
    "R": "Times-Roman", "I": "Times-Italic", "B": "Times-Bold", "BI": "Times-BoldItalic",
    "S": "Symbol", "H": "Helvetica", "HB": "Helvetica-Bold", "C": "Courier",
}
MATH_FONTS = ("R", "I", "B", "BI", "S")
TIMES = ("R", "I", "B", "BI")
FONT_ID = {short: index + 1 for index, short in enumerate(MATH_FONTS)}

# =================================================================================================
# 2. Values of src/core/math_parser.h that the tables depend on. The generated files assert
#    every one of them, so a change of the header cannot silently shift a table.
# =================================================================================================

CLASSES = ("Ord", "Op", "Bin", "Rel", "Open", "Close", "Punct", "Inner")
CLASS_ID = {name: index for index, name in enumerate(CLASSES)}

GLYPH_BITS = {"slant": 0x01, "small": 0x02, "outline": 0x04, "heavy": 0x08}
GLYPH_BIT_ENUM = {"slant": "kGlyphSlant", "small": "kGlyphSmall", "outline": "kGlyphOutline",
                  "heavy": "kGlyphHeavy"}
FRACTION_BAR, FRACTION_DISPLAY, FRACTION_TEXT = 0x01, 0x02, 0x04
ARRAY_CASES, ARRAY_ROW_GAP, ARRAY_ALIGNED = 0x20, 0x40, 0x80
CELL_STYLE = {"display": 0, "text": 1, "script": 2}
MAX_ENV_NAME = 32

# Delimiter keys of the data below, in the order of enum MathDelimiter.
DELIM_ENUM = (
    ("none", "kDelimNone"), ("lparen", "kDelimLParen"), ("rparen", "kDelimRParen"),
    ("lbrack", "kDelimLBrack"), ("rbrack", "kDelimRBrack"), ("lbrace", "kDelimLBrace"),
    ("rbrace", "kDelimRBrace"), ("vert", "kDelimVert"), ("Vert", "kDelimDblVert"),
    ("langle", "kDelimLAngle"), ("rangle", "kDelimRAngle"), ("lfloor", "kDelimLFloor"),
    ("rfloor", "kDelimRFloor"), ("lceil", "kDelimLCeil"), ("rceil", "kDelimRCeil"),
    ("slash", "kDelimSlash"), ("backslash", "kDelimBackslash"), ("uparrow", "kDelimUpArrow"),
    ("downarrow", "kDelimDownArrow"), ("updownarrow", "kDelimUpDownArrow"),
    ("Uparrow", "kDelimDblUpArrow"), ("Downarrow", "kDelimDblDownArrow"),
    ("Updownarrow", "kDelimDblUpDownArrow"),
)
DELIM_ID = {key: index for index, (key, _) in enumerate(DELIM_ENUM)}

# Accent commands in the order of enum MathAccent; 0..9 are Times accent glyphs.
ACCENT_ENUM = ("kAccentHat", "kAccentCheck", "kAccentTilde", "kAccentAcute", "kAccentGrave",
               "kAccentDot", "kAccentDdot", "kAccentBreve", "kAccentBar", "kAccentRing",
               "kAccentVec", "kAccentDddot", "kAccentWideHat", "kAccentWideTilde")
OVERUNDER_ENUM = ("kOverLine", "kUnderLine", "kOverRightArrow", "kOverLeftArrow",
                  "kOverLeftRightArrow", "kOverBrace", "kUnderBrace")
ENCLOSE_ENUM = ("kEncloseBox", "kEncloseCancel", "kEncloseBCancel", "kEncloseXCancel")
PHANTOM_ENUM = ("kPhantomBoth", "kPhantomWidth", "kPhantomHeight")
BIGOP_ENUM = ("kBigOpNone", "kBigOpSum", "kBigOpIntegral", "kBigOpCup", "kBigOpVee")
BIGOP_ID = {"sum": 1, "int": 2, "bigcup": 3, "bigvee": 4}
LIMITS_ENUM = ("kLimitsDefault", "kLimitsOn", "kLimitsOff")
STACK_ARROW_ENUM = ("kStackNoArrow", "kStackRightArrow", "kStackLeftArrow")

# =================================================================================================
# 3. Enumerations declared by math_symbols.inc and math_font_metrics.inc themselves.
# =================================================================================================

CMD_KINDS = (
    "Glyph", "Composite", "Negated", "Delimiter", "OpName", "Space", "Blackboard",
    "Fraction", "Infix", "Sqrt", "Accent", "OverUnder", "MathFont", "FontDecl",
    "Text", "TextDecl", "TextOnly", "Stack", "XArrow", "Enclose", "OperatorName",
    "Substack", "Not", "Left", "Right", "Middle", "Paired", "Sized", "Begin",
    "End", "RowSep", "Tag", "Hline", "Style", "Class", "Limits", "Phantom",
    "Mod", "HSpace", "Kern", "Dots", "ColorArg", "Ref", "Transparent", "Ignore",
)
VARIANT_NAMES = ("Normal", "Roman", "Bold", "Italic", "BoldItalic", "Blackboard", "Calligraphic",
                 "Fraktur", "Sans", "Mono")
VARIANT_ID = {name: index for index, name in enumerate(VARIANT_NAMES)}
UNI_KINDS = ("Command", "Glyph", "Minus", "Blackboard", "Space", "Ignore", "Prime", "Sup", "Sub")
ENV_COLUMNS = ("Centre", "Left", "Pairs", "Rcl", "Spec")
ENV_ARGS = ("None", "OptAlign", "DropBrace", "ArraySpec", "SubarraySpec")
DELIM_BASES = ("None", "Glyph", "Vector", "Composite")
PIECE_KINDS = ("Glyph", "Rect", "Line", "Ring")

# Text matrices "a b c d" of the transform ids 0..7 of a composite glyph piece.
TRANSFORMS = (
    "1 0 0 1",
    "-1 0 0 -1",
    "0 1 -1 0",
    "0.70711 0.70711 -0.70711 0.70711",
    "0.70711 -0.70711 0.70711 0.70711",
    "-0.70711 0.70711 -0.70711 -0.70711",
    "-0.70711 -0.70711 0.70711 -0.70711",
    "0.5 0 0 1",
)
TRANSFORM_OP_BYTES = 36
ROTATION_TRANSFORM = {0: 0, 180: 1, 90: 2, 45: 3, -45: 4, 135: 5, -135: 6}
HALF_WIDTH_TRANSFORM = 7

# =================================================================================================
# 4. Numbers the contract expects (docs/development/native_math.md). A different count or
#    size is reported, not refused; the values under "KNOWN" follow from the pinned inputs and
#    are refused when they differ.
# =================================================================================================

EXPECTED_SYMBOLS = (
    ("control words", 596), ("control word name bytes", 4327), ("Unicode entries", 328),
    ("environments", 38), ("environment name bytes", 316), ("ASCII entries", 95),
    ("variants", 10), ("delimiter words", 27), ("delimiter bases", 23),
    ("read-only bytes", 10994),
)
EXPECTED_METRICS = (
    ("glyph metric rows", 1120), ("composites", 57), ("composite pieces", 116),
    ("transform operator bytes", 288), ("accent codes", 10), ("delimiter bases", 23),
    ("text width entries", 190), ("read-only bytes", 14255),
)
KNOWN_ACCENT_CODES = (0xC3, 0xCF, 0xC4, 0xC2, 0xC1, 0xC7, 0xC8, 0xC6, 0xC5, 0xCA)
KNOWN_SYMBOL_GLYPHS = 189
KNOWN_SYMBOL_WIDTH_COUNT = 223          # codes 32 .. 254
KNOWN_SYMBOL_WIDTH_SUM = 110936
KNOWN_SYMBOL_WIDTH_TEXT_BYTES = 827
KNOWN_SYMBOL_FONT_BBOX = "-180 -293 1090 1010"
KNOWN_SYMBOL_STD_VW = "85"
# /F1 /F2 /F3 declare /WinAnsiEncoding: there, unlike in the fonts' built-in StandardEncoding,
# code 0x27 is the straight quote and code 0x60 the grave accent.
WIN_ANSI_TEXT_GLYPHS = {0x27: "quotesingle", 0x60: "grave"}
KNOWN_TEXT_WIDTH_SUMS = {"H": 50058, "HB": 52367}
KNOWN_COURIER_ADVANCE = 600
KNOWN_SPACE_ADVANCE = 250
KNOWN_METRICS = (       # (math font, code, row)
    ("S", 0x2D, (549, 11, 233, 535, 288)),      # Symbol minus
    ("I", 0x78, (444, -27, -11, 447, 441)),     # Times-Italic x
    ("R", 0xBC, (1000, 111, -11, 888, 100)),    # Times-Roman ellipsis
)
# The two code points outside the BMP are two tests in the parser, not table rows.
ASTRAL_UNICODE = {0x1D7D9: ("bb", "1"), 0x1D55C: ("bb", "k")}

# =================================================================================================
# 5. Symbol data. Glyphs are named, never numbered.
# =================================================================================================

# ---- 5.1 Simple symbols: one glyph of one font ---------------------------------------------------
# flags: sl    lowercase Greek: slanted and small in the Normal variant (kMathVariants)
#        SL    uppercase Greek that is always slanted (amsmath \varGamma ...)
#        pad   5/18 em before and after (kAtomPad)
#        degr  an approximation of the TeX symbol; see the note
SYMBOLS: list[dict] = []
_group = [""]


def group(name: str) -> None:
    _group[0] = name


def sym(names: str, font: str, glyph: str, cls: str, limits: str = "-", flags: str = "",
        note: str = "") -> None:
    SYMBOLS.append({"names": names.split(), "font": font, "glyph": glyph, "cls": cls,
                    "limits": limits, "flags": flags.split(), "note": note, "group": _group[0]})


group("greek-lower")
for _name in ["alpha", "beta", "gamma", "delta", "zeta", "eta", "theta", "iota", "kappa", "lambda",
              "mu", "nu", "xi", "omicron", "pi", "rho", "sigma", "tau", "upsilon", "chi", "psi",
              "omega"]:
    sym(_name, "S", _name, "Ord", flags="sl")
sym("varepsilon", "S", "epsilon", "Ord", flags="sl")
sym("epsilon", "S", "epsilon", "Ord", flags="sl degr", note="lunate epsilon unavailable; same glyph as varepsilon")
sym("vartheta", "S", "theta1", "Ord", flags="sl")
sym("varpi", "S", "omega1", "Ord", flags="sl")
sym("varsigma", "S", "sigma1", "Ord", flags="sl")
sym("phi", "S", "phi", "Ord", flags="sl", note="Symbol 0x66 is the straight (closed) phi")
sym("varphi", "S", "phi1", "Ord", flags="sl", note="Symbol 0x6A is the open (loopy) phi")
sym("varkappa", "S", "kappa", "Ord", flags="sl degr", note="same glyph as kappa")
sym("varrho", "S", "rho", "Ord", flags="sl degr", note="same glyph as rho")

group("greek-upper")
for _name in ["Gamma", "Delta", "Theta", "Lambda", "Xi", "Pi", "Sigma", "Phi", "Psi", "Omega"]:
    sym(_name, "S", _name, "Ord")
sym("Upsilon", "S", "Upsilon1", "Ord", note="curly Upsilon (Symbol Upsilon1), as in TeX")
for _name in ["Alpha", "Beta", "Epsilon", "Zeta", "Eta", "Iota", "Kappa", "Mu", "Nu", "Omicron",
              "Rho", "Tau", "Chi"]:
    sym(_name, "S", _name, "Ord", note="not a LaTeX command (KaTeX/unicode-math name); accepted")
for _name in ["Gamma", "Delta", "Theta", "Lambda", "Xi", "Pi", "Sigma", "Phi", "Psi", "Omega"]:
    sym("var" + _name, "S", _name, "Ord", flags="SL", note="amsmath italic capital")
sym("varUpsilon", "S", "Upsilon1", "Ord", flags="SL", note="amsmath italic capital")

group("ordinary")
sym("infty", "S", "infinity", "Ord")
sym("partial", "S", "partialdiff", "Ord")
sym("nabla", "S", "gradient", "Ord")
sym("forall", "S", "universal", "Ord")
sym("exists", "S", "existential", "Ord")
sym("emptyset varnothing", "S", "emptyset", "Ord")
sym("aleph", "S", "aleph", "Ord")
sym("Re", "S", "Rfraktur", "Ord")
sym("Im", "S", "Ifraktur", "Ord")
sym("wp", "S", "weierstrass", "Ord")
sym("angle", "S", "angle", "Ord")
sym("neg lnot", "S", "logicalnot", "Ord")
sym("bot", "S", "perpendicular", "Ord")
sym("surd", "S", "radical", "Ord")
sym("checkmark", "S", "radical", "Ord", flags="degr", note="shown as the radical sign")
sym("clubsuit", "S", "club", "Ord")
sym("diamondsuit", "S", "diamond", "Ord", flags="degr", note="filled suit instead of the outlined TeX one")
sym("heartsuit", "S", "heart", "Ord", flags="degr", note="filled suit instead of the outlined TeX one")
sym("spadesuit", "S", "spade", "Ord")
sym("lozenge Diamond", "S", "lozenge", "Ord")
sym("triangle bigtriangleup vartriangle", "S", "Delta", "Ord", flags="degr", note="shown as Symbol Delta")
sym("degree", "S", "degree", "Ord", note="gensymb/siunitx name; glyph is already raised")
sym("prime", "S", "minute", "Ord", note="already raised and script-sized")
sym("backslash", "R", "backslash", "Ord")
sym("imath", "I", "dotlessi", "Ord")
sym("jmath", "I", "j", "Ord", flags="degr", note="no dotless j; plain italic j")
sym("ell", "I", "l", "Ord", flags="degr", note="no script l; plain italic l")
sym("complement", "R", "C", "Ord", flags="degr", note="upright C")
sym("S", "R", "section", "Ord")
sym("P", "R", "paragraph", "Ord")
sym("pounds", "R", "sterling", "Ord")
sym("yen", "R", "yen", "Ord")
sym("copyright", "S", "copyrightserif", "Ord")
sym("euro", "S", "Euro", "Ord", note="eurosym name")
sym("sharp", "R", "numbersign", "Ord", flags="degr", note="shown as #")
sym("%", "R", "percent", "Ord")
sym("$", "R", "dollar", "Ord")
sym("&", "R", "ampersand", "Ord")
sym("#", "R", "numbersign", "Ord")
sym("_", "R", "underscore", "Ord")
sym("colon", "R", "colon", "Punct")
sym("ldots dotsc dotso hdots mathellipsis", "R", "ellipsis", "Inner")

group("binary")
sym("times", "S", "multiply", "Bin")
sym("div", "S", "divide", "Bin")
sym("pm", "S", "plusminus", "Bin")
sym("cdot centerdot", "S", "dotmath", "Bin")
sym("ast", "S", "asteriskmath", "Bin")
sym("star", "S", "asteriskmath", "Bin", flags="degr", note="six-pointed asterisk instead of a five-pointed star")
sym("bullet", "S", "bullet", "Bin")
sym("cap", "S", "intersection", "Bin")
sym("cup", "S", "union", "Bin")
sym("vee lor", "S", "logicalor", "Bin")
sym("wedge land", "S", "logicaland", "Bin")
sym("setminus smallsetminus", "R", "backslash", "Bin")
sym("oplus", "S", "circleplus", "Bin")
sym("otimes", "S", "circlemultiply", "Bin")
sym("dagger", "R", "dagger", "Bin")
sym("ddagger", "R", "daggerdbl", "Bin")

group("relation")
sym("leq le", "S", "lessequal", "Rel")
sym("geq ge", "S", "greaterequal", "Rel")
sym("leqslant", "S", "lessequal", "Rel", flags="degr", note="slanted-bar form unavailable")
sym("geqslant", "S", "greaterequal", "Rel", flags="degr", note="slanted-bar form unavailable")
sym("lt", "S", "less", "Rel")
sym("gt", "S", "greater", "Rel")
sym("neq ne", "S", "notequal", "Rel")
sym("equiv", "S", "equivalence", "Rel")
sym("approx", "S", "approxequal", "Rel")
sym("sim", "S", "similar", "Rel")
sym("cong", "S", "congruent", "Rel")
sym("propto varpropto", "S", "proportional", "Rel")
sym("in", "S", "element", "Rel")
sym("ni owns", "S", "suchthat", "Rel")
sym("notin", "S", "notelement", "Rel")
sym("subset", "S", "propersubset", "Rel")
sym("supset", "S", "propersuperset", "Rel")
sym("subseteq", "S", "reflexsubset", "Rel")
sym("supseteq", "S", "reflexsuperset", "Rel")
sym("nsubset", "S", "notsubset", "Rel", note="also the result of not + subset")
sym("perp", "S", "perpendicular", "Rel")
sym("mid", "R", "bar", "Rel")
sym("therefore", "S", "therefore", "Rel")
sym("prec", "S", "less", "Rel", flags="degr", note="curved ordering shown as <")
sym("succ", "S", "greater", "Rel", flags="degr", note="curved ordering shown as >")
sym("preceq preccurlyeq", "S", "lessequal", "Rel", flags="degr", note="curved ordering shown as <=")
sym("succeq succcurlyeq", "S", "greaterequal", "Rel", flags="degr", note="curved ordering shown as >=")
sym("approxeq", "S", "approxequal", "Rel", flags="degr", note="shown as approx")
sym("asymp", "S", "approxequal", "Rel", flags="degr", note="shown as approx")

group("arrow")
sym("leftarrow gets", "S", "arrowleft", "Rel")
sym("rightarrow to", "S", "arrowright", "Rel")
sym("leftrightarrow", "S", "arrowboth", "Rel")
sym("uparrow", "S", "arrowup", "Rel")
sym("downarrow", "S", "arrowdown", "Rel")
sym("Leftarrow", "S", "arrowdblleft", "Rel")
sym("Rightarrow", "S", "arrowdblright", "Rel")
sym("Leftrightarrow", "S", "arrowdblboth", "Rel")
sym("Uparrow", "S", "arrowdblup", "Rel")
sym("Downarrow", "S", "arrowdbldown", "Rel")
sym("Longrightarrow", "S", "arrowdblright", "Rel",
    note="same glyph as Rightarrow: Symbol has no double-shaft extender")
sym("Longleftarrow", "S", "arrowdblleft", "Rel", note="same glyph as Leftarrow")
sym("Longleftrightarrow", "S", "arrowdblboth", "Rel", note="same glyph as Leftrightarrow")
sym("implies", "S", "arrowdblright", "Rel", flags="pad")
sym("impliedby", "S", "arrowdblleft", "Rel", flags="pad")
sym("iff", "S", "arrowdblboth", "Rel", flags="pad")
sym("hookrightarrow rightharpoonup rightharpoondown leadsto rightsquigarrow twoheadrightarrow "
    "rightarrowtail dashrightarrow",
    "S", "arrowright", "Rel", flags="degr", note="decorated arrow shown as a plain rightarrow")
sym("hookleftarrow leftharpoonup leftharpoondown twoheadleftarrow leftarrowtail dashleftarrow",
    "S", "arrowleft", "Rel", flags="degr", note="decorated arrow shown as a plain leftarrow")
sym("carriagereturn", "S", "carriagereturn", "Ord", note="not a TeX name; exposes the Symbol glyph")

group("bigop")
sym("sum", "S", "summation", "Op", limits="D")
sym("prod", "S", "product", "Op", limits="D")
sym("int intop smallint", "S", "integral", "Op", limits="N")
sym("bigcup", "S", "union", "Op", limits="D")
sym("bigcap", "S", "intersection", "Op", limits="D")
sym("bigvee", "S", "logicalor", "Op", limits="D")
sym("bigwedge", "S", "logicaland", "Op", limits="D")
sym("bigoplus", "S", "circleplus", "Op", limits="D")
sym("bigotimes", "S", "circlemultiply", "Op", limits="D")

# Big operator (first name of its row) -> scale group (enum MathBigOp; the numbers are in the layout).
BIGOP_GROUP = {
    "sum": "sum", "prod": "sum", "coprod": "sum",
    "int": "int", "oint": "int", "iint": "int", "iiint": "int",
    "bigcup": "bigcup", "bigcap": "bigcup", "biguplus": "bigcup", "bigsqcup": "bigcup",
    "bigvee": "bigvee", "bigwedge": "bigvee", "bigoplus": "bigvee", "bigotimes": "bigvee",
    "bigodot": "bigvee",
}

# ---- 5.2 Negated relations: the base command with kAtomNegated -----------------------------------
NEGATED = {
    "nless": "lt", "ngtr": "gt", "nleq": "leq", "ngeq": "geq", "nsim": "sim", "ncong": "cong",
    "nequiv": "equiv", "napprox": "approx", "nmid": "mid", "nparallel": "parallel",
    "nsubseteq": "subseteq", "nsupseteq": "supseteq", "nsupset": "supset", "nexists": "exists",
    "notni": "ni", "nrightarrow": "rightarrow", "nleftarrow": "leftarrow",
    "nRightarrow": "Rightarrow", "nLeftarrow": "Leftarrow", "nleftrightarrow": "leftrightarrow",
    "nLeftrightarrow": "Leftrightarrow", "nvdash": "vdash", "nvDash": "models", "nprec": "prec",
    "nsucc": "succ",
}

# ---- 5.3 Delimiters --------------------------------------------------------------------------------
# tokens: what may follow \left \right \middle \big...; base: ("G", font, glyph), ("V",) for a
# vector-only delimiter, ("C", composite name), or None for the null delimiter.
DELIMS = [
    {"key": "none", "tokens": ["."], "base": None},
    {"key": "lparen", "tokens": ["(", "\\lparen", "\\lgroup"], "base": ("G", "R", "parenleft")},
    {"key": "rparen", "tokens": [")", "\\rparen", "\\rgroup"], "base": ("G", "R", "parenright")},
    {"key": "lbrack", "tokens": ["[", "\\lbrack"], "base": ("G", "R", "bracketleft")},
    {"key": "rbrack", "tokens": ["]", "\\rbrack"], "base": ("G", "R", "bracketright")},
    {"key": "lbrace", "tokens": ["\\{", "\\lbrace"], "base": ("G", "R", "braceleft")},
    {"key": "rbrace", "tokens": ["\\}", "\\rbrace"], "base": ("G", "R", "braceright")},
    {"key": "vert", "tokens": ["|", "\\vert", "\\lvert", "\\rvert"], "base": ("G", "R", "bar")},
    {"key": "Vert", "tokens": ["\\|", "\\Vert", "\\lVert", "\\rVert"], "base": ("V",)},
    {"key": "langle", "tokens": ["\\langle", "<"], "base": ("G", "S", "angleleft")},
    {"key": "rangle", "tokens": ["\\rangle", ">"], "base": ("G", "S", "angleright")},
    {"key": "lfloor", "tokens": ["\\lfloor"], "base": ("V",)},
    {"key": "rfloor", "tokens": ["\\rfloor"], "base": ("V",)},
    {"key": "lceil", "tokens": ["\\lceil"], "base": ("V",)},
    {"key": "rceil", "tokens": ["\\rceil"], "base": ("V",)},
    {"key": "slash", "tokens": ["/"], "base": ("G", "R", "slash")},
    {"key": "backslash", "tokens": ["\\backslash"], "base": ("G", "R", "backslash")},
    {"key": "uparrow", "tokens": ["\\uparrow"], "base": ("G", "S", "arrowup")},
    {"key": "downarrow", "tokens": ["\\downarrow"], "base": ("G", "S", "arrowdown")},
    {"key": "updownarrow", "tokens": ["\\updownarrow"], "base": ("C", "updownarrow")},
    {"key": "Uparrow", "tokens": ["\\Uparrow"], "base": ("G", "S", "arrowdblup")},
    {"key": "Downarrow", "tokens": ["\\Downarrow"], "base": ("G", "S", "arrowdbldown")},
    {"key": "Updownarrow", "tokens": ["\\Updownarrow"], "base": ("C", "Updownarrow")},
]
# Class of a delimiter token used bare (not after \left, \right, \middle, \big...).
DELIM_BARE_CLASS = {
    "(": "Open", ")": "Close", "[": "Open", "]": "Close", "\\{": "Open", "\\}": "Close",
    "\\lbrace": "Open", "\\rbrace": "Close", "\\lbrack": "Open", "\\rbrack": "Close",
    "\\lparen": "Open", "\\rparen": "Close", "\\lgroup": "Open", "\\rgroup": "Close",
    "|": "Ord", "\\vert": "Ord", "\\lvert": "Open", "\\rvert": "Close",
    "\\|": "Ord", "\\Vert": "Ord", "\\lVert": "Open", "\\rVert": "Close",
    "\\langle": "Open", "\\rangle": "Close", "\\lfloor": "Open", "\\rfloor": "Close",
    "\\lceil": "Open", "\\rceil": "Close", "/": "Ord", "\\backslash": "Ord",
    "\\uparrow": "Rel", "\\downarrow": "Rel", "\\updownarrow": "Rel", "\\Uparrow": "Rel",
    "\\Downarrow": "Rel", "\\Updownarrow": "Rel", "<": "Rel", ">": "Rel", ".": "Ord",
}

# ---- 5.4 Named operators: upright Times-Roman, class Op; "~" marks a thin space (167) ------------
OPNAMES_NOLIMITS = ["arccos", "arcsin", "arctan", "arg", "cos", "cosh", "cot", "coth", "csc", "deg",
                    "dim", "exp", "hom", "ker", "lg", "ln", "log", "sec", "sin", "sinh", "tan", "tanh"]
OPNAMES_LIMITS = {"lim": "lim", "liminf": "lim~inf", "limsup": "lim~sup", "max": "max", "min": "min",
                  "sup": "sup", "inf": "inf", "det": "det", "gcd": "gcd", "Pr": "Pr",
                  "argmax": "arg~max", "argmin": "arg~min"}

# ---- 5.5 Accents -----------------------------------------------------------------------------------
# Glyph accents: (command, StandardEncoding accent glyph), in the order of MathAccent 0..9.
ACCENTS = [
    ("hat", "circumflex"), ("check", "caron"), ("tilde", "tilde"), ("acute", "acute"),
    ("grave", "grave"), ("dot", "dotaccent"), ("ddot", "dieresis"), ("breve", "breve"),
    ("bar", "macron"), ("mathring", "ring"),
]
ACCENTS_DRAWN = ["vec", "dddot", "widehat", "widetilde"]         # MathAccent 10..13
OVERUNDER = {"overline": 0, "underline": 1, "underbar": 1, "overrightarrow": 2, "overleftarrow": 3,
             "overleftrightarrow": 4, "overbrace": 5, "underbrace": 6}

# ---- 5.6 Font switches -----------------------------------------------------------------------------
MATH_FONT_CMDS = {   # command with one math argument -> variant
    "mathnormal": "Normal", "mathrm": "Roman", "mathup": "Roman", "mathbf": "Bold", "bold": "Bold",
    "mathit": "Italic", "boldsymbol": "BoldItalic", "bm": "BoldItalic", "pmb": "BoldItalic",
    "mathbfit": "BoldItalic", "mathbb": "Blackboard", "mathbbm": "Blackboard", "mathds": "Blackboard",
    "Bbb": "Blackboard", "mathcal": "Calligraphic", "mathscr": "Calligraphic", "mathfrak": "Fraktur",
    "mathsf": "Sans", "mathtt": "Mono",
}
MATH_FONT_DECLS = {  # declaration: variant to the end of the enclosing group
    "rm": "Roman", "bf": "Bold", "it": "Italic", "cal": "Calligraphic", "sf": "Sans", "tt": "Mono",
    "mit": "Normal", "normalfont": "Roman",
}
TEXT_CMDS = {        # command with one text argument -> (set bits, clear bits); bold = 1, italic = 2
    "text": (0, 3), "textnormal": (0, 3), "textrm": (0, 2), "textup": (0, 2), "mbox": (0, 3),
    "hbox": (0, 3), "textmd": (0, 1), "textbf": (1, 0), "textit": (2, 0), "emph": (2, 0),
    "textsl": (2, 0), "textsf": (0, 2), "texttt": (0, 2), "intertext": (0, 3), "shortintertext": (0, 3),
    "fbox": (0, 3),
}
TEXT_BOXED = ("fbox",)   # the text is wrapped in an Enclose box
TEXT_DECLS = {"bfseries": (1, 0), "bf": (1, 0), "itshape": (2, 0), "it": (2, 0), "em": (2, 0),
              "slshape": (2, 0), "sl": (2, 0), "rm": (0, 3), "rmfamily": (0, 0), "upshape": (0, 2),
              "mdseries": (0, 1), "normalfont": (0, 3), "sf": (0, 3), "tt": (0, 3)}
# One-argument macros that expand to \left<l> #1 \right<r> (braket and physics packages, KaTeX).
PAIRED = {"bra": ("langle", "vert"), "Bra": ("langle", "vert"), "ket": ("vert", "rangle"),
          "Ket": ("vert", "rangle"), "braket": ("langle", "rangle"), "Braket": ("langle", "rangle"),
          "abs": ("vert", "vert"), "norm": ("Vert", "Vert")}
BB_ALIASES = {"R": "R", "N": "N", "Z": "Z", "Q": "Q", "C": "C", "reals": "R", "Reals": "R",
              "natnums": "N", "Complex": "C", "cnums": "C"}

# variant -> (font of Latin letters, font of digits, flags of Latin letters and digits,
#             flags of uppercase Greek, flags of lowercase Greek, flags on letters only)
VARIANTS = {
    "Normal":       ("I", "R", "", "", "slant small", False),
    "Roman":        ("R", "R", "", "", "small", False),
    "Bold":         ("B", "B", "", "heavy", "heavy small", False),
    "Italic":       ("I", "I", "", "slant", "slant small", False),
    "BoldItalic":   ("BI", "B", "", "heavy", "heavy slant small", False),
    "Blackboard":   ("B", "B", "outline", "", "slant small", False),
    "Calligraphic": ("BI", "R", "outline", "", "slant small", True),
    "Fraktur":      ("R", "R", "", "", "slant small", False),
    "Sans":         ("R", "R", "", "", "slant small", False),
    "Mono":         ("R", "R", "", "", "slant small", False),
}

# ---- 5.7 Spacing: widths in 1/1000 em. Names that are not letters are control symbols. -----------
SPACES = [
    (", thinspace", 167), (": > medspace", 222), ("; thickspace", 278),
    ("! negthinspace", -167), ("negmedspace", -222), ("negthickspace", -278),
    ("(space) space nobreakspace", 250),
    ("enspace enskip", 500), ("quad", 1000), ("qquad", 2000),
]

# ---- 5.8 Structural commands: (names, command kind, a, b, c) -------------------------------------
STRUCT = [
    ("frac", "Fraction", FRACTION_BAR, 0, 0),
    ("dfrac cfrac", "Fraction", FRACTION_BAR | FRACTION_DISPLAY, 0, 0),
    ("tfrac", "Fraction", FRACTION_BAR | FRACTION_TEXT, 0, 0),
    ("binom", "Fraction", 0, 1, 0),                      # b = 1: ( ) delimiters
    ("dbinom", "Fraction", FRACTION_DISPLAY, 1, 0),
    ("tbinom", "Fraction", FRACTION_TEXT, 1, 0),
    ("over", "Infix", FRACTION_BAR, 0, 0),
    ("atop", "Infix", 0, 0, 0),
    ("choose", "Infix", 0, 1, 0),
    ("sqrt", "Sqrt", 0, 0, 0),
    ("overset", "Stack", 0, 0, 0),
    ("underset", "Stack", 1, 0, 0),
    ("stackrel", "Stack", 2, 0, 0),
    ("xrightarrow", "XArrow", 1, 0, 0),                  # MathStackArrow
    ("xleftarrow", "XArrow", 2, 0, 0),
    ("boxed", "Enclose", 0, 0, 0),                       # MathEnclose
    ("cancel", "Enclose", 1, 0, 0),
    ("bcancel", "Enclose", 2, 0, 0),
    ("xcancel", "Enclose", 3, 0, 0),
    ("operatorname", "OperatorName", 0, 0, 0),
    ("substack", "Substack", 0, 0, 0),
    ("not", "Not", 0, 0, 0),
    ("left", "Left", 0, 0, 0),
    ("right", "Right", 0, 0, 0),
    ("middle", "Middle", 0, 0, 0),
    ("begin", "Begin", 0, 0, 0),
    ("end", "End", 0, 0, 0),
    ("cr newline tabularnewline", "RowSep", 0, 0, 0),
    ("tag", "Tag", 0, 0, 0),
    ("hline hdashline", "Hline", 0, 0, 0),
    ("displaystyle", "Style", 0, 0, 0),
    ("textstyle", "Style", 1, 0, 0),
    ("scriptstyle", "Style", 2, 0, 0),
    ("scriptscriptstyle", "Style", 3, 0, 0),
    ("mathord", "Class", CLASS_ID["Ord"], 0, 0),
    ("mathop", "Class", CLASS_ID["Op"], 0, 0),
    ("mathbin", "Class", CLASS_ID["Bin"], 0, 0),
    ("mathrel", "Class", CLASS_ID["Rel"], 0, 0),
    ("mathopen", "Class", CLASS_ID["Open"], 0, 0),
    ("mathclose", "Class", CLASS_ID["Close"], 0, 0),
    ("mathpunct", "Class", CLASS_ID["Punct"], 0, 0),
    ("mathinner", "Class", CLASS_ID["Inner"], 0, 0),
    ("limits", "Limits", 1, 0, 0),                       # MathLimitsMode
    ("nolimits", "Limits", 2, 0, 0),
    ("displaylimits", "Limits", 0, 0, 0),
    ("phantom", "Phantom", 0, 0, 0),                     # MathPhantom
    ("hphantom", "Phantom", 1, 0, 0),
    ("vphantom", "Phantom", 2, 0, 0),
    ("bmod", "Mod", 0, 0, 0),
    ("pmod", "Mod", 1, 0, 0),
    ("mod", "Mod", 2, 0, 0),
    ("pod", "Mod", 3, 0, 0),
    ("hspace mspace", "HSpace", 0, 0, 0),
    ("kern mkern hskip mskip", "Kern", 0, 0, 0),
    ("dots", "Dots", 0, 0, 0),
    ("textcolor colorbox", "ColorArg", 0, 0, 0),
    ("eqref", "Ref", 1, 0, 0),                           # a = 1: parentheses
    ("ref", "Ref", 0, 0, 0),
    ("smash mathclap mathrlap mathllap rlap llap clap ensuremath lefteqn", "Transparent", 0, 0, 0),
    # Ignore: a = raw {...} arguments to drop, b = 1 an optional * first, c = 1 an optional [...] first
    ("label vspace cline noalign bibitem color", "Ignore", 1, 1, 0),
    ("rule", "Ignore", 2, 0, 1),
    ("nonumber notag allowbreak nobreak displaybreak relax protect noindent centering hfill hfil "
     "null nolinebreak linebreak break par ignorespaces unskip tiny scriptsize footnotesize small "
     "normalsize large Large LARGE huge Huge mathstrut strut allowdisplaybreaks", "Ignore", 0, 0, 0),
    # TextOnly: 0 the ellipsis glyph 0xBC, 1 the backslash glyph, 2 the name as text
    ("textellipsis", "TextOnly", 0, 0, 0),
    ("textbackslash", "TextOnly", 1, 0, 0),
    ("TeX LaTeX", "TextOnly", 2, 0, 0),
]
SIZED_BASES = ("big", "Big", "bigg", "Bigg")                                 # level 1..4
SIZED_SUFFIXES = (("", "Ord"), ("l", "Open"), ("r", "Close"), ("m", "Rel"))

# ---- 5.9 Environments ------------------------------------------------------------------------------
# (names, left, right, columns, cell style, column gap in 1/1000 em, family, arguments)
# family: cases -> kArrayCases, aligned -> kArrayRowGap | kArrayAligned, rowgap -> kArrayRowGap.
# For the aligned family the gap is the one between column pairs. The class is Inner when the
# environment has a delimiter, else Ord.
ENVS = [
    ("matrix", "none", "none", "Centre", "text", 1000, "", "None"),
    ("pmatrix", "lparen", "rparen", "Centre", "text", 1000, "", "None"),
    ("bmatrix", "lbrack", "rbrack", "Centre", "text", 1000, "", "None"),
    ("Bmatrix", "lbrace", "rbrace", "Centre", "text", 1000, "", "None"),
    ("vmatrix", "vert", "vert", "Centre", "text", 1000, "", "None"),
    ("Vmatrix", "Vert", "Vert", "Centre", "text", 1000, "", "None"),
    ("matrix*", "none", "none", "Centre", "text", 1000, "", "OptAlign"),
    ("pmatrix*", "lparen", "rparen", "Centre", "text", 1000, "", "OptAlign"),
    ("bmatrix*", "lbrack", "rbrack", "Centre", "text", 1000, "", "OptAlign"),
    ("Bmatrix*", "lbrace", "rbrace", "Centre", "text", 1000, "", "OptAlign"),
    ("vmatrix*", "vert", "vert", "Centre", "text", 1000, "", "OptAlign"),
    ("Vmatrix*", "Vert", "Vert", "Centre", "text", 1000, "", "OptAlign"),
    ("smallmatrix", "none", "none", "Centre", "script", 500, "", "None"),
    ("cases", "lbrace", "none", "Left", "text", 1000, "cases", "None"),
    ("dcases", "lbrace", "none", "Left", "display", 1000, "cases", "None"),
    ("rcases", "none", "rbrace", "Left", "text", 1000, "cases", "None"),
    ("aligned align align* flalign flalign* split", "none", "none", "Pairs", "display", 1000,
     "aligned", "None"),
    ("alignedat alignat alignat*", "none", "none", "Pairs", "display", 0, "aligned", "DropBrace"),
    ("eqnarray eqnarray*", "none", "none", "Rcl", "display", 278, "rowgap", "None"),
    ("gathered gather gather* multline multline* equation equation* displaymath math",
     "none", "none", "Centre", "display", 0, "rowgap", "None"),
    ("array", "none", "none", "Spec", "text", 1000, "", "ArraySpec"),
    ("subarray", "none", "none", "Spec", "script", 0, "", "SubarraySpec"),
]
ENV_FAMILY_BITS = {"": 0, "cases": ARRAY_CASES, "aligned": ARRAY_ROW_GAP | ARRAY_ALIGNED,
                   "rowgap": ARRAY_ROW_GAP}

# ---- 5.10 ASCII characters typed in math mode: (character, font, glyph, class) -------------------
ASCII = [
    ("+", "S", "plus", "Bin"),
    ("-", "S", "minus", "Bin"),          # a real minus sign; Times has only a hyphen at 0x2D
    ("*", "S", "asteriskmath", "Bin"),
    ("/", "R", "slash", "Ord"),
    ("=", "S", "equal", "Rel"),
    ("<", "S", "less", "Rel"),
    (">", "S", "greater", "Rel"),
    (":", "R", "colon", "Rel"),
    (",", "R", "comma", "Punct"),        # always Punct; a decimal comma is written {,}
    (";", "R", "semicolon", "Punct"),
    (".", "R", "period", "Ord"),
    ("!", "R", "exclam", "Close"),
    ("?", "R", "question", "Close"),
    ("(", "R", "parenleft", "Open"),
    (")", "R", "parenright", "Close"),
    ("[", "R", "bracketleft", "Open"),
    ("]", "R", "bracketright", "Close"),
    ("|", "R", "bar", "Ord"),
    ('"', "R", "quotedblright", "Ord"),
    ("`", "R", "quoteleft", "Ord"),
    ("@", "R", "at", "Ord"),
    ("#", "R", "numbersign", "Ord"),
    ("$", "R", "dollar", "Ord"),
]
# Characters the parser handles itself; their kMathAscii rows are {0, 0}. Letters and digits too.
ASCII_PARSER = {" ": "ignored", "{": "group", "}": "group", "^": "superscript", "_": "subscript",
                "&": "cell separator", "~": "space 250", "%": "comment or percent sign",
                "\\": "control sequence", "'": "prime"}
ASCII_TIMES_REMAPPED = '"'     # the only Times row whose code is not the character's own
# Glyphs the parser emits from constants; verified here so that the constants stay true.
TEXT_ESCAPES = {"%": "percent", "$": "dollar", "&": "ampersand", "#": "numbersign",
                "_": "underscore", "{": "braceleft", "}": "braceright", "\\": "backslash",
                "?": "question", "-": "hyphen", "*": "asterisk"}
TEXT_ELLIPSIS_CODE = 0xBC
DIGIT_GLYPHS = ("zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine")

# ---- 5.11 Unicode code points typed in math mode ---------------------------------------------------
# A string is the control word the character behaves like. Otherwise: ("glyph", font, glyph,
# class), ("minus",), ("prime", count), ("sup", ascii), ("sub", ascii), ("bb", letter),
# ("space", width), ("ignore",).
UNICODE_TABLE = (
    (0x00A0, ("space", 250)),               # NO-BREAK SPACE
    (0x00A3, "pounds"),                     # POUND SIGN
    (0x00A5, "yen"),                        # YEN SIGN
    (0x00A7, "S"),                          # SECTION SIGN
    (0x00A9, "copyright"),                  # COPYRIGHT SIGN
    (0x00AC, "neg"),                        # NOT SIGN
    (0x00AD, ("ignore",)),                  # SOFT HYPHEN
    (0x00AE, ("glyph", "S", "registerserif", "Ord")),# REGISTERED SIGN
    (0x00B0, "degree"),                     # DEGREE SIGN
    (0x00B1, "pm"),                         # PLUS-MINUS SIGN
    (0x00B2, ("sup", "2")),                 # SUPERSCRIPT TWO
    (0x00B3, ("sup", "3")),                 # SUPERSCRIPT THREE
    (0x00B5, "mu"),                         # MICRO SIGN
    (0x00B6, "P"),                          # PILCROW SIGN
    (0x00B7, "cdot"),                       # MIDDLE DOT
    (0x00B9, ("sup", "1")),                 # SUPERSCRIPT ONE
    (0x00D7, "times"),                      # MULTIPLICATION SIGN
    (0x00F7, "div"),                        # DIVISION SIGN
    (0x0391, "Alpha"),                      # GREEK CAPITAL LETTER ALPHA
    (0x0392, "Beta"),                       # GREEK CAPITAL LETTER BETA
    (0x0393, "Gamma"),                      # GREEK CAPITAL LETTER GAMMA
    (0x0394, "Delta"),                      # GREEK CAPITAL LETTER DELTA
    (0x0395, "Epsilon"),                    # GREEK CAPITAL LETTER EPSILON
    (0x0396, "Zeta"),                       # GREEK CAPITAL LETTER ZETA
    (0x0397, "Eta"),                        # GREEK CAPITAL LETTER ETA
    (0x0398, "Theta"),                      # GREEK CAPITAL LETTER THETA
    (0x0399, "Iota"),                       # GREEK CAPITAL LETTER IOTA
    (0x039A, "Kappa"),                      # GREEK CAPITAL LETTER KAPPA
    (0x039B, "Lambda"),                     # GREEK CAPITAL LETTER LAMDA
    (0x039C, "Mu"),                         # GREEK CAPITAL LETTER MU
    (0x039D, "Nu"),                         # GREEK CAPITAL LETTER NU
    (0x039E, "Xi"),                         # GREEK CAPITAL LETTER XI
    (0x039F, "Omicron"),                    # GREEK CAPITAL LETTER OMICRON
    (0x03A0, "Pi"),                         # GREEK CAPITAL LETTER PI
    (0x03A1, "Rho"),                        # GREEK CAPITAL LETTER RHO
    (0x03A3, "Sigma"),                      # GREEK CAPITAL LETTER SIGMA
    (0x03A4, "Tau"),                        # GREEK CAPITAL LETTER TAU
    (0x03A5, ("glyph", "S", "Upsilon", "Ord")),# GREEK CAPITAL LETTER UPSILON
    (0x03A6, "Phi"),                        # GREEK CAPITAL LETTER PHI
    (0x03A7, "Chi"),                        # GREEK CAPITAL LETTER CHI
    (0x03A8, "Psi"),                        # GREEK CAPITAL LETTER PSI
    (0x03A9, "Omega"),                      # GREEK CAPITAL LETTER OMEGA
    (0x03B1, "alpha"),                      # GREEK SMALL LETTER ALPHA
    (0x03B2, "beta"),                       # GREEK SMALL LETTER BETA
    (0x03B3, "gamma"),                      # GREEK SMALL LETTER GAMMA
    (0x03B4, "delta"),                      # GREEK SMALL LETTER DELTA
    (0x03B5, "varepsilon"),                 # GREEK SMALL LETTER EPSILON
    (0x03B6, "zeta"),                       # GREEK SMALL LETTER ZETA
    (0x03B7, "eta"),                        # GREEK SMALL LETTER ETA
    (0x03B8, "theta"),                      # GREEK SMALL LETTER THETA
    (0x03B9, "iota"),                       # GREEK SMALL LETTER IOTA
    (0x03BA, "kappa"),                      # GREEK SMALL LETTER KAPPA
    (0x03BB, "lambda"),                     # GREEK SMALL LETTER LAMDA
    (0x03BC, "mu"),                         # GREEK SMALL LETTER MU
    (0x03BD, "nu"),                         # GREEK SMALL LETTER NU
    (0x03BE, "xi"),                         # GREEK SMALL LETTER XI
    (0x03BF, "omicron"),                    # GREEK SMALL LETTER OMICRON
    (0x03C0, "pi"),                         # GREEK SMALL LETTER PI
    (0x03C1, "rho"),                        # GREEK SMALL LETTER RHO
    (0x03C2, "varsigma"),                   # GREEK SMALL LETTER FINAL SIGMA
    (0x03C3, "sigma"),                      # GREEK SMALL LETTER SIGMA
    (0x03C4, "tau"),                        # GREEK SMALL LETTER TAU
    (0x03C5, "upsilon"),                    # GREEK SMALL LETTER UPSILON
    (0x03C6, "varphi"),                     # GREEK SMALL LETTER PHI
    (0x03C7, "chi"),                        # GREEK SMALL LETTER CHI
    (0x03C8, "psi"),                        # GREEK SMALL LETTER PSI
    (0x03C9, "omega"),                      # GREEK SMALL LETTER OMEGA
    (0x03D1, "vartheta"),                   # GREEK THETA SYMBOL
    (0x03D2, "Upsilon"),                    # GREEK UPSILON WITH HOOK SYMBOL
    (0x03D5, "phi"),                        # GREEK PHI SYMBOL
    (0x03D6, "varpi"),                      # GREEK PI SYMBOL
    (0x03F0, "varkappa"),                   # GREEK KAPPA SYMBOL
    (0x03F1, "varrho"),                     # GREEK RHO SYMBOL
    (0x03F5, "epsilon"),                    # GREEK LUNATE EPSILON SYMBOL
    (0x1D62, ("sub", "i")),                 # LATIN SUBSCRIPT SMALL LETTER I
    (0x2002, ("space", 500)),               # EN SPACE
    (0x2003, ("space", 1000)),              # EM SPACE
    (0x2004, ("space", 278)),               # THREE-PER-EM SPACE
    (0x2005, ("space", 222)),               # FOUR-PER-EM SPACE
    (0x2006, ("space", 167)),               # SIX-PER-EM SPACE
    (0x2007, ("space", 250)),               # FIGURE SPACE
    (0x2009, ("space", 167)),               # THIN SPACE
    (0x200A, ("ignore",)),                  # HAIR SPACE
    (0x200B, ("ignore",)),                  # ZERO WIDTH SPACE
    (0x200C, ("ignore",)),                  # ZERO WIDTH NON-JOINER
    (0x200D, ("ignore",)),                  # ZERO WIDTH JOINER
    (0x2010, ("minus",)),                   # HYPHEN
    (0x2011, ("minus",)),                   # NON-BREAKING HYPHEN
    (0x2012, ("minus",)),                   # FIGURE DASH
    (0x2013, ("minus",)),                   # EN DASH
    (0x2014, ("minus",)),                   # EM DASH
    (0x2016, "Vert"),                       # DOUBLE VERTICAL LINE
    (0x2018, ("glyph", "R", "quoteleft", "Ord")),# LEFT SINGLE QUOTATION MARK
    (0x2019, ("prime", 1)),                 # RIGHT SINGLE QUOTATION MARK
    (0x201C, ("glyph", "R", "quotedblleft", "Ord")),# LEFT DOUBLE QUOTATION MARK
    (0x201D, ("glyph", "R", "quotedblright", "Ord")),# RIGHT DOUBLE QUOTATION MARK
    (0x2020, "dagger"),                     # DAGGER
    (0x2021, "ddagger"),                    # DOUBLE DAGGER
    (0x2022, "bullet"),                     # BULLET
    (0x2026, "ldots"),                      # HORIZONTAL ELLIPSIS
    (0x202F, ("space", 250)),               # NARROW NO-BREAK SPACE
    (0x2032, ("prime", 1)),                 # PRIME
    (0x2033, ("prime", 2)),                 # DOUBLE PRIME
    (0x2034, ("prime", 3)),                 # TRIPLE PRIME
    (0x2057, ("prime", 4)),                 # QUADRUPLE PRIME
    (0x2060, ("ignore",)),                  # WORD JOINER
    (0x2061, ("ignore",)),                  # FUNCTION APPLICATION
    (0x2062, ("ignore",)),                  # INVISIBLE TIMES
    (0x2063, ("ignore",)),                  # INVISIBLE SEPARATOR
    (0x2064, ("ignore",)),                  # INVISIBLE PLUS
    (0x2070, ("sup", "0")),                 # SUPERSCRIPT ZERO
    (0x2071, ("sup", "i")),                 # SUPERSCRIPT LATIN SMALL LETTER I
    (0x2074, ("sup", "4")),                 # SUPERSCRIPT FOUR
    (0x2075, ("sup", "5")),                 # SUPERSCRIPT FIVE
    (0x2076, ("sup", "6")),                 # SUPERSCRIPT SIX
    (0x2077, ("sup", "7")),                 # SUPERSCRIPT SEVEN
    (0x2078, ("sup", "8")),                 # SUPERSCRIPT EIGHT
    (0x2079, ("sup", "9")),                 # SUPERSCRIPT NINE
    (0x207A, ("sup", "+")),                 # SUPERSCRIPT PLUS SIGN
    (0x207B, ("sup", "-")),                 # SUPERSCRIPT MINUS
    (0x207C, ("sup", "=")),                 # SUPERSCRIPT EQUALS SIGN
    (0x207D, ("sup", "(")),                 # SUPERSCRIPT LEFT PARENTHESIS
    (0x207E, ("sup", ")")),                 # SUPERSCRIPT RIGHT PARENTHESIS
    (0x207F, ("sup", "n")),                 # SUPERSCRIPT LATIN SMALL LETTER N
    (0x2080, ("sub", "0")),                 # SUBSCRIPT ZERO
    (0x2081, ("sub", "1")),                 # SUBSCRIPT ONE
    (0x2082, ("sub", "2")),                 # SUBSCRIPT TWO
    (0x2083, ("sub", "3")),                 # SUBSCRIPT THREE
    (0x2084, ("sub", "4")),                 # SUBSCRIPT FOUR
    (0x2085, ("sub", "5")),                 # SUBSCRIPT FIVE
    (0x2086, ("sub", "6")),                 # SUBSCRIPT SIX
    (0x2087, ("sub", "7")),                 # SUBSCRIPT SEVEN
    (0x2088, ("sub", "8")),                 # SUBSCRIPT EIGHT
    (0x2089, ("sub", "9")),                 # SUBSCRIPT NINE
    (0x208A, ("sub", "+")),                 # SUBSCRIPT PLUS SIGN
    (0x208B, ("sub", "-")),                 # SUBSCRIPT MINUS
    (0x208C, ("sub", "=")),                 # SUBSCRIPT EQUALS SIGN
    (0x208D, ("sub", "(")),                 # SUBSCRIPT LEFT PARENTHESIS
    (0x208E, ("sub", ")")),                 # SUBSCRIPT RIGHT PARENTHESIS
    (0x2090, ("sub", "a")),                 # LATIN SUBSCRIPT SMALL LETTER A
    (0x2091, ("sub", "e")),                 # LATIN SUBSCRIPT SMALL LETTER E
    (0x2092, ("sub", "o")),                 # LATIN SUBSCRIPT SMALL LETTER O
    (0x2093, ("sub", "x")),                 # LATIN SUBSCRIPT SMALL LETTER X
    (0x2096, ("sub", "k")),                 # LATIN SUBSCRIPT SMALL LETTER K
    (0x2098, ("sub", "m")),                 # LATIN SUBSCRIPT SMALL LETTER M
    (0x2099, ("sub", "n")),                 # LATIN SUBSCRIPT SMALL LETTER N
    (0x20AC, "euro"),                       # EURO SIGN
    (0x2102, ("bb", "C")),                  # DOUBLE-STRUCK CAPITAL C
    (0x210D, ("bb", "H")),                  # DOUBLE-STRUCK CAPITAL H
    (0x210F, "hbar"),                       # PLANCK CONSTANT OVER TWO PI
    (0x2111, "Im"),                         # BLACK-LETTER CAPITAL I
    (0x2113, "ell"),                        # SCRIPT SMALL L
    (0x2115, ("bb", "N")),                  # DOUBLE-STRUCK CAPITAL N
    (0x2118, "wp"),                         # SCRIPT CAPITAL P
    (0x2119, ("bb", "P")),                  # DOUBLE-STRUCK CAPITAL P
    (0x211A, ("bb", "Q")),                  # DOUBLE-STRUCK CAPITAL Q
    (0x211C, "Re"),                         # BLACK-LETTER CAPITAL R
    (0x211D, ("bb", "R")),                  # DOUBLE-STRUCK CAPITAL R
    (0x2122, ("glyph", "S", "trademarkserif", "Ord")),# TRADE MARK SIGN
    (0x2124, ("bb", "Z")),                  # DOUBLE-STRUCK CAPITAL Z
    (0x2126, "Omega"),                      # OHM SIGN
    (0x2127, "mho"),                        # INVERTED OHM SIGN
    (0x2135, "aleph"),                      # ALEF SYMBOL
    (0x2190, "leftarrow"),                  # LEFTWARDS ARROW
    (0x2191, "uparrow"),                    # UPWARDS ARROW
    (0x2192, "rightarrow"),                 # RIGHTWARDS ARROW
    (0x2193, "downarrow"),                  # DOWNWARDS ARROW
    (0x2194, "leftrightarrow"),             # LEFT RIGHT ARROW
    (0x2195, "updownarrow"),                # UP DOWN ARROW
    (0x2196, "nwarrow"),                    # NORTH WEST ARROW
    (0x2197, "nearrow"),                    # NORTH EAST ARROW
    (0x2198, "searrow"),                    # SOUTH EAST ARROW
    (0x2199, "swarrow"),                    # SOUTH WEST ARROW
    (0x219B, "nrightarrow"),                # RIGHTWARDS ARROW WITH STROKE
    (0x21A6, "mapsto"),                     # RIGHTWARDS ARROW FROM BAR
    (0x21A9, "hookleftarrow"),              # LEFTWARDS ARROW WITH HOOK
    (0x21AA, "hookrightarrow"),             # RIGHTWARDS ARROW WITH HOOK
    (0x21C4, "rightleftharpoons"),          # RIGHTWARDS ARROW OVER LEFTWARDS ARROW
    (0x21C6, "rightleftharpoons"),          # LEFTWARDS ARROW OVER RIGHTWARDS ARROW
    (0x21CB, "rightleftharpoons"),          # LEFTWARDS HARPOON OVER RIGHTWARDS HARPOON
    (0x21CC, "rightleftharpoons"),          # RIGHTWARDS HARPOON OVER LEFTWARDS HARPOON
    (0x21CF, "nRightarrow"),                # RIGHTWARDS DOUBLE ARROW WITH STROKE
    (0x21D0, "Leftarrow"),                  # LEFTWARDS DOUBLE ARROW
    (0x21D1, "Uparrow"),                    # UPWARDS DOUBLE ARROW
    (0x21D2, "Rightarrow"),                 # RIGHTWARDS DOUBLE ARROW
    (0x21D3, "Downarrow"),                  # DOWNWARDS DOUBLE ARROW
    (0x21D4, "Leftrightarrow"),             # LEFT RIGHT DOUBLE ARROW
    (0x21D5, "Updownarrow"),                # UP DOWN DOUBLE ARROW
    (0x2200, "forall"),                     # FOR ALL
    (0x2202, "partial"),                    # PARTIAL DIFFERENTIAL
    (0x2203, "exists"),                     # THERE EXISTS
    (0x2204, "nexists"),                    # THERE DOES NOT EXIST
    (0x2205, "emptyset"),                   # EMPTY SET
    (0x2206, "Delta"),                      # INCREMENT
    (0x2207, "nabla"),                      # NABLA
    (0x2208, "in"),                         # ELEMENT OF
    (0x2209, "notin"),                      # NOT AN ELEMENT OF
    (0x220B, "ni"),                         # CONTAINS AS MEMBER
    (0x220C, "notni"),                      # DOES NOT CONTAIN AS MEMBER
    (0x220E, "blacksquare"),                # END OF PROOF
    (0x220F, "prod"),                       # N-ARY PRODUCT
    (0x2210, "coprod"),                     # N-ARY COPRODUCT
    (0x2211, "sum"),                        # N-ARY SUMMATION
    (0x2212, ("minus",)),                   # MINUS SIGN
    (0x2213, "mp"),                         # MINUS-OR-PLUS SIGN
    (0x2215, ("glyph", "R", "slash", "Ord")),# DIVISION SLASH
    (0x2216, "setminus"),                   # SET MINUS
    (0x2217, "ast"),                        # ASTERISK OPERATOR
    (0x2218, "circ"),                       # RING OPERATOR
    (0x2219, "bullet"),                     # BULLET OPERATOR
    (0x221A, "sqrt"),                       # SQUARE ROOT
    (0x221D, "propto"),                     # PROPORTIONAL TO
    (0x221E, "infty"),                      # INFINITY
    (0x2220, "angle"),                      # ANGLE
    (0x2223, "mid"),                        # DIVIDES
    (0x2224, "nmid"),                       # DOES NOT DIVIDE
    (0x2225, "parallel"),                   # PARALLEL TO
    (0x2226, "nparallel"),                  # NOT PARALLEL TO
    (0x2227, "wedge"),                      # LOGICAL AND
    (0x2228, "vee"),                        # LOGICAL OR
    (0x2229, "cap"),                        # INTERSECTION
    (0x222A, "cup"),                        # UNION
    (0x222B, "int"),                        # INTEGRAL
    (0x222C, "iint"),                       # DOUBLE INTEGRAL
    (0x222D, "iiint"),                      # TRIPLE INTEGRAL
    (0x222E, "oint"),                       # CONTOUR INTEGRAL
    (0x2234, "therefore"),                  # THEREFORE
    (0x2235, "because"),                    # BECAUSE
    (0x2236, ("glyph", "R", "colon", "Rel")),# RATIO
    (0x223C, "sim"),                        # TILDE OPERATOR
    (0x2243, "simeq"),                      # ASYMPTOTICALLY EQUAL TO
    (0x2245, "cong"),                       # APPROXIMATELY EQUAL TO
    (0x2248, "approx"),                     # ALMOST EQUAL TO
    (0x2250, "doteq"),                      # APPROACHES THE LIMIT
    (0x2254, "coloneqq"),                   # COLON EQUALS
    (0x225C, "triangleq"),                  # DELTA EQUAL TO
    (0x2260, "neq"),                        # NOT EQUAL TO
    (0x2261, "equiv"),                      # IDENTICAL TO
    (0x2264, "leq"),                        # LESS-THAN OR EQUAL TO
    (0x2265, "geq"),                        # GREATER-THAN OR EQUAL TO
    (0x226A, "ll"),                         # MUCH LESS-THAN
    (0x226B, "gg"),                         # MUCH GREATER-THAN
    (0x2272, "lesssim"),                    # LESS-THAN OR EQUIVALENT TO
    (0x2273, "gtrsim"),                     # GREATER-THAN OR EQUIVALENT TO
    (0x227A, "prec"),                       # PRECEDES
    (0x227B, "succ"),                       # SUCCEEDS
    (0x227C, "preceq"),                     # PRECEDES OR EQUAL TO
    (0x227D, "succeq"),                     # SUCCEEDS OR EQUAL TO
    (0x2282, "subset"),                     # SUBSET OF
    (0x2283, "supset"),                     # SUPERSET OF
    (0x2284, "nsubset"),                    # NOT A SUBSET OF
    (0x2286, "subseteq"),                   # SUBSET OF OR EQUAL TO
    (0x2287, "supseteq"),                   # SUPERSET OF OR EQUAL TO
    (0x2288, "nsubseteq"),                  # NEITHER A SUBSET OF NOR EQUAL TO
    (0x2289, "nsupseteq"),                  # NEITHER A SUPERSET OF NOR EQUAL TO
    (0x228A, "subsetneq"),                  # SUBSET OF WITH NOT EQUAL TO
    (0x228B, "supsetneq"),                  # SUPERSET OF WITH NOT EQUAL TO
    (0x228E, "uplus"),                      # MULTISET UNION
    (0x228F, "sqsubset"),                   # SQUARE IMAGE OF
    (0x2290, "sqsupset"),                   # SQUARE ORIGINAL OF
    (0x2291, "sqsubseteq"),                 # SQUARE IMAGE OF OR EQUAL TO
    (0x2292, "sqsupseteq"),                 # SQUARE ORIGINAL OF OR EQUAL TO
    (0x2293, "sqcap"),                      # SQUARE CAP
    (0x2294, "sqcup"),                      # SQUARE CUP
    (0x2295, "oplus"),                      # CIRCLED PLUS
    (0x2296, "ominus"),                     # CIRCLED MINUS
    (0x2297, "otimes"),                     # CIRCLED TIMES
    (0x2298, "oslash"),                     # CIRCLED DIVISION SLASH
    (0x2299, "odot"),                       # CIRCLED DOT OPERATOR
    (0x22A2, "vdash"),                      # RIGHT TACK
    (0x22A3, "dashv"),                      # LEFT TACK
    (0x22A4, "top"),                        # DOWN TACK
    (0x22A5, "bot"),                        # UP TACK
    (0x22A8, "models"),                     # TRUE
    (0x22C0, "bigwedge"),                   # N-ARY LOGICAL AND
    (0x22C1, "bigvee"),                     # N-ARY LOGICAL OR
    (0x22C2, "bigcap"),                     # N-ARY INTERSECTION
    (0x22C3, "bigcup"),                     # N-ARY UNION
    (0x22C4, "diamond"),                    # DIAMOND OPERATOR
    (0x22C5, "cdot"),                       # DOT OPERATOR
    (0x22C6, "star"),                       # STAR OPERATOR
    (0x22C8, "bowtie"),                     # BOWTIE
    (0x22EE, "vdots"),                      # VERTICAL ELLIPSIS
    (0x22EF, "cdots"),                      # MIDLINE HORIZONTAL ELLIPSIS
    (0x22F1, "ddots"),                      # DOWN RIGHT DIAGONAL ELLIPSIS
    (0x2300, "emptyset"),                   # DIAMETER SIGN
    (0x2308, "lceil"),                      # LEFT CEILING
    (0x2309, "rceil"),                      # RIGHT CEILING
    (0x230A, "lfloor"),                     # LEFT FLOOR
    (0x230B, "rfloor"),                     # RIGHT FLOOR
    (0x2329, "langle"),                     # LEFT-POINTING ANGLE BRACKET
    (0x232A, "rangle"),                     # RIGHT-POINTING ANGLE BRACKET
    (0x25A0, "blacksquare"),                # BLACK SQUARE
    (0x25A1, "square"),                     # WHITE SQUARE
    (0x25B3, "triangle"),                   # WHITE UP-POINTING TRIANGLE
    (0x25CA, "lozenge"),                    # LOZENGE
    (0x25CB, "bigcirc"),                    # WHITE CIRCLE
    (0x25EF, "bigcirc"),                    # LARGE CIRCLE
    (0x25FB, "square"),                     # WHITE MEDIUM SQUARE
    (0x25FC, "blacksquare"),                # BLACK MEDIUM SQUARE
    (0x2660, "spadesuit"),                  # BLACK SPADE SUIT
    (0x2663, "clubsuit"),                   # BLACK CLUB SUIT
    (0x2665, "heartsuit"),                  # BLACK HEART SUIT
    (0x2666, "diamondsuit"),                # BLACK DIAMOND SUIT
    (0x27C2, "bot"),                        # PERPENDICULAR
    (0x27E8, "langle"),                     # MATHEMATICAL LEFT ANGLE BRACKET
    (0x27E9, "rangle"),                     # MATHEMATICAL RIGHT ANGLE BRACKET
    (0x27F5, "longleftarrow"),              # LONG LEFTWARDS ARROW
    (0x27F6, "longrightarrow"),             # LONG RIGHTWARDS ARROW
    (0x27F7, "longleftrightarrow"),         # LONG LEFT RIGHT ARROW
    (0x27F8, "Longleftarrow"),              # LONG LEFTWARDS DOUBLE ARROW
    (0x27F9, "Longrightarrow"),             # LONG RIGHTWARDS DOUBLE ARROW
    (0x27FA, "Longleftrightarrow"),         # LONG LEFT RIGHT DOUBLE ARROW
    (0x27FC, "longmapsto"),                 # LONG RIGHTWARDS ARROW FROM BAR
    (0x2A00, "bigodot"),                    # N-ARY CIRCLED DOT OPERATOR
    (0x2A01, "bigoplus"),                   # N-ARY CIRCLED PLUS OPERATOR
    (0x2A02, "bigotimes"),                  # N-ARY CIRCLED TIMES OPERATOR
    (0x2A04, "biguplus"),                   # N-ARY UNION OPERATOR WITH PLUS
    (0x2A06, "bigsqcup"),                   # N-ARY SQUARE UNION OPERATOR
    (0x2A2F, "times"),                      # VECTOR OR CROSS PRODUCT
    (0x2A7D, "leq"),                        # LESS-THAN OR SLANTED EQUAL TO
    (0x2A7E, "geq"),                        # GREATER-THAN OR SLANTED EQUAL TO
    (0x2AAF, "preceq"),                     # PRECEDES ABOVE SINGLE-LINE EQUALS SIGN
    (0x2AB0, "succeq"),                     # SUCCEEDS ABOVE SINGLE-LINE EQUALS SIGN
    (0x2C7C, ("sub", "j")),                 # LATIN SUBSCRIPT SMALL LETTER J
    (0x3008, "langle"),                     # LEFT ANGLE BRACKET
    (0x3009, "rangle"),                     # RIGHT ANGLE BRACKET
    (0xFEFF, ("ignore",)),                  # ZERO WIDTH NO-BREAK SPACE
    (0x1D55C, ("bb", "k")),                 # MATHEMATICAL DOUBLE-STRUCK SMALL K
    (0x1D7D9, ("bb", "1")),                 # MATHEMATICAL DOUBLE-STRUCK DIGIT ONE
)
UNICODE: dict[int, object] = {}

# =================================================================================================
# 6. Constructed symbols: recipes for symbols that exist in neither Times nor Symbol.
#    Units 1/1000 em; origin = the symbol's origin on the baseline. Pieces, in drawing order:
#      G(font, glyph, dx, dy, s=, sx=, rot=)  glyph: s uniform scale, sx = 0.5 half width,
#                                             rot degrees counter-clockwise about the scaled
#                                             AFM bounding-box centre, then moved by (dx, dy)
#      R(x0, y0, x1, y1)                      filled rectangle
#      L(x0, y0, x1, y1, t)                   stroked segment, butt caps, thickness t
#      C(cx, cy, r, t)                        stroked circle, centre-line radius r, thickness t
#    The order of this list is the composite id.
# =================================================================================================

AXIS = 262          # math axis: centre of Symbol minus (260.5), equal (265.5), plus (266.5)

COMPOSITES: list[dict] = []


def comp(names: str, cls: str, advance: int, pieces: list, limits: str = "-", note: str = "") -> None:
    COMPOSITES.append({"names": names.split(), "cls": cls, "advance": advance, "pieces": pieces,
                       "limits": limits, "note": note})


def G(font: str, glyph: str, dx: int = 0, dy: int = 0, **opts: float) -> tuple:
    return ("G", font, glyph, dx, dy, opts)


def R(x0: int, y0: int, x1: int, y1: int) -> tuple:
    return ("R", x0, y0, x1, y1)


def L(x0: int, y0: int, x1: int, y1: int, t: int) -> tuple:
    return ("L", x0, y0, x1, y1, t)


def C(cx: int, cy: int, r: int, t: int) -> tuple:
    return ("C", cx, cy, r, t)


# ---- ordinary ----
comp("hbar hslash", "Ord", 500, [G("I", "h"), L(38, 548, 292, 612, 36)],
     note="italic h with a rising stroke through the ascender")
comp("top intercal", "Ord", 658, [G("S", "perpendicular", rot=180)], note="Symbol perpendicular rotated 180")
comp("mho", "Ord", 768, [G("S", "Omega", rot=180)], note="Symbol Omega rotated 180")
comp("square Box", "Ord", 640, [R(70, 0, 570, 45), R(70, 455, 570, 500), R(70, 0, 115, 500),
                               R(525, 0, 570, 500)], note="500-unit square outline, stroke 45")
comp("blacksquare", "Ord", 640, [R(70, 0, 570, 500)], note="filled 500-unit square")
comp("vdots", "Ord", 278, [G("R", "period", 14, 0), G("R", "period", 14, 290),
                           G("R", "period", 14, 580)], note="three Times periods stacked, pitch 290")

# ---- inner (dots) ----
comp("cdots dotsb dotsm dotsi", "Inner", 1000, [G("R", "ellipsis", 0, 217)],
     note="Times ellipsis raised so the dots sit on the axis")
comp("ddots", "Inner", 950, [G("R", "period", 30, 580), G("R", "period", 350, 290),
                             G("R", "period", 670, 0)], note="three Times periods on a falling diagonal")

# ---- binary operators ----
comp("mp", "Bin", 549, [G("S", "plusminus", rot=180)], note="Symbol plusminus rotated 180")
comp("circ", "Bin", 500, [C(250, AXIS, 160, 40)], note="ring on the axis")
comp("bigcirc", "Bin", 1000, [C(500, AXIS, 440, 45)], note="large ring on the axis")
comp("odot", "Bin", 768, [C(388, 330, 320, 50), G("S", "dotmath", 269, 70)],
     note="ring matching Symbol circleplus + centred dotmath")
comp("ominus", "Bin", 768, [C(388, 330, 320, 50), R(68, 305, 708, 355)],
     note="ring matching Symbol circleplus + bar")
comp("oslash", "Bin", 768, [C(388, 330, 320, 50), L(162, 104, 614, 556, 50)],
     note="ring matching Symbol circleplus + diagonal")
comp("diamond", "Bin", 420, [G("S", "lozenge", 60, 31, s=0.62)], note="Symbol lozenge at 62%")
comp("uplus", "Bin", 768, [G("S", "union"), G("S", "plus", 235, 103, s=0.55)],
     note="Symbol union with a 55% plus inside")
comp("sqcup", "Bin", 630, [R(70, 0, 120, 530), R(70, 0, 560, 50), R(510, 0, 560, 530)])
comp("sqcap", "Bin", 630, [R(70, 0, 120, 530), R(70, 480, 560, 530), R(510, 0, 560, 530)])

# ---- relations ----
comp("ll", "Rel", 779, [G("S", "less"), G("S", "less", 230, 0)], note="two Symbol less signs, offset 230")
comp("gg", "Rel", 779, [G("S", "greater"), G("S", "greater", 230, 0)],
     note="two Symbol greater signs, offset 230")
comp("simeq", "Rel", 549, [G("S", "similar", 0, 105), R(11, 141, 537, 196)],
     note="Symbol similar raised 105 over one bar at the lower-equal position")
comp("doteq", "Rel", 549, [G("S", "equal"), G("S", "dotmath", 155, 240)], note="equal with a dot above")
comp("triangleq", "Rel", 549, [G("S", "equal"), G("S", "Delta", 136, 440, s=0.45)],
     note="equal with a 45% Symbol Delta above")
comp("lesssim", "Rel", 549, [G("S", "less", 0, 90), G("S", "similar", 0, -240)])
comp("gtrsim", "Rel", 549, [G("S", "greater", 0, 90), G("S", "similar", 0, -240)])
comp("coloneqq", "Rel", 799, [G("R", "colon", 0, 41), G("S", "equal", 250, 0)],
     note="Times colon centred on the axis + Symbol equal")
comp("parallel", "Rel", 380, [G("R", "bar"), G("R", "bar", 180, 0)], note="two Times bars, offset 180")
comp("subsetneq", "Rel", 713, [G("S", "reflexsubset"), L(330, -215, 410, -10, 45)])
comp("supsetneq", "Rel", 713, [G("S", "reflexsuperset"), L(300, -215, 380, -10, 45)])
comp("sqsubset", "Rel", 630, [R(70, 415, 560, 465), R(70, 60, 120, 465), R(70, 60, 560, 110)])
comp("sqsupset", "Rel", 630, [R(70, 415, 560, 465), R(510, 60, 560, 465), R(70, 60, 560, 110)])
comp("sqsubseteq", "Rel", 630, [R(70, 485, 560, 535), R(70, 130, 120, 535), R(70, 130, 560, 180),
                                R(70, -10, 560, 40)])
comp("sqsupseteq", "Rel", 630, [R(70, 485, 560, 535), R(510, 130, 560, 535), R(70, 130, 560, 180),
                                R(70, -10, 560, 40)])
comp("vdash", "Rel", 620, [R(60, 2, 115, 522), R(115, 234, 560, 289)])
comp("dashv", "Rel", 620, [R(505, 2, 560, 522), R(60, 234, 505, 289)])
comp("models vDash", "Rel", 620, [R(60, 2, 115, 522), R(115, 141, 560, 196), R(115, 335, 560, 390)])
comp("bowtie Join", "Rel", 630, [L(85, 40, 545, 484, 45), L(85, 484, 545, 40, 45),
                                 L(85, 18, 85, 506, 45), L(545, 18, 545, 506, 45)])
comp("because", "Rel", 863, [G("S", "therefore", rot=180)], note="Symbol therefore rotated 180")

# ---- arrows (all Rel) ----
comp("mapsto", "Rel", 987, [R(49, 113, 105, 383), G("S", "arrowright")],
     note="Symbol arrowright with a tail bar; shaft geometry from the arrowhorizex bbox")
comp("longrightarrow", "Rel", 1437, [G("S", "arrowhorizex", 79, 0, sx=0.5), G("S", "arrowright", 450, 0)],
     note="arrowhorizex at 50% width as shaft extension + arrowright")
comp("longleftarrow", "Rel", 1437, [G("S", "arrowleft"), G("S", "arrowhorizex", 867, 0, sx=0.5)])
comp("longleftrightarrow", "Rel", 1587, [G("S", "arrowleft"), G("S", "arrowright", 600, 0)],
     note="arrowleft and arrowright with overlapping shafts")
comp("longmapsto", "Rel", 1437, [R(49, 113, 105, 383), G("S", "arrowhorizex", 79, 0, sx=0.5),
                                 G("S", "arrowright", 450, 0)])
comp("updownarrow", "Rel", 603, [G("S", "arrowboth", -216, 207, rot=90)], note="Symbol arrowboth rotated 90")
comp("Updownarrow", "Rel", 603, [G("S", "arrowdblboth", -222, 210, rot=90)],
     note="Symbol arrowdblboth rotated 90")
comp("nearrow", "Rel", 860, [G("S", "arrowright", 27, 64, s=0.8, rot=45)])
comp("searrow", "Rel", 860, [G("S", "arrowright", 27, 64, s=0.8, rot=-45)])
comp("nwarrow", "Rel", 860, [G("S", "arrowright", 27, 64, s=0.8, rot=135)])
comp("swarrow", "Rel", 860, [G("S", "arrowright", 27, 64, s=0.8, rot=-135)])
comp("rightleftharpoons leftrightharpoons rightleftarrows leftrightarrows", "Rel", 740,
     [G("S", "arrowright", 0, 170, s=0.75), G("S", "arrowleft", 0, -40, s=0.75)],
     note="75% arrowright over 75% arrowleft (full arrow heads, not harpoons)")

# ---- big operators ----
comp("coprod", "Op", 823, [G("S", "product", rot=180)], limits="D", note="Symbol product rotated 180")
comp("oint", "Op", 274, [G("S", "integral"), C(146, 404, 125, 42)], limits="N",
     note="Symbol integral + ring centred on its bbox centre")
comp("iint", "Op", 484, [G("S", "integral"), G("S", "integral", 210, 0)], limits="N")
comp("iiint", "Op", 694, [G("S", "integral"), G("S", "integral", 210, 0), G("S", "integral", 420, 0)],
     limits="N")
comp("bigodot", "Op", 768, [C(388, 330, 320, 50), G("S", "dotmath", 269, 70)], limits="D")
comp("biguplus", "Op", 768, [G("S", "union"), G("S", "plus", 235, 103, s=0.55)], limits="D")
comp("bigsqcup", "Op", 630, [R(70, 0, 120, 530), R(70, 0, 560, 50), R(510, 0, 560, 530)], limits="D")

# The only glyph pieces whose offsets are not whole units; they are rounded to the nearest unit.
ROUNDED_OFFSETS = ("nearrow", "searrow", "nwarrow", "swarrow")
MAX_PIECES_PER_COMPOSITE = 4
MAX_PIECES = 255


# =================================================================================================
# 7. AFM files
# =================================================================================================

AFM_CHAR = re.compile(
    r"C\s+(-?\d+)\s*;\s*WX\s+(-?\d+)\s*;\s*N\s+(\S+)\s*;\s*B\s+(-?\d+)\s+(-?\d+)\s+(-?\d+)\s+(-?\d+)\s*;")
AFM_HEADER_KEYS = ("FontName", "FontBBox", "StdVW", "EncodingScheme")


class Font:
    """One AFM file: encoded glyphs by code and all glyphs by name."""

    def __init__(self, short: str, blob: bytes) -> None:
        self.short = short
        self.base_name = FONT_NAMES[short]
        self.file_name = self.base_name + ".afm"
        self.by_name: dict[str, tuple[int, int, tuple[int, int, int, int]]] = {}
        self.by_code: dict[int, str] = {}
        self.header: dict[str, str] = {}
        self.notices: list[str] = []
        self.problems: list[str] = []
        declared = -1
        parsed = 0
        for line in blob.decode("latin-1").splitlines():
            match = AFM_CHAR.match(line)
            if match:
                parsed += 1
                code, advance, name = int(match.group(1)), int(match.group(2)), match.group(3)
                bbox = (int(match.group(4)), int(match.group(5)), int(match.group(6)), int(match.group(7)))
                if name in self.by_name:
                    self.problems.append("glyph %r is listed twice" % name)
                self.by_name[name] = (code, advance, bbox)
                if code >= 0:
                    if code in self.by_code:
                        self.problems.append("code %d is used twice" % code)
                    self.by_code[code] = name
                continue
            key, _, value = line.partition(" ")
            if key == "Notice":
                self.notices.append(line.rstrip())
            elif key == "StartCharMetrics":
                declared = int(value.split()[0])
            elif key in AFM_HEADER_KEYS:
                self.header[key] = value.strip()
        if declared != parsed:
            self.problems.append("%d character lines parsed, StartCharMetrics says %d" % (parsed, declared))
        if self.header.get("FontName") != self.base_name:
            self.problems.append("FontName is %r" % self.header.get("FontName"))
        if len(self.notices) != 1:
            self.problems.append("%d Notice lines, expected 1" % len(self.notices))

    def glyph(self, name: str) -> tuple[int, int, tuple[int, int, int, int]]:
        """(code, advance, bbox) of an encoded glyph; KeyError when missing or unencoded."""
        if name not in self.by_name:
            raise KeyError("%s has no glyph named %r" % (self.base_name, name))
        code, advance, bbox = self.by_name[name]
        if code < 0:
            raise KeyError("%s glyph %r exists but is not encoded (C -1)" % (self.base_name, name))
        return code, advance, bbox


def locate_afm_dir(option: str | None) -> Path:
    if option:
        return Path(option)
    package_dirs: list[str] = []
    try:
        spec = importlib.util.find_spec("matplotlib")      # located, never imported
        if spec is not None and spec.submodule_search_locations:
            package_dirs = list(spec.submodule_search_locations)
    except (ImportError, ValueError):
        package_dirs = []
    for package_dir in package_dirs:
        candidate = Path(package_dir) / "mpl-data" / "fonts" / "pdfcorefonts"
        if candidate.is_dir():
            return candidate
    raise Failure(
        "the AFM input files were not found: pass --afm-dir <dir>, or install the Python package "
        "matplotlib, whose mpl-data/fonts/pdfcorefonts directory holds them (pinned to the files of "
        "Matplotlib 3.10.8)")


def read_inputs(afm_dir: Path) -> dict[str, bytes]:
    """Reads the nine pinned files; refuses any other bytes before anything else happens."""
    blobs: dict[str, bytes] = {}
    problems: list[str] = []
    for name, size, digest in PINNED_INPUTS:
        path = afm_dir / name
        try:
            blob = path.read_bytes()
        except OSError as error:
            problems.append("%s: cannot be read (%s)" % (name, error.strerror or error))
            continue
        actual = hashlib.sha256(blob).hexdigest()
        if actual != digest:
            problems.append("%s: SHA-256 %s (%d bytes) is not the pinned %s (%d bytes)"
                            % (name, actual, len(blob), digest, size))
        blobs[name] = blob
    if problems:
        raise Failure("input files rejected in %s:\n  - %s\nNothing was written."
                      % (afm_dir, "\n  - ".join(problems)))
    return blobs


def licence_paragraph(readme: bytes) -> list[str]:
    """The last paragraph of readme.txt, line by line, without trailing blanks."""
    paragraphs = [block for block in readme.decode("ascii").replace("\r\n", "\n").split("\n\n") if block.strip()]
    lines = [line.rstrip() for line in paragraphs[-1].strip("\n").split("\n")]
    if not lines[0].startswith(LICENCE_FIRST_WORDS) or not lines[-1].endswith(LICENCE_LAST_WORDS):
        raise Failure("readme.txt: the licence paragraph was not found where it is expected")
    return lines


# =================================================================================================
# 8. Resolution and verification
# =================================================================================================

def hex2(value: int) -> str:
    return "0x%02X" % value


def round_unit(value: float, what: str, errors: list[str]) -> int:
    """Nearest integer, halves to even (Python's round); refuses a value too close to call."""
    doubled = value * 2.0
    if abs(doubled - round(doubled)) > 1e-9 and abs(abs(value - math.floor(value)) - 0.5) < 1e-6:
        errors.append("%s: %r is too close to a half to round reproducibly" % (what, value))
    return int(round(value))


class Tables:
    """Everything both files are rendered from."""

    def __init__(self, fonts: dict[str, Font]) -> None:
        self.fonts = fonts
        self.errors: list[str] = []
        self.metrics: list[list[tuple[int, int, int, int, int]]] = []
        self.symbol_widths: list[int] = []
        self.text_widths: dict[str, list[int]] = {}
        self.accent_codes: list[int] = []
        self.composites: list[dict] = []
        self.pieces: list[dict] = []
        self.composite_id: dict[str, int] = {}
        self.symbol_info: dict[str, dict] = {}
        self.commands: list[dict] = []
        self.command_index: dict[str, int] = {}
        self.command_blob = 0
        self.unicode: list[dict] = []
        self.envs: list[dict] = []
        self.env_blob = 0
        self.ascii: list[dict] = []
        self.variants: list[dict] = []
        self.delim_words: list[dict] = []
        self.delim_base: list[dict] = []

    def err(self, message: str) -> None:
        self.errors.append(message)

    def resolve(self, font: str, glyph: str, where: str) -> tuple[int, int, tuple[int, int, int, int]]:
        """(code, advance, bbox) of a glyph named in the data; an error and a blank when it is not encoded."""
        if font not in self.fonts:
            self.err("%s: unknown font %r" % (where, font))
            return (0x20, 0, (0, 0, 0, 0))
        try:
            return self.fonts[font].glyph(glyph)
        except KeyError as problem:
            self.err("%s: %s" % (where, problem.args[0]))
            return (0x20, 0, (0, 0, 0, 0))

    def fit(self, value: int, low: int, high: int, what: str) -> int:
        if not low <= value <= high:
            self.err("%s: %d does not fit %d..%d" % (what, value, low, high))
            return 0
        return value

    # ---------------------------------------------------------------------------------- fonts
    def check_fonts(self) -> None:
        fonts = self.fonts
        for font in fonts.values():
            for problem in font.problems:
                self.err("%s: %s" % (font.file_name, problem))
        for short in MATH_FONTS:
            font = fonts[short]
            for code, name in sorted(font.by_code.items()):
                if not 0x20 <= code <= 0xFF:
                    self.err("%s: glyph %s has code %d outside 32..255" % (font.file_name, name, code))
                _, advance, bbox = font.by_name[name]
                for value in (advance,) + bbox:
                    self.fit(value, -32768, 32767, "%s glyph %s" % (font.file_name, name))
        # The four Times faces share one encoding, so a code is valid in all of them.
        reference = fonts["R"].by_code
        for short in TIMES[1:]:
            if fonts[short].by_code != reference:
                differing = sorted(set(reference.items()) ^ set(fonts[short].by_code.items()))
                self.err("%s: the code -> glyph-name map differs from Times-Roman (%s)"
                         % (fonts[short].file_name, differing[:4]))
        for short in TIMES:
            font = fonts[short]
            for code in range(ord("A"), ord("Z") + 1):
                for letter in (chr(code), chr(code).lower()):
                    if font.by_code.get(ord(letter)) != letter:
                        self.err("%s: letter %s is not at its ASCII code" % (font.file_name, letter))
            for digit, name in enumerate(DIGIT_GLYPHS):
                if font.by_code.get(ord("0") + digit) != name:
                    self.err("%s: digit %d is not at its ASCII code" % (font.file_name, digit))
            for code in list(range(0x20, 0x7F)) + [TEXT_ELLIPSIS_CODE]:
                if code not in font.by_code:
                    self.err("%s: text code 0x%02X is not encoded" % (font.file_name, code))
            if font.by_code.get(0x20) != "space" or font.by_name.get("space", (0, 0))[1] != KNOWN_SPACE_ADVANCE:
                self.err("%s: the space glyph is not code 32 with advance %d"
                         % (font.file_name, KNOWN_SPACE_ADVANCE))
            if font.by_code.get(TEXT_ELLIPSIS_CODE) != "ellipsis":
                self.err("%s: code 0x%02X is not the ellipsis" % (font.file_name, TEXT_ELLIPSIS_CODE))
            for char, glyph in TEXT_ESCAPES.items():
                code = self.resolve(short, glyph, "text character %r" % char)[0]
                if code != ord(char):
                    self.err("%s: glyph %s is at code %d, not at %d" % (font.file_name, glyph, code, ord(char)))
        # Accent glyphs of MathAccent 0..9.
        self.accent_codes = []
        for index, (command, glyph) in enumerate(ACCENTS):
            codes = {self.resolve(short, glyph, "accent \\%s" % command)[0] for short in TIMES}
            if codes != {KNOWN_ACCENT_CODES[index]}:
                self.err("accent \\%s: glyph %s has the codes %s, expected 0x%02X in all four Times faces"
                         % (command, glyph, sorted(codes), KNOWN_ACCENT_CODES[index]))
            self.accent_codes.append(min(codes))
        # Metric rows [font - 1][code - 0x20].
        self.metrics = []
        for short in MATH_FONTS:
            font = fonts[short]
            rows = []
            for code in range(0x20, 0x100):
                name = font.by_code.get(code)
                if name is None:
                    rows.append((0, 0, 0, 0, 0))
                else:
                    _, advance, bbox = font.by_name[name]
                    rows.append((advance,) + bbox)
            self.metrics.append(rows)
        for short, code, row in KNOWN_METRICS:
            actual = self.metrics[FONT_ID[short] - 1][code - 0x20]
            if actual != row:
                self.err("known value: %s code 0x%02X is %s, expected %s"
                         % (fonts[short].base_name, code, actual, row))
        # Symbol: the font object text of the layout hard-codes these.
        symbol = fonts["S"]
        if len(symbol.by_code) != KNOWN_SYMBOL_GLYPHS:
            self.err("Symbol: %d encoded glyphs, expected %d" % (len(symbol.by_code), KNOWN_SYMBOL_GLYPHS))
        outside = sorted(code for code in symbol.by_code if not 32 <= code <= 254)
        if outside:
            self.err("Symbol: encoded codes outside 32..254: %s" % outside)
        self.symbol_widths = [row[0] for row in self.metrics[FONT_ID["S"] - 1][:KNOWN_SYMBOL_WIDTH_COUNT]]
        if sum(self.symbol_widths) != KNOWN_SYMBOL_WIDTH_SUM:
            self.err("Symbol: the widths of codes 32..254 sum to %d, expected %d"
                     % (sum(self.symbol_widths), KNOWN_SYMBOL_WIDTH_SUM))
        if symbol.header.get("FontBBox") != KNOWN_SYMBOL_FONT_BBOX:
            self.err("Symbol: FontBBox is %r, expected %r" % (symbol.header.get("FontBBox"), KNOWN_SYMBOL_FONT_BBOX))
        if symbol.header.get("StdVW") != KNOWN_SYMBOL_STD_VW:
            self.err("Symbol: StdVW is %r, expected %r" % (symbol.header.get("StdVW"), KNOWN_SYMBOL_STD_VW))
        # Exact widths of the standard renderer's text fonts. Their font dictionaries declare
        # /WinAnsiEncoding, which names other glyphs than the fonts' built-in StandardEncoding
        # at two of the codes 32..126 (WIN_ANSI_TEXT_GLYPHS).
        self.text_widths = {}
        for short in ("H", "HB"):
            font = fonts[short]
            widths = []
            for code in range(32, 127):
                name = WIN_ANSI_TEXT_GLYPHS.get(code, font.by_code.get(code))
                if name is None or name not in font.by_name:
                    self.err("%s: code %d has no glyph" % (font.file_name, code))
                    widths.append(0)
                else:
                    widths.append(self.fit(font.by_name[name][1], 0, 65535, "%s code %d" % (font.file_name, code)))
            self.text_widths[short] = widths
            if sum(widths) != KNOWN_TEXT_WIDTH_SUMS[short]:
                self.err("known value: the widths of %s codes 32..126 sum to %d, expected %d"
                         % (font.base_name, sum(widths), KNOWN_TEXT_WIDTH_SUMS[short]))
        courier = fonts["C"]
        for code in range(32, 127):
            name = WIN_ANSI_TEXT_GLYPHS.get(code, courier.by_code.get(code))
            if name is None or name not in courier.by_name or courier.by_name[name][1] != KNOWN_COURIER_ADVANCE:
                self.err("Courier: code %d does not have advance %d" % (code, KNOWN_COURIER_ADVANCE))

    # ----------------------------------------------------------------------------- composites
    def glyph_piece(self, piece: tuple, where: str, may_round: bool) -> tuple[dict, tuple, tuple]:
        """A "G" recipe piece -> the emitted piece, the ink box of the recipe and the ink box as drawn.

        The two boxes differ only for the pieces of ROUNDED_OFFSETS, by less than one unit."""
        _, font, glyph, dx, dy, opts = piece
        unknown = set(opts) - {"s", "sx", "rot"}
        if unknown:
            self.err("%s: unknown piece option %s" % (where, sorted(unknown)))
        scale = opts.get("s", 1.0)
        half = opts.get("sx", 1.0)
        rotation = opts.get("rot", 0)
        code, _, (llx, lly, urx, ury) = self.resolve(font, glyph, where)
        if half != 1.0:
            if half != 0.5 or rotation != 0 or scale != 1.0:
                self.err("%s: sx is only available as 0.5, without s and rot" % where)
            transform = HALF_WIDTH_TRANSFORM
        elif rotation in ROTATION_TRANSFORM:
            transform = ROTATION_TRANSFORM[rotation]
        else:
            self.err("%s: rotation %r is not one of %s" % (where, rotation, sorted(ROTATION_TRANSFORM)))
            transform = 0
        a, b, c, d = (float(number) for number in TRANSFORMS[transform].split())
        if transform == HALF_WIDTH_TRANSFORM:
            ox, oy = float(dx), float(dy)
        else:       # rotate about the centre of the scaled bounding box, then move
            cx, cy = (llx + urx) / 2.0 * scale, (lly + ury) / 2.0 * scale
            ox = cx - (a * cx + c * cy) + dx
            oy = cy - (b * cx + d * cy) + dy
        whole = abs(ox - round(ox)) < 1e-9 and abs(oy - round(oy)) < 1e-9
        if not whole and not may_round:
            self.err("%s: the offset %.3f %.3f is not a whole number of units" % (where, ox, oy))
        if whole and may_round:
            self.err("%s: listed in ROUNDED_OFFSETS but its offset is whole" % where)
        ox_units = round_unit(ox, where, self.errors)
        oy_units = round_unit(oy, where, self.errors)
        size = scale * 1000.0
        if abs(size - round(size)) > 1e-9 or not 0 < size <= 32767:
            self.err("%s: the scale %r is not a positive whole number of thousandths" % (where, scale))
        size_units = int(round(size))
        xs, ys = [], []
        for x, y in ((llx, lly), (llx, ury), (urx, lly), (urx, ury)):
            xs.append(scale * (a * x + c * y))
            ys.append(scale * (b * x + d * y))
        recipe_box = (min(xs) + ox, min(ys) + oy, max(xs) + ox, max(ys) + oy)
        drawn_box = (min(xs) + ox_units, min(ys) + oy_units, max(xs) + ox_units, max(ys) + oy_units)
        emitted = {"kind": "Glyph", "font": FONT_ID.get(font, 0), "code": code, "transform": transform,
                   "v": [size_units, ox_units, oy_units, 0, 0],
                   "note": "/M%d %s%s%s" % (FONT_ID.get(font, 0), glyph,
                                             ", size %d" % size_units if size_units != 1000 else "",
                                             ", transform %d" % transform if transform else "")}
        if font not in FONT_ID:
            self.err("%s: font %r is not a math font" % (where, font))
        return emitted, recipe_box, drawn_box

    def resolve_composites(self) -> None:
        self.composites, self.pieces, self.composite_id = [], [], {}
        for cid, entry in enumerate(COMPOSITES):
            first_name = entry["names"][0]
            where = "composite \\%s" % first_name
            boxes = []
            drawn_boxes = []
            first_piece = len(self.pieces)
            for piece in entry["pieces"]:
                kind = piece[0]
                drawn = None
                if kind == "G":
                    emitted, box, drawn = self.glyph_piece(piece, where, first_name in ROUNDED_OFFSETS)
                elif kind == "R":
                    _, x0, y0, x1, y1 = piece
                    if not (x0 < x1 and y0 < y1):
                        self.err("%s: rectangle %s has no positive size" % (where, piece[1:]))
                    emitted = {"kind": "Rect", "font": 0, "code": 0, "transform": 0,
                               "v": [x0, y0, x1, y1, 0], "note": "rectangle"}
                    box = (float(x0), float(y0), float(x1), float(y1))
                elif kind == "L":
                    _, x0, y0, x1, y1, thickness = piece
                    length = math.sqrt(float((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0)))
                    if length <= 0 or thickness <= 0:
                        self.err("%s: line %s has no length or no thickness" % (where, piece[1:]))
                        length = 1.0
                    nx = -(y1 - y0) / length * thickness / 2.0
                    ny = (x1 - x0) / length * thickness / 2.0
                    emitted = {"kind": "Line", "font": 0, "code": 0, "transform": 0,
                               "v": [x0, y0, x1, y1, thickness], "note": "line, thickness %d" % thickness}
                    box = (min(x0 + nx, x0 - nx, x1 + nx, x1 - nx), min(y0 + ny, y0 - ny, y1 + ny, y1 - ny),
                           max(x0 + nx, x0 - nx, x1 + nx, x1 - nx), max(y0 + ny, y0 - ny, y1 + ny, y1 - ny))
                elif kind == "C":
                    _, cx, cy, radius, thickness = piece
                    if radius <= 0 or thickness <= 0:
                        self.err("%s: ring %s has no radius or no thickness" % (where, piece[1:]))
                    emitted = {"kind": "Ring", "font": 0, "code": 0, "transform": 0,
                               "v": [cx, cy, radius, thickness, 0],
                               "note": "ring, radius %d, thickness %d" % (radius, thickness)}
                    outer = radius + thickness / 2.0
                    box = (cx - outer, cy - outer, cx + outer, cy + outer)
                else:
                    self.err("%s: unknown piece kind %r" % (where, kind))
                    continue
                for value in emitted["v"]:
                    if not isinstance(value, int):
                        self.err("%s: piece value %r is not an integer" % (where, value))
                    else:
                        self.fit(value, -32768, 32767, "%s piece value" % where)
                emitted["owner"] = first_name
                self.pieces.append(emitted)
                boxes.append(box)
                drawn_boxes.append(drawn if drawn is not None else box)
            count = len(self.pieces) - first_piece
            if not 1 <= count <= MAX_PIECES_PER_COMPOSITE:
                self.err("%s: %d pieces, allowed 1..%d" % (where, count, MAX_PIECES_PER_COMPOSITE))
            if boxes:
                bbox = [round_unit(min(box[0] for box in boxes), where, self.errors),
                        round_unit(min(box[1] for box in boxes), where, self.errors),
                        round_unit(max(box[2] for box in boxes), where, self.errors),
                        round_unit(max(box[3] for box in boxes), where, self.errors)]
                # The box is that of the recipe; rounding an offset moves the ink by less than a unit.
                drawn = (min(box[0] for box in drawn_boxes), min(box[1] for box in drawn_boxes),
                         max(box[2] for box in drawn_boxes), max(box[3] for box in drawn_boxes))
                if any(abs(bound - edge) >= 1.0 for bound, edge in zip(bbox, drawn)):
                    self.err("%s: the ink box %s is a unit or more away from the drawn ink %s" % (where, bbox, drawn))
            else:
                bbox = [0, 0, 0, 0]
            for value in bbox + [entry["advance"]]:
                self.fit(value, -32768, 32767, "%s box" % where)
            if entry["cls"] not in CLASS_ID:
                self.err("%s: unknown class %r" % (where, entry["cls"]))
            self.composites.append({"id": cid, "names": entry["names"], "cls": entry["cls"],
                                    "limits": entry["limits"], "advance": entry["advance"], "bbox": bbox,
                                    "first": first_piece, "count": count})
            for name in entry["names"]:
                self.composite_id[name] = cid
        if len(self.pieces) > MAX_PIECES:
            self.err("composites: %d pieces in total, at most %d fit" % (len(self.pieces), MAX_PIECES))
        if len(self.composites) > 256:
            self.err("composites: %d entries do not fit an 8-bit id" % len(self.composites))

    # -------------------------------------------------------------------------- control words
    def bigop(self, entry: dict, where: str) -> int:
        """MathBigOp of a symbol or composite row; class Op, a limits letter and a group go together."""
        group_name = BIGOP_GROUP.get(entry["names"][0])
        is_op = entry["cls"] == "Op"
        if entry["limits"] not in ("-", "D", "N"):
            self.err("%s: limits %r is not -, D or N" % (where, entry["limits"]))
        if is_op != (entry["limits"] != "-") or is_op != (group_name is not None):
            self.err("%s: class Op, a limits letter and a BIGOP_GROUP entry must come together" % where)
        return BIGOP_ID[group_name] if group_name else 0

    def add(self, name: str, kind: str, a: int = 0, b: int = 0, c: int = 0, note: str = "") -> None:
        if not (name.isascii() and name.isalpha()):
            self.err("control word %r: a name must consist of ASCII letters" % name)
            return
        if kind not in CMD_KINDS:
            self.err("control word \\%s: unknown kind %r" % (name, kind))
            return
        for field, value in (("a", a), ("b", b), ("c", c)):
            self.fit(value, 0, 255, "control word \\%s field %s" % (name, field))
        for other in self.commands:
            if other["name"] == name:
                self.err("control word \\%s is defined twice: as %s and as %s" % (name, other["kind"], kind))
                return
        self.commands.append({"name": name, "kind": kind, "a": a, "b": b, "c": c, "note": note})

    def build_commands(self) -> None:
        self.commands, self.symbol_info = [], {}
        seen_symbol: set[str] = set()
        # --- glyphs ---
        for entry in SYMBOLS:
            first = entry["names"][0]
            where = "symbol \\%s" % first
            code, _, _ = self.resolve(entry["font"], entry["glyph"], where)
            if entry["font"] not in FONT_ID or entry["cls"] not in CLASS_ID:
                self.err("%s: unknown font %r or class %r" % (where, entry["font"], entry["cls"]))
                continue
            unknown = set(entry["flags"]) - {"sl", "SL", "pad", "degr"}
            if unknown:
                self.err("%s: unknown flags %s" % (where, sorted(unknown)))
            greek = 0
            if entry["group"] == "greek-lower":
                greek = 1
            elif entry["group"] == "greek-upper":
                greek = 3 if "SL" in entry["flags"] else 2
            if ("sl" in entry["flags"]) != (greek == 1) or ("SL" in entry["flags"] and greek != 3):
                self.err("%s: the flags sl / SL do not match its group %s" % (where, entry["group"]))
            if greek and entry["font"] != "S":
                self.err("%s: Greek letters come from Symbol" % where)
            bigop = self.bigop(entry, where)
            b = FONT_ID[entry["font"]] | CLASS_ID[entry["cls"]] << 3 | greek << 6
            c = bigop | (0x08 if entry["limits"] == "D" else 0) | (0x10 if "pad" in entry["flags"] else 0)
            parts = ["/M%d %s %s" % (FONT_ID[entry["font"]], hex2(code), entry["glyph"]), entry["cls"]]
            if greek:
                parts.append(("", "lowercase Greek", "uppercase Greek", "uppercase Greek, slanted")[greek])
            if bigop:
                parts.append(BIGOP_ENUM[bigop])
            if entry["limits"] == "D":
                parts.append("limits")
            if "pad" in entry["flags"]:
                parts.append("padded")
            if "degr" in entry["flags"]:
                parts.append("approximation")
            for name in entry["names"]:
                if name in seen_symbol:
                    self.err("symbol \\%s is listed twice" % name)
                seen_symbol.add(name)
                self.symbol_info[name] = {"font": entry["font"], "code": code, "cls": entry["cls"],
                                          "limits": entry["limits"], "glyph": entry["glyph"]}
                if name.isascii() and name.isalpha():
                    self.add(name, "Glyph", code, b, c, ", ".join(parts))
                elif (entry["font"], code, entry["cls"], greek, c) != ("R", ord(name[:1]), "Ord", 0, 0) \
                        or len(name) != 1:
                    # \% \$ \& \# \_ are a switch in the parser: Times-Roman, the character's own code.
                    self.err("control symbol \\%s: must be a Times-Roman Ord glyph at its own code" % name)
        # --- composites ---
        for composite, entry in zip(self.composites, COMPOSITES):
            where = "composite \\%s" % entry["names"][0]
            if entry["cls"] not in CLASS_ID:
                continue
            bigop = self.bigop(entry, where)
            c = bigop | (0x08 if entry["limits"] == "D" else 0)
            parts = ["composite %d" % composite["id"], entry["cls"]]
            if bigop:
                parts.append(BIGOP_ENUM[bigop])
            if entry["limits"] == "D":
                parts.append("limits")
            for name in entry["names"]:
                self.add(name, "Composite", composite["id"], CLASS_ID[entry["cls"]] << 3, c, ", ".join(parts))
        for name in BIGOP_GROUP:
            if not any(name == entry["names"][0] for entry in SYMBOLS + COMPOSITES):
                self.err("BIGOP_GROUP: \\%s is not the first name of a symbol or composite row" % name)
        # --- negated relations (the base index is filled in after sorting) ---
        for name, base in NEGATED.items():
            self.add(name, "Negated", 0, 0, 0, "negated \\%s" % base)
        # --- delimiter words that are not also symbols ---
        tokens_seen: set[str] = set()
        for index, entry in enumerate(DELIMS):
            if index >= len(DELIM_ENUM) or entry["key"] != DELIM_ENUM[index][0]:
                self.err("delimiter %d (%s): the order must be that of enum MathDelimiter" % (index, entry["key"]))
                continue
            for token in entry["tokens"]:
                if token in tokens_seen:
                    self.err("delimiter token %s is listed twice" % token)
                tokens_seen.add(token)
                if token not in DELIM_BARE_CLASS:
                    self.err("delimiter token %s has no bare class" % token)
                    continue
                word = token[1:] if token.startswith("\\") and len(token) > 2 else None
                if word is None or word in self.symbol_info or word in self.composite_id:
                    continue
                self.add(word, "Delimiter", index, CLASS_ID[DELIM_BARE_CLASS[token]], 0,
                         "delimiter %s, bare class %s" % (DELIM_ENUM[index][1], DELIM_BARE_CLASS[token]))
        if len(DELIMS) != len(DELIM_ENUM):
            self.err("delimiters: %d rows, enum MathDelimiter has %d" % (len(DELIMS), len(DELIM_ENUM)))
        for token in DELIM_BARE_CLASS:
            if token not in tokens_seen:
                self.err("bare class for the unknown delimiter token %s" % token)
        # --- named operators ---
        for name in OPNAMES_NOLIMITS:
            self.add(name, "OpName", 0, 0, 0, "operator name")
        for name, text in OPNAMES_LIMITS.items():
            if text.replace("~", "") != name or text.count("~") > 1:
                self.err("operator \\%s: its text %r must be the name with at most one ~" % (name, text))
            split = text.index("~") if "~" in text else 0
            self.add(name, "OpName", 1, split, 0,
                     "operator name, limits" + (", thin space after %d letters" % split if split else ""))
        # --- spaces ---
        for names, width in SPACES:
            for name in names.split():
                if name.isascii() and name.isalpha():
                    packed = self.fit(width, -32768, 32767, "space \\%s" % name) & 0xFFFF
                    self.add(name, "Space", packed & 0xFF, packed >> 8, 0, "space %d" % width)
        # --- blackboard shorthands ---
        for name, letter in BB_ALIASES.items():
            if not (len(letter) == 1 and letter.isascii() and letter.isalpha()):
                self.err("blackboard shorthand \\%s: %r is not an ASCII letter" % (name, letter))
                continue
            self.add(name, "Blackboard", ord(letter), 0, 0, "blackboard %s" % letter)
        # --- accents and decorations ---
        for index, name in enumerate([command for command, _ in ACCENTS] + ACCENTS_DRAWN):
            self.add(name, "Accent", index, 0, 0, ACCENT_ENUM[index] if index < len(ACCENT_ENUM) else "?")
        if len(ACCENTS) + len(ACCENTS_DRAWN) != len(ACCENT_ENUM):
            self.err("accents: %d commands, enum MathAccent has %d"
                     % (len(ACCENTS) + len(ACCENTS_DRAWN), len(ACCENT_ENUM)))
        for name, index in OVERUNDER.items():
            if not 0 <= index < len(OVERUNDER_ENUM):
                self.err("decoration \\%s: %d is not a MathOverUnder value" % (name, index))
                continue
            self.add(name, "OverUnder", index, 0, 0, OVERUNDER_ENUM[index])
        # --- fonts and text ---
        for name, variant in MATH_FONT_CMDS.items():
            if variant not in VARIANT_ID:
                self.err("font command \\%s: unknown variant %r" % (name, variant))
                continue
            self.add(name, "MathFont", VARIANT_ID[variant], 0, 0, "variant %s" % variant)
        for name, variant in MATH_FONT_DECLS.items():
            if variant not in VARIANT_ID:
                self.err("font declaration \\%s: unknown variant %r" % (name, variant))
                continue
            set_bits, clear_bits = TEXT_DECLS.get(name, (0, 0))
            self.add(name, "FontDecl", VARIANT_ID[variant], set_bits, clear_bits,
                     "variant %s; in text: set %d, clear %d" % (variant, set_bits, clear_bits))
        for name, (set_bits, clear_bits) in TEXT_CMDS.items():
            self.add(name, "Text", set_bits, clear_bits, 1 if name in TEXT_BOXED else 0,
                     "text, set %d, clear %d%s" % (set_bits, clear_bits, ", boxed" if name in TEXT_BOXED else ""))
        for name in TEXT_BOXED:
            if name not in TEXT_CMDS:
                self.err("TEXT_BOXED: \\%s is not a text command" % name)
        for name, (set_bits, clear_bits) in TEXT_DECLS.items():
            if name not in MATH_FONT_DECLS:
                self.add(name, "TextDecl", set_bits, clear_bits, 0,
                         "text declaration, set %d, clear %d" % (set_bits, clear_bits))
        for name, bits in list(TEXT_CMDS.items()) + list(TEXT_DECLS.items()):
            if not (0 <= bits[0] <= 3 and 0 <= bits[1] <= 3):
                self.err("text command \\%s: bits %s outside 0..3" % (name, bits))
        # --- paired delimiters, sized delimiters ---
        for name, (left, right) in PAIRED.items():
            if left not in DELIM_ID or right not in DELIM_ID:
                self.err("paired macro \\%s: unknown delimiter %s or %s" % (name, left, right))
                continue
            self.add(name, "Paired", DELIM_ID[left], DELIM_ID[right], 0,
                     "%s ... %s" % (DELIM_ENUM[DELIM_ID[left]][1], DELIM_ENUM[DELIM_ID[right]][1]))
        for level, base in enumerate(SIZED_BASES, 1):
            for suffix, cls in SIZED_SUFFIXES:
                self.add(base + suffix, "Sized", level, CLASS_ID[cls], 0, "level %d, %s" % (level, cls))
        # --- structure ---
        for names, kind, a, b, c in STRUCT:
            for name in names.split():
                self.add(name, kind, a, b, c, "")
        # --- sort, index, cross references ---
        self.commands.sort(key=lambda command: command["name"].encode("ascii"))
        names = [command["name"] for command in self.commands]
        for before, after in zip(names, names[1:]):
            if not before.encode("ascii") < after.encode("ascii"):
                self.err("control words: %s and %s are not in strictly increasing order" % (before, after))
        offset = 0
        self.command_index = {}
        for index, command in enumerate(self.commands):
            command["offset"] = self.fit(offset, 0, 65535, "name offset of \\%s" % command["name"])
            self.command_index[command["name"]] = index
            offset += len(command["name"]) + 1
        self.command_blob = offset
        if len(self.commands) > 65535:
            self.err("control words: %d entries do not fit a 16-bit index" % len(self.commands))
        for command in self.commands:
            if command["kind"] != "Negated":
                continue
            base = NEGATED[command["name"]]
            index = self.command_index.get(base)
            if index is None:
                self.err("negated \\%s refers to the unknown \\%s" % (command["name"], base))
                continue
            if self.commands[index]["kind"] not in ("Glyph", "Composite"):
                self.err("negated \\%s: its base \\%s is not a glyph or composite" % (command["name"], base))
            command["a"], command["b"] = index & 0xFF, index >> 8
            command["note"] = "negated \\%s (command %d)" % (base, index)
        used = {command["kind"] for command in self.commands}
        for kind in CMD_KINDS:
            if kind not in used:
                self.err("command kind kCmd%s has no control word" % kind)

    # ----------------------------------------------------------------------------- delimiters
    def build_delims(self) -> None:
        self.delim_base, self.delim_words = [], []
        for index, entry in enumerate(DELIMS):
            base = entry["base"]
            where = "delimiter %s" % entry["key"]
            if base is None:
                row = {"base": "None", "font": 0, "code": 0, "note": "no delimiter"}
            elif base[0] == "G":
                code, _, _ = self.resolve(base[1], base[2], where)
                row = {"base": "Glyph", "font": FONT_ID.get(base[1], 0), "code": code,
                       "note": "/M%d %s" % (FONT_ID.get(base[1], 0), base[2])}
            elif base[0] == "V":
                row = {"base": "Vector", "font": 0, "code": 0, "note": "vector shape at every size"}
            elif base[0] == "C":
                if base[1] not in self.composite_id:
                    self.err("%s: its base composite \\%s does not exist" % (where, base[1]))
                    row = {"base": "Composite", "font": 0, "code": 0, "note": "?"}
                else:
                    row = {"base": "Composite", "font": 0, "code": self.composite_id[base[1]],
                           "note": "composite %d" % self.composite_id[base[1]]}
            else:
                self.err("%s: unknown base %r" % (where, base))
                continue
            row["enum"] = DELIM_ENUM[index][1] if index < len(DELIM_ENUM) else "?"
            self.delim_base.append(row)
            for token in entry["tokens"]:
                bare = DELIM_BARE_CLASS.get(token)
                if token.startswith("\\") and len(token) > 2:
                    word = token[1:]
                    command = self.command_index.get(word)
                    if command is None:
                        self.err("%s: the delimiter word \\%s is not a control word" % (where, word))
                        continue
                    kind = self.commands[command]["kind"]
                    if kind not in ("Delimiter", "Glyph", "Composite"):
                        self.err("%s: \\%s is a %s command, not a delimiter or symbol" % (where, word, kind))
                    # A symbol used bare is typeset by its own row, which must agree with the delimiter.
                    if word in self.symbol_info:
                        info = self.symbol_info[word]
                        if base is None or base[0] != "G" or (base[1], base[2], bare) != \
                                (info["font"], info["glyph"], info["cls"]):
                            self.err("%s: the symbol \\%s and the delimiter base disagree" % (where, word))
                    if word in self.composite_id:
                        composite = self.composites[self.composite_id[word]]
                        if base is None or base[0] != "C" or self.composite_id.get(base[1]) != composite["id"] \
                                or bare != composite["cls"]:
                            self.err("%s: the composite \\%s and the delimiter base disagree" % (where, word))
                    self.delim_words.append({"command": command, "delimiter": index, "word": word,
                                             "enum": row["enum"]})
                elif len(token) == 1 and token not in "<>.":
                    # ( ) [ ] | / typed bare are the ASCII rows; they must match the delimiter's glyph.
                    ascii_row = next((r for r in ASCII if r[0] == token), None)
                    if ascii_row is None or base is None or base[0] != "G" or \
                            (base[1], base[2], bare) != ascii_row[1:]:
                        self.err("%s: the character %s and the delimiter base disagree" % (where, token))
        self.delim_words.sort(key=lambda word: word["command"])

    # -------------------------------------------------------------------------------- Unicode
    def build_unicode(self) -> None:
        self.unicode = []
        table: dict[int, object] = {}
        previous = -1
        for cp, target in UNICODE_TABLE:
            if cp in table:
                self.err("Unicode U+%04X is listed twice" % cp)
            if cp <= previous:
                self.err("Unicode U+%04X is not in increasing order" % cp)
            previous = cp
            table[cp] = target
        UNICODE.clear()
        UNICODE.update(table)
        astral = {cp: target for cp, target in table.items() if cp > 0xFFFF}
        if astral != ASTRAL_UNICODE:
            self.err("Unicode: the code points above U+FFFF must be exactly %s (two tests in the parser)"
                     % sorted("U+%X" % cp for cp in ASTRAL_UNICODE))
        for cp in sorted(table):
            target = table[cp]
            where = "Unicode U+%04X" % cp
            if cp < 0x80:
                self.err("%s: ASCII characters belong to the ASCII table" % where)
            row = {"cp": cp, "value": 0, "arg": 0}
            if isinstance(target, str):
                index = self.command_index.get(target)
                if index is None:
                    self.err("%s refers to the unknown \\%s" % (where, target))
                    index = 0
                row.update(kind="Command", value=index, note="\\%s" % target)
            elif target[0] == "glyph":
                _, font, glyph, cls = target
                code, _, _ = self.resolve(font, glyph, where)
                if font not in FONT_ID or cls not in CLASS_ID:
                    self.err("%s: unknown font %r or class %r" % (where, font, cls))
                    continue
                row.update(kind="Glyph", value=code | FONT_ID[font] << 8, arg=CLASS_ID[cls],
                           note="/M%d %s %s, %s" % (FONT_ID[font], hex2(code), glyph, cls))
            elif target[0] == "minus":
                row.update(kind="Minus", note="the minus sign")
            elif target[0] == "bb":
                glyph = target[1] if target[1].isalpha() else DIGIT_GLYPHS[int(target[1])]
                code, _, _ = self.resolve("B", glyph, where)
                if code != ord(target[1]):
                    self.err("%s: Times-Bold %s is not at its ASCII code" % (where, glyph))
                row.update(kind="Blackboard", arg=ord(target[1]), note="blackboard %s" % target[1])
            elif target[0] == "space":
                row.update(kind="Space", value=self.fit(target[1], 0, 65535, where), note="space %d" % target[1])
            elif target[0] == "ignore":
                row.update(kind="Ignore", note="ignored")
            elif target[0] == "prime":
                row.update(kind="Prime", arg=self.fit(target[1], 1, 8, where), note="%d prime(s)" % target[1])
            elif target[0] in ("sup", "sub"):
                char = target[1]
                if not (len(char) == 1 and 0x20 < ord(char) < 0x7F):
                    self.err("%s: %r is not a printable ASCII character" % (where, char))
                    continue
                if not char.isalnum() and not any(r[0] == char for r in ASCII):
                    self.err("%s: its ASCII equivalent %r has no ASCII row" % (where, char))
                row.update(kind="Sup" if target[0] == "sup" else "Sub", arg=ord(char),
                           note="%s %s" % ("superscript" if target[0] == "sup" else "subscript", char))
            else:
                self.err("%s: unknown target %r" % (where, target))
                continue
            if cp <= 0xFFFF:
                self.unicode.append(row)

    # --------------------------------------------------------------------------- environments
    def build_envs(self) -> None:
        self.envs = []
        for names, left, right, columns, style, gap, family, args in ENVS:
            if left not in DELIM_ID or right not in DELIM_ID or columns not in ENV_COLUMNS \
                    or style not in CELL_STYLE or family not in ENV_FAMILY_BITS or args not in ENV_ARGS:
                self.err("environment %s: unknown delimiter, columns, style, family or arguments" % names)
                continue
            cls = "Ord" if left == "none" and right == "none" else "Inner"
            aux = CELL_STYLE[style] | ENV_FAMILY_BITS[family]
            for name in names.split():
                if any(env["name"] == name for env in self.envs):
                    self.err("environment %s is defined twice" % name)
                    continue
                if not name.isascii() or not name.replace("*", "").isalpha() or "*" in name[:-1]:
                    self.err("environment %r: a name is ASCII letters with an optional final *" % name)
                    continue
                if len(name) > MAX_ENV_NAME:
                    self.err("environment %s: the name is longer than %d bytes" % (name, MAX_ENV_NAME))
                self.envs.append({"name": name, "gap": self.fit(gap, 0, 65535, "environment %s gap" % name),
                                  "left": DELIM_ID[left], "right": DELIM_ID[right], "columns": columns,
                                  "aux": aux, "cls": CLASS_ID[cls], "args": args,
                                  "note": "%s ... %s, %s style, %s" % (left, right, style, cls)})
        self.envs.sort(key=lambda env: env["name"].encode("ascii"))
        offset = 0
        for env in self.envs:
            env["offset"] = offset
            offset += len(env["name"]) + 1
        self.env_blob = offset

    # ------------------------------------------------------------------------ ASCII, variants
    def build_ascii(self) -> None:
        self.ascii = []
        rows = {}
        for char, font, glyph, cls in ASCII:
            where = "ASCII %r" % char
            if char in rows or len(char) != 1:
                self.err("%s is listed twice or is not one character" % where)
                continue
            if char in ASCII_PARSER or char.isalnum() or not 0x20 < ord(char) < 0x7F:
                self.err("%s: this character is handled by the parser, not by the table" % where)
                continue
            code, _, _ = self.resolve(font, glyph, where)
            if font not in FONT_ID or cls not in CLASS_ID:
                self.err("%s: unknown font %r or class %r" % (where, font, cls))
                continue
            if font in TIMES and char not in ASCII_TIMES_REMAPPED and code != ord(char):
                self.err("%s: Times glyph %s is at code %d, not at %d" % (where, glyph, code, ord(char)))
            rows[char] = (FONT_ID[font] | CLASS_ID[cls] << 3, code,
                          "/M%d %s, %s" % (FONT_ID[font], glyph, cls))
        for code in range(0x20, 0x7F):
            char = chr(code)
            shown = "0x%02X" % code + ("" if char in " \\" else " '%s'" % char)
            if char in rows:
                font_class, glyph_code, note = rows[char]
                self.ascii.append({"fontClass": font_class, "code": glyph_code, "note": "%s  %s" % (shown, note)})
            elif char.isalpha():
                self.ascii.append({"fontClass": 0, "code": 0, "note": "%s  parser: letter (variant table)" % shown})
            elif char.isdigit():
                self.ascii.append({"fontClass": 0, "code": 0, "note": "%s  parser: digit (variant table)" % shown})
            elif char in ASCII_PARSER:
                self.ascii.append({"fontClass": 0, "code": 0, "note": "%s  parser: %s" % (shown, ASCII_PARSER[char])})
            else:
                self.err("ASCII 0x%02X has neither a table row nor a parser role" % code)
                self.ascii.append({"fontClass": 0, "code": 0, "note": shown})

    def build_variants(self) -> None:
        self.variants = []
        if tuple(VARIANTS) != VARIANT_NAMES:
            self.err("variants: the order must be that of enum MathVariantId")
        for name, (letters, digits, latin, upper, lower, letters_only) in VARIANTS.items():
            bits = []
            for flags in (latin, upper, lower):
                unknown = set(flags.split()) - set(GLYPH_BITS)
                if unknown:
                    self.err("variant %s: unknown flags %s" % (name, sorted(unknown)))
                bits.append(sum(GLYPH_BITS.get(flag, 0) for flag in flags.split()))
            if letters not in TIMES or digits not in TIMES:
                self.err("variant %s: letters and digits come from the Times faces" % name)
                continue
            self.variants.append({"name": name, "letterFont": FONT_ID[letters], "digitFont": FONT_ID[digits],
                                  "latin": bits[0], "upper": bits[1], "lower": bits[2],
                                  "lettersOnly": 1 if letters_only else 0})

    def build(self) -> None:
        self.check_fonts()
        self.resolve_composites()
        self.build_commands()
        self.build_delims()
        self.build_unicode()
        self.build_envs()
        self.build_ascii()
        self.build_variants()


# =================================================================================================
# 9. Rendering
# =================================================================================================

def header_block(fonts: dict[str, Font], licence: list[str]) -> list[str]:
    lines = [
        "// Generated by %s - do not edit." % SCRIPT_NAME,
        "// Regenerate: python %s [--afm-dir <dir>]" % SCRIPT_NAME,
        "//",
        "// The character codes, advance widths and bounding boxes in this file are derived from the",
        "// Adobe Core 14 AFM (Adobe Font Metrics) files listed below.",
        "//",
        "// Notice lines of the AFM files read:",
    ]
    for short in FONT_NAMES:
        for notice in fonts[short].notices:
            lines.append("//   %s: %s" % (fonts[short].file_name, notice))
    lines += ["//", "// From the %s that accompanies the AFM files:" % README_FILE]
    lines += ["//   " + line for line in licence]
    lines += ["//", "// Input files and their SHA-256:"]
    for name, _, digest in PINNED_INPUTS:
        lines.append("//   %s  %s" % (digest, name))
    lines += ["// They are " + INPUT_ORIGIN[0], "// " + INPUT_ORIGIN[1]]
    return lines


def pin_asserts(groups: list[tuple[str, list[tuple[str, int]]]]) -> list[str]:
    lines = []
    for message, pins in groups:
        terms = ["%s == %d" % (expression, value) for expression, value in pins]
        text = "static_assert("
        for index, term in enumerate(terms):
            piece = term + (" && " if index + 1 < len(terms) else ",")
            if len(text) + len(piece) > 108:
                lines.append(text.rstrip())
                text = "              "
            text += piece
        lines.append(text.rstrip())
        lines.append('              "%s");' % message)
    return lines


def wrap_items(items: list[str], indent: str = "    ", width: int = 108) -> list[str]:
    lines, text = [], indent
    for item in items:
        if len(text) + len(item) > width and text.strip():
            lines.append(text.rstrip())
            text = indent
        text += item + " "
    if text.strip():
        lines.append(text.rstrip())
    return lines


def name_blob(names: list[str]) -> list[str]:
    """NUL-terminated names; the terminator of the last one is the literal's own."""
    items = ['"%s\\0"' % name for name in names[:-1]] + ['"%s"' % names[-1]]
    lines = wrap_items(items)
    lines[-1] += ";"
    return lines


def delim_base_rows(tables: Tables) -> list[str]:
    lines = ["static constexpr MathDelimBaseEntry kMathDelimBase[kDelimCount] = {"]
    for index, row in enumerate(tables.delim_base):
        body = "{kDelimBase%s, %d, %s}," % (row["base"], row["font"], hex2(row["code"]))
        lines.append("    %-34s// %2d %s: %s" % (body, index, row["enum"], row["note"]))
    lines.append("};")
    return lines


def render_symbols(tables: Tables, header: list[str]) -> str:
    out = list(header)
    out += [
        "",
        "// Parser tables of the native math module. Included once, by math_parser.cpp, inside",
        "// namespace TinyPdf::Internal { namespace { ... } } and after math_parser.h.",
        "",
        "enum MathCmdKind : uint8_t {",
    ]
    out += wrap_items(["kCmd%s," % kind for kind in CMD_KINDS] + ["kCmdKindCount"])
    out += ["};", "enum MathVariantId : uint8_t {"]
    out += wrap_items(["kVariant%s," % name for name in VARIANT_NAMES] + ["kVariantCount"])
    out += ["};", "enum MathUnicodeKind : uint8_t {"]
    out += wrap_items([("kUni%s," if index + 1 < len(UNI_KINDS) else "kUni%s") % kind
                       for index, kind in enumerate(UNI_KINDS)])
    out += ["};"]
    out.append("enum MathEnvColumns : uint8_t { %s };" % ", ".join("kEnvCols" + name for name in ENV_COLUMNS))
    out.append("enum MathEnvArgs : uint8_t {")
    out += wrap_items([("kEnvArgs%s," if index + 1 < len(ENV_ARGS) else "kEnvArgs%s") % name
                       for index, name in enumerate(ENV_ARGS)])
    out.append("};")
    out.append("enum MathDelimBase : uint8_t { %s };" % ", ".join("kDelimBase" + name for name in DELIM_BASES))
    out += [
        "",
        "struct MathCommand { uint16_t name; uint8_t kind, a, b, c; };                         //  6 bytes",
        "struct MathUnicodeEntry { uint16_t cp; uint16_t value; uint8_t kind; uint8_t arg; };  //  6 bytes",
        "struct MathEnvironment { uint16_t name; uint16_t colGap;",
        "                         uint8_t left, right, columns, aux, cls, args; };             // 10 bytes",
        "struct MathAsciiEntry { uint8_t fontClass; uint8_t code; };                           //  2 bytes",
        "struct MathVariant { uint8_t letterFont, digitFont, latinFlags,",
        "                     upperGreekFlags, lowerGreekFlags, lettersOnly; };                //  6 bytes",
        "struct MathDelimWord { uint16_t command; uint16_t delimiter; };                       //  4 bytes",
        "struct MathDelimBaseEntry { uint8_t base, font, code; };                              //  3 bytes",
        "static_assert(sizeof(MathCommand) == 6 && sizeof(MathUnicodeEntry) == 6 && sizeof(MathEnvironment) == 10 &&",
        "              sizeof(MathAsciiEntry) == 2 && sizeof(MathVariant) == 6 && sizeof(MathDelimWord) == 4 &&",
        '              sizeof(MathDelimBaseEntry) == 3, "math_symbols.inc record sizes");',
        "",
        "// The tables below hold these values of math_parser.h as plain numbers.",
    ]
    out += pin_asserts([
        ("math_symbols.inc: MathClass values",
         [("static_cast<int>(MathClass::%s)" % name, index) for index, name in enumerate(CLASSES)]),
        ("math_symbols.inc: MathDelimiter values",
         [(enum, index) for index, (_, enum) in enumerate(DELIM_ENUM)] + [("kDelimCount", len(DELIM_ENUM))]),
        ("math_symbols.inc: MathAccent values",
         [(enum, index) for index, enum in enumerate(ACCENT_ENUM)] + [("kAccentCount", len(ACCENT_ENUM))]),
        ("math_symbols.inc: MathOverUnder values", [(enum, index) for index, enum in enumerate(OVERUNDER_ENUM)]),
        ("math_symbols.inc: MathEnclose and MathPhantom values",
         [(enum, index) for index, enum in enumerate(ENCLOSE_ENUM)]
         + [(enum, index) for index, enum in enumerate(PHANTOM_ENUM)]),
        ("math_symbols.inc: MathBigOp, MathLimitsMode and MathStackArrow values",
         [(enum, index) for index, enum in enumerate(BIGOP_ENUM)] + [("kBigOpCount", len(BIGOP_ENUM))]
         + [(enum, index) for index, enum in enumerate(LIMITS_ENUM)]
         + [(enum, index) for index, enum in enumerate(STACK_ARROW_ENUM)]),
        ("math_symbols.inc: flag bits",
         [(GLYPH_BIT_ENUM[name], bit) for name, bit in GLYPH_BITS.items()]
         + [("kFractionBar", FRACTION_BAR), ("kFractionDisplay", FRACTION_DISPLAY),
            ("kFractionText", FRACTION_TEXT), ("kArrayStyleMask", 3), ("kArrayCases", ARRAY_CASES),
            ("kArrayRowGap", ARRAY_ROW_GAP), ("kArrayAligned", ARRAY_ALIGNED),
            ("kMathMaxEnvName", MAX_ENV_NAME)]),
    ])
    # ---- control words ----
    out += [
        "",
        "// Control words (ASCII letters after the backslash), sorted in byte order; binary search.",
        "// name = offset into kMathCommandNames. Payload per kind:",
        "//   kCmdGlyph       a code; b font | class << 3 | greek << 6 (1 lowercase, 2 uppercase,",
        "//                   3 uppercase always slanted); c MathBigOp | 0x08 display limits | 0x10 kAtomPad",
        "//   kCmdComposite   a composite id; b class << 3; c as kCmdGlyph",
        "//   kCmdNegated     a, b index of the base command (low byte, high byte)",
        "//   kCmdDelimiter   a MathDelimiter; b bare class",
        "//   kCmdOpName      a 1 = display limits; b letters before a thin space (167), 0 = none",
        "//   kCmdSpace       a, b width as int16_t (low byte, high byte), 1/1000 em",
        "//   kCmdBlackboard  a ASCII code of the letter",
        "//   kCmdFraction    a Fraction flags; b 1 = ( ) delimiters (kCmdInfix likewise)",
        "//   kCmdAccent      a MathAccent",
        "//   kCmdOverUnder   a MathOverUnder",
        "//   kCmdMathFont    a MathVariantId",
        "//   kCmdFontDecl    a MathVariantId; b, c text set and clear bits (1 bold, 2 italic)",
        "//   kCmdText        a, b text set and clear bits; c 1 = boxed (fbox)",
        "//   kCmdTextDecl    a, b text set and clear bits",
        "//   kCmdTextOnly    a 0 ellipsis glyph 0xBC, 1 backslash glyph, 2 the name as text",
        "//   kCmdStack       a 0 overset, 1 underset, 2 stackrel",
        "//   kCmdXArrow      a MathStackArrow",
        "//   kCmdEnclose     a MathEnclose",
        "//   kCmdPaired      a, b left and right MathDelimiter",
        "//   kCmdSized       a level 1..4; b class",
        "//   kCmdStyle       a level 0..3",
        "//   kCmdClass       a class",
        "//   kCmdLimits      a MathLimitsMode",
        "//   kCmdPhantom     a MathPhantom",
        "//   kCmdMod         a 0 bmod, 1 pmod, 2 mod, 3 pod",
        "//   kCmdRef         a 1 = parentheses",
        "//   kCmdIgnore      a raw {...} arguments to drop; b 1 = an optional * first;",
        "//                   c 1 = an optional [...] first",
        "//   every other kind: a, b, c are 0",
        "// Control symbols are a switch in the parser and have no rows. For reference, verified by the",
        "// generator: %s are Times-Roman glyphs at the character's own code;" % " ".join(
            "\\" + name for entry in SYMBOLS for name in entry["names"] if not name.isalpha()),
    ]
    spaces = ["\\%s %d" % ("(space)" if name == "(space)" else name, width)
              for names, width in SPACES for name in names.split() if not name.isalpha()]
    out.append("// spaces in 1/1000 em: %s." % ", ".join(spaces))
    out.append("inline constexpr int kMathCommandCount = %d;" % len(tables.commands))
    out.append("static constexpr char kMathCommandNames[] =")
    out += name_blob([command["name"] for command in tables.commands])
    out.append("static constexpr MathCommand kMathCommands[kMathCommandCount] = {")
    for index, command in enumerate(tables.commands):
        body = "{%4d, kCmd%s, %s, %s, %s}," % (command["offset"], command["kind"], hex2(command["a"]),
                                              hex2(command["b"]), hex2(command["c"]))
        note = ": " + command["note"] if command["note"] else ""
        out.append("    %-46s// %3d \\%s%s" % (body, index, command["name"], note))
    out.append("};")
    # ---- Unicode ----
    out += [
        "",
        "// Non-ASCII characters typed in math mode, sorted by code point (BMP only); binary search.",
        "//   kUniCommand     value = index into kMathCommands",
        "//   kUniGlyph       value = code | font << 8; arg = class",
        "//   kUniBlackboard  arg = ASCII code of the letter",
        "//   kUniSpace       value = width, 1/1000 em",
        "//   kUniPrime       arg = number of primes",
        "//   kUniSup, kUniSub  arg = the ASCII equivalent; a run of them is one script",
        "//   kUniMinus, kUniIgnore  no payload",
        "// Outside the BMP the parser itself maps %s." % " and ".join(
            "U+%X to blackboard %s" % (cp, ASTRAL_UNICODE[cp][1]) for cp in sorted(ASTRAL_UNICODE, reverse=True)),
        "static constexpr MathUnicodeEntry kMathUnicode[] = {",
    ]
    for row in tables.unicode:
        value = "0x%04X" % row["value"] if row["kind"] == "Glyph" else "%d" % row["value"]
        arg = hex2(row["arg"]) if row["kind"] in ("Blackboard", "Sup", "Sub") else "%d" % row["arg"]
        body = "{0x%04X, %s, kUni%s, %s}," % (row["cp"], value, row["kind"], arg)
        out.append("    %-40s// %s" % (body, row["note"]))
    out.append("};")
    # ---- environments ----
    out += [
        "",
        "// Environments, sorted by name in byte order. name = offset into kMathEnvNames; colGap in",
        "// 1/1000 em (for kEnvColsPairs: the gap between column pairs); left, right = MathDelimiter;",
        "// aux = the value of Array::aux; cls = MathClass.",
        "static constexpr char kMathEnvNames[] =",
    ]
    out += name_blob([env["name"] for env in tables.envs])
    out.append("static constexpr MathEnvironment kMathEnvironments[] = {")
    for env in tables.envs:
        body = "{%3d, %4d, %2d, %2d, kEnvCols%s, %s, %d, kEnvArgs%s}," % (
            env["offset"], env["gap"], env["left"], env["right"], env["columns"], hex2(env["aux"]),
            env["cls"], env["args"])
        out.append("    %-66s// %s: %s" % (body, env["name"], env["note"]))
    out.append("};")
    # ---- ASCII ----
    out += [
        "",
        "// ASCII characters typed in math mode; index = code - 0x20. fontClass = font | class << 3;",
        "// fontClass 0 marks a character the parser handles itself.",
        "static constexpr MathAsciiEntry kMathAscii[95] = {",
    ]
    for row in tables.ascii:
        body = "{%s, %s}," % (hex2(row["fontClass"]), hex2(row["code"]))
        out.append("    %-16s// %s" % (body, row["note"]))
    out.append("};")
    # ---- variants ----
    out += [
        "",
        "// Font variants: fonts of Latin letters and of digits, glyph bits of Latin letters and digits,",
        "// of uppercase Greek and of lowercase Greek; lettersOnly = the Latin bits apply to letters only.",
        "static constexpr MathVariant kMathVariants[kVariantCount] = {",
    ]
    for variant in tables.variants:
        body = "{%d, %d, %s, %s, %s, %d}," % (variant["letterFont"], variant["digitFont"], hex2(variant["latin"]),
                                             hex2(variant["upper"]), hex2(variant["lower"]), variant["lettersOnly"])
        out.append("    %-34s// kVariant%s" % (body, variant["name"]))
    out.append("};")
    # ---- delimiters ----
    out += [
        "",
        "// Control words accepted after left, right, middle and the big commands, sorted by command",
        "// index -> MathDelimiter.",
        "static constexpr MathDelimWord kMathDelimWords[] = {",
    ]
    for word in tables.delim_words:
        body = "{%d, %d}," % (word["command"], word["delimiter"])
        out.append("    %-14s// \\%s: %s" % (body, word["word"], word["enum"]))
    out.append("};")
    out += [
        "",
        "// What a delimiter is at its natural size: a glyph (font, code), a vector shape, or a composite",
        "// (code = composite id). The same rows are in math_font_metrics.inc.",
    ]
    out += delim_base_rows(tables)
    out += [
        "",
        "static_assert(sizeof(kMathCommandNames) == %d && sizeof(kMathEnvNames) == %d &&"
        % (tables.command_blob, tables.env_blob),
        "              sizeof(kMathUnicode) / sizeof(kMathUnicode[0]) == %d &&" % len(tables.unicode),
        "              sizeof(kMathEnvironments) / sizeof(kMathEnvironments[0]) == %d &&" % len(tables.envs),
        "              sizeof(kMathDelimWords) / sizeof(kMathDelimWords[0]) == %d," % len(tables.delim_words),
        '              "math_symbols.inc table sizes");',
    ]
    return "\n".join(out) + "\n"


def render_metrics(tables: Tables, header: list[str]) -> str:
    fonts = tables.fonts
    out = list(header)
    out += [
        "",
        "// Layout tables of the native math module. Included once, by math_layout.cpp, inside",
        "// namespace TinyPdf::Internal { namespace { ... } } and after math_parser.h.",
        "",
        "struct MathGlyphMetrics { int16_t advance, xMin, yMin, xMax, yMax; };                //  10 bytes",
        "struct MathComposite { int16_t advance; int16_t bbox[4];                             //  12 bytes",
        "                       uint8_t firstPiece, pieceCount; };",
        "struct MathCompositePiece { uint8_t kind, font, code, transform; int16_t v[5]; };    //  14 bytes",
        "struct MathDelimBaseEntry { uint8_t base, font, code; };                             //   3 bytes",
        "enum MathDelimBase : uint8_t { %s };" % ", ".join("kDelimBase" + name for name in DELIM_BASES),
        "enum MathPieceKind : uint8_t { %s };" % ", ".join("kPiece" + name for name in PIECE_KINDS),
        "static_assert(sizeof(MathGlyphMetrics) == 10 && sizeof(MathComposite) == 12 &&",
        "              sizeof(MathCompositePiece) == 14 && sizeof(MathDelimBaseEntry) == 3,",
        '              "math_font_metrics.inc record sizes");',
    ]
    out += pin_asserts([
        ("math_font_metrics.inc: MathDelimiter values",
         [(enum, index) for index, (_, enum) in enumerate(DELIM_ENUM)] + [("kDelimCount", len(DELIM_ENUM))]),
        ("math_font_metrics.inc: MathAccent values of the glyph accents",
         [(enum, index) for index, enum in enumerate(ACCENT_ENUM[:len(ACCENTS)])]),
    ])
    out += [
        "",
        "// [font - 1][code - 0x20], AFM units (1/1000 em): advance and glyph bounding box.",
        "// All-zero = not encoded.",
        "static constexpr MathGlyphMetrics kMathGlyphMetrics[5][224] = {",
    ]
    for index, short in enumerate(MATH_FONTS):
        font = fonts[short]
        out.append("    {   // [%d] /M%d %s" % (index, index + 1, font.base_name))
        for offset, row in enumerate(tables.metrics[index]):
            code = 0x20 + offset
            body = "{%d, %d, %d, %d, %d}," % row
            out.append("        %-32s// 0x%02X %s" % (body, code, font.by_code.get(code, "-")))
        out.append("    },")
    out.append("};")
    out += [
        "",
        "// Constructed symbols; the index is the composite id of a Composite node. bbox = xMin, yMin,",
        "// xMax, yMax of the ink. Pieces, in drawing order, are firstPiece .. firstPiece + pieceCount - 1.",
        "inline constexpr int kMathCompositeCount = %d;" % len(tables.composites),
        "inline constexpr int kMathCompositePieceCount = %d;" % len(tables.pieces),
        "static constexpr MathComposite kMathComposites[kMathCompositeCount] = {",
    ]
    for composite in tables.composites:
        body = "{%d, {%d, %d, %d, %d}, %d, %d}," % ((composite["advance"],) + tuple(composite["bbox"])
                                                  + (composite["first"], composite["count"]))
        names = ["\\" + name for name in composite["names"]]
        out.append("    %-40s// %2d %s" % (body, composite["id"], " ".join(names[:2] + ["..."] * (len(names) > 2))))
    out.append("};")
    out += [
        "",
        "// v: kPieceGlyph - size in 1/1000 of the composite's size, x offset, y offset;",
        "//    kPieceRect - x0, y0, x1, y1; kPieceLine - x0, y0, x1, y1, thickness;",
        "//    kPieceRing - centre x, centre y, radius, thickness. transform = index into kMathTransformOps.",
        "static constexpr MathCompositePiece kMathCompositePieces[kMathCompositePieceCount] = {",
    ]
    for index, piece in enumerate(tables.pieces):
        body = "{kPiece%s, %d, %s, %d, {%s}}," % (piece["kind"], piece["font"], hex2(piece["code"]),
                                                 piece["transform"], ", ".join("%d" % v for v in piece["v"]))
        out.append("    %-58s// %3d \\%s: %s" % (body, index, piece["owner"], piece["note"]))
    out.append("};")
    out += [
        "",
        '// "a b c d" of the text matrix for transform ids 0..7, ready to be followed by " x y Tm".',
        "static constexpr char kMathTransformOps[%d][%d] = {" % (len(TRANSFORMS), TRANSFORM_OP_BYTES),
    ]
    out += wrap_items(['"%s",' % text for text in TRANSFORMS[:-1]] + ['"%s" };' % TRANSFORMS[-1]], width=100)
    out += [
        "",
        "// Times accent glyph of MathAccent 0..9 (the same code in all four faces).",
        "static constexpr uint8_t kMathAccentCodes[%d] = {%s};"
        % (len(tables.accent_codes), ", ".join(hex2(code) for code in tables.accent_codes)),
        "",
        "// What a delimiter is at its natural size: a glyph (font, code), a vector shape, or a composite",
        "// (code = composite id). The same rows are in math_symbols.inc.",
    ]
    out += delim_base_rows(tables)
    out += [
        "",
        "// Advance widths of codes 32..126 in WinAnsiEncoding, the encoding of /F1 /F2 /F3: [0] Helvetica,",
        "// [1] Helvetica-Bold. Courier is %d for every code." % KNOWN_COURIER_ADVANCE,
        "static constexpr uint16_t kStandardTextWidths[2][95] = {",
    ]
    for short in ("H", "HB"):
        out.append("    {   // %s" % fonts[short].base_name)
        out += wrap_items(["%d," % width for width in tables.text_widths[short]], indent="        ", width=100)
        out.append("    },")
    out.append("};")
    out += [
        "",
        "// Sum of the %d values MathFontObject(4, ...) prints in /Widths: the advances of"
        % len(tables.symbol_widths),
        "// kMathGlyphMetrics[4][0 .. %d], codes 32..254." % (len(tables.symbol_widths) - 1),
        "inline constexpr int kMathSymbolWidthsSum = %d;" % sum(tables.symbol_widths),
    ]
    return "\n".join(out) + "\n"


def check_text(name: str, text: str, errors: list[str]) -> None:
    """Both files are ASCII with LF line ends, no tabs, no trailing blanks, no line splice."""
    try:
        text.encode("ascii")
    except UnicodeEncodeError as error:
        errors.append("%s: not ASCII at offset %d" % (name, error.start))
    for number, line in enumerate(text.split("\n"), 1):
        if line != line.rstrip() or "\t" in line or "\r" in line:
            errors.append("%s line %d: trailing blank, tab or carriage return" % (name, number))
        if line.endswith("\\") or "??" in line:
            errors.append("%s line %d: ends in a backslash or contains ??" % (name, number))
    if not text.endswith("\n") or text.endswith("\n\n"):
        errors.append("%s: must end with exactly one line feed" % name)


# =================================================================================================
# 10. Report and cross-check
# =================================================================================================

def size_report(tables: Tables) -> tuple[list[str], list[str]]:
    """Counts and read-only bytes of both files -> (report lines, differences from the contract)."""
    symbols = {
        "control words": len(tables.commands), "control word name bytes": tables.command_blob,
        "Unicode entries": len(tables.unicode), "environments": len(tables.envs),
        "environment name bytes": tables.env_blob, "ASCII entries": len(tables.ascii),
        "variants": len(tables.variants), "delimiter words": len(tables.delim_words),
        "delimiter bases": len(tables.delim_base),
    }
    symbols["read-only bytes"] = (tables.command_blob + len(tables.commands) * 6 + len(tables.unicode) * 6
                                  + tables.env_blob + len(tables.envs) * 10 + len(tables.ascii) * 2
                                  + len(tables.variants) * 6 + len(tables.delim_words) * 4
                                  + len(tables.delim_base) * 3)
    metric_rows = sum(len(rows) for rows in tables.metrics)
    text_widths = sum(len(widths) for widths in tables.text_widths.values())
    metrics = {
        "glyph metric rows": metric_rows, "composites": len(tables.composites),
        "composite pieces": len(tables.pieces), "transform operator bytes": len(TRANSFORMS) * TRANSFORM_OP_BYTES,
        "accent codes": len(tables.accent_codes), "delimiter bases": len(tables.delim_base),
        "text width entries": text_widths,
    }
    metrics["read-only bytes"] = (metric_rows * 10 + len(tables.composites) * 12 + len(tables.pieces) * 14
                                  + len(TRANSFORMS) * TRANSFORM_OP_BYTES + len(tables.accent_codes)
                                  + len(tables.delim_base) * 3 + text_widths * 2)
    lines, differences = [], []
    for title, actual, expected in ((SYMBOLS_FILE, symbols, EXPECTED_SYMBOLS),
                                    (METRICS_FILE, metrics, EXPECTED_METRICS)):
        lines.append(title)
        for key, value in expected:
            mark = "" if actual[key] == value else "   <-- the contract expects %d" % value
            if mark:
                differences.append("%s: %s is %d, the contract expects %d" % (title, key, actual[key], value))
            lines.append("  %-28s %6d%s" % (key, actual[key], mark))
    kinds = {kind: 0 for kind in CMD_KINDS}
    for command in tables.commands:
        kinds[command["kind"]] += 1
    lines.append("control words per kind")
    counts = ["%s %d" % (kind, count) for kind, count in kinds.items()]
    lines += wrap_items([text + "," for text in counts[:-1]] + counts[-1:], indent="  ", width=100)
    widths_text = " ".join("%d" % width for width in tables.symbol_widths)
    lines.append("Symbol /Widths: %d values, %d bytes between the brackets, sum %d, first %s, last %s"
                 % (len(tables.symbol_widths), len(widths_text), sum(tables.symbol_widths),
                    " ".join(widths_text.split()[:3]), " ".join(widths_text.split()[-3:])))
    if len(widths_text) != KNOWN_SYMBOL_WIDTH_TEXT_BYTES:
        differences.append("Symbol /Widths text is %d bytes, the contract expects %d"
                           % (len(widths_text), KNOWN_SYMBOL_WIDTH_TEXT_BYTES))
    lines.append("text widths: Helvetica sum %d, Helvetica-Bold sum %d, Courier %d for every code"
                 % (sum(tables.text_widths.get("H", [])), sum(tables.text_widths.get("HB", [])),
                    KNOWN_COURIER_ADVANCE))
    for short, code, _ in KNOWN_METRICS:
        lines.append("kMathGlyphMetrics[%d][0x%02X - 0x20] = {%s}"
                     % (FONT_ID[short] - 1, code,
                        ", ".join("%d" % v for v in tables.metrics[FONT_ID[short] - 1][code - 0x20])))
    return lines, differences


def cross_check(tables: Tables, path: Path) -> tuple[list[str], str]:
    """Compares the resolved tables with the JSON of the design's own table generator."""
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise Failure("%s: cannot be read as JSON (%s)" % (path, error))
    problems: list[str] = []
    # symbols: (font, code, class, limits) per control sequence
    reference: dict[str, tuple] = {}
    for entry in data["symbols"]:
        for name in entry["names"]:
            reference[name] = (entry["font"], entry["code"], entry["class"], entry["limits"])
    mine = {name: (info["font"], info["code"], info["cls"], info["limits"])
            for name, info in tables.symbol_info.items()}
    for name in sorted(set(reference) | set(mine)):
        if reference.get(name) != mine.get(name):
            problems.append("symbol \\%s: %s here, %s in the reference" % (name, mine.get(name), reference.get(name)))
    # composites: names, class, limits, advance, box, pieces
    if len(data["composites"]) != len(tables.composites):
        problems.append("composites: %d here, %d in the reference"
                        % (len(tables.composites), len(data["composites"])))
    for composite, entry in zip(tables.composites, data["composites"]):
        where = "composite %d \\%s" % (composite["id"], composite["names"][0])
        if (composite["names"], composite["cls"], composite["limits"], composite["advance"]) != \
                (entry["names"], entry["class"], entry["limits"], entry["advance"]):
            problems.append("%s: names, class, limits or advance differ from the reference" % where)
        if composite["bbox"] != entry["bbox"]:
            problems.append("%s: ink box %s here, %s in the reference" % (where, composite["bbox"], entry["bbox"]))
        pieces = tables.pieces[composite["first"]:composite["first"] + composite["count"]]
        if len(pieces) != len(entry["pieces"]):
            problems.append("%s: %d pieces here, %d in the reference" % (where, len(pieces), len(entry["pieces"])))
        for piece, other in zip(pieces, entry["pieces"]):
            if other["kind"] == "G":
                same = (piece["kind"] == "Glyph" and piece["font"] == FONT_ID[other["font"]]
                        and piece["code"] == other["code"] and piece["transform"] == other["transform"]
                        and piece["v"][0] == int(round(other["size"] * 1000))
                        and abs(piece["v"][1] - other["ox"]) <= 0.55 and abs(piece["v"][2] - other["oy"]) <= 0.55)
            else:
                kind = {"R": "Rect", "L": "Line", "C": "Ring"}[other["kind"]]
                same = piece["kind"] == kind and piece["v"][:len(other["args"])] == other["args"]
            if not same:
                problems.append("%s: piece %s differs from the reference %s" % (where, piece["v"], other))
    # delimiters: id, key, tokens, base
    if len(data["delims"]) != len(DELIMS):
        problems.append("delimiters: %d here, %d in the reference" % (len(DELIMS), len(data["delims"])))
    for index, (entry, other) in enumerate(zip(DELIMS, data["delims"])):
        base = list(entry["base"]) if entry["base"] is not None else None
        if (index, entry["key"], entry["tokens"], base) != \
                (other["id"], other["key"], other["tokens"], other["base"]):
            problems.append("delimiter %d %s differs from the reference" % (index, entry["key"]))
    # Unicode: the same target, hence the same resolved glyph
    reference_unicode = {int(cp, 16): (tuple(target) if isinstance(target, list) else target)
                         for cp, target in data["unicode"].items()}
    for cp in sorted(set(reference_unicode) | set(UNICODE)):
        if reference_unicode.get(cp) != UNICODE.get(cp):
            problems.append("Unicode U+%04X: %r here, %r in the reference"
                            % (cp, UNICODE.get(cp), reference_unicode.get(cp)))
    # ASCII rows and accent codes (the comma row is Punct in both)
    reference_ascii = {row["char"]: (row["font"], row["code"], row["class"]) for row in data["ascii"]
                       if row["char"] not in ASCII_PARSER}
    mine_ascii = {}
    for char, font, glyph, cls in ASCII:
        mine_ascii[char] = (font, tables.fonts[font].by_name.get(glyph, (0,))[0], cls)
    if reference_ascii != mine_ascii:
        problems.append("ASCII rows differ from the reference")
    if [row["code"] for row in data["accents"]] != tables.accent_codes:
        problems.append("accent codes differ from the reference")
    summary = ("cross-check against %s: %d symbol names, %d composites, %d delimiters, %d Unicode entries, "
               "%d ASCII rows, %d accent codes" % (path.name, len(mine), len(tables.composites), len(DELIMS),
                                                    len(UNICODE), len(mine_ascii), len(tables.accent_codes)))
    return problems, summary


# =================================================================================================
# 11. Command line
# =================================================================================================

def generate(afm_dir: Path, reference: Path | None) -> tuple[dict[str, str], list[str], list[str]]:
    """-> ({file name: text}, report lines, differences from the contract). Raises Failure."""
    blobs = read_inputs(afm_dir)
    fonts = {short: Font(short, blobs[FONT_NAMES[short] + ".afm"]) for short in FONT_NAMES}
    licence = licence_paragraph(blobs[README_FILE])
    tables = Tables(fonts)

    def stop_on_errors() -> None:
        if tables.errors:
            raise Failure("VERIFICATION FAILED:\n  - " + "\n  - ".join(tables.errors) + "\nNothing was written.")

    try:
        tables.build()
    except (KeyError, IndexError, TypeError, ValueError) as error:
        tables.errors.append("the tables could not be completed (%s: %s)" % (type(error).__name__, error))
    stop_on_errors()
    header = header_block(fonts, licence)
    files = {SYMBOLS_FILE: render_symbols(tables, header), METRICS_FILE: render_metrics(tables, header)}
    for name, text in files.items():
        check_text(name, text, tables.errors)
    report = ["inputs: %s (%d files, SHA-256 as pinned)" % (afm_dir, len(PINNED_INPUTS))]
    if reference is not None:
        problems, summary = cross_check(tables, reference)
        tables.errors += problems
        report.append(summary + (": equal" if not problems else ": DIFFERENT"))
    else:
        report.append("cross-check against the design's math_symbols.json: not run (no --reference-json)")
    stop_on_errors()
    lines, differences = size_report(tables)
    return files, report + lines, differences


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Generate src/core/math_symbols.inc and src/core/math_font_metrics.inc from the "
                    "Adobe Core 14 AFM files (pinned by SHA-256).")
    parser.add_argument("--afm-dir", help="directory with the eight AFM files and readme.txt; default: "
                        "mpl-data/fonts/pdfcorefonts of an installed matplotlib")
    parser.add_argument("--out-dir", default=str(ROOT / "src" / "core"),
                        help="where the two files are written (default: src/core of this repository)")
    parser.add_argument("--check", action="store_true",
                        help="write nothing; exit 1 when a file on disk differs from what would be generated")
    parser.add_argument("--reference-json",
                        help="math_symbols.json of the design's table generator: also compare every symbol, "
                             "composite, delimiter and Unicode entry with it")
    args = parser.parse_args()
    out_dir = Path(args.out_dir)
    try:
        afm_dir = locate_afm_dir(args.afm_dir)
        files, report, differences = generate(afm_dir, Path(args.reference_json) if args.reference_json else None)
    except Failure as failure:
        print(str(failure), file=sys.stderr)
        return 2
    print("\n".join(report))
    for difference in differences:
        print("NOTE: " + difference)
    print("VERIFICATION OK: every glyph name resolved to an encoded AFM glyph; no duplicate or dangling names; "
          "all values fit their fields.")
    status = 0
    for name, text in files.items():
        path = out_dir / name
        wanted = text.encode("ascii")
        try:
            current = path.read_bytes()
        except OSError:
            current = None
        if current == wanted:
            print("%s: up to date (%d bytes)" % (path, len(wanted)))
        elif args.check:
            print("%s: DIFFERS from the generated text" % path if current is not None
                  else "%s: MISSING" % path, file=sys.stderr)
            status = 1
        else:
            try:
                path.write_bytes(wanted)
            except OSError as error:
                print("%s: cannot be written (%s)" % (path, error.strerror or error), file=sys.stderr)
                return 2
            print("%s: written (%d bytes)" % (path, len(wanted)))
    if args.check:
        if status == 0:
            print("CHECK OK: both files match what would be generated.")
        else:
            print("CHECK FAILED: regenerate with python %s" % SCRIPT_NAME, file=sys.stderr)
    return status


if __name__ == "__main__":
    raise SystemExit(main())
