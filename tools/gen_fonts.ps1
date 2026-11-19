# Genere les polices Montserrat AVEC accents (src/fonts/lv_font_fr_XX.c).
# Meme police et memes symboles que les polices integrees de LVGL, plus le Latin-1
# (e, a, c, o, u... accentues, majuscules comprises), oe/OE, guillemets, tirets, euro.
# Prerequis : Node.js. Utilisation :
#   powershell -ExecutionPolicy Bypass -File tools\gen_fonts.ps1
$ErrorActionPreference = 'Stop'
$root  = Split-Path -Parent $PSScriptRoot
$out   = Join-Path $root 'src\fonts'
$lib   = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Arduino\libraries\lvgl\scripts\built_in_font'
$text  = Join-Path $lib 'Montserrat-Medium.ttf'
$sym   = Join-Path $lib 'FontAwesome5-Solid+Brands+Regular.woff'
New-Item -ItemType Directory -Force $out | Out-Null

# Doit rester identique a FONT_CHARS (dashboard.cpp) et devText() (web/index.html)
$ranges = '0x20-0x7E,0xA0-0xFF,0x152-0x153,0x2013-0x2014,0x2018-0x2019,0x201C-0x201E,0x2022,0x2026,0x20AC'
# Symboles LV_SYMBOL_* (liste de lvgl/scripts/built_in_font/built_in_font_gen.py)
$symbols = '61441,61448,61451,61452,61453,61457,61459,61461,61465,61468,61473,61478,61479,61480,61502,61507,61512,61515,61516,61517,61521,61522,61523,61524,61543,61544,61550,61552,61553,61556,61559,61560,61561,61563,61587,61589,61636,61637,61639,61641,61664,61671,61674,61683,61724,61732,61787,61931,62016,62017,62018,62019,62020,62087,62099,62212,62189,62810,63426,63650'

foreach ($s in 12,14,16,18,20,22,24,28,32,36,40,48) {
  $name = "lv_font_fr_$s"
  $file = Join-Path $out "$name.c"
  npx --yes lv_font_conv@1.5.3 --no-compress --no-prefilter --bpp 4 --size $s `
    --font $text -r $ranges --font $sym -r $symbols `
    --format lvgl --lv-font-name $name --force-fast-kern-format -o $file
  if ($LASTEXITCODE -ne 0) { throw "lv_font_conv a echoue ($s px)" }
  # Arduino : inclure lvgl.h directement
  $c = [IO.File]::ReadAllText($file)
  $c = $c -replace '(?s)#ifdef LV_LVGL_H_INCLUDE_SIMPLE.*?#endif', '#include "lvgl.h"'
  [IO.File]::WriteAllText($file, $c, (New-Object Text.UTF8Encoding $false))
  Write-Host "$name.c"
}
