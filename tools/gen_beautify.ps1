# ═══════════════════════════════════════════════════════════
#  gen_beautify.ps1 — 从桌面美化库生成服务端美化资源包 + manifest
#
#  输入: C:\Users\Lenovo\Desktop\美化
#  输出: assets\beautify\<md5>.bin  (内容寻址, 全库去重)
#        assets\beautify\halo\<id>.png  (光环预览图)
#        assets\beautify_manifest.json
#
#  规则:
#   - 跳过 "(1)" 浏览器重复下载副本 / .mp4 预览 / ELF(关键词回复, 铁律不带二进制)
#   - 刺球/去雾/渣渣鸟 源文件无扩展名 → manifest 里给目标文件名, 客户端重命名
#   - 每个分组隐式带"恢复原版"(客户端自动备份还原, 不走资源包)
# ═══════════════════════════════════════════════════════════
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

$src      = 'C:\Users\Lenovo\Desktop\美化'
$assets   = 'D:\Dev-Cpp\BallsHackProService\assets'
$dst      = Join-Path $assets 'beautify'
$manifest = Join-Path $assets 'beautify_manifest.json'
$tmp      = Join-Path $env:TEMP 'bhpro_beautify_gen'

Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force $dst -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $tmp, $dst | Out-Null

Add-Type -AssemblyName System.IO.Compression.FileSystem

# ── 解压 zip (GBK 文件名, 本机 CP936 下 .NET 按系统默认码页解码) ──
function Extract-Zip([string]$zip, [string]$to) {
    [System.IO.Compression.ZipFile]::ExtractToDirectory($zip, $to)
}

# 递归找目录 (压缩包内根目录名不定), 返回 DirectoryInfo 对象
function Find-Dirs([string]$root, [string]$name) {
    Get-ChildItem -Path $root -Recurse -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -eq $name }
}
function Find-Files([string]$root, [string]$pattern) {
    Get-ChildItem -Path $root -Recurse -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -like $pattern } | ForEach-Object { $_.FullName }
}

# ── 内容寻址存储: 返回相对 dst 的文件名 ──
$storeMap = @{}
function Store-File([string]$path, [string]$section) {
    $md5 = (Get-FileHash -Algorithm MD5 -Path $path).Hash.ToLower()
    if ($storeMap.ContainsKey($md5)) { return $storeMap[$md5] }
    $rel = "$section/$md5.bin"
    New-Item -ItemType Directory -Force -Path (Join-Path $dst $section) | Out-Null
    Copy-Item -LiteralPath $path -Destination (Join-Path $dst $rel) -Force
    $storeMap[$md5] = $rel
    return $rel
}

# ── 文件信息 (校验用) ──
function File-Info([string]$rel) {
    $p = Join-Path $dst $rel
    $md5 = (Get-FileHash -Algorithm MD5 -Path $p).Hash.ToLower()
    return @{ file = $rel; md5 = $md5; size = (Get-Item $p).Length }
}

# ═══════════════════════════════════════════════════════════
#  解压
# ═══════════════════════════════════════════════════════════
$zKw   = Join-Path $tmp 'kw';    Extract-Zip (Join-Path $src '关键词美化.zip') $zKw
$zCq   = Join-Path $tmp 'cq';    Extract-Zip (Join-Path $src '刺球 音效 去雾 去皮 美化.zip') $zCq
$zSp   = Join-Path $tmp 'spore'; Extract-Zip (Join-Path $src '狍子美化.zip') $zSp
$zHalo = Join-Path $tmp 'halo';  Extract-Zip (Join-Path $src '箭头美化.zip') $zHalo

$kwRoot = (Find-Dirs $zKw '关键词美化' | Select-Object -First 1)
$spRoot = (Find-Dirs $zSp '爱心蛋146'  | Select-Object -First 1)
$haloRoot = (Find-Dirs $zHalo '其他箭头' | Select-Object -First 1)
$bufen = Join-Path $src '部分美化'

