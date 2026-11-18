// isim SwiftUI: local refresh. SwiftUI re-evaluates only the views whose inputs changed; isim evaluates and lays out
// the whole graph on every change, which is fine for a state change but not for content that redraws every frame on
// its own (TimelineView(.animation), animatable data in flight, keyframes) or on a schedule (TimelineView(.periodic)):
// re-rendering a full screen 60 times a second keeps the main thread busy and every touch waits behind it.
//
// Such views are *scopes*: a frame or timeline entry they ask for re-evaluates the scope alone, lays it out in the
// frame it had and mounts it into its existing views. That holds when the new content answers every size proposal
// the layout around it made exactly as before (so nothing outside moves); otherwise, and for anything a local pass
// cannot reproduce (safe-area extension, matched geometry, right to left), the whole graph renders as before.
import UIKit
@_spi(SwiftUI) import Observation

/// Views that may redraw on their own without a state change (TimelineView); animatable views are scopes too.
protocol _LocalRefreshView {}

// partial frames (UIKit, UIGraphicsBase.m): the changes made in between are confined to a region of the screen
@_silgen_name("isim_ui_damage_begin") func _isimDamageBegin()
@_silgen_name("isim_ui_damage_end") func _isimDamageEnd(_ x: Double, _ y: Double, _ w: Double, _ h: Double)
// content caches (UIKit, UIView.m): instances of this class keep their drawing as pixels between setNeedsDisplay calls
@_silgen_name("isim_ui_content_cacheable") func _isimContentCacheable(_ c: AnyClass)
/// how far past its views' frames a refreshed subtree may draw (shadows, blurs, strokes on the edge)
let _damageMargin: CGFloat = 48
/// ISIM_FRAME_STATS=1: each local refresh is logged with the region it redraws
let _frameStats = (Int(ProcessInfo.processInfo.environment["ISIM_FRAME_STATS"] ?? "") ?? 0) > 0

// Scoped updates: every view with a body is a scope too. Its body is tracked on its own (Observation reads), and its
// @State belongs to it: a change re-evaluates that scope alone, like SwiftUI re-evaluates only the bodies that read
// what changed. When it cannot (its layout changed, it writes navigation bar items or preferences read above it, an
// animated transaction), its enclosing scope is tried, then the whole graph renders as before.
// Off by default (ISIM_SWIFTUI_SCOPED=1 turns it on): where a state change reshapes a whole screen (most of an app's
// taps) the attempt fails and the whole render follows, which costs more than it saves.
let _scopedUpdates = ProcessInfo.processInfo.environment["ISIM_SWIFTUI_SCOPED"] == "1"
// Observation (observation-support.c): the access list withObservationTracking records into on this thread
@_silgen_name("_isim_observation_tls_swap") func _isimObservationSwap(_ list: UnsafeMutableRawPointer?) -> UnsafeMutableRawPointer?

/// One evaluation of a scope's body and its Observation tracking, cancelled once the scope evaluates again or goes
/// (so properties read once and never changed do not keep observers for every past render).
final class _ScopeToken: @unchecked Sendable {
    var live = true
    var tracking: ObservationTracking?
    func end() { live = false; tracking?.cancel(); tracking = nil }
}

/// A scope seen in the last render: what to evaluate again, where, and what it mounted.
@MainActor final class _LocalScope {
    let path: String
    var view: any View
    var ctx: _Context
    var node: _Node
    /// the render (`_Graph.renderCount`) it was resolved in; a later render makes it stale until it registers again
    var renderStamp: Int
    /// the view keys its subtree mounted (known: recorded when it mounted; else found from the mounted keys)
    var keys: Set<String> = []
    var keysKnown = false
    /// the scope it is inside (nil: directly under the root)
    var parentPath: String?
    /// its body's Observation tracking (scoped updates)
    var token: _ScopeToken?
    /// the resolve pass (`_Graph.scopePass`) that last registered it
    var pass = 0
    init(path: String, view: any View, ctx: _Context, node: _Node, renderStamp: Int) {
        self.path = path; self.view = view; self.ctx = ctx; self.node = node; self.renderStamp = renderStamp
    }
    /// Whether a path-derived key (a node path, a view key `path|Type`, a storage key `path#name`) belongs to the
    /// subtree at `base`.
    nonisolated static func isInside(_ key: String, _ base: String) -> Bool {
        guard key.hasPrefix(base) else { return false }
        if key.count == base.count { return true }
        let next = key[key.index(key.startIndex, offsetBy: base.count)]
        return next == "/" || next == "|" || next == "#"
    }
}

