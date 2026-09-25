#if !defined(LITE_VERSION) && !defined(DISABLE_INTERPRETER)
#include "net_spoof_js.h"

#include "helpers_js.h"
#include "modules/wifi/net_spoof.h"

#include <IPAddress.h>

// ---------------------------------------------------------------------------
// Shared helpers
//
// Every binding below returns a plain object, and the object plus any array
// inside it are pinned in JSGCRefs while they are filled: mquickjs has a
// compacting heap, so each JS_NewString() can relocate everything and a plain
// JSValue local would go stale. That is why the code always works on the
// pointer returned by JS_PushGCRef() and re-reads it with *obj after every
// allocating call (native_wifiGetCapturedPackets() in wifi_js.cpp is the
// canonical write-up of the hazard).
// ---------------------------------------------------------------------------

static bool ns_ip_arg(JSContext *ctx, JSValue val, IPAddress &out, String &txt) {
    if (!JS_IsString(ctx, val)) return false;
    txt = js_tocstring_copy(ctx, val);
    txt.trim();
    if (txt.length() == 0) return false;
    return out.fromString(txt.c_str());
}

/// Push a new pinned object. On failure *obj is JS_EXCEPTION.
static JSValue *ns_new_object(JSContext *ctx, JSGCRef *ref) {
    JSValue *obj = JS_PushGCRef(ctx, ref);
    *obj = JS_NewObject(ctx);
    return obj;
}

static void ns_set_error(JSContext *ctx, JSValue *obj, const String &err) {
    JS_SetPropertyStr(ctx, *obj, "ok", JS_NewBool(false));
    JS_SetPropertyStr(
        ctx, *obj, "error", JS_NewString(ctx, err.length() ? err.c_str() : "unknown error")
    );
}

// ===========================================================================
// arpSpoof
// ===========================================================================

JSValue native_arpSpoofStart(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: arpSpoof.start(targetIP: string, gatewayIP?: string)
    //   Two-way ARP poison between targetIP and the gateway (default: the
    //   current default gateway). Needs an associated station: wifi.connect().
    // returns: { ok, targetIP, gatewayIP, targetMAC, gatewayMAC, pairs, error? }
    if (argc < 1) return JS_ThrowTypeError(ctx, "arpSpoof.start(targetIP:string, gatewayIP?:string)");

    IPAddress target, gateway;
    String targetTxt, gatewayTxt;
    if (!ns_ip_arg(ctx, argv[0], target, targetTxt)) {
        return JS_ThrowTypeError(ctx, "arpSpoof.start: targetIP must be an IPv4 address");
    }
    bool haveGateway = false;
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        haveGateway = ns_ip_arg(ctx, argv[1], gateway, gatewayTxt);
        if (!haveGateway) {
            return JS_ThrowTypeError(ctx, "arpSpoof.start: gatewayIP must be an IPv4 address");
        }
    }

    JSGCRef obj_ref;
    JSValue *obj = ns_new_object(ctx, &obj_ref);
    if (JS_IsException(*obj)) {
        JS_PopGCRef(ctx, &obj_ref);
        return JS_ThrowOutOfMemory(ctx);
    }

    JS_SetPropertyStr(ctx, *obj, "targetIP", JS_NewString(ctx, target.toString().c_str()));
    JS_SetPropertyStr(
        ctx, *obj, "gatewayIP", JS_NewString(ctx, haveGateway ? gateway.toString().c_str() : "")
    );
    JS_SetPropertyStr(ctx, *obj, "ok", JS_NewBool(false));

    String err;
    if (!netspoof_arp_start(target, haveGateway ? gateway : IPAddress((uint32_t)0), err)) {
        ns_set_error(ctx, obj, err);
        JS_SetPropertyStr(ctx, *obj, "pairs", JS_NewInt32(ctx, netspoof_arp_pair_count()));
        return JS_PopGCRef(ctx, &obj_ref);
    }

    // Report the pair that is actually active, so a script can log the MACs.
    char tMac[18] = {0}, gMac[18] = {0};
    IPAddress pT, pG;
    int pairs = netspoof_arp_pair_count();
    for (int i = 0; i < pairs; i++) {
        if (netspoof_arp_pair_info(i, pT, pG, tMac, gMac) && pT == target) break;
    }

    JS_SetPropertyStr(ctx, *obj, "ok", JS_NewBool(true));
    if (!haveGateway) {
        JS_SetPropertyStr(ctx, *obj, "gatewayIP", JS_NewString(ctx, pG.toString().c_str()));
    }
    JS_SetPropertyStr(ctx, *obj, "targetMAC", JS_NewString(ctx, tMac));
    JS_SetPropertyStr(ctx, *obj, "gatewayMAC", JS_NewString(ctx, gMac));
    JS_SetPropertyStr(ctx, *obj, "pairs", JS_NewInt32(ctx, pairs));
    return JS_PopGCRef(ctx, &obj_ref);
}

