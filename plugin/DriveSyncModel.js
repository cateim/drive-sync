.pragma library

// Pure logic of the DriveSync bar widget. The state itself is decided by the
// app (`drive-sync status --json --all`, version 3); this file only checks
// what the app printed and turns it into what the bar shows, and builds every
// command the widget runs. No QML here, so node:test covers it.

var MAX_ERROR_LENGTH = 200
var EPOCH_MAX = 99999999999

// Every status the app can print (Backend::summarize), and its tones.
var STATUSES = ["ok", "running", "resync", "paused", "failed", "interrupted", "skipped", "timerOff",
                "stale", "noDrive", "noRuns", "noLog", "notLoaded", "unavailable", "conflicts",
                "fixing", "fixWaiting", "fixFailed", "configError"]
var TONES = ["ok", "busy", "warning", "error"]
var OUTCOMES = ["success", "failed", "interrupted"]

// One glyph per bar state, Material Design Icons from the Nerd Font.
var GLYPHS = {
  ok: 0xF0160,        // cloud-check
  syncing: 0xF063F,   // cloud-sync
  resync: 0xF063F,    // cloud-sync
  paused: 0xF18F6,    // weather-cloudy-clock: no cloud-pause in the font
  warning: 0xF09E0,   // cloud-alert
  failing: 0xF09E0,   // cloud-alert
  broken: 0xF09E0,    // cloud-alert
  timerOff: 0xF0164,  // cloud-off-outline
  unknown: 0xF0164    // cloud-off-outline
}

var URGENT_STATES = ["failing", "broken", "timerOff"]
var DIMMED_STATES = ["syncing", "resync", "unknown"]

// Actions in the panel, in cursor order.
var ACTIONS = ["sync", "pause", "log", "folder"]

function isObject(value) {
  return value !== null && typeof value === "object" && !Array.isArray(value)
}

// The contract prints times as integers; anything else is "unknown" (0).
function toEpoch(value) {
  return typeof value === "number" && isFinite(value) && Math.floor(value) === value &&
         value > 0 && value <= EPOCH_MAX ? value : 0
}

// Identifiers such as "lostListings" or "listing"; anything else is empty.
function toKey(value) {
  return typeof value === "string" && /^[A-Za-z][A-Za-z-]{0,31}$/.test(value) ? value : ""
}

function oneOf(value, list, fallback) {
  return typeof value === "string" && list.indexOf(value) !== -1 ? value : fallback
}

// Log text for display: no control characters, one line, bounded.
function cleanText(value) {
  var text = String(value === undefined || value === null ? "" : value)
  text = text.replace(/[\u0000-\u001f\u007f]+/g, " ").replace(/\s+/g, " ").trim()
  if (text.length > MAX_ERROR_LENGTH) text = text.substring(0, MAX_ERROR_LENGTH - 1) + "…"
  return text
}

function parseJson(raw) {
  try {
    return JSON.parse(String(raw === undefined || raw === null ? "" : raw))
  } catch (e) {
    return null
  }
}

// The keys version 2 and each account of version 3 share, checked one by one
// against what the app prints; anything else becomes empty, false or 0.
function readFields(d) {
  var lastRun = isObject(d.lastRun) ? d.lastRun : {}
  var currentRun = isObject(d.currentRun) ? d.currentRun : {}
  var warnings = []
  if (Array.isArray(d.warnings)) {
    for (var i = 0; i < d.warnings.length && warnings.length < 8; i++) {
      var warning = toKey(d.warnings[i])
      if (warning !== "") warnings.push(warning)
    }
  }
  return {
    status: oneOf(d.status, STATUSES, "unknown"),
    tone: oneOf(d.tone, TONES, "unknown"),
    causeKind: toKey(d.causeKind),
    causeDetail: cleanText(d.causeDetail),
    warnings: warnings,
    running: d.running === true,
    resync: d.resync === true,
    canSyncNow: d.canSyncNow === true,
    lastSuccess: toEpoch(d.lastSuccess),
    lastRunStart: toEpoch(lastRun.start),
    lastRunEnd: toEpoch(lastRun.end),
    lastRunOutcome: oneOf(lastRun.outcome, OUTCOMES, ""),
    currentRunStart: toEpoch(currentRun.start),
    phase: toKey(currentRun.phase),
    nextRun: toEpoch(d.nextRun),
    failingSince: toEpoch(d.failingSince),
    typicalSeconds: count(d.typicalSeconds),
    conflicts: count(d.conflicts),
    managed: d.managed === true,
    paused: d.paused === true,
    pausedUntil: toEpoch(d.pausedUntil)
  }
}