extension _Graph {
    /// `animates`: a view that may redraw on its own (animatable data, TimelineView); else a view with a body
    func _beginScope(_ path: String, animates: Bool = true) { scopeStack.append((path, false, animates)) }

    /// A frame (or a timeline entry) is asked for while a scope that animates evaluates: that scope will refresh.
    /// Returns its path, nil outside any such scope (the whole graph renders).
    func _requestInScope() -> String? {
        guard let i = scopeStack.lastIndex(where: { $0.animates }) else { return nil }
        scopeStack[i].requested = true
        return scopeStack[i].path
    }

    /// Evaluates a scope's body with its own Observation tracking: what it reads (and what the views it builds read,
    /// down to the next scope) re-evaluates it alone when it changes.
    func _trackScope(_ path: String, _ token: _ScopeToken, _ body: () -> _Node) -> _Node {
        // what withObservationTracking does, with the access list on its own (not added to the enclosing scope's) and
        // the tracking kept, to cancel
        var list: ObservationTracking._AccessList?
        let n = withUnsafeMutablePointer(to: &list) { ptr -> _Node in
            let outer = _isimObservationSwap(UnsafeMutableRawPointer(ptr))
            defer { _ = _isimObservationSwap(outer) }
            return body()
        }
        guard let list else { return n }
        let tracking = ObservationTracking(list)
        token.tracking = tracking
        ObservationTracking._installTracking(tracking, willSet: { [weak self] t in
            t.cancel()
            guard token.live else { return }           // (evaluated again since: that tracking counts)
            token.live = false
            self?.invalidate(scope: path)
        })
        return n
    }

    func _endScope(_ view: any View, _ ctx: _Context, _ node: _Node, token: _ScopeToken? = nil) {
        guard let e = scopeStack.popLast(), !measuring, e.requested || token != nil else { return }
        let s: _LocalScope
        if let old = localScopes[e.path] {
            s = old; s.view = view; s.ctx = ctx; s.node = node; s.renderStamp = renderCount
            if let token, s.token !== token { s.token?.end(); s.token = token }
        } else {
            s = _LocalScope(path: e.path, view: view, ctx: ctx, node: node, renderStamp: renderCount)
            s.token = token
            localScopes[e.path] = s
        }
        s.keysKnown = false
        s.parentPath = scopeStack.last?.path
        s.pass = scopePass
        if e.requested { scopeByNode[ObjectIdentifier(node)] = s }
    }

    /// A scope's tracked reads or its @State changed: re-evaluate it alone (at the next layout of the host view).
    nonisolated func invalidate(scope path: String) {
        guard Thread.isMainThread else {
            Task { @MainActor [weak self] in self?.invalidate(scope: path) }
            return
        }
        MainActor.assumeIsolated {
            guard _scopedUpdates else { invalidate(); return }
            onInvalidate?()
            dirtyScopes.insert(path)
            if rendering { pending = true; return }
            hostView?.setNeedsLayout()
        }
    }

    /// Re-evaluates the scopes whose state changed, each alone or within an enclosing scope. False when that is not
    /// possible: the whole graph renders (layoutSubviews).
    func updateScopes() -> Bool {
        let dirty = dirtyScopes.sorted { $0.count < $1.count }        // outermost first: it refreshes those inside
        dirtyScopes = []
        var done: [String] = []
        for p in dirty where !done.contains(where: { _LocalScope.isInside(p, $0) }) {
            var path: String? = p, tries = 0
            while true {
                guard let q = path, tries < 4, localScopes[q] != nil else { return false }
                if done.contains(where: { _LocalScope.isInside(q, $0) }) { break }
                if refreshLocal(q, forState: true) { done.append(q); break }
                path = localScopes[q]?.parentPath; tries += 1
            }
        }
        return true
    }