$sections = @(
    @{ id = 'spore';   name = '孢子美化' },
    @{ id = 'keyword'; name = '关键词皮肤' },
    @{ id = 'halo';    name = '光环美化' },
    @{ id = 'spike';   name = '刺球美化' },
    @{ id = 'fog';     name = '去迷雾' },
    @{ id = 'skin';    name = '去皮肤' }
)

$groups = @()
$gid = 0

# ═══════════════════════════════════════════════════════════
#  孢子 (bz_*.unity3d_u_*) → alltextures/ballmaterial1
# ═══════════════════════════════════════════════════════════
$sporeDir = (Join-Path $bufen '孢子')
$sporeBases = @(
    @{ id = 'spore-xuehua';  base = '雪花';   stem = 'bz_7' },
    @{ id = 'spore-qingshu'; base = '情书';   stem = 'bz_sf136' },
    @{ id = 'spore-pugy';    base = '蒲公英'; stem = 'bz_sf134' }
)
foreach ($b in $sporeBases) {
    $seen = @{}
    $styles = @()
    # 优先带 stem 后缀的源文件 (无扩展名副本同名时去重)
    Get-ChildItem -LiteralPath $sporeDir -File | Where-Object {
        ($_.Name -like "*$($b.stem).unity3d_u_*" -or $_.Name -like "*$($b.base)*") -and
        $_.Name -notlike '*恢复*' -and $_.Name -notlike '*(1)*'
    } | Sort-Object { if ($_.Name -like "*$($b.stem).unity3d_u_*") { 0 } else { 1 } } | ForEach-Object {
        # 目标文件名: 优先源文件名里的 stem 部分; 无扩展名源文件(重命名目标)用固定名
        if ($_.Name -match "$($b.stem)\.unity3d_u_\d+") {
            $target = $Matches[0]
        } else {
            $target = "$($b.stem).unity3d_u_1"
        }
        # 样式名 = 去掉 stem+目标名 和 基础名 后的部分
        $n = $_.Name -replace [regex]::Escape($target), ''
        $n = $n -replace [regex]::Escape($b.base), ''
        $n = $n -replace '^\s+|\s+$', ''
        if ($n -and -not $seen.ContainsKey($n)) {
            $seen[$n] = $true
            $rel = Store-File $_.FullName 'spore'
            $info = File-Info $rel
            $styles += @{
                name  = $n
                files = @( @{ target = $target; file = $info.file; md5 = $info.md5; size = $info.size } )
            }
        }
    }
    if ($styles.Count) {
        $gid++
        $groups += @{
            id = $b.id; section = 'spore'; name = $b.base
            dir = 'alltextures/ballmaterial1'
            altDirs = @('alltextures/ballmaterial')
            matchPrefix = $true
            styles = $styles
        }
    }
}

# 荷叶 bz_sf135: 桌面 狍子美化/荷叶/小泳圈bz_sf135.unity3d_u_4
$hyFile = Join-Path $src '狍子美化\荷叶\小泳圈bz_sf135.unity3d_u_4'
if (Test-Path $hyFile) {
    $rel = Store-File $hyFile 'spore'
    $info = File-Info $rel
    $gid++
    $groups += @{
        id = 'spore-heye'; section = 'spore'; name = '荷叶'
        dir = 'alltextures/ballmaterial1'
        altDirs = @('alltextures/ballmaterial')
        matchPrefix = $true
        styles = @( @{
            name = '小泳圈'
            files = @( @{ target = 'bz_sf135.unity3d_u_4'; file = $info.file; md5 = $info.md5; size = $info.size } )
        } )
    }
}

# 爱心蛋 bz_sf146 (狍子美化.zip, 每个样式一个 bz_sf146.unity3d_u_4)
if ($spRoot) {
    $styles = @()
    Get-ChildItem -LiteralPath $spRoot.FullName -Directory | ForEach-Object {
        $f = Get-ChildItem -LiteralPath $_.FullName -File | Select-Object -First 1
        if ($f) {
            $rel = Store-File $f.FullName 'spore'
            $info = File-Info $rel
            $styles += @{
                name = $_.Name
                files = @( @{ target = $f.Name; file = $info.file; md5 = $info.md5; size = $info.size } )
            }
        }
    }
    if ($styles.Count) {
        $gid++
        $groups += @{
            id = 'spore-aixindan'; section = 'spore'; name = '爱心蛋'
            dir = 'alltextures/ballmaterial1'
            altDirs = @('alltextures/ballmaterial')
            matchPrefix = $true
            styles = $styles
        }
    }
}

