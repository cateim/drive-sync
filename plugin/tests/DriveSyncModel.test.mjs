// Run from the plugin directory: node --test tests/*.test.mjs
import { test } from "node:test"
import assert from "node:assert/strict"
import { readFileSync, readdirSync, writeFileSync, mkdtempSync, mkdirSync, chmodSync, symlinkSync, existsSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { execFileSync } from "node:child_process"
import vm from "node:vm"
import { fileURLToPath } from "node:url"
import { isDeepStrictEqual } from "node:util"

// The model is a QML `.pragma library` script; drop the pragma and load it
// into a sandbox so node sees the same functions QML does.
const source = readFileSync(new URL("../DriveSyncModel.js", import.meta.url), "utf8")
const model = {}
vm.runInNewContext(source.replace(/^\.pragma library\s*$/m, ""), model)

const STATUSES = ["ok", "running", "resync", "paused", "failed", "interrupted", "skipped", "timerOff",
                  "stale", "noDrive", "noRuns", "noLog", "notLoaded", "unavailable", "conflicts",
                  "fixing", "fixWaiting", "fixFailed", "configError"]

// What `drive-sync status --json` prints with drive-sync in sync (contract version 2).
const HEALTHY = {
  version: 2, now: 1794076940, status: "ok", tone: "ok",
  causeKind: "", causeDetail: "", warnings: [],
  running: false, resync: false, canSyncNow: true,
  lastSuccess: 1794076584, lastRun: { start: 1794076098, end: 1794076584, outcome: "success" },
  currentRun: { start: 0, phase: "" }, nextRun: 1794076998,
  failingSince: 0, typicalSeconds: 480, conflicts: 0,
  managed: true, paused: false, pausedUntil: 0
}
const json = (overrides = {}) => JSON.stringify({ ...HEALTHY, ...overrides })

// One account as version 3 prints it: version 2's keys minus version and now,
// plus id, label and folder.
const ACCOUNT = {
  status: "ok", tone: "ok", causeKind: "", causeDetail: "", warnings: [],
  running: false, resync: false, canSyncNow: true,
  lastSuccess: 1794076584, lastRun: { start: 1794076098, end: 1794076584, outcome: "success" },
  currentRun: { start: 0, phase: "" }, nextRun: 1794076998,
  failingSince: 0, typicalSeconds: 480, conflicts: 0,
  managed: true, paused: false, pausedUntil: 0,
  id: "account1", label: "Drive", folder: "/home/u/Drive"
}
const account = (overrides = {}) => ({ ...ACCOUNT, ...overrides })
const v3 = (accounts, top = {}) => JSON.stringify({
  version: 3, now: 1794076940, tone: "ok", worst: accounts.length ? accounts[0].id : "",
  canSyncNow: accounts.some((a) => a.canSyncNow), accounts, ...top
})
const two = (first = {}, second = {}) => model.parseStatus(v3([
  account(first), account({ id: "account2", label: "Drive2", folder: "/home/u/Drive2", ...second })]))
const plain = (value) => JSON.parse(JSON.stringify(value))

// What the icon shows for one account with these fields, and for raw output.
const shown = (overrides = {}) => model.shownAccount(model.parseStatus(v3([account(overrides)])))
const shownRaw = (raw) => model.shownAccount(model.parseStatus(raw))

test("the shown account reads a healthy account", () => {
  const s = shown()
  assert.equal(s.ok, true)
  assert.equal(s.status, "ok")
  assert.equal(s.tone, "ok")
  assert.equal(s.lastSuccess, 1794076584)
  assert.equal(s.lastRunStart, 1794076098)
  assert.equal(s.lastRunEnd, 1794076584)
  assert.equal(s.lastRunOutcome, "success")
  assert.equal(s.nextRun, 1794076998)
  assert.equal(s.canSyncNow, true)
  assert.equal(s.conflicts, 0)
  assert.equal(s.causeDetail, "")
})

test("the shown account rejects non-JSON, other versions and a missing clock", () => {
  assert.equal(shownRaw("now=1794076940").ok, false)
  // Version 1 has no pause, version 2 one account: an older drive-sync must not look like either.
  assert.equal(shownRaw(json({ version: 1 })).ok, false)
  assert.equal(shownRaw(json()).ok, false)
  assert.equal(shownRaw(v3([account()], { now: 0 })).ok, false)
  assert.equal(shownRaw("[1, 2]").ok, false)
  assert.equal(shownRaw(null).ok, false)
})

test("an account never trusts field shapes", () => {
  const s = shown({
    lastSuccess: "1794076584", nextRun: -5, failingSince: 12.5,
    status: { evil: true }, tone: "rainbow", warnings: "timerOff",
    running: "true", canSyncNow: 1, conflicts: "7", lastRun: "yesterday"
  })
  assert.equal(s.lastSuccess, 0)
  assert.equal(s.nextRun, 0)
  assert.equal(s.failingSince, 0)
  assert.equal(s.status, "unknown")
  assert.equal(s.tone, "unknown")
  assert.deepEqual([...s.warnings], [])
  assert.equal(s.running, false)
  assert.equal(s.canSyncNow, false)
  assert.equal(s.conflicts, 0)
  assert.equal(s.lastRunStart, 0)
  assert.equal(shown({ warnings: ["timerDisabled", "<b>x</b>", 3] }).warnings.length, 1)
})

test("causeDetail keeps markup inert on one bounded line", () => {
  const markup = '<img src="https://example.invalid/tracker"> <b>Path1</b>'
  // The QML sinks are PlainText; the model must not drop or alter the characters.
  assert.equal(shown({ causeDetail: markup }).causeDetail, markup)
  assert.equal(shown({ causeDetail: "x".repeat(500) }).causeDetail.length, 200)
  assert.equal(model.cleanText("a\u0000b\u001b[31mc\r\nd"), "a b [31mc d")
})

test("barState maps every status", () => {
  const expected = {
    ok: "ok", running: "syncing", resync: "resync", paused: "paused", failed: "failing",
    interrupted: "warning", skipped: "warning", stale: "warning", noRuns: "warning", conflicts: "warning",
    timerOff: "timerOff", noDrive: "broken", noLog: "broken", notLoaded: "broken", unavailable: "broken",
    fixing: "resync", fixWaiting: "warning", fixFailed: "broken", configError: "broken"
  }
  const tones = {
    ok: "ok", running: "busy", resync: "busy", paused: "warning", failed: "error", interrupted: "warning",
    skipped: "warning", stale: "warning", noRuns: "warning", conflicts: "warning", timerOff: "error",
    noDrive: "error", noLog: "error", notLoaded: "error", unavailable: "error",
    fixing: "busy", fixWaiting: "warning", fixFailed: "error", configError: "error"
  }
  for (const status of STATUSES) {
    assert.equal(model.barState(shown({ status, tone: tones[status] })), expected[status], status)
  }
  assert.equal(model.barState(shown({ status: "", tone: "" })), "unknown")
})

test("a missing or old drive-sync shows unknown", () => {
  assert.equal(model.barState(shownRaw("")), "unknown")
  assert.equal(model.barState(shownRaw("usage: drive-sync [open]")), "unknown")
})

test("canSyncNow follows the app", () => {
  assert.equal(model.canSyncNow(shown()), true)
  assert.equal(model.canSyncNow(shown({ canSyncNow: false })), false)
  assert.equal(model.canSyncNow(shownRaw("")), false)
})

// Quickshell ends a process that failed to start without ending its output
// stream, and an old drive-sync would open a window instead of answering. Both
// must end as "unknown", never as the last good snapshot.
test("a poll that ends without output or hangs shows unknown", () => {
  const qml = readFileSync(new URL("../DriveSyncService.qml", import.meta.url), "utf8")
  assert.match(qml, /onRunningChanged:[^}]*applyStatus\(""\)/)
  const watchdog = qml.match(/id:\s*watchdog[\s\S]*?interval:\s*(\d+)/)
  assert.ok(watchdog, "no watchdog timer on the status poll")
  assert.ok(Number(watchdog[1]) > 35000, "the watchdog must outlast a cold find plus systemctl")
})