JSValue native_arpSpoofStop(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: arpSpoof.stop()
    //   Restores the real ARP mappings and stops poisoning.
    // returns: { ok, restored, pairs, error? }
    JSGCRef obj_ref;
    JSValue *obj = ns_new_object(ctx, &obj_ref);
    if (JS_IsException(*obj)) {
        JS_PopGCRef(ctx, &obj_ref);
        return JS_ThrowOutOfMemory(ctx);
    }

    int before = netspoof_arp_pair_count();
    String err;
    bool ok = netspoof_arp_stop(err);
    JS_SetPropertyStr(ctx, *obj, "ok", JS_NewBool(ok));
    JS_SetPropertyStr(ctx, *obj, "restored", JS_NewInt32(ctx, before));
    JS_SetPropertyStr(ctx, *obj, "pairs", JS_NewInt32(ctx, netspoof_arp_pair_count()));
    if (!ok) JS_SetPropertyStr(ctx, *obj, "error", JS_NewString(ctx, err.c_str()));
    return JS_PopGCRef(ctx, &obj_ref);
}

JSValue native_arpSpoofGetStatus(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: arpSpoof.getStatus()
    // returns: { ok, active, pairs, frames, uptimeMs, hookInstalled, targets,
    //            dnsRules, httpRules,
    //            arp: [{ targetIP, gatewayIP, targetMAC, gatewayMAC }] }
    JSGCRef obj_ref;
    JSValue *obj = ns_new_object(ctx, &obj_ref);
    if (JS_IsException(*obj)) {
        JS_PopGCRef(ctx, &obj_ref);
        return JS_ThrowOutOfMemory(ctx);
    }

    int pairs = netspoof_arp_pair_count();
    JS_SetPropertyStr(ctx, *obj, "ok", JS_NewBool(true));
    JS_SetPropertyStr(ctx, *obj, "active", JS_NewBool(pairs > 0));
    JS_SetPropertyStr(ctx, *obj, "pairs", JS_NewInt32(ctx, pairs));
    JS_SetPropertyStr(ctx, *obj, "frames", JS_NewInt32(ctx, (int)netspoof_arp_frames()));
    JS_SetPropertyStr(ctx, *obj, "uptimeMs", JS_NewInt32(ctx, (int)netspoof_arp_uptime_ms()));
    JS_SetPropertyStr(ctx, *obj, "hookInstalled", JS_NewBool(netspoof_hook_installed()));
    JS_SetPropertyStr(ctx, *obj, "targets", JS_NewInt32(ctx, netspoof_target_count()));
    JS_SetPropertyStr(ctx, *obj, "dnsRules", JS_NewInt32(ctx, netspoof_dns_rule_count()));
    JS_SetPropertyStr(
        ctx,
        *obj,
        "httpRules",
        JS_NewInt32(ctx, netspoof_http_redirect_count() + netspoof_http_inject_count())
    );

    JSGCRef arr_ref;
    JSValue *arr = JS_PushGCRef(ctx, &arr_ref);
    *arr = JS_NewArray(ctx, pairs);
    if (JS_IsException(*arr)) {
        JS_PopGCRef(ctx, &arr_ref);
        return JS_PopGCRef(ctx, &obj_ref);
    }
    for (int i = 0; i < pairs; i++) {
        IPAddress t, g;
        char tMac[18] = {0}, gMac[18] = {0};
        if (!netspoof_arp_pair_info(i, t, g, tMac, gMac)) continue;

        JSGCRef entry_ref;
        JSValue *entry = JS_PushGCRef(ctx, &entry_ref);
        *entry = JS_NewObject(ctx);
        if (JS_IsException(*entry)) {
            JS_PopGCRef(ctx, &entry_ref);
            break;
        }
        JS_SetPropertyStr(ctx, *entry, "targetIP", JS_NewString(ctx, t.toString().c_str()));
        JS_SetPropertyStr(ctx, *entry, "gatewayIP", JS_NewString(ctx, g.toString().c_str()));
        JS_SetPropertyStr(ctx, *entry, "targetMAC", JS_NewString(ctx, tMac));
        JS_SetPropertyStr(ctx, *entry, "gatewayMAC", JS_NewString(ctx, gMac));
        JS_SetPropertyUint32(ctx, *arr, (uint32_t)i, *entry);
        JS_PopGCRef(ctx, &entry_ref);
    }
    JS_SetPropertyStr(ctx, *obj, "arp", *arr);
    JS_PopGCRef(ctx, &arr_ref);
    return JS_PopGCRef(ctx, &obj_ref);
}

