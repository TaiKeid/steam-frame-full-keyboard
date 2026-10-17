"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs");
const vm = require("node:vm");
const factory = vm.runInNewContext(fs.readFileSync(process.argv[2], "utf8"));
const MAIN = "valve.steam.gamepadui.main";

function fixture(open = false, production = false, external = false, sharedApi = false) {
    let time = 0, tick, observer, progress = 0, deferHide = false;
    const calls = [], deliveries = [], nodes = new Set();
    const status = {bIsOpen: open, bMinimal: true, sOverlayKey: MAIN, bDispatchEventsToSteamVR: false};
    Object.defineProperty(status, "sInitialText", {get() { throw Error("must not read field text"); }});
    const controllerText = value => { // Backspace uses Steam's existing ControllerKeyboardSendText mapping.
        root.SteamClient.Input.ControllerKeyboardSendText(value);
    };
    const store = {VRKeyboardStatus: status, controllerText};
    function raw() {
        const value = {m_bIsOpen: status.bIsOpen, m_sOpenForOverlayKey: status.sOverlayKey,
            m_bDispatchEventsToSteamVR: status.bDispatchEventsToSteamVR,
            m_eVRKeyboardFlags: status.bMinimal ? 13 : 0};
        Object.defineProperty(value, "m_sInitialKeyboardText", {get() { throw Error("must not read raw text"); }});
        store.m_VRKeyboardState = value;
    }
    raw();
    const manager = {logicalOpen: open, m_fnVROnTextEnteredOverride() { throw Error("native text route must stay unused"); },
        m_Instance: {GetVROverlayKey() { return MAIN; }},
        SetVRKeyboardOpenForThisInstance(value) {
            if (this.logicalOpen === value) return;
            this.logicalOpen = value;
            if (!value) { this.m_currentVirtualKeyboardRef = null; this.m_ActiveElementProps = null; }
        },
        SetVirtualKeyboardHidden() {
            this.m_currentVirtualKeyboardRef = null; this.m_ActiveElementProps = null;
            keyboard.Hide();
        },
        SetVirtualKeyboardShownInternal(open) {
            if (open) keyboard.Show();
            else keyboard.Hide();
        },
        HandleVirtualKeyDown(text) {
            if (text === "Enter" && this.m_ActiveElementProps?.onEnterKeyPress?.() === "VKClose" || text === "VKClose")
                this.SetVirtualKeyboardHidden();
            else { calls.push(text); this.m_fnVROnTextEnteredOverride(text); }
        }};
    function update(values) {
        Object.assign(status, values); raw();
        manager.SetVRKeyboardOpenForThisInstance(status.bIsOpen && status.sOverlayKey === MAIN);
        observer?.(); tick?.();
    }
    const keyboard = {
        Show() { calls.push("native-show"); update({bIsOpen: true, sOverlayKey: MAIN}); },
        Hide() { calls.push("native-hide"); if (!deferHide) update({bIsOpen: false}); },
        RegisterForStatus(fn) { observer = fn; return {unregister() { observer = null; }}; }
    };
    manager.m_ownerWindow = {SteamClient: {OpenVR: {Keyboard: keyboard}}};
    const api = {ShowWindow() { calls.push("window-show"); }, HideWindow() {
        calls.push("window-hide"); update({bIsOpen: false}); update({bIsOpen: true});
    }, BringToFront() { calls.push("focus"); }};
    const oldKey = manager.HandleVirtualKeyDown, oldOpen = manager.SetVRKeyboardOpenForThisInstance;
    const oldShow = api.ShowWindow, oldHide = keyboard.Hide;
    const oldFront = api.BringToFront, oldFieldShow = manager.SetVirtualKeyboardShownInternal;
    const popup = {window: {SteamClient: {Window: api}, document: {
        documentElement: {appendChild(node) { nodes.add(node); }},
        createElement() { return {setAttribute() {}, remove() { nodes.delete(this); }}; }
    }}};
    const root = {SteamClient: {OpenVR: {Keyboard: keyboard}, Input: {
        ControllerKeyboardSendText(value) { deliveries.push(value); }
    }}, performance: {now: () => time},
        setInterval(fn) { tick = fn; return 1; }, clearInterval() { tick = null; },
        SteamUIStore: {WindowStore: {SteamUIWindows: [{VirtualKeyboardManager: manager}]}},
        g_PopupManager: {GetPopupForVROverlayKey() { return popup; }}};
    if (production) {
        const require = key => key === "store" ? {store} : {controllerText};
        require.m = {store: function() { return "get VRKeyboardStatus()"; },
            keyboard: function() { return "SetVirtualKeyboardShownInternal ControllerKeyboardSendText"; }};
        root.webpackChunksteamui = [];
        root.webpackChunksteamui.push = function(chunk) { Array.prototype.push.call(this, chunk); chunk[2](require); };
    }
    const otherKey = "valve.steam.gamepadui.barpopup";
    const other = sharedApi ? {...manager, m_Instance: {GetVROverlayKey() { return otherKey; }},
        m_ActiveElementProps: {onTextEntered(value) { deliveries.push(value); }}} : null;
    if (other) root.SteamUIStore.WindowStore.SteamUIWindows.push({VirtualKeyboardManager: other});
    const bridge = factory(root, production ? undefined : store, external);
    return {bridge, calls, deliveries, nodes, manager, other, api, keyboard, oldKey, oldOpen, oldHide, oldShow,
        oldFront, oldFieldShow,
        nativeOpen() { return status.bIsOpen; },
        step(ack = 0, ready = true, fallback = false, advance = true) {
            return bridge.step({progress: advance ? ++progress : progress, ready, ack, fallback, fallbackRequest: ack});
        },
        status: update,
        now() { return time; },
        advance(ms) { time += ms; tick?.(); },
        newTarget() { manager.m_currentVirtualKeyboardRef = {}; manager.m_ActiveElementProps = {}; tick?.(); },
        deferHide(value) { deferHide = value; },
        alive() { return tick !== null; },
        observing() { return !!observer; }};
}
function owned(f) {
    f.step(); f.status({bIsOpen: true}); const s = f.step();
    assert(s.open && s.eligible && !s.active);
    assert(!f.nativeOpen(), "takeover closes native scene, collision and move handle");
    assert(f.step(s.request).active);
    return s.request;
}
function externalOwned(f) {
    f.step(); f.status({bIsOpen: true, sOverlayKey: "gamescope.gamescope-0.window.62"});
    let s = f.step();
    assert(f.nativeOpen() && !s.eligible && s.externalCandidate, "leave native until exact-target guard is armed");
    s = f.bridge.prepareExternal(s.request, s.externalCandidate.key);
    s = f.step();
    assert(s.local && s.eligible && !f.nativeOpen(), "close the complete external native presentation");
    assert(f.step(s.request).active);
    return s.request;
}
{
    const f = fixture(false, false, true), request = externalOwned(f);
    // App, helper and native guard are all gone: no further step/restore ack.
    f.advance(901);
    assert(f.bridge.expired() && f.nodes.size === 0 && !f.bridge.diagnostics().active);
    assert(f.bridge.diagnostics().externalRestore && !f.nativeOpen(),
        "no external provider can restore a prior request after its guard is gone");
    f.status({bIsOpen: true, sOverlayKey: "gamescope.gamescope-0.window.62"});
    f.api.ShowWindow();
    assert(f.nativeOpen() && f.calls.includes("window-show") && !f.bridge.send(request, "stale"),
        "a fresh native summon stays visible after all replacement processes exit");
    f.advance(601);
    assert(!f.alive() && !f.observing() && f.keyboard.Hide === f.oldHide,
        "the expired hook removes itself without a surviving helper");
}
for (const production of [false, true]) {
    const f = fixture(false, production, false, true);
    f.step(); f.status({bIsOpen: true, sOverlayKey: "valve.steam.gamepadui.barpopup"});
    const s = f.step();
    assert(s.eligible && f.step(s.request).active && !f.nativeOpen(),
        "a second owner sharing the provider can acquire its native session");
    assert.equal(f.calls.filter(c => c === "native-hide").length, 1,
        "takeover calls the original provider rather than another owner's hook");
    assert(f.bridge.send(s.request, "shared provider fixture"));
    assert(f.bridge.dismiss(s.request) && !f.step().open && !f.nativeOpen());
    assert.equal(f.calls.filter(c => c === "native-hide").length, 2, "dismiss invokes the provider once");
    f.status({sOverlayKey: MAIN});
    assert(owned(f), "the first owner can still summon after the shared owner's dismissal");
    f.bridge.dispose();
    assert.equal(f.keyboard.Hide, f.oldHide, "restore the single shared provider hook");
}
{
    const f = fixture(false, false, true), generation = externalOwned(f);
    assert.equal(f.bridge.send(generation, "private fixture"), false, "external text stays in the local input backend");
    for(let i=0;i<8;++i) { f.status({bIsOpen:false}); assert.equal(f.step(generation).request,generation); }
    assert(f.bridge.dismiss(generation));
    assert(!f.step().open && !f.step().externalRestore, "X cancels native recovery without reopening");
    const next = externalOwned(f); assert.notEqual(next, generation);
    f.step(next, true, true);
    const s=f.step();
    assert(s.externalRestore && !s.active && !s.external, "escape asks the guard to restore its exact target");
    assert(!f.calls.includes("native-show"), "never restore an external target through Steam's own window");
    f.bridge.dispose();
}
{
    const f=fixture(false, false, true), generation=externalOwned(f);
    f.advance(901); const s=f.bridge.diagnostics();
    assert(!s.active && s.externalRestore && f.nodes.size===0, "UI lease withdraws external suppression before guard recovery");
    f.bridge.dispose();
}
for (const staleField of [false, true]) {
    const f = fixture(false, false, true);
    if (staleField) f.newTarget();
    const generation = externalOwned(f);
    // The app closes, leaving ours visible. The dashboard's keyboard button
    // now opens for Steam instead of the former Gamescope window.
    f.status({bIsOpen: true, sOverlayKey: MAIN, bDispatchEventsToSteamVR: true});
    const s = f.step(generation);
    assert(!s.open && !s.eligible && !f.nativeOpen(),
        "dashboard button after closing an app must dismiss both presentations");
    assert(!s.external && !s.externalRestore, "cancel the former app guard rather than restore native");
    assert(!f.calls.includes("native-show"));
    f.status({bIsOpen: true, sOverlayKey: MAIN});
    const next = f.step();
    assert(next.eligible && f.step(next.request).active,
        "a later dashboard button summon must open ours normally");
    f.bridge.dispose();
}
{
    const f = fixture(false, false, true), previous = externalOwned(f);
    f.newTarget();
    f.manager.SetVirtualKeyboardShownInternal(true);
    const s = f.step();
    assert(s.eligible && !s.local && s.request !== previous && !f.nativeOpen(),
        "automatic Steam field focus transfers an app session instead of toggling off");
    assert(f.step(s.request).active && f.bridge.send(s.request, "field fixture"));
    f.manager.SetVirtualKeyboardShownInternal(true);
    const again = f.step();
    assert(again.eligible && f.step(again.request).active,
        "explicitly refocusing the same field must keep replacement open");
    f.bridge.dispose();
}
{
    const f = fixture(false, false, true), generation = owned(f);
    f.status({bIsOpen:true,sOverlayKey:"gamescope.gamescope-0.window.72"});
    const s=f.step(generation);
    assert(!s.open && !f.nativeOpen() && !s.externalRestore,
        "the local-app button can dismiss a panel opened in Steam too");
    f.bridge.dispose();
}
{
    const f=fixture(false,false,true), generation=externalOwned(f);
    f.status({bIsOpen:true,sOverlayKey:"unknown.overlay"});
    assert(f.nativeOpen() && !f.step(generation).eligible,
        "unsupported providers still stay native rather than becoming global toggles");
    f.bridge.dispose();
}
{
    const f=fixture(false, false, true); f.step();
    f.status({bIsOpen:true,sOverlayKey:"gamescope.gamescope-0.window.62"});
    const stale=f.step(); f.status({sOverlayKey:"unknown.overlay"});
    assert(!f.bridge.prepareExternal(stale.request,stale.externalCandidate.key).eligible);
    assert(f.nativeOpen() && !f.calls.includes("native-hide"), "stale guard preparation must not close another target");
    f.bridge.dispose();
}
for(const values of [{bDispatchEventsToSteamVR:true},{bMinimal:false},{sOverlayKey:"gamescope.other"}]) {
    const f=fixture(false,false,true); f.step();
    f.status({bIsOpen:true,sOverlayKey:"gamescope.gamescope-0.window.62",...values});
    const s=f.step(); assert(!s.eligible && !s.externalCandidate && f.nativeOpen());
    f.bridge.dispose();
}
for (const production of [false, true]) {
    for (const reason of ["unsupported", "missing route", "unprepared external", "not ready"]) {
        const f = fixture(false, production, true);
        if (reason === "missing route") f.manager.m_fnVROnTextEnteredOverride = null;
        f.step(); f.api.ShowWindow();
        assert(!f.calls.includes("window-show"), "pre-status show is initially gated");
        f.status({bIsOpen: true, sOverlayKey: reason === "unsupported" ? "unknown.overlay" :
            reason === "unprepared external" ? "gamescope.gamescope-0.window.62" : MAIN});
        if (reason === "not ready") f.step(0, false);
        for (let i = 0; i < 8; ++i) { f.advance(100); f.step(0, reason !== "not ready"); }
        assert(f.nativeOpen() && !f.step(0, reason !== "not ready").eligible);
        assert.equal(f.nodes.size, 0, "fallback removes suppression");
        assert.equal(f.calls.filter(c => c === "window-show").length, 1,
            `${reason} replays the gated show exactly once`);
        f.bridge.dispose();
    }
}
{
    const f = fixture(); const generation = owned(f);
    f.api.ShowWindow();
    assert(f.bridge.dismiss(generation));
    assert(!f.calls.includes("window-show"), "explicit dismissal discards a gated show");
    f.bridge.dispose();
}
for (const production of [false, true]) {
    const f = fixture(false, production); f.step();
    assert.equal(f.nodes.size, 1, "prearm transparent texture without reading initial text");
    f.api.ShowWindow(); assert.equal(f.calls.length, 0, "gate a pre-show call");
    f.status({bIsOpen: true}); const s = f.step();
    assert(!f.nativeOpen() && s.open && s.eligible && !s.active);
    assert.equal(f.bridge.send(s.request, "early"), false);
    f.manager.HandleVirtualKeyDown("stock-controller");
    assert.deepEqual(f.calls, ["native-hide"], "stock events stay blocked during readiness");
    assert(f.step(s.request).active);
    assert(f.bridge.send(s.request, "café 世界"));
    for (let i = 0; i < 10; ++i) {
        f.status({bIsOpen: false}); // Duplicate native callbacks must not create requests.
        assert.equal(f.step(s.request).request, s.request);
        assert(f.bridge.send(s.request, "a"));
    }
    assert.equal(f.calls.filter(c => c === "native-hide").length, 1);
    assert(!f.calls.includes("window-hide"), "never use the focus-reopening browser hide path");
    assert(!f.calls.includes("native-show"), "clicking/typing cannot reopen the native session");
    f.bridge.dispose();
    assert(f.nativeOpen(), "dispose independently restores native presentation");
    assert.equal(f.manager.HandleVirtualKeyDown, f.oldKey);
    assert.equal(f.manager.SetVRKeyboardOpenForThisInstance, f.oldOpen);
    assert.equal(f.keyboard.Hide, f.oldHide);
    assert.equal(f.api.ShowWindow, f.oldShow);
    assert(!f.alive() && !f.observing(), "remove watchdog and status callback");
}
{
    const f = fixture(false, true); f.status({bDispatchEventsToSteamVR: true});
    const generation = owned(f);
    assert(f.bridge.send(generation, "a") && f.bridge.send(generation, "Backspace"));
    assert.deepEqual(f.deliveries, ["a", "Backspace"],
        "native-button Steam requests use the existing controller route after native presentation closes");
    f.bridge.dispose();
}
{
    const f = fixture(); f.newTarget(); const field = [];
    f.manager.m_ActiveElementProps.onTextEntered = value => field.push(value);
    const original = f.manager.m_fnVROnTextEnteredOverride;
    f.status({bDispatchEventsToSteamVR: true}); const generation = owned(f);
    assert(f.bridge.send(generation, "café 世界") && f.bridge.send(generation, "ArrowLeft"));
    assert.deepEqual(field, ["café 世界", "ArrowLeft"], "retained Steam field receives direct logical keys");
    assert.equal(f.deliveries.length, 0, "field dispatch cannot leak to another focused input route");
    assert.equal(f.manager.m_fnVROnTextEnteredOverride, original, "restore renderer override after each dispatch");
    const refreshed = [];
    f.manager.m_ActiveElementProps = {onTextEntered: value => refreshed.push(value)};
    assert.equal(f.step(generation).request, generation, "field rerenders do not invalidate an acknowledged request");
    assert(f.bridge.send(generation, "b"));
    assert.deepEqual(refreshed, ["b"], "a stable field uses its refreshed callback");
    f.manager.m_ActiveElementProps.onTextEntered = () => { throw Error("fixture field rejected text"); };
    assert.throws(() => f.bridge.send(generation, "unsent"));
    assert.equal(f.manager.m_fnVROnTextEnteredOverride, original, "dispatch errors also restore the renderer override");
    f.bridge.dispose();
}
{
    const f = fixture(); f.newTarget(); const target = f.manager.m_currentVirtualKeyboardRef;
    const generation = owned(f);
    assert.equal(f.manager.m_currentVirtualKeyboardRef, target, "physical close preserves Steam field target");
    assert(f.manager.logicalOpen);
    assert(f.bridge.dismiss(generation));
    assert(!f.step(generation).open && !f.nativeOpen() && !f.manager.logicalOpen);
    assert.equal(f.manager.m_currentVirtualKeyboardRef, null, "explicit X runs target cleanup");
    for (let i = 0; i < 15; ++i) { f.status({bIsOpen: false}); assert(!f.step(generation).open); }
    assert(!f.calls.includes("native-show"), "X cannot start a show/hide loop");
    f.status({bIsOpen: true}); let s = f.step();
    assert(s.request !== generation && f.step(s.request).active);
    assert(!f.bridge.dismiss(generation), "stale dismissal cannot close a later request");
    f.bridge.dispose();
}
{
    const f = fixture(); const generation = owned(f);
    f.status({bIsOpen: true}); // Same-target native keyboard button toggles ours off.
    assert(!f.step(generation).open && !f.nativeOpen());
    f.status({bIsOpen: true}); const s = f.step();
    assert(s.request !== generation && f.step(s.request).active, "next button press opens ours again");
    f.bridge.dispose();
}
{
    const f = fixture(); let generation = owned(f);
    f.newTarget(); const s = f.step(generation);
    assert(s.request !== generation && !s.active, "same-overlay field change needs fresh readiness");
    assert(!f.bridge.send(generation, "stale"));
    assert(f.step(generation, true, true).eligible, "old escape cannot affect new target");
    generation = s.request; assert(f.step(generation).active);
    f.manager.m_ActiveElementProps.onEnterKeyPress = () => "VKClose";
    assert(f.bridge.send(generation, "Enter"));
    assert(!f.step(generation).open && !f.nativeOpen(), "Steam field Enter can close the retained session");
    f.bridge.dispose();
}
{
    const f = fixture(); f.newTarget(); const target = f.manager.m_currentVirtualKeyboardRef;
    const generation = owned(f);
    f.step(generation, true, true);
    assert(f.nativeOpen() && !f.step(generation).eligible);
    assert.equal(f.manager.m_currentVirtualKeyboardRef, target, "escape preserves exact field target");
    assert.equal(f.nodes.size, 0);
    assert(!f.step(generation).active, "escape cannot retake its restored native session");
    f.bridge.dispose();
}
{
    const f = fixture(); f.step(); f.status({bIsOpen: true}); const s = f.step();
    f.advance(401);
    assert(f.nativeOpen() && !f.step(s.request).active, "late readiness restores native once");
    assert.equal(f.calls.filter(c => c === "native-show").length, 1);
    f.bridge.dispose();
}
{
    const f = fixture(); const generation = owned(f);
    f.advance(901);
    assert(f.nativeOpen() && !f.step(generation).active, "independent lease restores usable native");
    assert.equal(f.nodes.size, 0);
    assert(!f.bridge.send(generation, "late"));
    f.bridge.dispose();
}
{
    const f = fixture(); const generation = owned(f);
    for (let i = 0; i < 5; ++i) { f.advance(200); f.step(generation, true, false, false); }
    assert(f.nativeOpen() && !f.step(generation).active, "helper traffic cannot renew render-loop health");
    f.bridge.dispose();
}
for (const values of [
    {sOverlayKey: "gamescope.gamescope-0.window.72"},
    {sOverlayKey: "unknown.overlay"},
    {bDispatchEventsToSteamVR: true, bMinimal: false},
    {sOverlayKey: "valve.steam.gamepadui.barpopup"},
]) {
    const f = fixture(); f.step(); f.status({bIsOpen: true, ...values});
    assert(f.nativeOpen() && !f.step().eligible, "unrestorable/external targets remain native");
    assert(!f.calls.includes("native-hide"));
    assert.equal(f.nodes.size, 0);
    f.bridge.dispose();
}
{
    const f = fixture(true);
    assert(!f.step().eligible && f.nativeOpen(), "leave an already-open native session alone");
    f.status({bIsOpen: false}); f.status({bIsOpen: true}); const s = f.step();
    assert(f.step(s.request).active);
    f.bridge.dispose();
}
{
    const f = fixture(); f.manager.m_fnVROnTextEnteredOverride = null;
    f.step(); f.status({bIsOpen: true}); assert(!f.step().eligible);
    f.advance(60); f.manager.m_fnVROnTextEnteredOverride = () => {}; f.advance(50);
    const s = f.step(); assert(s.eligible && f.step(s.request).active, "wait briefly for renderer callback");
    f.bridge.dispose();
}
{
    const f = fixture(); f.deferHide(true); f.step(); f.status({bIsOpen: true}); const s = f.step();
    assert(f.nativeOpen() && !s.eligible && !f.step(s.request).active,
        "never show ours while native close has not completed");
    f.advance(401);
    assert(f.nativeOpen() && !f.step(s.request).eligible, "failed close leaves stock usable");
    assert.equal(f.calls.filter(c => c === "native-hide").length, 1, "failed close does not retry in a loop");
    f.bridge.dispose();
}
{
    const f = fixture(); f.deferHide(true); f.step(); f.status({bIsOpen: true}); const s = f.step();
    f.status({sOverlayKey: "gamescope.other.window"});
    assert(f.nativeOpen() && !f.step(s.request).eligible,
        "a different provider opening before close acknowledgement must remain native");
    assert(!f.calls.includes("native-show"), "never restore the old provider over a new native request");
    f.bridge.dispose();
}
{
    const f = fixture(); const other = () => 42;
    f.api.ShowWindow = other; f.bridge.dispose();
    assert.equal(f.api.ShowWindow, other, "cleanup preserves another tool's later hook");
}
// Reproducible event walks combine real production hooks with independent safety
// properties. Include the seed/trace on failure so every discovered sequence can
// become a focused regression. No real endpoint or user text is involved.
let deadlineChecks = 0, laterDeadlineChecks = 0;
for (let seed = 1; seed <= 128; ++seed) {
    let random = seed, previousRequest = 0;
    const next = n => { random = (Math.imul(random, 1664525) + 1013904223) >>> 0; return random % n; };
    const f = fixture(false, seed % 2 === 0, true, seed % 3 === 0), trace = [];
    const opened = key => f.status({bIsOpen: true, sOverlayKey: key,
        bMinimal: true, bDispatchEventsToSteamVR: false});
    // Keep the deadline in the test, independently of the bridge's timer. A new
    // target gets its own deadline; readiness heartbeats cannot extend it.
    let pending;
    const observeRequest = () => {
        const s = f.bridge.diagnostics();
        if (s.active || !(s.eligible || s.closingNative || s.externalCandidate)) pending = null;
        else if (pending?.request !== s.request) pending = {request: s.request, until: f.now() + 400};
    };
    const advanceAndCheck = (ms, later) => {
        const due = pending && f.now() + ms >= pending.until;
        f.advance(ms);
        if (due) {
            const s = f.bridge.diagnostics();
            assert(!s.eligible && !s.closingNative && (f.nativeOpen() || s.externalRestore),
                "every unacknowledged request reaches native or exact-overlay recovery by its deadline");
            ++deadlineChecks;
            if (later) ++laterDeadlineChecks;
        }
    };
    try {
        f.step(); f.api.ShowWindow(); f.deferHide(next(2) === 0);
        opened([MAIN, "gamescope.gamescope-0.window.62", "unknown.overlay",
            "valve.steam.gamepadui.barpopup"][next(4)]);
        const candidate = f.bridge.diagnostics().externalCandidate;
        if (candidate && next(2)) f.bridge.prepareExternal(candidate.request, candidate.key);
        observeRequest();
        advanceAndCheck(401, false);
        const expiredRequest = f.bridge.diagnostics();
        assert(!expiredRequest.eligible && (f.nativeOpen() || expiredRequest.externalRestore),
            "an unacknowledged request reaches native or exact-overlay recovery by its deadline");
        observeRequest();
        for (let step = 0; step < 160 && f.alive(); ++step) {
            const op = next(17), before = f.bridge.diagnostics();
            trace.push(op);
            switch (op) {
            case 0: f.step(before.request); break;
            case 1: f.step(before.request - 1, true, false, false); break;
            case 2: f.step(0, false); break;
            case 3: opened(MAIN); break;
            case 4: opened("gamescope.gamescope-0.window.62"); break;
            case 5: opened("unknown.overlay"); break;
            case 6: opened("valve.steam.gamepadui.barpopup"); break;
            case 7: f.status({bIsOpen: false}); break;
            case 8: f.api.ShowWindow(); break;
            case 9: f.api.BringToFront(); break;
            case 10: f.newTarget(); break;
            case 11: f.deferHide(next(2) === 0); break;
            case 12: f.bridge.dismiss(before.request); break;
            case 13: f.step(before.request, true, true); break;
            case 14:
                if (before.externalCandidate)
                    f.bridge.prepareExternal(before.request - next(2), before.externalCandidate.key);
                break;
            case 15:
                f.newTarget(); f.manager.SetVirtualKeyboardShownInternal(true); break;
            case 16: advanceAndCheck([16, 50, 100, 401, 901][next(5)], true); break;
            }
            const s = f.bridge.diagnostics();
            assert(s.request >= previousRequest, "request identities never go backwards");
            previousRequest = s.request;
            assert(!s.active || s.eligible && !f.nativeOpen(), "takeover never overlaps native presentation");
            if (f.bridge.expired()) assert.equal(f.nodes.size, 0, "expired leases leave nothing hidden");
            const delivered = f.deliveries.length;
            if (!s.active) assert(!f.bridge.send(s.request, "inactive fixture"), "inactive requests reject input");
            assert(!f.bridge.send(s.request + 1, "stale fixture"), "wrong request identities reject input");
            assert.equal(f.deliveries.length, delivered, "rejected dispatch cannot reach any target");
            if (step % 20 === 19) {
                f.step(0, false);
                assert(f.bridge.expired() && f.nodes.size === 0 && !f.bridge.diagnostics().active,
                    "readiness withdrawal always removes takeover and presentation suppression");
            }
            observeRequest();
        }
        f.bridge.dispose();
        assert.equal(f.nodes.size, 0);
        assert(!f.alive() && !f.observing());
        assert.equal(f.manager.HandleVirtualKeyDown, f.oldKey);
        assert.equal(f.manager.SetVRKeyboardOpenForThisInstance, f.oldOpen);
        assert.equal(f.manager.SetVirtualKeyboardShownInternal, f.oldFieldShow);
        assert.equal(f.keyboard.Hide, f.oldHide);
        assert.equal(f.api.ShowWindow, f.oldShow);
        assert.equal(f.api.BringToFront, f.oldFront);
    } catch (error) {
        error.message += ` (seed ${seed}, events ${trace.join(",")})`;
        throw error;
    } finally { f.bridge.dispose(); }
}
assert(laterDeadlineChecks > 0, "random walks must exercise deadlines after their initial request");
console.log(`Native bridge: lifecycle regressions, 128 seeded event sequences and ${deadlineChecks} request deadlines (${laterDeadlineChecks} after initial setup) passed.`);