    /// Anything in a subtree that the views around it read after a render: preferences (navigation titles and
    /// toolbars go through the navigation level, counted by `_NavLevel.writes`).
    static func _writesUpward(_ n: _Node) -> Bool {
        if n is _PreferenceNode || n is _AnchorSourceNode { return true }
        for c in n.children where _writesUpward(c) { return true }
        if let b = (n as? _BackgroundNode)?.background, _writesUpward(b) { return true }
        if let l = n as? _ListNode {
            for s in l.sections { if let h = s.header, _writesUpward(h) { return true }; for r in s.rows where _writesUpward(r) { return true } }
        }
        return false
    }

    /// Re-evaluates the scope at `path` in place. False when it cannot (not on screen, stale, or its layout changed):
    /// the caller renders the whole graph instead.
    /// `forState`: a state change, not an animation frame (what the scope writes for the views around it is checked).
    func refreshLocal(_ path: String, forState: Bool = false) -> Bool {
        guard !rendering, let s = localScopes[path], s.renderStamp == renderCount, let host = hostView, host.window != nil else { return false }
        let old = s.node
        guard !old.transparent, let v = views[old.viewKey], let sup = v.superview, v.window != nil else { return false }
        let answers = old.sizeCache
        guard !answers.isEmpty else { return false }
        let inside = { (k: String) in _LocalScope.isInside(k, path) }
        let oldKeys = s.keysKnown ? s.keys : mountedKeys.filter(inside)
        if forState && Self._writesUpward(old) { return false }
        let navWrites = _NavLevel.writes
        scopePass += 1

        rendering = true
        _Node.layoutEpoch += 1
        let saved = (usedKeys, usedChanges, usedTasks, usedAppear, usedSubscriptions, focusLinks, postRender)
        usedKeys = []; usedChanges = []; usedTasks = []; usedAppear = []; usedSubscriptions = []; focusLinks = []; postRender = []
        scopeStack = []
        // a timeline entry or an animation frame is not part of any transaction
        let ctx = s.ctx.with { $0._transaction = Transaction() }
        var resolved: _Node?
        withObservationTracking { resolved = _resolve(s.view, ctx) } onChange: { [weak self] in self?.invalidate() }
        let node = resolved!

        // (navigation bar items are gathered by the whole render; preferences are read by the views around)
        var ok = type(of: node) == type(of: old) && node.viewKey == old.viewKey && !node.transparent && !Self._needsFullLayout(node) &&
            _NavLevel.writes == navWrites && !(forState && Self._writesUpward(node))
        if ok {
            _containerSizes = [host.bounds.inset(by: safeArea).size]
            for (k, size) in answers {
                let a = node.sizeThatFits(_Proposal(width: k.w < 0 ? nil : k.w, height: k.h < 0 ? nil : k.h))
                if abs(a.width - size.width) > 0.01 || abs(a.height - size.height) > 0.01 { ok = false; break }
            }
        }
        if ok {
            node.place(old.frame)
            _containerSizes = []
            // what the views change stays where the subtree was and is: only that part of the screen is drawn again
            let before = _screenExtent(v)
            _isimDamageBegin()
            defer {
                let r = before.union(_screenExtent(v)).insetBy(dx: -_damageMargin, dy: -_damageMargin)
                _isimDamageEnd(r.minX, r.minY, r.width, r.height)
                if _frameStats { print("isim local refresh: \(path) \(type(of: s.view)) -> \(r)") }
            }
            let index = sup.subviews.firstIndex { $0 === v }
            let mountedBefore = mountedKeys
            mountedKeys = []; fresh = []; mountedViews = []; originShift = [:]; oldSizes = [:]
            mount(node, in: sup, order: 0)
            // (mount brings a view it reuses to the front: the scope keeps its place among its siblings)
            if let i = index, sup.subviews.firstIndex(where: { $0 === v }) != i { sup.insertSubview(v, at: i) }
            let now = mountedKeys
            for k in oldKeys where !now.contains(k) { views[k]?.removeFromSuperview(); views[k] = nil }
            s.keys = now; s.keysKnown = true
            mountedKeys = mountedBefore.subtracting(oldKeys).union(now)
            // scopes inside it that this pass did not meet again are gone
            for (k, sc) in localScopes where sc.pass != scopePass && k != path && inside(k) { sc.token?.end(); localScopes[k] = nil }
            fresh = []; mountedViews = []
            // what the subtree no longer uses goes, as after a render (only inside the scope)
            storage = storage.filter { usedKeys.contains($0.key) || !inside($0.key) }
            changeValues = changeValues.filter { usedChanges.contains($0.key) || !inside($0.key) }
            for (k, t) in tasks where !usedTasks.contains(k) && inside(k) { t.cancel(); tasks[k] = nil }
            for k in subscriptions.keys where !usedSubscriptions.contains(k) && inside(k) { subscriptions[k] = nil; receiveActions[k] = nil }
            for k in appeared where !usedAppear.contains(k) && inside(k) { appeared.remove(k); disappearActions.removeValue(forKey: k)?() }
        }
        _containerSizes = []
        scopeByNode = [:]
        if !ok {
            // its new nodes are not mounted: it and the scopes this pass met inside it are stale until a refresh of a
            // scope around them, or the whole render, registers them again
            s.renderStamp = -1
            for (k, sc) in localScopes where sc.pass == scopePass && inside(k) { sc.renderStamp = -1 }
        }
        if !ok && _frameStats { print("isim local refresh: \(path) \(type(of: s.view)) -> whole render (layout changed)\(forState ? " [state]" : "")") }
        usedKeys.formUnion(saved.0); usedChanges.formUnion(saved.1); usedTasks.formUnion(saved.2)
        usedAppear.formUnion(saved.3); usedSubscriptions.formUnion(saved.4)
        focusLinks = saved.5.filter { !inside($0.0) } + focusLinks
        let work = postRender
        postRender = saved.6
        rendering = false
        for w in work { w() }
        if ok { verifyGeometry() }
        if pending { pending = false; hostView?.setNeedsLayout() }
        return ok
    }