test("lastRunSeconds only for a finished run", () => {
  assert.equal(model.lastRunSeconds(shown()), 486)
  assert.equal(model.lastRunSeconds(shown({ lastRun: { start: 1794076098, end: 0, outcome: "interrupted" } })), 0)
})

// The late rule lives in the app now; the widget must not keep a copy.
test("the widget leaves the late rule to the app", () => {
  const manifest = JSON.parse(readFileSync(new URL("../manifest.json", import.meta.url), "utf8"))
  assert.equal(manifest.id, "drive-sync")
  assert.equal("staleAfterMin" in manifest.barWidget.defaults, false)
  assert.equal(manifest.barWidget.schema.some((s) => s.key === "staleAfterMin"), false)
  const qml = readFileSync(new URL("../DriveSync.qml", import.meta.url), "utf8")
  assert.doesNotMatch(qml, /staleAfter/)
})

// "Late" is three sync intervals, which only the app knows: an ok status
// whose last success is old (a 60 minute interval) stays ok on the bar.
test("late is the app's call: an ok snapshot an hour old stays ok", () => {
  const now = 1794076098
  const s = shownRaw(JSON.stringify({ version: 3, now: now, worst: "account1", accounts: [
    { id: "account1", status: "ok", tone: "ok", lastSuccess: now - 3600 }] }))
  assert.equal(s.status, "ok")
  assert.equal(model.barState(s), "ok")
})

test("helper scripts parse in bash", () => {
  execFileSync("bash", ["-n", new URL("../open-history.sh", import.meta.url).pathname])
})

test("glyphs map to the Nerd Font cloud icons", () => {
  assert.equal(model.glyph("ok").codePointAt(0), 0xF0160)
  assert.equal(model.glyph("syncing").codePointAt(0), 0xF063F)
  assert.equal(model.glyph("warning").codePointAt(0), 0xF09E0)
  assert.equal(model.glyph("failing").codePointAt(0), 0xF09E0)
  assert.equal(model.glyph("nonsense").codePointAt(0), 0xF0164)
  assert.equal(model.isUrgent("failing"), true)
  assert.equal(model.isUrgent("warning"), false)
  assert.equal(model.isDimmed("syncing"), true)
})

test("ageParts picks the coarsest readable unit", () => {
  const HOUR = 3600
  assert.deepEqual({ ...model.ageParts(30) }, { unit: "now", value: 0 })
  assert.deepEqual({ ...model.ageParts(125) }, { unit: "minutes", value: 2 })
  assert.deepEqual({ ...model.ageParts(3 * HOUR + 59) }, { unit: "hours", value: 3 })
  assert.deepEqual({ ...model.ageParts(50 * HOUR) }, { unit: "days", value: 2 })
  assert.deepEqual({ ...model.ageParts(-10) }, { unit: "now", value: 0 })
})

test("the action cursor and clampInt", () => {
  assert.deepEqual([...model.ACTIONS], ["sync", "pause", "log", "folder"])
  assert.equal(model.stepAction(0, -1), 3)
  assert.equal(model.stepAction(3, 1), 0)
  assert.equal(model.clampInt("abc", 30, 10, 600), 30)
  assert.equal(model.clampInt("5", 30, 10, 600), 10)
})

