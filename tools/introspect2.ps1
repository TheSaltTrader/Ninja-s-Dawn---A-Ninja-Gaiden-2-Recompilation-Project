$enc = "C:\Program Files (x86)\Microsoft Expression\Encoder 4"
Add-Type -Path "$enc\SDK\Microsoft.Expression.Encoder.dll"
Write-Output "=== AdvancedVC1VideoProfile: keyframe / GOP / bframe properties ==="
[Microsoft.Expression.Encoder.Profiles.AdvancedVC1VideoProfile].GetProperties() |
  Where-Object { $_.Name -match "Key|GOP|BFrame|Frame|Complex|Mode" } |
  ForEach-Object { "  {0,-26} {1}" -f $_.Name, $_.PropertyType.Name }
