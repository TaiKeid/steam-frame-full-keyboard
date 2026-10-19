// Evaluated only in Steam's SharedJSContext. Never read field or clipboard text.
// Native presentation is closed; the owning Steam manager retains the typing target.
(function installNativeBridge(root, testStore, externalSupported) {
    "use strict";
    const VERSION = 1, LEASE_MS = 900, REQUEST_MS = 400;
    const now = () => root.performance.now();
    let store = testStore;
    let controllerText = testStore?.controllerText;
    if (!store) {
        let require;
        const chunks = root.webpackChunksteamui;
        if (!chunks || typeof chunks.push !== "function") throw Error("steam-ui-unavailable");
        const id = Symbol("framekeyboard");
        chunks.push([[id], {}, r => { require = r; }]);
        const index = chunks.findIndex(c => c[0]?.[0] === id);
        if (index >= 0) chunks.splice(index, 1);
        for (const key of Object.keys(require?.m || {})) {
            if (!String(require.m[key]).includes("get VRKeyboardStatus()")) continue;
            for (const value of Object.values(require(key))) {
                if (value && typeof value === "object" && "VRKeyboardStatus" in value) store = value;
            }
        }
        for (const key of Object.keys(require?.m || {})) {
            const source = String(require.m[key]);
            if (!source.includes("SetVirtualKeyboardShownInternal") || !source.includes("ControllerKeyboardSendText")) continue;
            const choices = Object.values(require(key)).filter(value => typeof value === "function" &&
                String(value).includes("ControllerKeyboardSendText") && String(value).includes("Backspace"));
            if (choices.length === 1) controllerText = choices[0];
        }
    }
    const popup = root.g_PopupManager?.GetPopupForVROverlayKey?.("valve.steam.gamepadui.keyboard");
    const win = popup?.m_popup || popup?.window;
    const windowApi = win?.SteamClient?.Window;
    const windows = root.SteamUIStore?.WindowStore?.SteamUIWindows;
    const managers = Array.isArray(windows) ? windows.map(w => w.VirtualKeyboardManager).filter(Boolean) : [];
    if (!store?.m_VRKeyboardState || !win?.document || !windowApi || !managers.length ||
        typeof windowApi.ShowWindow !== "function" || typeof windowApi.BringToFront !== "function")
        throw Error("steam-hooks-unavailable");
    function nativeStatus() {
        // VRKeyboardStatus also contains initial text. Read only these raw metadata fields.
        if (testStore) {
            const s = store.VRKeyboardStatus;
            return {open: s.bIsOpen, key: s.sOverlayKey, dispatch: s.bDispatchEventsToSteamVR,
                flags: s.bMinimal ? 13 : 0};
        }
        const s = store.m_VRKeyboardState;
        if (typeof s?.m_bIsOpen !== "boolean") throw Error("steam-status-changed");
        return {open: s.m_bIsOpen, key: s.m_sOpenForOverlayKey,
            dispatch: s.m_bDispatchEventsToSteamVR, flags: s.m_eVRKeyboardFlags};
    }
    const hooks = [], owners = [];
    const keyboardApi = root.SteamClient?.OpenVR?.Keyboard;
    const externalHide = keyboardApi?.Hide;
    const localTarget = s => externalSupported && s.dispatch === false && s.flags === 13 &&
        /^gamescope\.gamescope-[0-9]+(?:\.window\.[0-9]+)?$/.test(s.key) && typeof externalHide === "function";
    let preparedExternal = null, externalRestore = null;
    function hook(object, key, wrap) {
        const descriptor = Object.getOwnPropertyDescriptor(object, key), original = object[key];
        if (typeof original !== "function" || descriptor && !descriptor.configurable && !descriptor.writable)
            throw Error("steam-hook-readonly");
        const replacement = wrap(original);
        Object.defineProperty(object, key, {configurable: true, writable: true, value: replacement});
        hooks.push(() => {
            if (object[key] !== replacement) return;
            if (descriptor) Object.defineProperty(object, key, descriptor);
            else delete object[key];
        });
        return original;
    }
    const ownSteam = key => typeof key === "string" && (key === "valve.steam.gamepadui.main" ||
        key === "valve.steam.gamepadui.barpopup" || key.startsWith("valve.steam.gamepadui.appoverlay."));
    let native = nativeStatus(), previousOpen = native.open, previousKey = native.key;
    let ignoreOpen = native.open, rawDeadline = 0, restoringUntil = 0;
    let restoringOwner = null, restoreTimer;
    let logical = null, closingNative = false, deadline = 0, request = 1, acknowledged = 0;
    let expectedClose = null;
    let lease = 0, progress = -1, disposed = false, inspecting = false, interceptedShow = false;
    let lastClock = now(), lastStep = now(), timer, subscription;
    const oldWindowShow = windowApi.ShowWindow;
    const style = win.document.createElement("style");
    style.setAttribute("data-framekeyboard-native", "1");
    style.textContent = ":root, body { opacity: 0 !important; pointer-events: none !important; }";
    let styled = false;
    function setHidden(hidden) {
        if (hidden && !styled) { win.document.documentElement.appendChild(style); styled = true; }
        if (!hidden && styled) { style.remove(); styled = false; }
    }
    const fresh = () => !disposed && now() < lease;
    function routeReady(owner) {
        return typeof owner.manager.m_ActiveElementProps?.onTextEntered === "function" ||
            owner.key === "valve.steam.gamepadui.main" && typeof controllerText === "function";
    }
    function potential(s) {
        return localTarget(s) || ownSteam(s.key) && (s.dispatch === false || (s.flags & 1) !== 0) &&
            owners.some(o => o.key === s.key && routeReady(o));
    }
    function candidate(s) {
        if (localTarget(s)) return preparedExternal?.key === s.key ? preparedExternal : null;
        const choices = owners.filter(o => o.key === s.key && routeReady(o) &&
            typeof o.manager.m_fnVROnTextEnteredOverride === "function");
        return potential(s) && choices.length === 1 ? choices[0] : null;
    }
    const targetOf = manager => manager.m_currentVirtualKeyboardRef || manager.m_ActiveElementProps || null;
    function fieldRequest(s) {
        const owner = candidate(s), intent = owner?.fieldIntent;
        return owner && !owner.external && intent && now() < intent.until &&
            intent.target === targetOf(owner.manager) ? owner : null;
    }
    function buttonRequest(s) {
        if (localTarget(s)) return true;
        const owner = candidate(s);
        // Dashboard buttons bypass the field manager. A retained field reference
        // can be stale after returning from an app, so it is not proof of focus.
        return owner?.fieldHook && !fieldRequest(s);
    }
    function blocking() {
        return fresh() && !ignoreOpen && !restoringUntil &&
            (logical || !native.open || potential(native) && now() < rawDeadline);
    }
    function restoreWindow() {
        setHidden(false);
        if (interceptedShow && native.open) oldWindowShow.call(windowApi);
        interceptedShow = false;
    }
    function unsubscribe() {
        if (typeof subscription === "function") subscription();
        else subscription?.unregister?.();
        subscription = null;
    }
    function finishRestore(success) {
        const owner = restoringOwner;
        restoringOwner = null;
        restoringUntil = 0;
        root.clearTimeout(restoreTimer);
        if (disposed) unsubscribe();
        if (owner && !success) owner.setOpen.call(owner.manager, false);
    }
    function checkRestore(final = false) {
        if (!restoringOwner) return;
        const status = nativeStatus();
        if (status.open && status.key === restoringOwner.key) finishRestore(true);
        else if (status.open || final || now() >= restoringUntil) finishRestore(false);
    }
    function releaseLogical(restore, closeNative = true) {
        const session = logical;
        logical = null;
        expectedClose = null;
        closingNative = false;
        acknowledged = 0;
        ++request;
        ignoreOpen = true;
        if (!restore) interceptedShow = false;
        restoreWindow();
        if (!session) return;
        if (session.owner.external) {
            if (restore) {
                externalRestore = {key: session.owner.key, request};
                restoringUntil = now() + REQUEST_MS;
            } else session.owner.hide.call(keyboardApi);
            preparedExternal = null;
        } else if (restore && !session.dashboard &&
            (!session.target || targetOf(session.owner.manager))) {
            // Preserve the field while the exact owning window's original provider opens again.
            restoringUntil = now() + REQUEST_MS;
            restoringOwner = session.owner;
            // Disposal removes the hooks and regular timer. This bounded check
            // still clears a failed restore without touching a newer provider.
            restoreTimer = root.setTimeout(() => {
                try { checkRestore(true); } catch (_) { finishRestore(false); }
            }, REQUEST_MS);
            session.owner.show.call(session.owner.api);
            oldWindowShow.call(windowApi);
        } else {
            // A missing field cannot justify closing another provider's newer
            // native request while fallback withdraws the old session.
            if (closeNative && (!restore || !native.open || native.key === session.owner.key))
                session.owner.hide.call(session.owner.api);
            // Hide can fail or complete asynchronously. An already-open native
            // keyboard for this owner must retain its ordinary Steam state.
            const status = nativeStatus();
            if (!status.open || status.key !== session.owner.key)
                session.owner.setOpen.call(session.owner.manager, false);
        }
    }
    function begin(owner) {
        logical = {owner, target: owner.external ? owner.key : targetOf(owner.manager),
            props: owner.manager?.m_ActiveElementProps};
        ++request;
        acknowledged = 0;
        deadline = now() + REQUEST_MS;
        closingNative = true;
        expectedClose = owner.external ? null : {owner, request};
        owner.fieldIntent = null;
        if (!owner.external) owner.setOpen.call(owner.manager, true);
        // Window.HideWindow leaves dashboard scene/hit geometry alive and can
        // trigger focus-driven reopening. Close presentation through its provider.
        owner.hide.call(owner.external ? keyboardApi : owner.api);
    }
    function inspect() {
        if (inspecting || disposed) return;
        inspecting = true;
        try {
            const time = now();
            if (time - lastClock > LEASE_MS || time < lastClock) lease = 0;
            lastClock = time;
            if (win.closed || root.g_PopupManager.GetPopupForVROverlayKey("valve.steam.gamepadui.keyboard") !== popup)
                throw Error("steam-popup-changed");
            native = nativeStatus();
            checkRestore();
            if (!restoringOwner && restoringUntil && (native.open || time >= restoringUntil)) restoringUntil = 0;
            if (!native.open && !restoringUntil) ignoreOpen = false;
            const newOpen = native.open && (!previousOpen || native.key !== previousKey);
            if (newOpen) rawDeadline = time + REQUEST_MS;
            previousOpen = native.open;
            previousKey = native.key;
            if (logical) {
                if (!fresh() || time >= deadline && acknowledged !== request) {
                    releaseLogical(true);
                } else if (newOpen && !closingNative && acknowledged === request && buttonRequest(native)) {
                    // The resident panel is already open, even if its previous app
                    // was closed. Consume this button toggle across supported owners.
                    releaseLogical(false);
                } else if (newOpen && !closingNative && fieldRequest(native)) {
                    const owner = fieldRequest(native), previous = logical.owner;
                    logical = null;
                    expectedClose = null;
                    preparedExternal = null;
                    if (!previous.external && previous !== owner)
                        previous.setOpen.call(previous.manager, false);
                    begin(owner);
                } else if (native.open && (native.key !== logical.owner.key || !potential(native))) {
                    // A different provider can open before our close acknowledgement.
                    // Its native session must survive; never reopen the old owner over it.
                    const previous = logical.owner;
                    logical = null;
                    expectedClose = null;
                    if (!previous.external) previous.setOpen.call(previous.manager, false);
                    closingNative = false;
                    acknowledged = 0;
                    ++request;
                    ignoreOpen = true;
                    preparedExternal = null;
                } else if (closingNative) {
                    if (!native.open && !expectedClose) closingNative = false;
                } else if (newOpen) {
                    const owner = candidate(native);
                    const changedTarget = owner !== logical.owner ||
                        owner && !owner.external && targetOf(owner.manager) !== logical.target;
                    if (changedTarget) {
                        const previous = logical.owner;
                        logical = null;
                        expectedClose = null;
                        if (!previous.external && previous !== owner)
                            previous.setOpen.call(previous.manager, false);
                        if (owner) begin(owner);
                        else { acknowledged = 0; ++request; ignoreOpen = true; }
                    } else {
                        // Native presentation is closed, so the next same-target
                        // native button Show toggles our resident panel off.
                        releaseLogical(false);
                    }
                } else if (logical.owner.external) {
                    // Gamescope input stays on the existing local libei/Unicode backend.
                } else if (typeof logical.owner.manager.m_fnVROnTextEnteredOverride !== "function") {
                    releaseLogical(true);
                } else {
                    const m = logical.owner.manager;
                    // React can replace props after every character while the field
                    // reference stays stable. Refresh callbacks without retargeting.
                    logical.props = m.m_ActiveElementProps;
                    const target = targetOf(m);
                    if (target !== logical.target) {
                        if (!target && logical.owner.key !== "valve.steam.gamepadui.main") {
                            releaseLogical(false);
                        } else {
                            // A cleared main-window field returns to ordinary dashboard
                            // input. Do not retain Steam's field route or keyboard-open
                            // flag after its target is gone. The new generation cancels
                            // queued output before the native app changes routes.
                            logical.dashboard = !target;
                            logical.target = target;
                            ++request;
                            acknowledged = 0;
                            deadline = time + REQUEST_MS;
                            if (logical.dashboard) logical.owner.setOpen.call(m, false);
                        }
                    }
                }
            } else if (native.open && !ignoreOpen && !restoringUntil) {
                const owner = candidate(native);
                if (fresh() && owner && time < rawDeadline) begin(owner);
                else if (!fresh() || !potential(native) || time >= rawDeadline) ignoreOpen = true;
            }
            // A show can arrive before its native status. Replay it when that
            // request falls back, even if we never acquired a logical session.
            if (blocking()) setHidden(true);
            else restoreWindow();
        } finally { inspecting = false; }
    }
    function snapshot() {
        const eligible = !!logical && !closingNative && !ignoreOpen;
        return {version: VERSION, request, open: !!logical || !!native.open, eligible,
            active: eligible && acknowledged === request && fresh(),
            local: !!(logical?.owner.external || logical?.dashboard),
            external: logical?.owner.external ? {key: logical.owner.key, request} : null,
            externalCandidate: !logical && !ignoreOpen && native.open && localTarget(native) &&
                fresh() && now() < rawDeadline ? {key: native.key, flags: native.flags, request} : null,
            externalRestore};
    }
    function dispose() {
        if (disposed) return;
        lease = 0;
        try { releaseLogical(true); restoreWindow(); } finally {
            disposed = true;
            root.clearInterval(timer);
            // Observe a pending restore until its bounded check completes. A
            // later summon for this owner must cancel cleanup before another
            // bridge can take it over and close native again.
            if (!restoringOwner) unsubscribe();
            else if (!subscription) checkRestore(true);
            for (const undo of hooks.reverse()) undo();
        }
    }
    try {
        const hookedApis = new Map();
        for (const manager of managers) {
            const api = manager.m_ownerWindow?.SteamClient?.OpenVR?.Keyboard;
            const key = manager.m_Instance?.GetVROverlayKey?.();
            if (!ownSteam(key) || typeof api?.Show !== "function" || typeof api?.Hide !== "function" ||
                typeof manager.SetVRKeyboardOpenForThisInstance !== "function") continue;
            const originals = hookedApis.get(api) || {show: api.Show, hide: api.Hide};
            const owner = {manager, api, key, ...originals};
            if (typeof manager.SetVirtualKeyboardShownInternal === "function") {
                owner.fieldHook = true;
                hook(manager, "SetVirtualKeyboardShownInternal", original => function(open, ...args) {
                    if (open && targetOf(this))
                        owner.fieldIntent = {target: targetOf(this), until: now() + REQUEST_MS};
                    else if (!open) owner.fieldIntent = null;
                    return original.call(this, open, ...args);
                });
            }
            owner.setOpen = hook(manager, "SetVRKeyboardOpenForThisInstance", original => function(open) {
                if (!open && logical?.owner.manager === this) {
                    // Consume exactly the close reaction caused by this request's
                    // Hide. Keep acquisition pending if status arrives first.
                    if (closingNative && expectedClose?.owner === owner &&
                        expectedClose.request === request && !nativeStatus().open) {
                        expectedClose = null;
                        inspect();
                        return;
                    }
                    // Steam has independently closed its field. Never restore it.
                    releaseLogical(false, false);
                }
                return original.call(this, open);
            });
            owner.keyDown = hook(manager, "HandleVirtualKeyDown", original => function(...args) {
                if (logical?.owner.manager === this && !logical.dashboard && fresh()) return;
                return original.apply(this, args);
            });
            owners.push(owner);
            if (!hookedApis.has(api)) {
                hookedApis.set(api, originals);
                hook(api, "Hide", original => function(...args) {
                    if (logical?.owner.api === this) releaseLogical(false);
                    return original.apply(this, args);
                });
            }
        }
        if (!owners.length) throw Error("steam-target-restoration-unavailable");
        for (const name of ["ShowWindow", "BringToFront"]) hook(windowApi, name, original => function(...args) {
            try { inspect(); if (blocking()) { interceptedShow = true; return; } }
            catch (_) { dispose(); }
            return original.apply(this, args);
        });
        // Ignore every callback argument, including initial field text.
        const keyboard = root.SteamClient?.OpenVR?.Keyboard;
        if (typeof keyboard?.RegisterForStatus === "function") subscription = keyboard.RegisterForStatus(() => {
            try { if (disposed) checkRestore(); else inspect(); } catch (_) { dispose(); }
        });
        inspect();
        timer = root.setInterval(() => {
            try {
                inspect();
                if (!fresh() && now() - lastStep > 1500) dispose();
            } catch (_) { dispose(); }
        }, 50);
    } catch (error) { dispose(); throw error; }
    return {
        version: VERSION,
        expired() { return !fresh(); },
        diagnostics() { return {...snapshot(), nativeOpen: !!native.open, closingNative,
            expectedClose: !!expectedClose, session: !!logical, restoring: !!restoringOwner}; },
        step(frame) {
            if (disposed) return {version: VERSION, unavailable: true};
            lastStep = now();
            try {
                if (Number.isSafeInteger(frame.progress) && frame.progress > progress) {
                    progress = frame.progress;
                    lease = frame.ready ? now() + LEASE_MS : 0;
                }
                inspect();
                if (externalRestore && frame.externalRestored === externalRestore.request) externalRestore = null;
                if (frame.fallback && frame.fallbackRequest === request) releaseLogical(true);
                if (snapshot().eligible && fresh() && frame.ack === request &&
                    (acknowledged === request || now() < deadline)) acknowledged = request;
                inspect();
                return snapshot();
            } catch (_) { dispose(); return {version: VERSION, unavailable: true}; }
        },
        send(generation, text) {
            inspect();
            if (!snapshot().active || generation !== request || typeof text !== "string" || text.length > 8192)
                return false;
            const session = logical, manager = session.owner.manager;
            if (session.owner.external || session.dashboard) return false;
            const previous = manager.m_fnVROnTextEnteredOverride;
            const targetText = session.props?.onTextEntered;
            const deliver = typeof targetText === "function" ? targetText : controllerText;
            if (typeof deliver !== "function") return false;
            // The renderer's override can call VRKeyboard.SendText, which requires
            // an open native session. Keep Enter/Done handling in the manager, but
            // dispatch this one call directly to its retained field/controller route.
            const override = value => deliver(value);
            manager.m_fnVROnTextEnteredOverride = override;
            try { session.owner.keyDown.call(manager, text, false); }
            finally {
                if (manager.m_fnVROnTextEnteredOverride === override)
                    manager.m_fnVROnTextEnteredOverride = previous;
            }
            inspect();
            return true;
        },
        dismiss(generation) {
            inspect();
            if (!snapshot().active || generation !== request) return false;
            releaseLogical(false);
            inspect();
            return true;
        },
        prepareExternal(generation, key) {
            inspect();
            const pending = snapshot().externalCandidate;
            if (!pending || pending.request !== generation || pending.key !== key) return snapshot();
            preparedExternal = {external: true, key, api: keyboardApi, hide: externalHide};
            inspect();
            return snapshot();
        },
        dispose,
    };
})
