"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs");
const vm = require("node:vm");
const factory = vm.runInNewContext(fs.readFileSync(process.argv[2], "utf8"));
const MAIN = "valve.steam.gamepadui.main";

function fixture(open = false, production = false, external = false) {
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
    const bridge = factory(root, production ? undefined : store, external);
    return {bridge, calls, deliveries, nodes, manager, api, keyboard, oldKey, oldOpen, oldHide, oldShow,
        nativeOpen() { return status.bIsOpen; },
        step(ack = 0, ready = true, fallback = false, advance = true) {
            return bridge.step({progress: advance ? ++progress : progress, ready, ack, fallback, fallbackRequest: ack});
        },
        status: update,
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
console.log("Native bridge: closed native scene, stable target ownership, buttons, Enter, readiness and fallback passed.");
