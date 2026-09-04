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

// Constant-net holdout handling.
//
// The constant nets ($PACKER_GND_NET / $PACKER_VCC_NET) are not routed by the
// main router; routeVcc() bridges every constant sink to the pseudo-constant
// backbone through whatever pips the finished signal routing left free.  On
// xc7 the fan-in to a LUT pin from GND is narrow (the tile's two GFAN wires
// plus the BYP/FAN bounces), so a signal net sitting on the last free one
// leaves a sink unreached.  Such a "holdout" used to be counted, logged and
// forgotten: nextpnr exited 0 and the bitstream left the IMUX unprogrammed,
// which on xc7 reads as logic 1.  A GND-tied RAM32M address bit then floated
// high and the memory silently corrupted (writes at 16-31, reads at 0-15).
//
// The fix is to treat a holdout as a routing problem for the main router:
// drive it from a LUT with a constant INIT placed on a free LUT bel next to
// the sink, move the sink onto that LUT's net, and re-run the router.  The
// router keeps every arc that is already legally routed, so the re-route
// only has to find paths for the new nets, ripping up whatever blocks them.
// Anything still unreached after that is a hard error unless the user opts
// out explicitly (--allow-const-holdouts / NEXTPNR_ALLOW_CONST_HOLDOUTS).

#include <map>
#include <set>
#include "cells.h"
#include "design_utils.h"
#include "log.h"
#include "nextpnr.h"
#include "util.h"

NEXTPNR_NAMESPACE_BEGIN

namespace {

// Value of a provably constant net: 0, 1, or -1 when it is not provably
// constant.  Follows the pseudo-constant nets and constant-output LUTs: a LUT
// whose connected inputs are all constant and whose INIT does not depend on
// its unconnected inputs.  This is how the packer buffers a constant into a
// pin that needs a real LUT output (CARRY4 S), so a chain of at most a few
// LUTs is expected.
int const_net_value(Context *ctx, NetInfo *net, int depth = 0)
{
    if (net == nullptr)
        return -1;
    if (net->name == ctx->id("$PACKER_GND_NET"))
        return 0;
    if (net->name == ctx->id("$PACKER_VCC_NET"))
        return 1;
    if (depth > 4)
        return -1;
    CellInfo *drv = net->driver.cell;
    if (drv == nullptr || drv->type != id_SLICE_LUTX || net->driver.port != id_O6)
        return -1;
    auto init_it = drv->params.find(ctx->id("INIT"));
    if (init_it == drv->params.end() || init_it->second.is_string)
        return -1;
    const std::string &init = init_it->second.str;
    const IdString a_ports[6] = {id_A1, id_A2, id_A3, id_A4, id_A5, id_A6};
    // Number of logical inputs: the LUTk type the packer recorded, else the
    // highest connected pin.  INIT is stored zero-padded (a LUT1's 2-bit INIT
    // arrives as 32 bits), so its size says nothing about the input count.
    int k = 0;
    std::string orig = str_or_default(drv->attrs, ctx->id("X_ORIG_TYPE"), "");
    if (orig.size() == 4 && orig.compare(0, 3, "LUT") == 0 && orig[3] >= '1' && orig[3] <= '6') {
        k = orig[3] - '0';
    } else {
        for (int i = 0; i < 6; i++)
            if (get_net_or_empty(drv, a_ports[i]) != nullptr)
                k = i + 1;
    }
    int width = 1 << k;
    if (int(init.size()) < width)
        return -1;
    int fixed[6];
    for (int i = 0; i < k; i++) {
        NetInfo *in = get_net_or_empty(drv, a_ports[i]);
        if (in == nullptr) {
            fixed[i] = -1; // unconnected: INIT must not depend on it
            continue;
        }
        fixed[i] = const_net_value(ctx, in, depth + 1);
        if (fixed[i] < 0)
            return -1;
    }
    int result = -1;
    for (int idx = 0; idx < width; idx++) {
        bool reachable = true;
        for (int i = 0; i < k; i++)
            if (fixed[i] >= 0 && ((idx >> i) & 1) != fixed[i])
                reachable = false;
        if (!reachable)
            continue;
        int bit = (init.at(idx) == Property::S1) ? 1 : 0;
        if (result < 0)
            result = bit;
        else if (result != bit)
            return -1;
    }
    return result;
}

// A CARRY4 DI[i] tied to a constant is a don't-care when S[i] is constant 1:
// CO[i] = S[i] ? CI[i] : DI[i] and O[i] = S[i] ^ CI[i], so the DI mux is
// never selected.  (yosys emits this shape for the unused upper bits of a
// carry chain.)  Such a pin can be left unrouted without affecting the logic.
bool holdout_is_dont_care(Context *ctx, const Arch::ConstHoldout &h)
{
    if (h.cell->type != id_CARRY4)
        return false;
    std::string p = h.port.str(ctx);
    if (p.size() != 3 || p.compare(0, 2, "DI") != 0)
        return false;
    NetInfo *s = get_net_or_empty(h.cell, ctx->id(std::string("S") + p[2]));
    return const_net_value(ctx, s) == 1;
}

} // namespace

