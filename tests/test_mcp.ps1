param([Parameter(Mandatory=$true)][string]$Executable, [Parameter(Mandatory=$true)][string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$script:requestId = 0
function Check($Condition, [string]$Message) { if (!$Condition) { throw $Message } }
function Rpc([string]$Method, $Params) {
    $script:requestId++
    $request = @{jsonrpc='2.0'; id=$script:requestId; method=$Method; params=$Params} | ConvertTo-Json -Depth 12 -Compress
    $script:process.StandardInput.WriteLine($request)
    $script:process.StandardInput.Flush()
    $read = $script:process.StandardOutput.ReadLineAsync()
    if (!$read.Wait(20000)) { throw "MCP timeout: $Method" }
    Check ($null -ne $read.Result) 'Unexpected MCP EOF'
    $response = $read.Result | ConvertFrom-Json
    Check ($response.id -eq $script:requestId) 'Response ID mismatch'
    Check (!$response.error) "RPC error: $($read.Result)"
    return $response.result
}
function Tool([string]$Name, $Arguments, [switch]$ExpectError) {
    $result = Rpc 'tools/call' @{name="pixelforge_$Name"; arguments=$Arguments}
    Check ([bool]$result.isError -eq [bool]$ExpectError) "Unexpected tool status: $($result | ConvertTo-Json -Depth 5 -Compress)"
    $script:lastContent = $result.content
    return ($result.content[0].text | ConvertFrom-Json)
}
function Start-Server {
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $Executable
    $info.Arguments = '--mcp'
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Hidden
    $info.RedirectStandardInput = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.StandardInputEncoding = [System.Text.UTF8Encoding]::new($false)
    $info.StandardOutputEncoding = [System.Text.UTF8Encoding]::new($false)
    $script:process = [System.Diagnostics.Process]::Start($info)
    $init = Rpc 'initialize' @{protocolVersion='2025-11-25'; capabilities=@{}; clientInfo=@{name='PixelForge test'; version='1'}}
    Check ($init.serverInfo.name -eq 'PixelForge') 'Initialization failed'
    $script:process.StandardInput.WriteLine('{"jsonrpc":"2.0","method":"notifications/initialized"}')
}
function Stop-Server {
    if ($script:process -and !$script:process.HasExited) {
        $script:process.StandardInput.Close()
        if (!$script:process.WaitForExit(5000)) { $script:process.Kill(); throw 'Server did not exit on stdin EOF' }
        Check ($script:process.ExitCode -eq 0) 'Server exited with an error'
    }
}
try {
    Start-Server
    $catalog = Rpc 'tools/list' @{}
    Check ($catalog.tools.Count -eq 6) 'Expected six tools on a single JSONL line'
    $null = Tool task @{action='get'} -ExpectError
    $task = Tool task @{action='begin'; prompt='Draw a pixel gem: Grüne Farbe 🎨'}
    $id = $task.task_id
    $state = Tool task @{action='get'}
    Check ($state.prompt -like '*Grüne*') 'Unicode prompt lost'
    $null = Tool task @{action='accept'; task_id=$id; width=4294967328; height=32} -ExpectError
    $state = Tool task @{action='accept'; task_id=$id; width=32; height=32}
    $rev = $state.revision
    $null = Tool palette @{action='set'; colors='00000000,FF152B42,FF247DA6,FF4BDACB,FFDFFFF7'}
    $patch = 'R,10,6,12,20,1;R,6,10,20,12,1;R,9,9,14,14,2;R,12,7,8,18,2;L,10,10,20,20,3;H,12,10,7,4;V,11,11,6,3;P,10,11,4'
    $state = Tool edit @{task_id=$id; expected_revision=$rev; patch=$patch}
    $oldRev = $rev; $rev = $state.revision
    Check ($rev -eq $oldRev + 1) 'Batch revision must increment once'
    $null = Tool edit @{task_id=$id; expected_revision=$oldRev; patch='P,0,0,1'} -ExpectError
    $null = Tool edit @{task_id=$id; expected_revision=$rev; patch='P,0,0,1;L,0,0,-2147483648,0,2'} -ExpectError
    $region = Tool view @{action='inspect'; task_id=$id; expected_revision=$rev; x=0; y=0; width=1; height=1}
    Check ($region.rle -eq '1:0') 'Invalid batch changed the canvas'
    $noop = Tool edit @{task_id=$id; expected_revision=$rev; patch='P,0,0,1;P,0,0,0'}
    Check ($noop.revision -eq $rev -and $noop.changed_pixels -eq 0) 'Net no-op changed revision'
    $null = Tool view @{action='render'; task_id=$id; expected_revision=$rev; x=2147483647; width=2; height=2} -ExpectError
    $render = Tool view @{action='render'; task_id=$id; expected_revision=$rev; scale=8}
    Check ($script:lastContent[1].mimeType -eq 'image/png') 'Render image missing'
    $cached = Tool view @{action='render'; task_id=$id; expected_revision=$rev; scale=8; known_observation=$render.observation}
    Check ($cached.unchanged -and $script:lastContent.Count -eq 1) 'Observation caching failed'
    $state = Tool history @{action='undo'; task_id=$id; expected_revision=$rev}; $rev = $state.revision
    $state = Tool history @{action='redo'; task_id=$id; expected_revision=$rev}; $rev = $state.revision
    $null = Tool task @{action='finish'; task_id=$id; expected_revision=$rev; summary='Gem complete'}
    $null = Tool view @{action='render'; task_id=$id; expected_revision=$rev; scale=1}
    $path = Join-Path $OutputDirectory 'mcp-gem.png'
    $null = Tool io @{action='export'; task_id=$id; expected_revision=$rev; path=$path}
    $bitmap = [System.Drawing.Bitmap]::new($path)
    try {
        Check ($bitmap.Width -eq 32 -and $bitmap.Height -eq 32) 'Export dimensions changed'
        Check ($bitmap.GetPixel(0,0).A -eq 0) 'PNG lost transparency'
        Check ($bitmap.GetPixel(12,10).ToArgb() -eq [System.Drawing.ColorTranslator]::FromHtml('#DFFFF7').ToArgb()) 'PNG color mismatch'
    } finally { $bitmap.Dispose() }
    $null = Tool edit @{task_id=$id; expected_revision=$rev; patch='P,0,0,1'} -ExpectError
    $ref = Tool task @{action='begin'; prompt='Use reference'; content_reference=$path; style_reference=$path}
    $movedPath = Join-Path $OutputDirectory 'mcp-gem-loaded.png'
    Move-Item -LiteralPath $path -Destination $movedPath -Force
    try { $reference = Tool view @{action='content_reference'; task_id=$ref.task_id} }
    finally { Move-Item -LiteralPath $movedPath -Destination $path -Force }
    Check ($reference.width -eq 32 -and $script:lastContent[1].type -eq 'image') 'Reference delivery failed'
    $null = Tool task @{action='reject'; task_id=$ref.task_id; reason='Test rejection'}
    Stop-Server
    # A new process starts task/revision counters over. Its render must not
    # accidentally load the previous process's on-disk observation.
    Start-Server
    $state = Tool task @{action='begin'; prompt='Different canvas'}; $id = $state.task_id
    $state = Tool task @{action='accept'; task_id=$id; width=32; height=32}
    $state = Tool edit @{task_id=$id; expected_revision=$state.revision; patch='R,0,0,32,32,#FFFF0000'}
    $render2 = Tool view @{action='render'; task_id=$id; expected_revision=$state.revision; scale=8}
    Check ($render2.observation -ne $render.observation) 'Cross-session render collision'
    $bitmap = [System.Drawing.Bitmap]::new($render2.path)
    try { Check ($bitmap.GetPixel(0,0).R -eq 255 -and $bitmap.GetPixel(0,0).A -eq 255) 'Stale cached PNG' } finally { $bitmap.Dispose() }
    Stop-Server
    Write-Output 'MCP integration passed: lifecycle, framing, patches, revisions, history, caching, references and transparent PNG export.'
} finally {
    if ($script:process -and !$script:process.HasExited) { $script:process.Kill(); $script:process.WaitForExit() }
}
