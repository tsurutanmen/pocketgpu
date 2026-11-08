param([string]$Path)
# .exe が 32 ビットか 64 ビットかを PE の見出しから読む
try {
  $fs = [IO.File]::OpenRead($Path)
  $br = New-Object IO.BinaryReader($fs)
  $fs.Position = 0x3C
  $fs.Position = $br.ReadInt32() + 4
  $m = $br.ReadUInt16()
  $br.Close()
  if ($m -eq 0x8664) { 'x64' } elseif ($m -eq 0x14c) { 'x86' } else { 'x64' }
} catch { 'x64' }
