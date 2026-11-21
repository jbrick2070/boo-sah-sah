# Rebuild the two curved blimp signs from their source catalogs. Windows WPF
# shapes each complete phrase before rasterization (including RTL and Indic
# clusters). Store playback uses these finished masks and needs no language pack.
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName PresentationCore, WindowsBase, PresentationFramework
$repo = Split-Path $PSScriptRoot -Parent
$cultureInvariant = [System.Globalization.CultureInfo]::InvariantCulture
$body = [System.Text.StringBuilder]::new()
[void]$body.AppendLine('// Generated from assets/strings by tools/gen_blimp_signs.ps1.')
[void]$body.AppendLine('static constexpr BlimpSigns kBlimpSigns[] = {')

function Get-SignFace([string]$Tag, [string]$Value) {
    $culture = [System.Globalization.CultureInfo]::GetCultureInfo($Tag)
    $family = switch ($Tag) {
        'ja-JP' {'Yu Gothic UI'}
        'ko-KR' {'Malgun Gothic'}
        'zh-TW' {'Microsoft JhengHei UI'}
        'zh-CN' {'Microsoft YaHei UI'}
        'hi-IN' {'Nirmala UI'}
        'th-TH' {'Leelawadee UI'}
        default {'Segoe UI'}
    }
    $face = [Windows.Media.Typeface]::new([Windows.Media.FontFamily]::new($family),
        [Windows.FontStyles]::Normal, [Windows.FontWeights]::Bold, [Windows.FontStretches]::Normal)
    $glyphs = $null
    if (-not $face.TryGetGlyphTypeface([ref]$glyphs)) { throw "$Tag requires $family." }
    foreach ($c in $Value.ToCharArray()) {
        if (-not [char]::IsWhiteSpace($c) -and -not $glyphs.CharacterToGlyphMap.ContainsKey([int]$c)) {
            throw "$family lacks U+$(([int]$c).ToString('X4')) for $Tag."
        }
    }
    $dir = if ($culture.TextInfo.IsRightToLeft) { [Windows.FlowDirection]::RightToLeft }
           else { [Windows.FlowDirection]::LeftToRight }
    $text = [Windows.Media.FormattedText]::new($Value, $culture, $dir, $face,
        100.0, [Windows.Media.Brushes]::White, 1.0)
    $outline = $text.BuildGeometry([Windows.Point]::new(0,0))
    $bounds = $outline.Bounds
    if ($bounds.IsEmpty) { throw "$Tag has an empty sign." }
    $scale = [Math]::Min(248.0/$bounds.Width, 40.0/$bounds.Height)
    $w = [int][Math]::Ceiling($bounds.Width*$scale)+2
    $h = [int][Math]::Ceiling($bounds.Height*$scale)+2
    $outline.Transform = [Windows.Media.MatrixTransform]::new($scale,0,0,$scale,
        1.0-$bounds.X*$scale,1.0-$bounds.Y*$scale)
    $visual = [Windows.Media.DrawingVisual]::new()
    $dc = $visual.RenderOpen()
    $dc.DrawGeometry([Windows.Media.Brushes]::White,$null,$outline)
    $dc.Close()
    $bitmap = [Windows.Media.Imaging.RenderTargetBitmap]::new($w,$h,96,96,[Windows.Media.PixelFormats]::Pbgra32)
    $bitmap.Render($visual)
    $pixels = [byte[]]::new($w*$h*4)
    $bitmap.CopyPixels($pixels,$w*4,0)
    $rows = [System.Text.StringBuilder]::new()
    $ink = 0
    for ($y=0; $y -lt $h; ++$y) {
        for ($x=0; $x -lt $w; $x+=4) {
            $nibble = 0
            for ($i=0; $i -lt 4 -and $x+$i -lt $w; ++$i) {
                if ($pixels[4*($y*$w+$x+$i)+3] -ge 88) {
                    $nibble = $nibble -bor (8 -shr $i)
                    ++$ink
                }
            }
            [void]$rows.Append($nibble.ToString('x',$cultureInvariant))
        }
    }
    if ($ink -lt 12) { throw "$Tag produced no readable ink." }
    $literal = $Value.Replace('\','\\').Replace('"','\"')
    return ('{L"' + $literal + '",' + $w + ',' + $h + ',"' + $rows.ToString() + '"}')
}

$catalogs = @(Get-ChildItem -LiteralPath (Join-Path $repo 'assets/strings') -Filter '*.json' |
    Sort-Object @{ Expression={ if ($_.BaseName -eq 'en-US') { '0' } else { '1'+$_.BaseName } } })
foreach ($catalog in $catalogs) {
    $tag = $catalog.BaseName
    $strings = (Get-Content -Raw -Encoding UTF8 -LiteralPath $catalog.FullName | ConvertFrom-Json).strings
    $faces = foreach ($key in @('blimp.bloomLate','blimp.stayWild')) {
        $value = if ($tag -eq 'en-US') { $strings.$key.text } else { $strings.$key }
        if ([string]::IsNullOrWhiteSpace($value)) { throw "Missing $tag $key." }
        Get-SignFace $tag $value
    }
    [void]$body.AppendLine('    {"'+$tag+'",')
    [void]$body.AppendLine('     '+$faces[0]+',')
    [void]$body.AppendLine('     '+$faces[1]+'},')
}
[void]$body.AppendLine('};')
$path = Join-Path $repo 'src/render/blimp_signs.inc'
[IO.File]::WriteAllText($path,$body.ToString().Replace("`r`n","`n"),[Text.UTF8Encoding]::new($false))
Write-Output "Generated $($catalogs.Count) pairs of shaped blimp signs."
