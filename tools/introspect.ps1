$enc = "C:\Program Files (x86)\Microsoft Expression\Encoder 4"
Add-Type -Path "$enc\SDK\Microsoft.Expression.Encoder.dll"
$asm = [Reflection.Assembly]::LoadFrom("$enc\SDK\Microsoft.Expression.Encoder.dll")

Write-Output "=== MediaItem properties mentioning size/resize/aspect/crop ==="
[Microsoft.Expression.Encoder.MediaItem].GetProperties() |
  Where-Object { $_.Name -match "Resize|Size|Aspect|Crop|Stretch|Fit" } |
  ForEach-Object { "  {0,-28} {1}" -f $_.Name, $_.PropertyType.Name }

Write-Output ""
Write-Output "=== AdvancedVC1VideoProfile properties mentioning size/aspect ==="
[Microsoft.Expression.Encoder.Profiles.AdvancedVC1VideoProfile].GetProperties() |
  Where-Object { $_.Name -match "Size|Aspect|Ratio|Crop|Fit" } |
  ForEach-Object { "  {0,-28} {1}" -f $_.Name, $_.PropertyType.Name }

Write-Output ""
Write-Output "=== any enum named *ResizeMode* and its values ==="
$asm.GetTypes() | Where-Object { $_.IsEnum -and $_.Name -match "Resize|Aspect|Stretch" } |
  ForEach-Object { "  {0}: {1}" -f $_.FullName, ([Enum]::GetNames($_) -join ", ") }
