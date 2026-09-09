/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2026  The openXC7 contributors
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

// Constant-net holdouts: sinks routeVcc() cannot reach are driven from a local
// constant LUT and the design is re-routed; leftovers are fatal unless allowed.

#include <algorithm>
#include <map>
#include <set>
#include "cells.h"
#include "design_utils.h"
#include "log.h"
#include "nextpnr.h"
#include "util.h"

NEXTPNR_NAMESPACE_BEGIN

namespace {

// How far (in tiles) to look for a free LUT bel around a holdout's tile.
const int const_lut_search_radius = 24;
// Re-route passes before the remaining holdouts are reported.
const int const_holdout_max_passes = 6;
// LUTs to follow when proving a net constant.
const int const_net_lut_depth = 4;

// Value of a provably constant net (through constant-output LUTs): 0, 1, or -1.
int const_net_value(Context *ctx, NetInfo *net, int depth = 0)
{
    const bool no_net = net == nullptr;
    if (no_net)
        return -1;
    const bool is_gnd_net = net->name == ctx->id("$PACKER_GND_NET");
    if (is_gnd_net)
        return 0;
    const bool is_vcc_net = net->name == ctx->id("$PACKER_VCC_NET");
    if (is_vcc_net)
        return 1;
    const bool too_deep = depth > const_net_lut_depth;
    if (too_deep)
        return -1;
    CellInfo *drv = net->driver.cell;
    const bool driven_by_lut_o6 = drv != nullptr && drv->type == id_SLICE_LUTX && net->driver.port == id_O6;
    if (!driven_by_lut_o6)
        return -1;
    auto init_it = drv->params.find(ctx->id("INIT"));
    const bool has_bit_init = init_it != drv->params.end() && !init_it->second.is_string;
    if (!has_bit_init)
        return -1;
    const std::string &init = init_it->second.str;
    const IdString a_ports[6] = {id_A1, id_A2, id_A3, id_A4, id_A5, id_A6};
    // Input count from the recorded LUTk type, else the highest connected pin
    // (INIT is zero-padded, so its size cannot be used).
    int k = 0;
    std::string orig = str_or_default(drv->attrs, ctx->id("X_ORIG_TYPE"), "");
    const bool orig_is_lutk = orig.size() == 4 && orig.compare(0, 3, "LUT") == 0 && orig[3] >= '1' && orig[3] <= '6';
    if (orig_is_lutk) {
        k = orig[3] - '0';
    } else {
        for (int i = 0; i < 6; i++) {
            const bool input_connected = get_net_or_empty(drv, a_ports[i]) != nullptr;
            if (input_connected)
                k = i + 1;
        }
    }
    int width = 1 << k;
    const bool init_too_short = int(init.size()) < width;
    if (init_too_short)
        return -1;
    int fixed[6];
    for (int i = 0; i < k; i++) {
        NetInfo *in = get_net_or_empty(drv, a_ports[i]);
        const bool input_unconnected = in == nullptr;
        if (input_unconnected) {
            fixed[i] = -1; // INIT must not depend on it
            continue;
        }
        fixed[i] = const_net_value(ctx, in, depth + 1);
        const bool input_not_constant = fixed[i] < 0;
        if (input_not_constant)
            return -1;
    }
    int result = -1;
    for (int idx = 0; idx < width; idx++) {
        bool reachable = true;
        for (int i = 0; i < k; i++) {
            const bool row_contradicts_fixed_input = fixed[i] >= 0 && ((idx >> i) & 1) != fixed[i];
            if (row_contradicts_fixed_input)
                reachable = false;
        }
        if (!reachable)
            continue;
        int bit = (init.at(idx) == Property::S1) ? 1 : 0;
        const bool first_reachable_row = result < 0;
        const bool row_disagrees = result != bit;
        if (first_reachable_row)
            result = bit;
        else if (row_disagrees)
            return -1;
    }
    return result;
}

// A CARRY4 DI[i] is a don't-care when S[i] is constant 1 (the DI mux is never selected).
bool holdout_is_dont_care(Context *ctx, const Arch::ConstHoldout &h)
{
    const bool is_carry4 = h.cell->type == id_CARRY4;
    if (!is_carry4)
        return false;
    std::string p = h.port.str(ctx);
    const bool is_di_pin = p.size() == 3 && p.compare(0, 2, "DI") == 0;
    if (!is_di_pin)
        return false;
    NetInfo *s = get_net_or_empty(h.cell, ctx->id(std::string("S") + p[2]));
    return const_net_value(ctx, s) == 1;
}

} // namespace