JSValue native_arpSpoofGetTargets(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: arpSpoof.getTargets(rescan?: boolean)
    //   Hosts found on the local subnet (ARP scan). Needs a station
    //   connection. Rescans only when the list is still empty unless
    //   `rescan` is true.
    // returns: { ok, count, targets: [{ ip, mac, gateway, spoofed, pairedWith }],
    //            error? }
    bool rescan = false;
    if (argc > 0 && !JS_IsUndefined(argv[0])) rescan = JS_ToBool(ctx, argv[0]);

    JSGCRef obj_ref;
    JSValue *obj = ns_new_object(ctx, &obj_ref);
    if (JS_IsException(*obj)) {
        JS_PopGCRef(ctx, &obj_ref);
        return JS_ThrowOutOfMemory(ctx);
    }

    String err;
    int found = netspoof_target_count();
    if (rescan || found == 0) {
        found = netspoof_scan_targets(8000, err);
        if (found < 0) {
            JS_SetPropertyStr(ctx, *obj, "ok", JS_NewBool(false));
            JS_SetPropertyStr(ctx, *obj, "count", JS_NewInt32(ctx, 0));
            JS_SetPropertyStr(ctx, *obj, "error", JS_NewString(ctx, err.c_str()));
            JSGCRef empty_ref;
            JSValue *empty = JS_PushGCRef(ctx, &empty_ref);
            *empty = JS_NewArray(ctx, 0);
            JS_SetPropertyStr(ctx, *obj, "targets", *empty);
            JS_PopGCRef(ctx, &empty_ref);
            return JS_PopGCRef(ctx, &obj_ref);
        }
    }

    JSGCRef arr_ref;
    JSValue *arr = JS_PushGCRef(ctx, &arr_ref);
    *arr = JS_NewArray(ctx, found);
    if (JS_IsException(*arr)) {
        JS_PopGCRef(ctx, &arr_ref);
        return JS_PopGCRef(ctx, &obj_ref);
    }

    for (int i = 0; i < found; i++) {
        const NetSpoofTarget *t = netspoof_target(i);
        if (!t) continue;

        int pairIdx = -1;
        bool spoofed = netspoof_arp_pair_index(t->ip, &pairIdx);

        JSGCRef entry_ref;
        JSValue *entry = JS_PushGCRef(ctx, &entry_ref);
        *entry = JS_NewObject(ctx);
        if (JS_IsException(*entry)) {
            JS_PopGCRef(ctx, &entry_ref);
            break;
        }
        JS_SetPropertyStr(ctx, *entry, "ip", JS_NewString(ctx, t->ip.toString().c_str()));
        JS_SetPropertyStr(ctx, *entry, "mac", JS_NewString(ctx, t->macStr.c_str()));
        JS_SetPropertyStr(ctx, *entry, "gateway", JS_NewBool(t->gateway));
        JS_SetPropertyStr(ctx, *entry, "spoofed", JS_NewBool(spoofed));
        if (spoofed) {
            IPAddress pT, pG;
            char tMac[18] = {0}, gMac[18] = {0};
            netspoof_arp_pair_info(pairIdx, pT, pG, tMac, gMac);
            JS_SetPropertyStr(ctx, *entry, "pairedWith", JS_NewString(ctx, gMac));
        }
        JS_SetPropertyUint32(ctx, *arr, (uint32_t)i, *entry);
        JS_PopGCRef(ctx, &entry_ref);
    }
    JS_SetPropertyStr(ctx, *obj, "targets", *arr);
    JS_SetPropertyStr(ctx, *obj, "count", JS_NewInt32(ctx, found));
    JS_SetPropertyStr(ctx, *obj, "ok", JS_NewBool(true));
    JS_PopGCRef(ctx, &arr_ref);
    return JS_PopGCRef(ctx, &obj_ref);
}