// Unbind everything the constant nets own, so the main router can re-run
// against the signal routing alone and routeVcc() can fill in again.
void Arch::ripupConstNets()
{
    for (const char *name : {"$PACKER_GND_NET", "$PACKER_VCC_NET"}) {
        auto it = nets.find(id(name));
        if (it == nets.end())
            continue;
        std::vector<WireId> wires;
        for (auto &w : it->second->wires)
            wires.push_back(w.first);
        for (auto w : wires)
            unbindWire(w);
    }
}

// Drive the holdouts from constant LUTs: one LUT per (constant, sink tile),
// placed on a free 6LUT bel (with its 5LUT free too) as close to the sinks as
// possible.  The LUT has no inputs and INIT all-0 or all-1, so its output is
// the constant whatever the unrouted input pins float to.  Returns the number
// of LUTs added; holdouts that could not be placed are returned in unplaced
// and stay on the pseudo-constant net.
int Arch::insertConstDrivers(const std::vector<ConstHoldout> &holdouts, std::vector<ConstHoldout> &unplaced)
{
    Context *ctx = getCtx();
    const int max_radius = 24;
    auto tile_is_logic = [&](int tile) {
        IdString t(chip_info->tile_types[chip_info->tile_insts[tile].type].type);
        return t == id_CLBLL_L || t == id_CLBLL_R || t == id_CLBLM_L || t == id_CLBLM_R || t == id_CLEL_L ||
               t == id_CLEL_R || t == id_CLEM || t == id_CLEM_R;
    };
    // Ring search outwards from the sink's tile for a free 6LUT+5LUT pair
    // that the slice validity rules accept with the constant LUT on it.
    auto place_lut = [&](CellInfo *lut, Loc origin) -> BelId {
        for (int r = 0; r <= max_radius; r++) {
            for (int dy = -r; dy <= r; dy++) {
                for (int dx = -r; dx <= r; dx++) {
                    if (std::max(std::abs(dx), std::abs(dy)) != r)
                        continue;
                    int x = origin.x + dx, y = origin.y + dy;
                    if (x < 0 || y < 0 || x >= getGridDimX() || y >= getGridDimY())
                        continue;
                    int tile = y * getGridDimX() + x;
                    if (!tile_is_logic(tile))
                        continue;
                    for (BelId bel : getBelsByTile(x, y)) {
                        if (getBelType(bel) != id_SLICE_LUTX)
                            continue;
                        Loc l = getBelLocation(bel);
                        if ((l.z & 0xF) != BEL_6LUT || !checkBelAvail(bel))
                            continue;
                        BelId lut5 = getBelByLocation(Loc(l.x, l.y, (l.z & ~0xF) | BEL_5LUT));
                        if (lut5 != BelId() && !checkBelAvail(lut5))
                            continue;
                        bindBel(bel, lut, STRENGTH_STRONG);
                        if (isBelLocationValid(bel))
                            return bel;
                        unbindBel(bel);
                    }
                }
            }
        }
        return BelId();
    };

    // Group by (constant, sink tile): one driver LUT serves every unreached
    // pin of that constant in the tile.  std::map keeps the order deterministic.
    std::map<std::pair<int, int>, std::vector<ConstHoldout>> groups;
    for (auto &h : holdouts) {
        NPNR_ASSERT(h.cell->bel != BelId());
        groups[std::make_pair(h.value ? 1 : 0, h.cell->bel.tile)].push_back(h);
    }

    int added = 0, seq = 0;
    for (auto &g : groups) {
        bool value = g.first.first != 0;
        const char *base = value ? "$PACKER_VCC_NET" : "$PACKER_GND_NET";
        IdString cname, nname;
        do {
            cname = id(stringf("%s$HOLDOUT$LUT$%d", base, seq));
            nname = id(stringf("%s$holdout$%d", base, seq));
            seq++;
        } while (cells.count(cname) || nets.count(nname));

        std::unique_ptr<CellInfo> lut = create_cell(ctx, id_SLICE_LUTX, cname);
        // A LUT1 with its input unconnected: fasm.cc indexes INIT by the
        // logical inputs mapped onto physical pins, none here, so every INIT
        // bit written is INIT[0] and the truth table is the constant.
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

        Loc origin = getBelLocation(g.second.front().cell->bel);
        BelId bel = place_lut(lut_ptr, origin);
        if (bel == BelId()) {
            log_warning("    no free LUT bel within %d tiles of %s for a %s driver\n", max_radius,
                        nameOfBel(g.second.front().cell->bel), value ? "VCC" : "GND");
            disconnect_port(ctx, lut_ptr, id_O6);
            cells.erase(cname);
            nets.erase(nname);
            for (auto &h : g.second)
                unplaced.push_back(h);
            continue;
        }
        for (auto &h : g.second) {
            disconnect_port(ctx, h.cell, h.port);
            connect_port(ctx, net_ptr, h.cell, h.port);
            assignCellInfo(h.cell);
            log_info("    %s.%s <- %s (bel %s)\n", h.cell->name.c_str(ctx), h.port.c_str(ctx), cname.c_str(ctx),
                     nameOfBel(bel));
        }
        added++;
    }
    return added;
}