// Unbind everything the constant nets own ahead of a re-route.
void Arch::ripupConstNets()
{
    for (const char *name : {"$PACKER_GND_NET", "$PACKER_VCC_NET"}) {
        auto it = nets.find(id(name));
        const bool net_absent = it == nets.end();
        if (net_absent)
            continue;
        std::vector<WireId> wires;
        for (auto &w : it->second->wires)
            wires.push_back(w.first);
        for (auto w : wires)
            unbindWire(w);
    }
}

// Drive the holdouts from input-less constant LUTs, one per (constant, sink
// tile); returns the LUT count, with unplaceable holdouts in unplaced.
int Arch::insertConstDrivers(const std::vector<ConstHoldout> &holdouts, std::vector<ConstHoldout> &unplaced)
{
    Context *ctx = getCtx();
    const int max_radius = const_lut_search_radius;
    auto tile_is_logic = [&](int tile) {
        IdString t(chip_info->tile_types[chip_info->tile_insts[tile].type].type);
        return t == id_CLBLL_L || t == id_CLBLL_R || t == id_CLBLM_L || t == id_CLBLM_R || t == id_CLEL_L ||
               t == id_CLEL_R || t == id_CLEM || t == id_CLEM_R;
    };
    // Ring search outwards from the sink's tile for a free, valid 6LUT+5LUT pair.
    auto place_lut = [&](CellInfo *lut, Loc origin) -> BelId {
        for (int r = 0; r <= max_radius; r++) {
            for (int dy = -r; dy <= r; dy++) {
                for (int dx = -r; dx <= r; dx++) {
                    const bool on_ring = std::max(std::abs(dx), std::abs(dy)) == r;
                    if (!on_ring)
                        continue;
                    int x = origin.x + dx, y = origin.y + dy;
                    const bool on_grid = x >= 0 && y >= 0 && x < getGridDimX() && y < getGridDimY();
                    if (!on_grid)
                        continue;
                    int tile = y * getGridDimX() + x;
                    const bool is_logic_tile = tile_is_logic(tile);
                    if (!is_logic_tile)
                        continue;
                    for (BelId bel : getBelsByTile(x, y)) {
                        const bool is_lut_bel = getBelType(bel) == id_SLICE_LUTX;
                        if (!is_lut_bel)
                            continue;
                        Loc l = getBelLocation(bel);
                        const bool free_6lut = (l.z & 0xF) == BEL_6LUT && checkBelAvail(bel);
                        if (!free_6lut)
                            continue;
                        BelId lut5 = getBelByLocation(Loc(l.x, l.y, (l.z & ~0xF) | BEL_5LUT));
                        const bool free_5lut = lut5 == BelId() || checkBelAvail(lut5);
                        if (!free_5lut)
                            continue;
                        bindBel(bel, lut, STRENGTH_STRONG);
                        const bool placement_valid = isBelLocationValid(bel);
                        if (placement_valid)
                            return bel;
                        unbindBel(bel);
                    }
                }
            }
        }
        return BelId();
    };

    // Every user on a holdout's site wire (e.g. SLICEM WA1..6 and A1..6) must
    // move with it, since one wire cannot be driven by two nets.
    std::map<IdString, std::map<WireId, std::vector<PortRef>>> users_by_wire;
    auto wire_users = [&](NetInfo *net, WireId w) -> const std::vector<PortRef> & {
        auto &by_wire = users_by_wire[net->name];
        const bool not_yet_grouped = by_wire.empty();
        if (not_yet_grouped)
            for (auto &usr : net->users)
                by_wire[ctx->getNetinfoSinkWire(net, usr)].push_back(usr);
        return by_wire.at(w);
    };
    auto sink_wire = [&](const ConstHoldout &h) {
        PortRef pr;
        pr.cell = h.cell;
        pr.port = h.port;
        return ctx->getNetinfoSinkWire(h.net, pr);
    };
    auto move_users = [&](NetInfo *from, WireId w, NetInfo *to, const char *how) {
        std::vector<PortRef> users = wire_users(from, w);
        for (auto &pr : users) {
            disconnect_port(ctx, pr.cell, pr.port);
            connect_port(ctx, to, pr.cell, pr.port);
            assignCellInfo(pr.cell);
            log_info("    %s.%s <- %s%s\n", pr.cell->name.c_str(ctx), pr.port.c_str(ctx), to->name.c_str(ctx), how);
        }
    };

    // One driver LUT per (constant, sink tile); std::map keeps the order deterministic.
    std::map<std::pair<int, int>, std::vector<ConstHoldout>> groups;
    for (auto &h : holdouts) {
        NPNR_ASSERT(h.cell->bel != BelId());
        groups[std::make_pair(h.value ? 1 : 0, h.cell->bel.tile)].push_back(h);
    }

    int added = 0, seq = 0;
    for (auto &g : groups) {
        bool value = g.first.first != 0;
        NetInfo *cnet = g.second.front().net;
        // A wire an earlier pass already gave to a LUT of this constant takes
        // its remaining users; anything else gets a new LUT.
        std::vector<WireId> need_new;
        std::vector<ConstHoldout> need_new_holdouts;
        std::set<WireId> seen;
        for (auto &h : g.second) {
            WireId w = sink_wire(h);
            const bool wire_already_seen = !seen.insert(w).second;
            if (wire_already_seen)
                continue;
            NetInfo *bound = getBoundWireNet(w);
            const bool wire_has_this_constant =
                    bound != nullptr && bound != cnet && const_net_value(ctx, bound) == (value ? 1 : 0);
            if (wire_has_this_constant) {
                move_users(cnet, w, bound, " (wire already driven by it)");
                continue;
            }
            need_new.push_back(w);
        }
        for (auto &h : g.second) {
            const bool wire_needs_new_lut = std::find(need_new.begin(), need_new.end(), sink_wire(h)) != need_new.end();
            if (wire_needs_new_lut)
                need_new_holdouts.push_back(h);
        }
        const bool every_wire_already_driven = need_new.empty();
        if (every_wire_already_driven)
            continue;

        const char *base = value ? "$PACKER_VCC_NET" : "$PACKER_GND_NET";
        IdString cname, nname;
        while (true) {
            cname = id(stringf("%s$HOLDOUT$LUT$%d", base, seq));
            nname = id(stringf("%s$holdout$%d", base, seq));
            seq++;
            const bool name_taken = cells.count(cname) || nets.count(nname);
            if (!name_taken)
                break;
        }

        std::unique_ptr<CellInfo> lut = create_cell(ctx, id_SLICE_LUTX, cname);
        // A LUT1 with no input: fasm.cc then writes INIT[0] to every truth-table bit.
        lut->attrs[id("X_ORIG_TYPE")] = std::string("LUT1");
        lut->attrs[id("X_ORIG_PORT_O6")] = std::string("O");
        lut->params[id("INIT")] = Property(value ? 3 : 0, 2);
        std::unique_ptr<NetInfo> net{new NetInfo};
        net->name = nname;
        connect_port(ctx, net.get(), lut.get(), id_O6);
        CellInfo *lut_ptr = lut.get();
        NetInfo *net_ptr = net.get();
        cells[cname] = std::move(lut);
        nets[nname] = std::move(net);
        assignCellInfo(lut_ptr);

        Loc origin = getBelLocation(need_new_holdouts.front().cell->bel);
        BelId bel = place_lut(lut_ptr, origin);
        const bool no_free_lut_bel = bel == BelId();
        if (no_free_lut_bel) {
            log_warning("    no free LUT bel within %d tiles of %s for a %s driver\n", max_radius,
                        nameOfBel(need_new_holdouts.front().cell->bel), value ? "VCC" : "GND");
            disconnect_port(ctx, lut_ptr, id_O6);
            cells.erase(cname);
            nets.erase(nname);
            for (auto &h : need_new_holdouts)
                unplaced.push_back(h);
            continue;
        }
        std::string how = stringf(" (bel %s)", nameOfBel(bel));
        for (WireId w : need_new)
            move_users(cnet, w, net_ptr, how.c_str());
        added++;
    }
    return added;
}