// ===========================================================================
// dnsSpoof
// ===========================================================================

static JSValue ns_dns_add_rule_common(
    JSContext *ctx, int argc, JSValue *argv, bool wildcardMode, const char *usage
) {
    if (argc < 2 || !JS_IsString(ctx, argv[0]) || !JS_IsString(ctx, argv[1])) {
        return JS_ThrowTypeError(ctx, "%s", usage);
    }

    String domain = js_tocstring_copy(ctx, argv[0]);
    IPAddress ip;
    String ipTxt;
    if (!ns_ip_arg(ctx, argv[1], ip, ipTxt)) {
        return JS_ThrowTypeError(ctx, "%s: the IP must be an IPv4 address", usage);
    }
    domain.trim();

    bool wildcard = wildcardMode;
    if (domain.length() == 0) {
        if (!wildcardMode) {
            return JS_ThrowTypeError(ctx, "%s: domain is required", usage);
        }
        domain = "*";
    }
    if (domain == "*") wildcard = true;

    JSGCRef obj_ref;
    JSValue *obj = ns_new_object(ctx, &obj_ref);
    if (JS_IsException(*obj)) {
        JS_PopGCRef(ctx, &obj_ref);
        return JS_ThrowOutOfMemory(ctx);
    }

    JS_SetPropertyStr(ctx, *obj, "domain", JS_NewString(ctx, domain.c_str()));
    JS_SetPropertyStr(ctx, *obj, "ip", JS_NewString(ctx, ipTxt.c_str()));
    JS_SetPropertyStr(ctx, *obj, "ok", JS_NewBool(false));

    String err;
    if (!netspoof_dns_add_rule(domain, ipTxt, wildcard, err)) {
        ns_set_error(ctx, obj, err);
        JS_SetPropertyStr(ctx, *obj, "rules", JS_NewInt32(ctx, netspoof_dns_rule_count()));
        return JS_PopGCRef(ctx, &obj_ref);
    }

    // Adding a rule is what turns interception on: the engine needs at least
    // one rule to be useful, so there is no separate "arm" step.
    String startErr;
    bool started = netspoof_dns_start(startErr);

    JS_SetPropertyStr(ctx, *obj, "ok", JS_NewBool(started));
    JS_SetPropertyStr(ctx, *obj, "wildcard", JS_NewBool(wildcard));
    JS_SetPropertyStr(ctx, *obj, "active", JS_NewBool(netspoof_dns_active()));
    JS_SetPropertyStr(ctx, *obj, "rules", JS_NewInt32(ctx, netspoof_dns_rule_count()));
    if (!started) JS_SetPropertyStr(ctx, *obj, "error", JS_NewString(ctx, startErr.c_str()));
    return JS_PopGCRef(ctx, &obj_ref);
}

JSValue native_dnsSpoofStart(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: dnsSpoof.start(domain: string, ip: string)
    //   Redirects `domain` (and its subdomains) to `ip` and starts answering.
    //   Needs an ARP pair to actually see the queries: run arpSpoof.start first.
    // returns: { ok, domain, ip, wildcard, active, rules, error? }
    return ns_dns_add_rule_common(ctx, argc, argv, false, "dnsSpoof.start(domain:string, ip:string)");
}

JSValue native_dnsSpoofStartAll(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: dnsSpoof.startAll(pattern?: string, ip: string)
    //   Answers every DNS query with `ip`. '*' (or an empty pattern) matches
    //   every name; anything else is treated as a name/suffix to match.
    // returns: { ok, domain, ip, wildcard, active, rules, error? }
    return ns_dns_add_rule_common(
        ctx, argc, argv, true, "dnsSpoof.startAll(pattern:string, ip:string)"
    );
}