// Every Text in the plugin shows log or plugin text, so none may fall back to
// AutoText, where markup-shaped log content would render as rich text.
test("every QML Text sets textFormat: Text.PlainText", () => {
  const dir = new URL("../", import.meta.url)
  for (const name of readdirSync(dir).filter((f) => f.endsWith(".qml"))) {
    const qml = readFileSync(new URL(name, dir), "utf8")
    const blocks = qml.split(/\bText\s*\{/).slice(1)
    for (const block of blocks) {
      const body = block.slice(0, block.indexOf("}"))
      assert.match(body, /textFormat:\s*Text\.PlainText/, `${name}: Text without PlainText`)
    }
  }
})

test("every key used in QML exists in en.json and pt-BR.json", () => {
  const dir = new URL("../", import.meta.url)
  const en = JSON.parse(readFileSync(new URL("i18n/en.json", dir), "utf8"))
  const pt = JSON.parse(readFileSync(new URL("i18n/pt-BR.json", dir), "utf8"))
  for (const name of readdirSync(dir).filter((f) => f.endsWith(".qml"))) {
    const qml = readFileSync(new URL(name, dir), "utf8")
    for (const [, key] of qml.matchAll(/strings\.tn?\("([^"]+)"/g)) {
      // "status." + status and "age." + unit are built at runtime; checked below.
      if (key.endsWith(".")) continue
      assert.ok(key in en, `${name}: "${key}" missing in en.json`)
      assert.ok(key in pt, `${name}: "${key}" missing in pt-BR.json`)
    }
  }
  for (const status of [...STATUSES, "unknown"]) {
    for (const catalog of [en, pt]) assert.ok(`status.${status}` in catalog, `status.${status} missing`)
  }
  for (const unit of ["minutes", "hours", "days"]) {
    for (const catalog of [en, pt]) {
      const entry = catalog[`age.${unit}`]
      assert.ok(entry && entry.one && entry.other, `age.${unit} needs one and other`)
    }
  }
})

test("pause and resume follow the app", () => {
  const paused = shown({ status: "paused", tone: "warning", paused: true, pausedUntil: 1794080540, canSyncNow: false })
  assert.equal(paused.paused, true)
  assert.equal(paused.pausedUntil, 1794080540)
  assert.equal(model.barState(paused), "paused")
  assert.equal(model.glyph("paused").codePointAt(0), 0xF18F6)
  assert.equal(model.isUrgent("paused"), false)
  assert.equal(model.canResume(paused), true)
  assert.equal(model.canPause(paused), false)
  assert.equal(model.canSyncNow(paused), false)

  assert.equal(model.canPause(shown()), true)
  assert.equal(model.canResume(shown()), false)
  // The old units never look at the mark: no pause before a takeover.
  assert.equal(model.canPause(shown({ managed: false })), false)
  assert.equal(model.canPause(shown({ resync: true })), false)
  assert.equal(model.canPause(shownRaw("")), false)
  assert.equal(shown({ paused: "yes", pausedUntil: "soon" }).paused, false)
  assert.equal(shown({ pausedUntil: "soon" }).pausedUntil, 0)
})

// Every command the service runs comes from a model builder, which only
// ever puts a checked account id in argv; the status poll is a fixed list.
test("the widget changes drive-sync only through drive-sync", () => {
  const qml = readFileSync(new URL("../DriveSyncService.qml", import.meta.url), "utf8")
  assert.doesNotMatch(qml, /"systemctl"/)
  // The only drive-sync literal is the status poll.
  assert.deepEqual(qml.match(/"drive-sync"/g), ['"drive-sync"'])
  const assignments = [...qml.matchAll(/^\s*(?:\w+\.)?command\s*[:=]\s*(.+)$/gm)].map((m) => m[1].trim())
  assert.deepEqual(assignments.sort(), [
    '["drive-sync", "status", "--json", "--all"]',
    "command",
    "root.syncQueue[0]"])
  // What reaches those two names: the arguments of control() and startSyncs().
  const calls = [...qml.matchAll(/\b(?:root\.)?(control|startSyncs)\((?!command\)|commands\))([^\n]*)\)/g)]
  assert.ok(calls.length >= 6, "the actions call control() and startSyncs()")
  for (const [, , arg] of calls) assert.match(arg, /^\[?Model\.\w+\(/, arg)
  assert.match(qml, /function control\(command\)/)
  assert.match(qml, /function startSyncs\(commands\)/)
  // Opening goes through the model too: the window with a checked id, a checked folder.
  assert.match(qml, /var args = Model\.openArgs\(root\.historyPath, id\)\n\s*if \(args\) Quickshell\.execDetached\(args\)/)
  assert.match(qml, /Model\.folderToOpen\(root\.all, folder, root\.home\)/)
  assert.doesNotMatch(qml, /command\s*[:=][^\n]*\+/)
})

test("hoursInFormat reads the hour tokens outside quoted text", () => {
  assert.equal(model.hoursInFormat("ddd d MMM HH:mm"), "24")
  assert.equal(model.hoursInFormat("dddd h:mm:ss ap"), "12")
  assert.equal(model.hoursInFormat("h:mm AP"), "12")
  assert.equal(model.hoursInFormat("h:mm"), "24")  // Qt's h is 0 to 23 without AM/PM
  assert.equal(model.hoursInFormat("'at' HH:mm"), "24")
  assert.equal(model.hoursInFormat("'It''s' h:mm a"), "12")
  assert.equal(model.hoursInFormat("d MMMM 'W'ww yyyy"), "")
  assert.equal(model.hoursInFormat(undefined), "")
})

test("barClockHours follows the clock on the bar", () => {
  const user = JSON.stringify({ bar: { position: "top", layout: { center: [
    { id: "omarchy.clock", format: "ddd d MMM h:mm AP", verticalFormat: "HH\nmm" }] } } })
  assert.equal(model.barClockHours([user], false), "12")
  assert.equal(model.barClockHours([user], true), "24")  // a side bar shows the vertical format
  assert.equal(model.barClockHours([JSON.stringify({ bar: { layout: { right: [{ id: "omarchy.clock" }] } } })], false), "24")
  assert.equal(model.barClockHours([JSON.stringify({ bar: { layout: { left: [{ id: "omarchy.workspaces" }] } } })], false), "")
  assert.equal(model.barClockHours(["not json", ""], false), "")
  // A user file without a layout leaves it to Omarchy's defaults.
  const defaults = JSON.stringify({ bar: { layout: { center: [{ id: "omarchy.clock", format: "h:mm AP" }] } } })
  assert.equal(model.barClockHours([JSON.stringify({ idle: {} }), defaults], false), "12")
  assert.equal(model.barClockHours([user, defaults.replace("h:mm AP", "HH:mm")], false), "12")
})

test("clockTimeFormat falls back when the bar says nothing", () => {
  assert.equal(model.clockTimeFormat("24", "h:mm AP"), "HH:mm")
  assert.equal(model.clockTimeFormat("12", "HH:mm"), "h:mm AP")
  assert.equal(model.clockTimeFormat("", "HH:mm"), "HH:mm")
})

test("warnings get the window's orange on the bar and in the panel", () => {
  assert.equal(model.isWarning("warning"), true)
  for (const state of ["ok", "syncing", "paused", "failing", "broken", "timerOff", "unknown"])
    assert.equal(model.isWarning(state), false, state)
  const qml = readFileSync(new URL("../DriveSync.qml", import.meta.url), "utf8")
  const window = readFileSync(new URL("../../src/StatusPanel.qml", import.meta.url), "utf8")
  const orange = qml.match(/readonly property color warning: "(#[0-9a-f]{6})"/)
  assert.ok(orange, "the widget names its warning color")
  assert.match(window, new RegExp(`"warning" \\? "${orange[1]}"`))
  assert.match(qml, /activeColor: root\.stateColor/)
  assert.match(qml, /color: root\.stateColor/)
})

test("the failing line has a sentence for every cause the app names", () => {
  const qml = readFileSync(new URL("../DriveSync.qml", import.meta.url), "utf8")
  for (const kind of ["lostListings", "outOfSync", "timeout", "systemd", "filtersChanged", "fixInterrupted", "maxDelete",
                      "signIn", "firstSyncPending"])
    assert.match(qml, new RegExp(`case "${kind}": return strings\\.t\\("cause\\.${kind}"\\)`), kind)
  // A fix that stopped says why too, not only a failed run.
  assert.match(qml, /root\.syncState === "failing" \|\| root\.status\.status === "fixFailed"/)
  // An "error" with no line, or a kind this widget does not know, still says something.
  assert.match(qml, /status\.causeDetail !== "" \? status\.causeDetail : strings\.t\("cause\.unknown"\)/)
})

test("an expired or revoked sign-in asks to sign in again, never for a terminal", () => {
  for (const lang of ["en", "pt-BR"]) {
    const catalog = JSON.parse(readFileSync(new URL(`../i18n/${lang}.json`, import.meta.url), "utf8"))
    assert.ok("cause.signIn" in catalog, lang)
    assert.doesNotMatch(catalog["cause.signIn"], /reconnect/, lang)
  }
  const en = JSON.parse(readFileSync(new URL("../i18n/en.json", import.meta.url), "utf8"))
  assert.match(en["cause.signIn"], /sign in again/)
})

test("every status the app prints has a headline in both catalogs", () => {
  // The headline key is built at run time ("status." + status), so the
  // literal-key test above cannot see a missing one.
  for (const lang of ["en", "pt-BR"]) {
    const catalog = JSON.parse(readFileSync(new URL(`../i18n/${lang}.json`, import.meta.url), "utf8"))
    for (const status of STATUSES) assert.ok(`status.${status}` in catalog, `${lang}: status.${status}`)
  }
})

test("configError is a status the bar knows", () => {
  const broken = shown({ status: "configError", tone: "error", causeDetail: "config.json is not a JSON object" })
  assert.equal(broken.status, "configError")
  assert.equal(model.barState(broken), "broken")
  for (const lang of ["en", "pt-BR"]) {
    const catalog = JSON.parse(readFileSync(new URL(`../i18n/${lang}.json`, import.meta.url), "utf8"))
    assert.ok("status.configError" in catalog, lang)
    // The JSON names no unit, so the headline cannot either.
    assert.doesNotMatch(catalog["status.notLoaded"], /drive-sync\.service/, lang)
  }
})

test("the new causes have a text in both languages", () => {
  const qml = readFileSync(new URL("../DriveSync.qml", import.meta.url), "utf8")
  for (const kind of ["oldDriveSync", "otherAccount", "unchecked"]) {
    assert.match(qml, new RegExp(`case "${kind}": return strings\\.t\\("cause\\.${kind}"\\)`), kind)
    for (const lang of ["en", "pt-BR"]) {
      const catalog = JSON.parse(readFileSync(new URL(`../i18n/${lang}.json`, import.meta.url), "utf8"))
      assert.ok(`cause.${kind}` in catalog, `${lang}: cause.${kind}`)
    }
  }
})

test("oldDriveSync warning shows in the panel", () => {
  assert.equal(model.hasWarning(shown({ warnings: ["conflicts", "oldDriveSync"] }), "oldDriveSync"), true)
  assert.equal(model.hasWarning(shown({ warnings: ["conflicts"] }), "oldDriveSync"), false)
  assert.equal(model.hasWarning(shownRaw("not json"), "oldDriveSync"), false)
  assert.equal(model.hasWarning(null, "oldDriveSync"), false)
  const qml = readFileSync(new URL("../DriveSync.qml", import.meta.url), "utf8")
  assert.match(qml, /Model\.hasWarning\(root\.status, "oldDriveSync"\)/)
  assert.match(qml, /strings\.t\("warning\.oldDriveSync"\)/)
  for (const lang of ["en", "pt-BR"]) {
    const catalog = JSON.parse(readFileSync(new URL(`../i18n/${lang}.json`, import.meta.url), "utf8"))
    assert.ok(typeof catalog["warning.oldDriveSync"] === "string" && catalog["warning.oldDriveSync"] !== "", lang)
  }
})

// ---- Version 3: every account (`drive-sync status --json --all`) ----

test("parseStatus reads version 3", () => {
  const s = two({}, { status: "failed", tone: "error", canSyncNow: false })
  assert.equal(s.ok, true)
  assert.equal(s.now, 1794076940)
  assert.equal(s.accounts.length, 2)
  assert.equal(s.accounts[0].id, "account1")
  assert.equal(s.accounts[0].label, "Drive")
  assert.equal(s.accounts[0].folder, "/home/u/Drive")
  assert.equal(s.accounts[1].status, "failed")
  assert.equal(s.accounts[1].canSyncNow, false)
  assert.equal(s.canSyncNow, true)
  // Each account carries the clock, so the one-snapshot helpers work on it.
  assert.equal(s.accounts[1].ok, true)
  assert.equal(s.accounts[1].now, 1794076940)
  assert.equal(model.barState(s.accounts[1]), "failing")
})

test("parseStatus rejects version 2 and anything else", () => {
  assert.equal(model.parseStatus(json()).ok, false)
  assert.equal(model.parseStatus(v3([account()], { version: 4 })).ok, false)
  assert.equal(model.parseStatus(v3([account()], { now: 0 })).ok, false)
  assert.equal(model.parseStatus(v3([account()], { now: undefined })).ok, false)
  assert.equal(model.parseStatus(v3([account()], { accounts: { 0: ACCOUNT } })).ok, false)
  assert.equal(model.parseStatus("not json").ok, false)
  assert.equal(model.parseStatus("").ok, false)
  assert.equal(model.parseStatus(null).ok, false)
  assert.equal(model.parseStatus(v3([account()], { version: 2 })).accounts.length, 0)
  assert.equal(model.shownAccount(model.parseStatus("")).ok, false)
  assert.equal(model.barState(model.shownAccount(model.parseStatus(""))), "unknown")
})

test("every account keeps the 21 keys the app prints", () => {
  assert.deepEqual([...model.ACCOUNT_KEYS].sort(), [
    "canSyncNow", "causeDetail", "causeKind", "conflicts", "currentRun", "failingSince", "folder", "id",
    "label", "lastRun", "lastSuccess", "managed", "nextRun", "paused", "pausedUntil", "resync", "running",
    "status", "tone", "typicalSeconds", "warnings"])
  const full = {
    status: "paused", tone: "warning", causeKind: "maxDelete", causeDetail: "too many", warnings: ["conflicts"],
    running: true, resync: true, canSyncNow: true, lastSuccess: 1794070000,
    lastRun: { start: 1794071000, end: 1794071060, outcome: "failed" },
    currentRun: { start: 1794072000, phase: "listing" }, nextRun: 1794073000, failingSince: 1794071000,
    typicalSeconds: 300, conflicts: 4, managed: true, paused: true, pausedUntil: 1794074000,
    id: "account7", label: "Work", folder: "/srv/work"
  }
  assert.deepEqual(Object.keys(full).sort(), [...model.ACCOUNT_KEYS].sort())
  const a = model.parseAccount(full)
  assert.equal(a.status, "paused")
  assert.equal(a.tone, "warning")
  assert.equal(a.causeKind, "maxDelete")
  assert.equal(a.causeDetail, "too many")
  assert.deepEqual([...a.warnings], ["conflicts"])
  assert.equal(a.running, true)
  assert.equal(a.resync, true)
  assert.equal(a.canSyncNow, true)
  assert.equal(a.lastSuccess, 1794070000)
  assert.equal(a.lastRunStart, 1794071000)
  assert.equal(a.lastRunEnd, 1794071060)
  assert.equal(a.lastRunOutcome, "failed")
  assert.equal(a.currentRunStart, 1794072000)
  assert.equal(a.phase, "listing")
  assert.equal(a.nextRun, 1794073000)
  assert.equal(a.failingSince, 1794071000)
  assert.equal(a.typicalSeconds, 300)
  assert.equal(a.conflicts, 4)
  assert.equal(a.managed, true)
  assert.equal(a.paused, true)
  assert.equal(a.pausedUntil, 1794074000)
  assert.equal(a.id, "account7")
  assert.equal(a.label, "Work")
  assert.equal(a.folder, "/srv/work")
  // A label is display text: one line, bounded.
  assert.equal(model.parseAccount(account({ label: "a\nb" })).label, "a b")
  assert.ok(model.parseAccount(account({ label: "x".repeat(300) })).label.length <= 64)
})

test("the icon shows the account named worst", () => {
  const worst = model.parseStatus(v3([account(), account({ id: "account2", status: "failed", tone: "error" })],
                                     { worst: "account2", tone: "error" }))
  assert.equal(worst.worst, 1)
  assert.equal(model.shownAccount(worst).id, "account2")
  assert.equal(model.shownAccount(worst).ok, true)
  assert.equal(model.shownAccount(worst).now, 1794076940)
  assert.equal(model.barState(model.shownAccount(worst)), "failing")
  // The view with no account has an empty id, and so has worst.
  const view = model.parseStatus(v3([account({ id: "", managed: false })], { worst: "" }))
  assert.equal(view.worst, 0)
  assert.equal(model.shownAccount(view).id, "")
  const unknown = model.parseStatus(v3([account(), account({ id: "account2" })], { worst: "account9" }))
  assert.equal(unknown.worst, 0)
  assert.equal(model.shownAccount(unknown).id, "account1")
})

test("an empty list is noLog", () => {
  const s = model.parseStatus(v3([], { tone: "error", worst: "", canSyncNow: false }))
  assert.equal(s.ok, true)
  assert.equal(s.worst, -1)
  const shown = model.shownAccount(s)
  assert.equal(shown.ok, true)
  assert.equal(shown.status, "noLog")
  assert.equal(shown.tone, "error")
  assert.equal(shown.id, "")
  assert.equal(shown.folder, "")
  assert.equal(shown.lastSuccess, 0)
  assert.equal(model.barState(shown), "broken")
  assert.equal(model.canSyncNow(shown), false)
  for (const lang of ["en", "pt-BR"]) {
    const catalog = JSON.parse(readFileSync(new URL(`../i18n/${lang}.json`, import.meta.url), "utf8"))
    assert.ok("status.noLog" in catalog, lang)
  }
})

test("ids that are not account ids never reach a command", () => {
  const bad = ["account0", "account01", "account100", "Account1", "account1 ", "account1; rm -rf ~", "--json", 42, null,
               undefined, "account1\n"]
  for (const id of bad) {
    assert.equal(model.parseAccount(account({ id })), null, String(id))
    const s = model.parseStatus(v3([account({ id }), account({ id: "account2" })]))
    assert.deepEqual(plain(s.accounts.map((a) => a.id)), ["account2"], String(id))
    for (const build of [model.syncCommand, model.pauseCommand, model.resumeCommand])
      assert.equal(build(id), null, `${build.name}(${String(id)})`)
    assert.equal(model.openArgs("/x/open-history.sh", id), null, String(id))
  }
  for (const id of ["account1", "account9", "account10", "account99"])
    assert.notEqual(model.parseAccount(account({ id })), null, id)
  // Two entries with one id would be one account twice: the second goes.
  assert.equal(model.parseStatus(v3([account(), account()])).accounts.length, 1)
  // At most eight accounts are kept.
  const many = Array.from({ length: 12 }, (_, i) => account({ id: `account${i + 1}` }))
  assert.equal(model.parseStatus(v3(many)).accounts.length, model.MAX_ACCOUNTS)
  assert.equal(model.MAX_ACCOUNTS, 8)
})

test("a folder must be absolute", () => {
  for (const folder of ["Drive", "../x", "/ok\n", "/a\u0000b", 42, null, "", "~/Drive"])
    assert.equal(model.parseAccount(account({ folder })).folder, "", String(folder))
  assert.equal(model.parseAccount(account({ folder: "/mnt/My Drive" })).folder, "/mnt/My Drive")
})

test("sync goes to every account that can", () => {
  const s = two({}, { canSyncNow: false })
  assert.deepEqual(plain(model.syncCommands(s)), [["drive-sync", "sync", "--account", "account1"]])
  assert.deepEqual(plain(model.syncCommands(two())), [["drive-sync", "sync", "--account", "account1"],
                                                      ["drive-sync", "sync", "--account", "account2"]])
  assert.equal(model.anyCanSync(s), true)
  assert.equal(model.anyCanSync(two({ canSyncNow: false }, { canSyncNow: false })), false)
  assert.deepEqual(plain(model.syncCommands(model.parseStatus(""))), [])
  assert.equal(model.anyCanSync(model.parseStatus("")), false)
})

test("the view with no account syncs without --account", () => {
  const s = model.parseStatus(v3([account({ id: "", managed: false })], { worst: "" }))
  assert.deepEqual(plain(model.syncCommands(s)), [["drive-sync", "sync"]])
  assert.deepEqual(plain(model.syncCommand("")), ["drive-sync", "sync"])
  assert.deepEqual(plain(model.pauseCommand("")), ["drive-sync", "pause", "--minutes", "60"])
  assert.deepEqual(plain(model.resumeCommand("")), ["drive-sync", "resume"])
  assert.deepEqual(plain(model.openArgs("/x/open-history.sh", "")), ["bash", "/x/open-history.sh"])
})

test("pause and resume per account and for all", () => {
  assert.deepEqual(plain(model.pauseCommand("account2")), ["drive-sync", "pause", "--account", "account2", "--minutes", "60"])
  assert.deepEqual(plain(model.resumeCommand("account2")), ["drive-sync", "resume", "--account", "account2"])
  assert.deepEqual(plain(model.syncCommand("account2")), ["drive-sync", "sync", "--account", "account2"])
  assert.deepEqual(plain(model.pauseAllCommand()), ["drive-sync", "pause", "--minutes", "60"])
  assert.deepEqual(plain(model.resumeAllCommand()), ["drive-sync", "resume"])
  assert.deepEqual(plain(model.openArgs("/x/open-history.sh", "account2")), ["bash", "/x/open-history.sh", "account2"])

  const paused = two({}, { status: "paused", tone: "warning", paused: true, canSyncNow: false })
  assert.equal(model.anyPaused(paused), true)
  assert.equal(model.anyCanPause(paused), true)
  assert.equal(model.anyPaused(two()), false)
  assert.equal(model.anyCanPause(two({ managed: false }, { managed: false })), false)
  assert.equal(model.anyBusy(two()), false)
  assert.equal(model.anyBusy(two({}, { running: true })), true)
  assert.equal(model.anyBusy(two({ resync: true })), true)
  assert.equal(model.anyBusy(model.parseStatus("")), false)
})

test("panel targets for one and for several accounts", () => {
  const one = model.parseStatus(v3([account()]))
  assert.deepEqual(plain(model.panelTargets(one)), plain(model.ACTIONS.map((action) => ({ account: -1, action }))))
  assert.deepEqual(plain(model.panelTargets(model.parseStatus(""))), plain(model.ACTIONS.map((action) => ({ account: -1, action }))))
  assert.deepEqual(plain(model.panelTargets(two())), [
    { account: 0, action: "open" }, { account: 0, action: "sync" }, { account: 0, action: "pause" }, { account: 0, action: "folder" },
    { account: 1, action: "open" }, { account: 1, action: "sync" }, { account: 1, action: "pause" }, { account: 1, action: "folder" },
    { account: -1, action: "sync" }, { account: -1, action: "pause" }, { account: -1, action: "log" }])
  assert.equal(model.stepTarget(0, -1, 11), 10)
  assert.equal(model.stepTarget(10, 1, 11), 0)
  assert.equal(model.stepTarget(3, 1, 11), 4)
  assert.equal(model.stepTarget(5, 1, 0), 0)
})

test("the cursor's account", () => {
  const targets = model.panelTargets(two())
  assert.equal(model.cursorAccount(targets, true, 5), 1)
  assert.equal(model.cursorAccount(targets, true, 9), -1)
  assert.equal(model.cursorAccount(targets, false, 5), -1)
  assert.equal(model.cursorAccount(targets, true, 40), -1)
})

test("displayFolder shows the home as ~", () => {
  assert.equal(model.displayFolder("/home/u/Drive", "/home/u"), "~/Drive")
  assert.equal(model.displayFolder("/home/u/Drive", "/home/u/"), "~/Drive")
  assert.equal(model.displayFolder("/home/u", "/home/u"), "~")
  assert.equal(model.displayFolder("/home/user2/Drive", "/home/u"), "/home/user2/Drive")
  assert.equal(model.displayFolder("/mnt/Drive", ""), "/mnt/Drive")
  assert.equal(model.displayFolder("/mnt/Drive", "/"), "/mnt/Drive")
})

// ---- The panel, the service and open-history.sh (version 3) ----

test("the widget polls version 3", () => {
  const qml = readFileSync(new URL("../DriveSyncService.qml", import.meta.url), "utf8")
  assert.match(qml, /command: \["drive-sync", "status", "--json", "--all"\]/)
  assert.match(qml, /Model\.parseStatus\(raw\)/)
  assert.match(qml, /status: Model\.shownAccount\(all\)/)
  for (const name of ["DriveSync.qml", "DriveSyncService.qml"]) {
    const text = readFileSync(new URL("../" + name, import.meta.url), "utf8")
    assert.doesNotMatch(text, /parseSnapshot/, name)
  }
  assert.equal(model.parseSnapshot, undefined)
})

// Runs open-history.sh with nothing but fakes on PATH: a missing fake must
// fail, never reach the real uwsm-app and open this machine's window.
function runOpenHistory(args, clients) {
  const dir = mkdtempSync(join(tmpdir(), "drive-sync-open-"))
  const bin = join(dir, "bin")
  mkdirSync(bin)
  const log = join(dir, "log")
  const fake = (name, body) => {
    writeFileSync(join(bin, name), "#!/usr/bin/bash\n" + body + "\n")
    chmodSync(join(bin, name), 0o755)
  }
  fake("hyprctl", `if [[ \$1 == clients ]]; then printf '%s\\n' '${clients}'; else printf 'hyprctl %s\\n' "\$*" >> '${log}'; fi`)
  fake("uwsm-app", `printf 'uwsm-app %s\\n' "\$*" >> '${log}'`)
  for (const tool of ["jq", "head"]) symlinkSync(execFileSync("/usr/bin/bash", ["-c", `command -v ${tool}`]).toString().trim(), join(bin, tool))
  const script = new URL("../open-history.sh", import.meta.url).pathname
  execFileSync("/usr/bin/bash", [script, ...args], { cwd: dir, env: { PATH: bin, HOME: dir } })
  const lines = existsSync(log) ? readFileSync(log, "utf8").trim().split("\n") : []
  const touched = existsSync(join(dir, "x"))
  rmSync(dir, { recursive: true, force: true })
  return { lines, touched }
}

test("open-history.sh passes only an account id", () => {
  assert.deepEqual(runOpenHistory(["account2"], "[]").lines, ["uwsm-app -- drive-sync open --account account2"])
  assert.deepEqual(runOpenHistory(["account99"], "[]").lines, ["uwsm-app -- drive-sync open --account account99"])
  for (const args of [["account2; touch x"], ["--help"], [], ["account0"], ["account100"], ["account2", "extra"], ["$(touch x)"]]) {
    const run = runOpenHistory(args, "[]")
    if (args.length === 2) {
      // A second argument is ignored; the first one is still checked on its own.
      assert.deepEqual(run.lines, ["uwsm-app -- drive-sync open --account account2"], args.join(" "))
    } else {
      assert.deepEqual(run.lines, ["uwsm-app -- drive-sync"], args.join(" "))
    }
    assert.equal(run.touched, false, args.join(" "))
  }
})

test("an open window is only focused", () => {
  const clients = JSON.stringify([{ class: "kitty", address: "0x111" }, { class: "drive-sync", address: "0xabc" }])
  for (const args of [[], ["account2"]]) {
    const { lines } = runOpenHistory(args, clients)
    assert.equal(lines.length, 1, args.join(" "))
    assert.match(lines[0], /^hyprctl dispatch .*address:0xabc/, args.join(" "))
    assert.ok(!lines.some((line) => line.startsWith("uwsm-app")), args.join(" "))
  }
})

test("the folder action uses the account's folder", () => {
  assert.equal(model.folderToOpen(two(), "/home/u/Drive2", "/home/u"), "/home/u/Drive2")
  assert.equal(model.folderToOpen(two(), "/mnt/Elsewhere", "/home/u"), "/mnt/Elsewhere")
  // An account whose folder the model refused opens nothing, never ~/Drive.
  assert.equal(model.folderToOpen(two(), "", "/home/u"), "")
  assert.equal(model.folderToOpen(two(), "Drive", "/home/u"), "")
  // ~/Drive only when there is no account at all (or no answer).
  const none = model.parseStatus(v3([], { tone: "error", worst: "" }))
  assert.equal(model.folderToOpen(none, "", "/home/u"), "/home/u/Drive")
  assert.equal(model.folderToOpen(none, "", "/home/u/"), "/home/u/Drive")
  assert.equal(model.folderToOpen(model.parseStatus(""), "", "/home/u"), "/home/u/Drive")
  assert.equal(model.folderToOpen(none, "", ""), "")
  assert.equal(model.folderToOpen(none, "", "relative"), "")
  const qml = readFileSync(new URL("../DriveSyncService.qml", import.meta.url), "utf8")
  assert.doesNotMatch(qml, /"\/Drive"/)
  assert.doesNotMatch(qml, /drivePath/)
  const panel = readFileSync(new URL("../DriveSync.qml", import.meta.url), "utf8")
  // The middle click and O open the shown account's folder.
  assert.match(panel, /Qt\.MiddleButton\) sync\.openFolder\(root\.status\.folder\)/)
})