void Arch::routeConstants(std::function<void()> reroute)
{
    Context *ctx = getCtx();
    int max_passes = const_holdout_max_passes;
    const char *passes_env = getenv("NEXTPNR_CONST_HOLDOUT_PASSES");
    const bool passes_overridden = passes_env != nullptr;
    if (passes_overridden)
        max_passes = atoi(passes_env);
    const bool allow = getenv("NEXTPNR_ALLOW_CONST_HOLDOUTS") != nullptr || allow_const_holdouts;
    const bool no_drivers = getenv("NEXTPNR_NO_CONST_LUT_DRIVERS") != nullptr;

    // Fatal by default: an unrouted constant sink reads as 1 in silicon.
    auto report = [&](const std::vector<std::pair<ConstHoldout, const char *>> &left) {
        for (auto &e : left) {
            const ConstHoldout &h = e.first;
            const char *why = e.second;
            PortRef pr;
            pr.cell = h.cell;
            pr.port = h.port;
            WireId sink = ctx->getNetinfoSinkWire(h.net, pr);
            std::string msg = stringf("constant %s not delivered to %s.%s (bel %s, wire %s): %s\n",
                                      h.value ? "VCC" : "GND", h.cell->name.c_str(ctx), h.port.c_str(ctx),
                                      nameOfBel(h.cell->bel), sink == WireId() ? "?" : nameOfWire(sink), why);
            if (allow)
                log_warning("%s", msg.c_str());
            else
                log_nonfatal_error("%s", msg.c_str());
        }
        if (allow)
            log_warning("%d constant sink(s) left unrouted; their silicon value is undefined "
                        "(--allow-const-holdouts given)\n",
                        int(left.size()));
        else
            log_error("%d constant sink(s) could not be routed; the bitstream would be wrong. "
                      "Pass --allow-const-holdouts (or set NEXTPNR_ALLOW_CONST_HOLDOUTS=1) to build anyway.\n",
                      int(left.size()));
    };

    std::set<std::pair<IdString, IdString>> given_up; // (cell, port) already reported
    std::vector<std::pair<ConstHoldout, const char *>> given_up_list;
    const char *why_unplaced = no_drivers ? "constant LUT drivers disabled" : "no route or no free LUT bel";
    auto give_up = [&](const ConstHoldout &h, const char *why) {
        given_up.insert(std::make_pair(h.cell->name, h.port));
        given_up_list.push_back(std::make_pair(h, why));
    };
    int drivers = 0, dont_care = 0, passes = 0;
    std::vector<ConstHoldout> holdouts;
    for (int pass = 0;; pass++) {
        holdouts = routeVcc();
        std::vector<ConstHoldout> real;
        for (auto &h : holdouts) {
            // Don't-cares first: a pin nothing selects is never an error.
            const bool is_dont_care = holdout_is_dont_care(ctx, h);
            if (is_dont_care) {
                log_info("    %s.%s (bel %s): CARRY4 DI with S=1 never selected, left unrouted\n",
                         h.cell->name.c_str(ctx), h.port.c_str(ctx), nameOfBel(h.cell->bel));
                disconnect_port(ctx, h.cell, h.port);
                dont_care++;
                continue;
            }
            const bool already_given_up = given_up.count(std::make_pair(h.cell->name, h.port)) != 0;
            if (already_given_up)
                continue;
            // A sink wire no pip drives (DSP48E1 INMODE/ALUMODE/OPMODE, #159) is
            // unreachable for any net, so report it rather than add a driver.
            PortRef pr;
            pr.cell = h.cell;
            pr.port = h.port;
            WireId sink = ctx->getNetinfoSinkWire(h.net, pr);
            auto uphill = getPipsUphill(sink);
            const bool sink_has_uphill_pip = uphill.begin() != uphill.end();
            if (!sink_has_uphill_pip) {
                give_up(h, "no pip drives the sink wire");
                continue;
            }
            real.push_back(h);
        }
        const bool nothing_left = real.empty();
        if (nothing_left)
            break;
        if (no_drivers) {
            for (auto &h : real)
                give_up(h, why_unplaced);
            break;
        }
        const bool out_of_passes = pass >= max_passes;
        if (out_of_passes) {
            log_warning("constant holdouts remain after %d re-route pass(es)\n", pass);
            for (auto &h : real)
                give_up(h, why_unplaced);
            break;
        }
        log_info("Constant fill left %d sink(s) unreached; driving them from local constant LUTs (pass %d)\n",
                 int(real.size()), pass + 1);
        std::vector<ConstHoldout> unplaced;
        drivers += insertConstDrivers(real, unplaced);
        for (auto &h : unplaced)
            give_up(h, why_unplaced);
        const bool nothing_placed = unplaced.size() == real.size();
        if (nothing_placed)
            break; // nothing changed, re-routing would not help
        // Re-route with the constant nets unbound; the next routeVcc fills in again.
        ripupConstNets();
        findSourceSinkLocations();
        reroute();
        passes++;
    }
    const bool anything_changed = drivers > 0 || dont_care > 0;
    if (anything_changed)
        log_info("Constant holdouts: %d local constant LUT(s) added, %d re-route pass(es), %d don't-care CARRY4 DI "
                 "pin(s) left unrouted\n",
                 drivers, passes, dont_care);
    // Only report sinks the final fill still left unreached.
    std::set<std::pair<IdString, IdString>> still_left;
    for (auto &h : holdouts)
        still_left.insert(std::make_pair(h.cell->name, h.port));
    for (auto it = given_up_list.begin(); it != given_up_list.end();) {
        const bool still_unreached = still_left.count(std::make_pair(it->first.cell->name, it->first.port)) != 0;
        if (still_unreached) {
            ++it;
            continue;
        }
        log_info("    %s.%s reached by the final constant fill after all\n", it->first.cell->name.c_str(ctx),
                 it->first.port.c_str(ctx));
        it = given_up_list.erase(it);
    }
    const bool have_leftovers = !given_up_list.empty();
    if (have_leftovers)
        report(given_up_list);
}

NEXTPNR_NAMESPACE_END