    /// Where a view and its subviews draw, in screen points (their frames; shadows and blurs reach past them, which
    /// `_damageMargin` covers). CGRect.null when the view is not on screen.
    func _screenExtent(_ v: UIView) -> CGRect {
        guard let w = v.window else { return .null }
        var r = CGRect.null
        func walk(_ x: UIView) {
            if x.isHidden { return }
            r = r.union(x.convert(x.bounds, to: nil))
            for s in x.subviews { walk(s) }
        }
        walk(v)
        return r.isNull ? r : r.offsetBy(dx: w.frame.minX, dy: w.frame.minY)
    }

    /// Whether the scope at `path` shows nothing now: hidden, outside the window, or clipped away by an ancestor
    /// (with `_damageMargin` for shadows). False when that is not known: the refresh decides.
    func _scopeOffscreen(_ path: String) -> Bool {
        guard !rendering, let s = localScopes[path], s.renderStamp == renderCount, !s.node.transparent,
              let v = views[s.node.viewKey], let w = v.window else { return false }
        var r = CGRect.null
        func walk(_ x: UIView) {
            if x.isHidden { return }
            r = r.union(x.convert(x.bounds, to: nil))
            for c in x.subviews { walk(c) }
        }
        walk(v)
        if r.isNull { return true }
        r = r.insetBy(dx: -_damageMargin, dy: -_damageMargin)
        var a: UIView? = v.superview
        while let x = a, x !== w {
            if x.isHidden || x.alpha <= 0 { return true }
            if x.clipsToBounds { r = r.intersection(x.convert(x.bounds, to: nil)) }
            a = x.superview
        }
        r = r.intersection(w.bounds)
        return r.isNull || r.isEmpty
    }

    /// Layout a local pass cannot reproduce: the render's passes over the whole tree (safe-area extension, matched
    /// geometry, right-to-left mirroring) reach into the subtree.
    static func _needsFullLayout(_ n: _Node) -> Bool {
        if n is _IgnoreSafeAreaNode || n is _MatchedNode || n.isRTL { return true }
        for c in n.children where _needsFullLayout(c) { return true }
        if let b = (n as? _BackgroundNode)?.background, _needsFullLayout(b) { return true }
        return false
    }
}