test("one account keeps 5.1's panel", () => {
  const qml = readFileSync(new URL("../DriveSync.qml", import.meta.url), "utf8")
  assert.match(qml, /readonly property bool several: root\.accounts\.length >= 2/)
  const repeater = qml.indexOf("Repeater {")
  assert.ok(repeater > 0, "a Repeater lists the accounts")
  assert.match(qml.slice(repeater, repeater + 200), /model: root\.accounts/)
  // The block that holds the Repeater shows only from two accounts on.
  const holder = qml.lastIndexOf("visible: root.several", repeater)
  assert.ok(holder > 0 && qml.lastIndexOf("visible: !root.several", repeater) < holder,
            "the account list is visible only with two or more accounts")
  assert.match(qml, /visible: !root\.several/)
  // With one account the cursor walks 5.1's four icons, in 5.1's order.
  assert.deepEqual(plain(model.panelTargets(model.parseStatus(v3([account()])))).map((t) => t.action),
                   ["sync", "pause", "log", "folder"])
  // The cursor never points past the targets after the list shrinks.
  assert.match(qml, /onTargetsChanged: if \(root\.actionIndex >= root\.targets\.length\) root\.actionIndex = 0/)
})

test("fillText puts every value in at once", () => {
  // A folder named like a placeholder stays as it is.
  assert.equal(model.fillText("%1: %2", ["50%2", "Synced"]), "50%2: Synced")
  assert.equal(model.fillText("%1: %2", ["Drive", "a %1 b"]), "Drive: a %1 b")
  assert.equal(model.fillText("Open %1", ["~/Drive"]), "Open ~/Drive")
  assert.equal(model.fillText("%1 %3", ["a"]), "a %3")
})