JSValue native_dnsSpoofStop(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: dnsSpoof.stop()
    //   Stops answering. Rules are kept until clearRules().
    // returns: { ok, wasActive, active, queries, answers, rules }
    JSGCRef obj_ref;
    JSValue *obj = ns_new_object(ctx, &obj_ref);
    if (JS_IsException(*obj)) {
        JS_PopGCRef(ctx, &obj_ref);
        return JS_ThrowOutOfMemory(ctx);
    }

    bool wasActive = netspoof_dns_active();
    String err;
    netspoof_dns_stop(err);
    JS_SetPropertyStr(ctx, *obj, "ok", JS_NewBool(true));
    JS_SetPropertyStr(ctx, *obj, "wasActive", JS_NewBool(wasActive));
    JS_SetPropertyStr(ctx, *obj, "active", JS_NewBool(netspoof_dns_active()));
    JS_SetPropertyStr(ctx, *obj, "queries", JS_NewInt32(ctx, (int)netspoof_dns_queries()));
    JS_SetPropertyStr(ctx, *obj, "answers", JS_NewInt32(ctx, (int)netspoof_dns_answers()));
    JS_SetPropertyStr(ctx, *obj, "rules", JS_NewInt32(ctx, netspoof_dns_rule_count()));
    return JS_PopGCRef(ctx, &obj_ref);
}

JSValue native_dnsSpoofGetStatus(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: dnsSpoof.getStatus()
    // returns: { ok, active, queries, answers, rules, hasWildcard, arpActive,
    //            hookInstalled, rulesList: [{ domain, ip, wildcard, hits }] }
    JSGCRef obj_ref;
    JSValue *obj = ns_new_object(ctx, &obj_ref);
    if (JS_IsException(*obj)) {
        JS_PopGCRef(ctx, &obj_ref);
        return JS_ThrowOutOfMemory(ctx);
    }

    int count = netspoof_dns_rule_count();
    JS_SetPropertyStr(ctx, *obj, "ok", JS_NewBool(true));
    JS_SetPropertyStr(ctx, *obj, "active", JS_NewBool(netspoof_dns_active()));
    JS_SetPropertyStr(ctx, *obj, "queries", JS_NewInt32(ctx, (int)netspoof_dns_queries()));
    JS_SetPropertyStr(ctx, *obj, "answers", JS_NewInt32(ctx, (int)netspoof_dns_answers()));
    JS_SetPropertyStr(ctx, *obj, "rules", JS_NewInt32(ctx, count));
    JS_SetPropertyStr(ctx, *obj, "arpActive", JS_NewBool(netspoof_arp_active()));
    JS_SetPropertyStr(ctx, *obj, "hookInstalled", JS_NewBool(netspoof_hook_installed()));

    bool wildcard = false;
    JSGCRef arr_ref;
    JSValue *arr = JS_PushGCRef(ctx, &arr_ref);
    *arr = JS_NewArray(ctx, count);
    if (JS_IsException(*arr)) {
        JS_PopGCRef(ctx, &arr_ref);
        return JS_PopGCRef(ctx, &obj_ref);
    }
    for (int i = 0; i < count; i++) {
        const NetSpoofRule *r = netspoof_dns_rule(i);
        if (!r) continue;
        if (r->wildcard) wildcard = true;

        JSGCRef entry_ref;
        JSValue *entry = JS_PushGCRef(ctx, &entry_ref);
        *entry = JS_NewObject(ctx);
        if (JS_IsException(*entry)) {
            JS_PopGCRef(ctx, &entry_ref);
            break;
        }
        JS_SetPropertyStr(ctx, *entry, "domain", JS_NewString(ctx, r->match.c_str()));
        JS_SetPropertyStr(ctx, *entry, "ip", JS_NewString(ctx, r->value.c_str()));
        JS_SetPropertyStr(ctx, *entry, "wildcard", JS_NewBool(r->wildcard));
        JS_SetPropertyStr(ctx, *entry, "hits", JS_NewInt32(ctx, (int)r->hits));
        JS_SetPropertyUint32(ctx, *arr, (uint32_t)i, *entry);
        JS_PopGCRef(ctx, &entry_ref);
    }
    JS_SetPropertyStr(ctx, *obj, "rulesList", *arr);
    JS_SetPropertyStr(ctx, *obj, "hasWildcard", JS_NewBool(wildcard));
    JS_PopGCRef(ctx, &arr_ref);
    return JS_PopGCRef(ctx, &obj_ref);
}

