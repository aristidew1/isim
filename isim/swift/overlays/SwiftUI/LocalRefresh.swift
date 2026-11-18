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

/// A scope seen in the last render that asked for a refresh: what to evaluate again, where, and what it mounted.
@MainActor final class _LocalScope {
    let path: String
    var view: any View
    var ctx: _Context
    var node: _Node
    /// the render (`_Graph.renderCount`) it was resolved in; a later render makes it stale until it registers again
    var renderStamp: Int
    /// the view keys its subtree mounted
    var keys: Set<String> = []
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
    func _beginScope(_ path: String) { scopeStack.append((path, false)) }

    /// A frame (or a timeline entry) is asked for while a scope evaluates: that scope will refresh. Returns its path,
    /// nil outside any scope (the whole graph renders).
    func _requestInScope() -> String? {
        guard let i = scopeStack.indices.last else { return nil }
        scopeStack[i].requested = true
        return scopeStack[i].path
    }

    func _endScope(_ view: any View, _ ctx: _Context, _ node: _Node) {
        guard let e = scopeStack.popLast(), e.requested, !measuring else { return }
        let s: _LocalScope
        if let old = localScopes[e.path] {
            s = old; s.view = view; s.ctx = ctx; s.node = node; s.renderStamp = renderCount
        } else {
            s = _LocalScope(path: e.path, view: view, ctx: ctx, node: node, renderStamp: renderCount)
            localScopes[e.path] = s
        }
        scopeByNode[ObjectIdentifier(node)] = s
    }

    /// Re-evaluates the scope at `path` in place. False when it cannot (not on screen, stale, or its layout changed):
    /// the caller renders the whole graph instead.
    func refreshLocal(_ path: String) -> Bool {
        guard !rendering, let s = localScopes[path], s.renderStamp == renderCount, let host = hostView, host.window != nil else { return false }
        let old = s.node
        guard !old.transparent, let v = views[old.viewKey], let sup = v.superview, v.window != nil else { return false }
        let answers = old.sizeCache
        guard !answers.isEmpty else { return false }
        let oldKeys = s.keys
        let inside = { (k: String) in _LocalScope.isInside(k, path) }

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

        var ok = type(of: node) == type(of: old) && node.viewKey == old.viewKey && !node.transparent && !Self._needsFullLayout(node)
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
            s.keys = now
            mountedKeys = mountedBefore.union(now)
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
        if !ok { s.renderStamp = -1 }           // its new node is not mounted: stale until the full render registers it
        if !ok && _frameStats { print("isim local refresh: \(path) \(type(of: s.view)) -> whole render (layout changed)") }
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

    /// Layout a local pass cannot reproduce: the render's passes over the whole tree (safe-area extension, matched
    /// geometry, right-to-left mirroring) reach into the subtree.
    static func _needsFullLayout(_ n: _Node) -> Bool {
        if n is _IgnoreSafeAreaNode || n is _MatchedNode || n.isRTL { return true }
        for c in n.children where _needsFullLayout(c) { return true }
        if let b = (n as? _BackgroundNode)?.background, _needsFullLayout(b) { return true }
        return false
    }
}