void Arch::routeConstants(std::function<void()> reroute)
{
    Context *ctx = getCtx();
    int max_passes = 6;
    if (const char *e = getenv("NEXTPNR_CONST_HOLDOUT_PASSES"))
        max_passes = atoi(e);
    bool allow = getenv("NEXTPNR_ALLOW_CONST_HOLDOUTS") != nullptr ||
                 settings.find(id("allow-const-holdouts")) != settings.end();
    bool no_drivers = getenv("NEXTPNR_NO_CONST_LUT_DRIVERS") != nullptr;

    // Report what is left.  Fatal by default: an unrouted constant sink has an
    // undefined value in silicon (an unprogrammed xc7 IMUX reads 1), which is
    // exactly the silent corruption this pass exists to prevent.
    auto report = [&](const std::vector<ConstHoldout> &left, const char *why) {
        for (auto &h : left) {
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
    std::vector<ConstHoldout> given_up_list;
    int drivers = 0, dont_care = 0, passes = 0;
    for (int pass = 0;; pass++) {
        auto holdouts = routeVcc();
        std::vector<ConstHoldout> real;
        for (auto &h : holdouts) {
            if (given_up.count(std::make_pair(h.cell->name, h.port)))
                continue;
            if (holdout_is_dont_care(ctx, h)) {
                log_info("    %s.%s (bel %s): CARRY4 DI with S=1 never selected, left unrouted\n",
                         h.cell->name.c_str(ctx), h.port.c_str(ctx), nameOfBel(h.cell->bel));
                disconnect_port(ctx, h.cell, h.port);
                dont_care++;
                continue;
            }
            real.push_back(h);
        }
        if (real.empty())
            break;
        if (no_drivers) {
            given_up_list.insert(given_up_list.end(), real.begin(), real.end());
            break;
        }
        if (pass >= max_passes) {
            log_warning("constant holdouts remain after %d re-route pass(es)\n", pass);
            given_up_list.insert(given_up_list.end(), real.begin(), real.end());
            break;
        }
        log_info("Constant fill left %d sink(s) unreached; driving them from local constant LUTs (pass %d)\n",
                 int(real.size()), pass + 1);
        std::vector<ConstHoldout> unplaced;
        drivers += insertConstDrivers(real, unplaced);
        for (auto &h : unplaced) {
            given_up.insert(std::make_pair(h.cell->name, h.port));
            given_up_list.push_back(h);
        }
        if (unplaced.size() == real.size()) {
            break; // nothing changed, re-routing would not help
        }
        // Re-route: the constant nets give up their wires and the main router
        // runs again.  Arcs that are already legally routed are kept, so this
        // only has to route the new constant-LUT nets (ripping up whatever is
        // in their way); routeVcc then fills the constants in around the result.
        ripupConstNets();
        findSourceSinkLocations();
        reroute();
        passes++;
    }
    if (drivers > 0 || dont_care > 0)
        log_info("Constant holdouts: %d local constant LUT(s) added, %d re-route pass(es), %d don't-care CARRY4 DI "
                 "pin(s) left unrouted\n",
                 drivers, passes, dont_care);
    if (!given_up_list.empty())
        report(given_up_list, no_drivers ? "constant LUT drivers disabled" : "no route or no free LUT bel");
}

NEXTPNR_NAMESPACE_END