# ═══════════════════════════════════════════════════════════
#  关键词皮肤 (pifu_XXX_lvN.unity3d_u_4) → keyskin/pifuN
# ═══════════════════════════════════════════════════════════
function New-KeywordGroup([string]$id, [string]$name, [string]$dir, [string]$skinNum, [string]$styleRoot, [bool]$matchPrefix) {
    $styles = @()
    Get-ChildItem -LiteralPath $styleRoot -Directory | ForEach-Object {
        $styleDir = $_.FullName
        $styleName = $_.Name
        $files = @()
        foreach ($lv in 1, 2, 3) {
            $target = "pifu_$($skinNum)_lv$lv.unity3d_u_4"
            $top = Join-Path $styleDir $target
            $lvNames = @('一级', '二级', '三级')
            $lvDir = Join-Path $styleDir $lvNames[$lv - 1]
            $cand = @($top)
            if (Test-Path $lvDir) { $cand += (Join-Path $lvDir $target) }
            $found = $cand | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
            if ($found) {
                $rel = Store-File $found 'keyword'
                $info = File-Info $rel
                $files += @{ target = $target; file = $info.file; md5 = $info.md5; size = $info.size }
            }
        }
        if ($files.Count) {
            $styles += @{ name = $styleName; files = $files }
        }
    }
    if ($styles.Count) {
        $gid++
        $script:groups += @{
            id = $id; section = 'keyword'; name = $name
            dir = $dir; altDirs = @(); matchPrefix = $matchPrefix
            styles = $styles
        }
    }
}

$kwSkins = @(
    @{ id = 'kw-jimo';  name = '寂寞星球'; num = '017'; dir = 'keyskin/pifu1' },
    @{ id = 'kw-juzi';  name = '桔子宝贝'; num = '031'; dir = 'keyskin/pifu3' },
    @{ id = 'kw-xigua'; name = 'cool西瓜'; num = '025'; dir = 'keyskin/pifu2' }
)
foreach ($s in $kwSkins) {
    $root = Get-ChildItem -LiteralPath $kwRoot.FullName -Directory | Where-Object { $_.Name -like "*$($s.num)*" } | Select-Object -First 1
    if ($root) { New-KeywordGroup $s.id $s.name $s.dir $s.num $root.FullName $true }
}

# 渣渣鸟018 (部分美化/关键词, 无扩展名 → u_1)
$zznRoot = Join-Path $bufen '关键词'
$zznStyles = @{}
Get-ChildItem -LiteralPath $zznRoot -File | Where-Object { $_.Name -like '*渣渣鸟美化*' -and $_.Name -notlike '*(1)*' } | ForEach-Object {
    $n = $_.Name
    $lv = if ($n -like '一级*') { 1 } elseif ($n -like '二级*') { 2 } elseif ($n -like '三级*') { 3 } else { 0 }
    $style = $n -replace '^(一|二|三)级渣渣鸟美化', ''
    if ($lv -and $style) {
        $rel = Store-File $_.FullName 'keyword'
        $info = File-Info $rel
        if (-not $zznStyles.ContainsKey($style)) { $zznStyles[$style] = @() }
        $zznStyles[$style] += @{ target = "pifu_018_lv$lv.unity3d_u_1"; file = $info.file; md5 = $info.md5; size = $info.size }
    }
}
if ($zznStyles.Count) {
    $styles = @()
    foreach ($k in $zznStyles.Keys) {
        $styles += @{ name = $k; files = $zznStyles[$k] }
    }
    $gid++
    $groups += @{
        id = 'kw-zhaniao'; section = 'keyword'; name = '渣渣鸟'
        dir = 'keyskin/pifu1'; altDirs = @(); matchPrefix = $true
        styles = $styles
    }
}

