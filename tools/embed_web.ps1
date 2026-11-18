# Regenere web_page.h a partir de web/index.html et web/login.html.
# A relancer apres chaque modification des pages web :
#   powershell -ExecutionPolicy Bypass -File tools\embed_web.ps1
$root = Split-Path -Parent $PSScriptRoot
$dst  = Join-Path $root 'web_page.h'
$utf8 = New-Object System.Text.UTF8Encoding $false

function Embed($name, $file) {
  $html = [System.IO.File]::ReadAllText((Join-Path $root "web\$file"), $utf8)
  if ($html.Contains(')HTMLPAGE"')) { throw "$file contient le delimiteur )HTMLPAGE`"" }
  Write-Host ("{0} : {1} octets" -f $file, $html.Length)
  return "static const char $name[] PROGMEM = R`"HTMLPAGE($html)HTMLPAGE`";`n"
}

$out  = "// FICHIER GENERE par tools/embed_web.ps1 depuis web/*.html - ne pas modifier.`n"
$out += "#pragma once`n#include <Arduino.h>`n`n"
$out += Embed 'INDEX_HTML' 'index.html'
$out += "`n"
$out += Embed 'LOGIN_HTML' 'login.html'
[System.IO.File]::WriteAllText($dst, $out, $utf8)
Write-Host "web_page.h genere"