JSValue native_dnsSpoofClearRules(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: dnsSpoof.clearRules()
    //   Drops every rule. Interception keeps running and then answers nothing.
    // returns: { ok, cleared, rules }
    JSGCRef obj_ref;
    JSValue *obj = ns_new_object(ctx, &obj_ref);
    if (JS_IsException(*obj)) {
        JS_PopGCRef(ctx, &obj_ref);
        return JS_ThrowOutOfMemory(ctx);
    }

    int before = netspoof_dns_rule_count();
    netspoof_dns_clear_rules();
    JS_SetPropertyStr(ctx, *obj, "ok", JS_NewBool(true));
    JS_SetPropertyStr(ctx, *obj, "cleared", JS_NewInt32(ctx, before));
    JS_SetPropertyStr(ctx, *obj, "rules", JS_NewInt32(ctx, netspoof_dns_rule_count()));
    return JS_PopGCRef(ctx, &obj_ref);
}

// ===========================================================================
// httpInterceptor
// ===========================================================================

JSValue native_httpInterceptorStart(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: httpInterceptor.start()
    //   Turns plain-HTTP interception on. Needs an ARP pair (arpSpoof.start)
    //   for the requests to arrive here. Matching requests are answered with
    //   the configured redirect/injection and every request is recorded for
    //   getInterceptedData().
    // returns: { ok, active, redirects, injections, error? }
    JSGCRef obj_ref;
    JSValue *obj = ns_new_object(ctx, &obj_ref);
    if (JS_IsException(*obj)) {
        JS_PopGCRef(ctx, &obj_ref);
        return JS_ThrowOutOfMemory(ctx);
    }

    String err;
    bool ok = netspoof_http_start(err);
    JS_SetPropertyStr(ctx, *obj, "ok", JS_NewBool(ok));
    JS_SetPropertyStr(ctx, *obj, "active", JS_NewBool(netspoof_http_active()));
    JS_SetPropertyStr(ctx, *obj, "redirects", JS_NewInt32(ctx, netspoof_http_redirect_count()));
    JS_SetPropertyStr(ctx, *obj, "injections", JS_NewInt32(ctx, netspoof_http_inject_count()));
    if (!ok) JS_SetPropertyStr(ctx, *obj, "error", JS_NewString(ctx, err.c_str()));
    return JS_PopGCRef(ctx, &obj_ref);
}

JSValue native_httpInterceptorStop(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: httpInterceptor.stop()
    // returns: { ok, wasActive, active, requests, redirected, injected }
    JSGCRef obj_ref;
    JSValue *obj = ns_new_object(ctx, &obj_ref);
    if (JS_IsException(*obj)) {
        JS_PopGCRef(ctx, &obj_ref);
        return JS_ThrowOutOfMemory(ctx);
    }

    bool wasActive = netspoof_http_active();
    String err;
    netspoof_http_stop(err);
    JS_SetPropertyStr(ctx, *obj, "ok", JS_NewBool(true));
    JS_SetPropertyStr(ctx, *obj, "wasActive", JS_NewBool(wasActive));
    JS_SetPropertyStr(ctx, *obj, "active", JS_NewBool(netspoof_http_active()));
    JS_SetPropertyStr(ctx, *obj, "requests", JS_NewInt32(ctx, (int)netspoof_http_requests()));
    JS_SetPropertyStr(ctx, *obj, "redirected", JS_NewInt32(ctx, (int)netspoof_http_redirected()));
    JS_SetPropertyStr(ctx, *obj, "injected", JS_NewInt32(ctx, (int)netspoof_http_injected()));
    return JS_PopGCRef(ctx, &obj_ref);
}