# 所有分身美化 → ui/keyskin/ (pifu_017i/018i/025i/031i)
$fenRoot = Get-ChildItem -LiteralPath $kwRoot.FullName -Directory | Where-Object { $_.Name -like '*所有分身*' } | Select-Object -First 1
if ($fenRoot) {
    $files = @()
    Get-ChildItem -LiteralPath $fenRoot.FullName -File | Where-Object { $_.Name -like '*.unity3d_u_*' } | ForEach-Object {
        $rel = Store-File $_.FullName 'keyword'
        $info = File-Info $rel
        $files += @{ target = $_.Name; file = $info.file; md5 = $info.md5; size = $info.size }
    }
    if ($files.Count) {
        $gid++
        $groups += @{
            id = 'kw-fenshen'; section = 'keyword'; name = '所有分身'
            dir = 'ui/keyskin'; altDirs = @(); matchPrefix = $true
            styles = @( @{ name = '分身美化'; files = $files } )
        }
    }
}

# 红甲改黑甲 (皮肤→皮肤) → keyskin/pifu41
$hjFile = Join-Path $src '[公益]红甲改黑甲\pifu_351_lv3.unity3d_u_5'
if (Test-Path $hjFile) {
    $rel = Store-File $hjFile 'keyword'
    $info = File-Info $rel
    $gid++
    $groups += @{
        id = 'kw-hongjia'; section = 'keyword'; name = '红色甲壳虫'
        dir = 'keyskin/pifu41'; altDirs = @(); matchPrefix = $true
        styles = @( @{
            name = '黑甲'
            files = @( @{ target = 'pifu_351_lv3.unity3d_u_5'; file = $info.file; md5 = $info.md5; size = $info.size } )
        } )
    }
}

# ═══════════════════════════════════════════════════════════
#  刺球 (4模式, 无扩展名 → 重命名目标)
# ═══════════════════════════════════════════════════════════
$spikeModes = @(
    @{ id = 'spike-shuangguan'; name = '双冠'; dir = 'alltextures/map3';          target = 'ciqiu_yuzhou_5.unity3d_u_4' },
    @{ id = 'spike-tuanzhan';   name = '团战'; dir = 'ui/activity22';              target = 'cici2.unity3d_u_4' },
    @{ id = 'spike-taosha';     name = '逃杀'; dir = 'alltextures/ingameeffect';   target = 'ciqiu_dataosha.unity3d_u_4' },
    @{ id = 'spike-daoju';      name = '道具'; dir = 'alltextures/map6';           target = 'ciqiu_freeworld_blue.unity3d_u_2' }
)
$cqSpikeRoot = (Find-Dirs $zCq '刺球' | Select-Object -First 1)
foreach ($m in $spikeModes) {
    $modeDir = Get-ChildItem -LiteralPath $cqSpikeRoot.FullName -Directory | Where-Object { $_.Name -eq $m.name } | Select-Object -First 1
    if (-not $modeDir) { continue }
    $styles = @()
    Get-ChildItem -LiteralPath $modeDir.FullName -File | ForEach-Object {
        $rel = Store-File $_.FullName 'spike'
        $info = File-Info $rel
        $styles += @{
            name = $_.Name
            files = @( @{ target = $m.target; file = $info.file; md5 = $info.md5; size = $info.size } )
        }
    }
    $gid++
    $groups += @{
        id = $m.id; section = 'spike'; name = $m.name
        dir = $m.dir; altDirs = @(); matchPrefix = $true
        styles = $styles
    }
}