test("the tooltip with one account reads as 5.1's", () => {
  for (const lang of ["en", "pt-BR"]) {
    const catalog = JSON.parse(readFileSync(new URL(`../i18n/${lang}.json`, import.meta.url), "utf8"))
    assert.equal(model.fillText(catalog["tooltip.account"], ["Drive", "X"]), model.fillText(catalog["tooltip"], ["X"]), lang)
    for (const key of ["action.syncAccount", "action.pauseAccount", "action.resumeAccount", "action.openAccount",
                       "action.openFolder", "keys.openFolder", "hero.account"])
      assert.match(catalog[key], /%1/, `${lang}: ${key}`)
    assert.doesNotMatch(catalog["action.openFolder"], /~\/Drive/, lang)
    // With the folder at ~/Drive, the icon and ? read exactly as in 5.1.
    const before = { en: "Open ~/Drive", "pt-BR": "Abrir o ~/Drive" }[lang]
    assert.equal(model.fillText(catalog["action.openFolder"], ["~/Drive"]), before, lang)
    assert.equal(model.fillText(catalog["keys.openFolder"], ["~/Drive"]), before, lang)
  }
  const qml = readFileSync(new URL("../DriveSync.qml", import.meta.url), "utf8")
  // Labels come from folder names: they go in through fillText, never .arg().
  assert.doesNotMatch(qml, /\.label\)?\.arg|arg\([^)]*\.label/)
})