function count(value) {
  return typeof value === "number" && isFinite(value) && value > 0 ? Math.min(Math.floor(value), 999999) : 0
}

// ---- Version 3: every account (`drive-sync status --json --all`) ----

// Ids as the app writes them (account1 to account99). Only an id that passes
// this ever goes into a command line; the view with no account has "".
var ACCOUNT_ID = /^account([1-9][0-9]?)$/
// A panel with more rows than this would not fit on a screen; the app's own
// limit is lower than the regex allows.
var MAX_ACCOUNTS = 8
var MAX_LABEL_LENGTH = 64
// The keys each account of version 3 has, every time.
var ACCOUNT_KEYS = ["status", "tone", "causeKind", "causeDetail", "warnings", "running", "resync", "canSyncNow",
                    "lastSuccess", "lastRun", "currentRun", "nextRun", "failingSince", "typicalSeconds",
                    "conflicts", "managed", "paused", "pausedUntil", "id", "label", "folder"]

function isAccountId(id) {
  return typeof id === "string" && ACCOUNT_ID.test(id)
}

// "" (the view with no account) or an account id; anything else is no id at all.
function isIdOrView(id) {
  return id === "" || isAccountId(id)
}

// A folder the panel may open: absolute, with no control character, so it
// can never be read as an option or break a line.
function toFolder(value) {
  return typeof value === "string" && value.charAt(0) === "/" && !/[\u0000-\u001f\u007f]/.test(value) ? value : ""
}

function toLabel(value) {
  var text = cleanText(value)
  return text.length > MAX_LABEL_LENGTH ? text.substring(0, MAX_LABEL_LENGTH - 1) + "…" : text
}

// One account of version 3, or null when its id is not one the app writes:
// such an entry is dropped whole, so its id can never reach a command.
function parseAccount(value) {
  if (!isObject(value) || !isIdOrView(value.id)) return null
  var fields = readFields(value)
  fields.id = value.id
  fields.label = toLabel(value.label)
  fields.folder = toFolder(value.folder)
  return fields
}

// What `drive-sync status --json --all` printed. Only version 3 with a clock
// and a list is a status; each account carries `ok` and the clock, so the
// one-snapshot helpers (barState, canSyncNow, canPause...) work on it.
function parseStatus(raw) {
  var data = parseJson(raw)
  var ok = isObject(data) && data.version === 3 && toEpoch(data.now) > 0 && Array.isArray(data.accounts)
  var now = ok ? toEpoch(data.now) : 0
  var accounts = []
  var seen = {}
  if (ok) {
    for (var i = 0; i < data.accounts.length && accounts.length < MAX_ACCOUNTS; i++) {
      var entry = parseAccount(data.accounts[i])
      // One id twice would be one account shown twice, acting as one.
      if (!entry || seen["id:" + entry.id]) continue
      seen["id:" + entry.id] = true
      entry.ok = true
      entry.now = now
      accounts.push(entry)
    }
  }
  var worst = -1
  for (var j = 0; ok && j < accounts.length; j++) {
    if (accounts[j].id === data.worst) { worst = j; break }
  }
  if (worst < 0 && accounts.length > 0) worst = 0
  return {
    ok: ok,
    now: now,
    tone: oneOf(ok ? data.tone : "", TONES, "unknown"),
    worst: worst,
    canSyncNow: ok && data.canSyncNow === true,
    accounts: accounts
  }
}

// What the icon shows: the account the app named worst; with nothing at all,
// "drive-sync has not run yet", as version 2 said; with no answer, unknown.
function shownAccount(status) {
  if (!status || !status.ok) {
    var unknown = readFields({})
    unknown.ok = false
    unknown.now = 0
    unknown.id = ""
    unknown.label = ""
    unknown.folder = ""
    return unknown
  }
  if (status.worst >= 0 && status.worst < status.accounts.length) return status.accounts[status.worst]
  var none = readFields({ status: "noLog", tone: "error" })
  none.ok = true
  none.now = status.now
  none.id = ""
  none.label = ""
  none.folder = ""
  return none
}

function accountsOf(status) {
  return status && status.ok && Array.isArray(status.accounts) ? status.accounts : []
}

function some(status, test) {
  var accounts = accountsOf(status)
  for (var i = 0; i < accounts.length; i++) if (test(accounts[i])) return true
  return false
}

function anyCanSync(status) { return some(status, canSyncNow) }
function anyPaused(status) { return some(status, canResume) }
function anyCanPause(status) { return some(status, canPause) }
function anyBusy(status) { return some(status, function(a) { return a.running || a.resync }) }

