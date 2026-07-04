$authPath = "$env:USERPROFILE\.codex\auth.json"
$auth = Get-Content $authPath -Raw | ConvertFrom-Json

$token = $auth.tokens.access_token
$accountId = $auth.tokens.account_id

$headers = @{
  "Authorization" = "Bearer $token"
  "ChatGPT-Account-ID" = $accountId
  "Accept" = "application/json"
  "Origin" = "https://chatgpt.com"
  "Referer" = "https://chatgpt.com/codex"
  "originator" = "Codex Desktop"
}

$usage = Invoke-RestMethod `
  -Uri "https://chatgpt.com/backend-api/wham/usage" `
  -Headers $headers `
  -Method Get

$ledger = Invoke-RestMethod `
  -Uri "https://chatgpt.com/backend-api/wham/rate-limit-reset-credits" `
  -Headers $headers `
  -Method Get

"`nUSAGE SUMMARY"
$usage.rate_limit_reset_credits | Format-List *

"`nRESET CREDIT LEDGER"
$ledger.credits | Select-Object `
  title,
  status,
  granted_at,
  expires_at |
  Format-Table -AutoSize

$banked = $usage.rate_limit_reset_credits.available_count
$credits = @($ledger.credits)

$ledgerUsable = @(
  $credits | Where-Object {
    $_.status -match "available|pending|active|granted"
  }
).Count

"`nCOUNTS"
[pscustomobject]@{
  BankedFromUsage = $banked
  LedgerTotal = $credits.Count
  LedgerUsable = $ledgerUsable
  NotReflectedYet = [Math]::Max($ledgerUsable - $banked, 0)
} | Format-List