# ═══════════════════════════════════════════════════════════
#  去迷雾 (红雾/蓝雾, 无扩展名 → 重命名)
# ═══════════════════════════════════════════════════════════
$fogItems = @(
    @{ name = '去红雾';   file = '去红雾';       dir = 'alltextures/map3'; target = 'partnerdunew.unity3d_u_4' },
    @{ name = '去蓝雾不留坑'; file = '去蓝雾不留坑'; dir = 'alltextures/map1'; target = 'caocongnew.unity3d_u_4' }
)
$fogStyles = @()
foreach ($f in $fogItems) {
    $srcFile = Join-Path $zCq "去雾\$($f.file)"
    if (-not (Test-Path -LiteralPath $srcFile)) { $srcFile = Join-Path $zCq "刺球 音效 去雾 去皮 美化\去雾\$($f.file)" }
    if (-not (Test-Path -LiteralPath $srcFile)) { continue }
    $rel = Store-File $srcFile 'fog'
    $info = File-Info $rel
    $fogStyles += @{
        name = $f.name
        files = @( @{ target = $f.target; file = $info.file; md5 = $info.md5; size = $info.size } )
    }
}
$gid++
$groups += @{
    id = 'fog-wumai'; section = 'fog'; name = '迷雾'
    dir = ''; altDirs = @(); matchPrefix = $true
    styles = $fogStyles
}

# ═══════════════════════════════════════════════════════════
#  光环 (uipic.unity3d_u_<md5> + ver.xml md5 手术)
# ═══════════════════════════════════════════════════════════
if ($haloRoot) {
    $styles = @()
    Get-ChildItem -LiteralPath $haloRoot.FullName -Directory | ForEach-Object {
        $uipic = Get-ChildItem -LiteralPath $_.FullName -File | Where-Object { $_.Name -like 'uipic.unity3d_u_*' } | Select-Object -First 1
        if ($uipic) {
            $rel = Store-File $uipic.FullName 'halo'
            $info = File-Info $rel
            # 目标名 = uipic.unity3d_u_<内容md5> (机制已验证: 后缀=md5)
            $target = "uipic.unity3d_u_$($info.md5)"
            # 预览图
            $png = Get-ChildItem -LiteralPath $_.FullName -File | Where-Object { $_.Name -eq '1.png' } | Select-Object -First 1
            $preview = ''
            if ($png) {
                $preview = "halo/$($info.md5.Substring(0,8)).png"
                Copy-Item -LiteralPath $png.FullName -Destination (Join-Path $dst $preview) -Force
            }
            $styles += @{
                name = $_.Name
                preview = $preview
                files = @( @{ target = $target; file = $info.file; md5 = $info.md5; size = $info.size } )
            }
        }
    }
    if ($styles.Count) {
        $gid++
        $groups += @{
            id = 'halo-jiantou'; section = 'halo'; name = '箭头光环'
            dir = ''; altDirs = @(); matchPrefix = $false
            verPatch = @{ file = 'common/data/uipic.unity3d' }
            styles = $styles
        }
    }
}

# ═══════════════════════════════════════════════════════════
#  去皮肤 (ver.xml 删 shopconfig 行, 客户端备份可还原)
# ═══════════════════════════════════════════════════════════
$gid++
$groups += @{
    id = 'skin-qupi'; section = 'skin'; name = '去皮肤'
    dir = ''; altDirs = @(); matchPrefix = $false
    mechanism = @{ type = 'verdel'; match = 'shopconfig.unity3d' }
    styles = @( @{ name = '去皮'; files = @() } )
}

# ═══════════════════════════════════════════════════════════
#  输出 manifest
# ═══════════════════════════════════════════════════════════
$manifestObj = @{
    version  = 1
    sections = $sections
    groups   = $groups
}
$json = $manifestObj | ConvertTo-Json -Depth 12
# ConvertTo-Json 会转义中文为 \uXXXX — 保持原样避免编码问题, 客户端解析无碍
[IO.File]::WriteAllText($manifest, $json, [Text.UTF8Encoding]::new($false))

$fileCount = (Get-ChildItem -Path $dst -Recurse -File).Count
$totalSize = (Get-ChildItem -Path $dst -Recurse -File | Measure-Object Length -Sum).Sum
$styleCount = 0
foreach ($g in $groups) { $styleCount += $g.styles.Count }
Write-Output "=== 生成完成 ==="
Write-Output "分组数: $($groups.Count)  样式数: $styleCount  文件数: $fileCount"
Write-Output ("资源包大小: {0:N1} MB" -f ($totalSize / 1MB))
Write-Output "manifest: $manifest"