static JSValue ns_http_add_rule(
    JSContext *ctx, int argc, JSValue *argv, bool redirect, const char *usage
) {
    if (argc < 2 || !JS_IsString(ctx, argv[0]) || !JS_IsString(ctx, argv[1])) {
        return JS_ThrowTypeError(ctx, "%s", usage);
    }
    String pattern = js_tocstring_copy(ctx, argv[0]);
    String value = js_tocstring_copy(ctx, argv[1]);
    pattern.trim();

    bool wildcard = (pattern == "*" || pattern.length() == 0);

    JSGCRef obj_ref;
    JSValue *obj = ns_new_object(ctx, &obj_ref);
    if (JS_IsException(*obj)) {
        JS_PopGCRef(ctx, &obj_ref);
        return JS_ThrowOutOfMemory(ctx);
    }

    JS_SetPropertyStr(ctx, *obj, "pattern", JS_NewString(ctx, pattern.c_str()));
    JS_SetPropertyStr(ctx, *obj, "ok", JS_NewBool(false));

    String err;
    bool ok = redirect ? netspoof_http_add_redirect(pattern, value, wildcard, err)
                       : netspoof_http_add_injection(pattern, value, wildcard, err);

    if (redirect) {
        JS_SetPropertyStr(ctx, *obj, "redirectURL", JS_NewString(ctx, value.c_str()));
        JS_SetPropertyStr(ctx, *obj, "redirects", JS_NewInt32(ctx, netspoof_http_redirect_count()));
    } else {
        JS_SetPropertyStr(ctx, *obj, "htmlBytes", JS_NewInt32(ctx, (int)value.length()));
        JS_SetPropertyStr(ctx, *obj, "injections", JS_NewInt32(ctx, netspoof_http_inject_count()));
    }
    if (!ok) {
        ns_set_error(ctx, obj, err);
        return JS_PopGCRef(ctx, &obj_ref);
    }
    JS_SetPropertyStr(ctx, *obj, "ok", JS_NewBool(true));
    JS_SetPropertyStr(ctx, *obj, "wildcard", JS_NewBool(wildcard));
    return JS_PopGCRef(ctx, &obj_ref);
}

JSValue native_httpInterceptorAddRedirect(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: httpInterceptor.addRedirect(pattern: string, redirectURL: string)
    //   `pattern` is matched (case-insensitively) against the host and against
    //   the full URL; "*" matches everything. The reply is a 302 to redirectURL.
    // returns: { ok, pattern, redirectURL, redirects, wildcard, error? }
    return ns_http_add_rule(
        ctx, argc, argv, true, "httpInterceptor.addRedirect(pattern:string, redirectURL:string)"
    );
}

JSValue native_httpInterceptorAddInjection(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: httpInterceptor.addInjection(pattern: string, htmlContent: string)
    //   The reply is "HTTP/1.1 200 OK" with htmlContent as the body, trimmed so
    //   the whole response fits one ethernet frame (~1400 bytes).
    // returns: { ok, pattern, htmlBytes, injections, wildcard, error? }
    return ns_http_add_rule(
        ctx, argc, argv, false, "httpInterceptor.addInjection(pattern:string, htmlContent:string)"
    );
}

JSValue native_httpInterceptorClearRules(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    // usage: httpInterceptor.clearRules()
    // returns: { ok, clearedRedirects, clearedInjections, redirects, injections }
    JSGCRef obj_ref;
    JSValue *obj = ns_new_object(ctx, &obj_ref);
    if (JS_IsException(*obj)) {
        JS_PopGCRef(ctx, &obj_ref);
        return JS_ThrowOutOfMemory(ctx);
    }

    int r = netspoof_http_redirect_count();
    int i = netspoof_http_inject_count();
    netspoof_http_clear_rules();
    JS_SetPropertyStr(ctx, *obj, "ok", JS_NewBool(true));
    JS_SetPropertyStr(ctx, *obj, "clearedRedirects", JS_NewInt32(ctx, r));
    JS_SetPropertyStr(ctx, *obj, "clearedInjections", JS_NewInt32(ctx, i));
    JS_SetPropertyStr(ctx, *obj, "redirects", JS_NewInt32(ctx, netspoof_http_redirect_count()));
    JS_SetPropertyStr(ctx, *obj, "injections", JS_NewInt32(ctx, netspoof_http_inject_count()));
    return JS_PopGCRef(ctx, &obj_ref);
}