// The commands the widget runs. Each puts `--account <id>` right after the
// command word (the app reads it only there) and returns null for an id that
// is neither "" nor an account id, so nothing else reaches argv.
function accountArgs(id) {
  return isAccountId(id) ? ["--account", id] : []
}

function syncCommand(id) {
  return isIdOrView(id) ? ["drive-sync", "sync"].concat(accountArgs(id)) : null
}

// One hour from the bar: a pause with no end is never one key away.
function pauseCommand(id) {
  return isIdOrView(id) ? ["drive-sync", "pause"].concat(accountArgs(id), ["--minutes", "60"]) : null
}

function resumeCommand(id) {
  return isIdOrView(id) ? ["drive-sync", "resume"].concat(accountArgs(id)) : null
}

function pauseAllCommand() {
  return ["drive-sync", "pause", "--minutes", "60"]
}

function resumeAllCommand() {
  return ["drive-sync", "resume"]
}

// One `sync` per account that can: the plain `drive-sync sync` stops at the
// first account that refuses (a paused one), even when another one started.
function syncCommands(status) {
  var commands = []
  var accounts = accountsOf(status)
  for (var i = 0; i < accounts.length; i++) {
    var command = canSyncNow(accounts[i]) ? syncCommand(accounts[i].id) : null
    if (command) commands.push(command)
  }
  return commands
}

// open-history.sh, with the account to open the window on.
function openArgs(script, id) {
  return isIdOrView(id) ? ["bash", String(script)].concat(isAccountId(id) ? [id] : []) : null
}

// The folder the folder button opens: the account's own, which the app
// printed. ~/Drive only when no account is known at all (none, or no
// answer), as 5.1 did; an account whose folder was refused opens nothing,
// never another folder.
function folderToOpen(status, folder, home) {
  var own = toFolder(folder)
  if (own !== "") return own
  if (accountsOf(status).length > 0) return ""
  var base = String(home === undefined || home === null ? "" : home).replace(/\/+$/, "")
  return toFolder(base) !== "" ? base + "/Drive" : ""
}

// A translated text with its %1, %2... filled in one pass. QML's arg() fills
// the lowest placeholder left, so a label (a folder name) holding "%2" would
// get the next value put inside it.
function fillText(template, values) {
  return String(template === undefined || template === null ? "" : template).replace(/%([1-9])/g, function(match, n) {
    var value = values[Number(n) - 1]
    return value === undefined ? match : String(value)
  })
}

// A folder as the panel names it: the home as "~".
function displayFolder(folder, home) {
  var text = String(folder === undefined || folder === null ? "" : folder)
  var base = String(home === undefined || home === null ? "" : home).replace(/\/+$/, "")
  if (base === "") return text
  if (text === base) return "~"
  return text.indexOf(base + "/") === 0 ? "~" + text.substring(base.length) : text
}

// What the panel's cursor walks over. With one account (or none) it is 5.1's
// row of icons; from two on, each account's row (its header, which opens the
// window on it, then sync, pause and folder), then the general row.
function panelTargets(status) {
  var targets = []
  var accounts = accountsOf(status)
  if (accounts.length < 2) {
    for (var a = 0; a < ACTIONS.length; a++) targets.push({ account: -1, action: ACTIONS[a] })
    return targets
  }
  var perAccount = ["open", "sync", "pause", "folder"]
  for (var i = 0; i < accounts.length; i++)
    for (var k = 0; k < perAccount.length; k++) targets.push({ account: i, action: perAccount[k] })
  var general = ["sync", "pause", "log"]
  for (var g = 0; g < general.length; g++) targets.push({ account: -1, action: general[g] })
  return targets
}

function stepTarget(index, delta, count) {
  if (count <= 0) return 0
  return ((index + delta) % count + count) % count
}

// The account under the cursor, or -1 (no cursor, or a general target).
function cursorAccount(targets, cursorActive, index) {
  if (!cursorActive || !targets || index < 0 || index >= targets.length) return -1
  return targets[index].account
}

// The icon state on the bar, from the app's status and tone.
function barState(snapshot) {
  if (!snapshot || !snapshot.ok) return "unknown"
  if (snapshot.status === "timerOff") return "timerOff"
  if (snapshot.status === "paused") return "paused"
  switch (snapshot.tone) {
    case "ok": return "ok"
    // A fix rebuilds the base, as a resync does.
    case "busy": return snapshot.status === "resync" || snapshot.status === "fixing" ? "resync" : "syncing"
    case "warning": return "warning"
    case "error": return snapshot.status === "failed" ? "failing" : "broken"
  }
  return "unknown"
}

// Whether the app named this warning (readFields keeps at most 8).
function hasWarning(snapshot, key) {
  return !!snapshot && snapshot.ok && Array.isArray(snapshot.warnings) && snapshot.warnings.indexOf(key) !== -1
}