// After a package upgrade the running shell keeps the old widget until
// "omarchy restart shell", but Omarchy reloads its catalogs as soon as they
// change on disk. So every key the last committed widget reads keeps the text
// it had there, and a new meaning is a new key. The baseline is HEAD: once a
// version is committed, it is the one the next upgrade must not break.
test("keys the installed widget reads keep their text", () => {
  const repo = fileURLToPath(new URL("../../", import.meta.url))
  const head = (path) => execFileSync("git", ["show", `HEAD:${path}`], { cwd: repo, encoding: "utf8" })
  const files = execFileSync("git", ["ls-tree", "--name-only", "HEAD", "plugin/"], { cwd: repo, encoding: "utf8" })
    .split("\n").filter((name) => /\.(qml|js)$/.test(name))
  const read = new Set()
  for (const name of files)
    for (const [, key] of head(name).matchAll(/strings\.tn?\("([^"]+)"/g)) read.add(key)
  assert.ok(read.has("tooltip"), "no key found in the committed widget")
  const problems = []
  for (const lang of ["en", "pt-BR"]) {
    const before = JSON.parse(head(`plugin/i18n/${lang}.json`))
    const now = JSON.parse(readFileSync(new URL(`../i18n/${lang}.json`, import.meta.url), "utf8"))
    // "status." + status and "age." + unit are built at runtime: every key
    // of the committed catalog under that prefix is one the widget can read.
    const keys = [...read].flatMap((key) => key.endsWith(".") ? Object.keys(before).filter((k) => k.startsWith(key)) : [key])
    for (const key of keys) {
      if (!(key in before)) continue
      if (!(key in now)) problems.push(`${lang}: ${key} removed`)
      else if (!isDeepStrictEqual(now[key], before[key]))
        problems.push(`${lang}: ${key} was ${JSON.stringify(before[key])}, now ${JSON.stringify(now[key])}`)
    }
  }
  assert.deepEqual(problems, [])
})

test("the IPC answers for every account", () => {
  const qml = readFileSync(new URL("../DriveSync.qml", import.meta.url), "utf8")
  assert.match(qml, /function syncNow\(\): string \{[^}]*Model\.anyCanSync\(root\.all\)[^}]*sync\.syncAll\(\)/)
  assert.match(qml, /function togglePause\(\): string \{[^}]*Model\.anyPaused\(root\.all\)[^}]*Model\.anyCanPause\(root\.all\)[^}]*sync\.togglePauseAll\(\)/)
  assert.match(qml, /Qt\.RightButton\) sync\.syncAll\(\)/)
})