JSValue native_httpInterceptorGetInterceptedData(
    JSContext *ctx, JSValue *this_val, int argc, JSValue *argv
) {
    // usage: httpInterceptor.getInterceptedData(clear?: boolean)
    //   Everything the interceptor saw, newest first. "kind" is "dns" or
    //   "http"; "action" is "observed", "redirected" or "injected".
    // returns: { ok, count, requests, dnsQueries, redirected, injected,
    //            data: [{ time, kind, action, clientIP, clientMAC, method,
    //                     host, path, data }] }
    bool clear = (argc > 0 && !JS_IsUndefined(argv[0])) ? JS_ToBool(ctx, argv[0]) : false;

    JSGCRef obj_ref;
    JSValue *obj = ns_new_object(ctx, &obj_ref);
    if (JS_IsException(*obj)) {
        JS_PopGCRef(ctx, &obj_ref);
        return JS_ThrowOutOfMemory(ctx);
    }

    int count = netspoof_capture_count();
    JS_SetPropertyStr(ctx, *obj, "count", JS_NewInt32(ctx, count));
    JS_SetPropertyStr(ctx, *obj, "requests", JS_NewInt32(ctx, (int)netspoof_http_requests()));
    JS_SetPropertyStr(ctx, *obj, "dnsQueries", JS_NewInt32(ctx, (int)netspoof_dns_queries()));
    JS_SetPropertyStr(ctx, *obj, "redirected", JS_NewInt32(ctx, (int)netspoof_http_redirected()));
    JS_SetPropertyStr(ctx, *obj, "injected", JS_NewInt32(ctx, (int)netspoof_http_injected()));

    JSGCRef arr_ref;
    JSValue *arr = JS_PushGCRef(ctx, &arr_ref);
    *arr = JS_NewArray(ctx, count);
    if (JS_IsException(*arr)) {
        JS_PopGCRef(ctx, &arr_ref);
        return JS_PopGCRef(ctx, &obj_ref);
    }

    static const char *kKindNames[] = {"dns", "http"};
    static const char *kActionNames[] = {"observed", "redirected", "injected"};

    for (int i = 0; i < count; i++) {
        const NetSpoofCapture *c = netspoof_capture(i);
        if (!c) continue;

        JSGCRef entry_ref;
        JSValue *entry = JS_PushGCRef(ctx, &entry_ref);
        *entry = JS_NewObject(ctx);
        if (JS_IsException(*entry)) {
            JS_PopGCRef(ctx, &entry_ref);
            break;
        }
        JS_SetPropertyStr(ctx, *entry, "time", JS_NewInt32(ctx, (int)c->ts));
        JS_SetPropertyStr(ctx, *entry, "kind", JS_NewString(ctx, kKindNames[c->kind > 1 ? 0 : c->kind]));
        JS_SetPropertyStr(
            ctx, *entry, "action", JS_NewString(ctx, kActionNames[c->action > 2 ? 0 : c->action])
        );
        JS_SetPropertyStr(ctx, *entry, "clientIP", JS_NewString(ctx, c->clientIp.toString().c_str()));
        JS_SetPropertyStr(ctx, *entry, "clientMAC", JS_NewString(ctx, c->clientMac));
        JS_SetPropertyStr(ctx, *entry, "method", JS_NewString(ctx, c->method[0] ? c->method : "DNS"));
        JS_SetPropertyStr(ctx, *entry, "host", JS_NewString(ctx, c->host));
        JS_SetPropertyStr(ctx, *entry, "path", JS_NewString(ctx, c->path));
        JS_SetPropertyStr(ctx, *entry, "data", JS_NewString(ctx, c->data));
        JS_SetPropertyUint32(ctx, *arr, (uint32_t)i, *entry);
        JS_PopGCRef(ctx, &entry_ref);
    }

    JS_SetPropertyStr(ctx, *obj, "data", *arr);
    JS_SetPropertyStr(ctx, *obj, "ok", JS_NewBool(true));
    JS_PopGCRef(ctx, &arr_ref);

    if (clear) netspoof_capture_clear();
    return JS_PopGCRef(ctx, &obj_ref);
}

// ===========================================================================
// Lifecycle
// ===========================================================================

void netspoof_js_cleanup() { netspoof_cleanup(); }

#endif