// The app knows whether a manual run makes sense; the widget just asks.
function canSyncNow(snapshot) {
  return !!snapshot && snapshot.ok && snapshot.canSyncNow
}

// A pause needs DriveSync's own units: the old ones never look at the mark.
function canPause(snapshot) {
  return !!snapshot && snapshot.ok && snapshot.managed && !snapshot.paused && !snapshot.resync
}

function canResume(snapshot) {
  return !!snapshot && snapshot.ok && snapshot.paused
}

// Seconds the last finished run took, 0 when it did not finish or is unknown.
function lastRunSeconds(snapshot) {
  if (!snapshot || snapshot.lastRunStart === 0 || snapshot.lastRunEnd < snapshot.lastRunStart) return 0
  return snapshot.lastRunEnd - snapshot.lastRunStart
}

function glyph(state) {
  var code = GLYPHS[state] || GLYPHS.unknown
  return String.fromCodePoint(code)
}

function isUrgent(state) {
  return URGENT_STATES.indexOf(state) !== -1
}

// Conflicts, a late or skipped run: worth a look, not an alarm.
function isWarning(state) {
  return state === "warning"
}

function isDimmed(state) {
  return DIMMED_STATES.indexOf(state) !== -1
}

// Age in the coarsest unit that still reads well: { unit, value }.
function ageParts(seconds) {
  var s = Math.max(0, Math.floor(Number(seconds) || 0))
  if (s < 60) return { unit: "now", value: 0 }
  if (s < 3600) return { unit: "minutes", value: Math.floor(s / 60) }
  if (s < 86400) return { unit: "hours", value: Math.floor(s / 3600) }
  return { unit: "days", value: Math.floor(s / 86400) }
}

function clampInt(value, fallback, min, max) {
  var n = parseInt(String(value), 10)
  if (!isFinite(n)) n = fallback
  return Math.max(min, Math.min(max, n))
}

function stepAction(index, delta) {
  var n = ACTIONS.length
  return ((index + delta) % n + n) % n
}

// Clock times show 12 or 24 hours like the clock on the Omarchy bar, the
// same rule as the app's (src/clockformat.cpp).

// "12" with an AM/PM marker and an "h", "24" with any other hour, "" with no
// hour at all. Quoted text is literal, so "'at' HH:mm" is "24".
function hoursInFormat(format) {
  var text = String(format === undefined || format === null ? "" : format)
  var quoted = false
  var twelveHour = false
  var anyHour = false
  var amPm = false
  for (var i = 0; i < text.length; i++) {
    var c = text.charAt(i)
    if (c === "'") {
      if (text.charAt(i + 1) === "'") i++
      else quoted = !quoted
      continue
    }
    if (quoted) continue
    if (c === "h") {
      twelveHour = true
      anyHour = true
    } else if (c === "H") {
      anyHour = true
    } else if (c === "a" || c === "A") {
      amPm = true
    }
  }
  if (!anyHour) return ""
  return amPm && twelveHour ? "12" : "24"
}

// The first entry with the clock's id anywhere in a bar layout.
function findClock(value) {
  if (!value || typeof value !== "object") return null
  if (!Array.isArray(value) && value.id === "omarchy.clock") return value
  for (var key in value) {
    var found = findClock(value[key])
    if (found) return found
  }
  return null
}

// The hours of the bar clock, from the text of shell.json files in order of
// precedence (the user's, then Omarchy's defaults): the layout comes from the
// first file that has one. `vertical` is the bar's own and picks the vertical
// format. "" when no layout has a clock.
function barClockHours(texts, vertical) {
  var layout = null
  for (var i = 0; i < texts.length; i++) {
    var bar = null
    try {
      var parsed = JSON.parse(String(texts[i]))
      bar = parsed && typeof parsed === "object" ? parsed.bar : null
    } catch (e) {
      continue
    }
    if (bar && typeof bar === "object" && !Array.isArray(bar) && "layout" in bar) {
      layout = bar.layout
      break
    }
  }
  var clock = findClock(layout)
  if (!clock) return ""
  // Omarchy's own defaults, for an entry that names no format, are both 24 hours.
  var format = vertical ? clock.verticalFormat : clock.format
  if (typeof format !== "string") format = vertical ? "HH mm" : "dddd HH:mm"
  return hoursInFormat(format)
}

// The Qt format for a clock time, or the fallback when the bar says nothing.
function clockTimeFormat(hours, fallback) {
  if (hours === "24") return "HH:mm"
  if (hours === "12") return "h:mm AP"
  return fallback
}
