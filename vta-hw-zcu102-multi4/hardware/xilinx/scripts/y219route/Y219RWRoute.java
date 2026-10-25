// #### JO added (2026-10-24): reroute the nets of a routed design that use a PIP in one tile row, timing-driven
// (RapidWright 2025.2 RWRoute; 2023.2 fails on the clock nets of this design). The lab ZCU102 has stuck-at-0 configuration cells in tile row Y219
// (log/BOARD_TODO.md 2026-10-23), and Vivado has no routing blockage. For every net with a PIP in that row, this keeps
// the part of its routing that the source reaches without such a PIP (stubs left hanging are dropped), and RWRoute's
// partial router routes the sinks cut off there, timing-driven, with every PIP in the row excluded from its routing
// graph. Other nets keep Vivado's routing unless a cut-off sink cannot be routed without their nodes (soft
// preserve; 2026-10-24 the region around the AXI crossbar had no free path between neighboring slices without it): RWRoute
// then routes them again too, with the same exclusion. Y219Route.java (RapidWright 2020.1, own A* on hop count) did the
// same without a delay model and lost 1.6-1.9 ns on the nets that cross the row.
// Place the design with rows row-1..row+1 empty (no site pin there): half of the IMUX inputs in those rows are fed from
// the row. Static nets are left alone (Vivado ties them off).
// With -Dpips=stuck_int_pips.txt (stuckpips.py) only the INT-tile PIPs of the row that set a stuck cell are banned
// (2026-10-25: 85 of the 460 row PIPs of blk_f); without it, every PIP in the row.
// RWRoute's timing does not know the design's clocks (every criticality 0, so it routes for wirelength and
// congestion only); -Dcrit=<0..1> gives every connection it routes that criticality, so that its cost is mostly the
// estimated delay (2026-10-25 rs_f: 46 nets rerouted without it, WNS -0.909 on an HP port net that crosses the row).
// usage: RAPIDWRIGHT_PATH=<data> java -Xmx100g -cp rapidwright-2025.2.2-standalone-lin64.jar Y219RWRoute.java \
//          <in.dcp> <out.dcp> [row=219]       (needs <in>.edf next to the DCP: write_edif in Vivado; the data dir needs
//          timing/ultrascaleplus/*.txt, unzip them from the jar)
import com.xilinx.rapidwright.design.Design;
import com.xilinx.rapidwright.design.Net;
import com.xilinx.rapidwright.design.SitePinInst;
import com.xilinx.rapidwright.device.IntentCode;
import com.xilinx.rapidwright.device.Node;
import com.xilinx.rapidwright.device.PIP;
import com.xilinx.rapidwright.rwroute.Connection;
import com.xilinx.rapidwright.rwroute.PartialRouter;
import com.xilinx.rapidwright.rwroute.RWRouteConfig;
import com.xilinx.rapidwright.rwroute.RouteNode;
import com.xilinx.rapidwright.rwroute.RouteNodeGraph;
import com.xilinx.rapidwright.timing.delayestimator.DelayEstimatorBase;
import com.xilinx.rapidwright.timing.delayestimator.InterconnectInfo;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.*;

public class Y219RWRoute {
  static int row;

  static Set<String> stuck;  // "<from wire> <to wire>" of the INT PIPs that set a stuck cell, or null: all PIPs

  static boolean bad(PIP p) {
    if (p == null || p.getTile().getTileYCoordinate() != row) return false;
    return stuck == null || p.getTile().getName().startsWith("INT_X") && stuck.contains(p.getStartWireName() + " " + p.getEndWireName());
  }

  // RWRoute cannot take a data net with routing on a clock leaf node (Vivado puts some of the reset net there), so
  // those branches are routed again as well
  static boolean cut(PIP p) { return bad(p) || p.getEndNode().getIntentCode() == IntentCode.NODE_GLOBAL_LEAF; }

  static final Set<Net> onLeaf = new HashSet<>();  // the other nets with such routing, which RWRoute must not route again

  static class Router extends PartialRouter {
    Router(Design d, RWRouteConfig c, Collection<SitePinInst> pins) { super(d, c, pins, true); }  // soft preserve: it may route nets around it again

    static class Graph extends RouteNodeGraphPartialTimingDriven {
      Graph(Design d, RWRouteConfig c, DelayEstimatorBase e) { super(d, c, e); }

      @Override
      protected boolean isExcluded(RouteNode parent, Node child) {
        return bad(PIP.getArbitraryPIP(parent.getNode(), child)) || super.isExcluded(parent, child);
      }
    }

    static final float CRIT = Float.parseFloat(System.getProperty("crit", "0"));

    @Override
    protected void routeIndirectConnection(Connection c) {
      if (CRIT > 0) c.setCriticality(CRIT);
      super.routeIndirectConnection(c);
    }

    @Override
    protected RouteNodeGraph createRouteNodeGraph() {
      return new Graph(design, config, new DelayEstimatorBase(design.getDevice(), new InterconnectInfo(), false, 0));
    }

    @Override
    protected Collection<Net> pickNetsToUnpreserve(Connection c) {
      List<Net> r = new ArrayList<>(super.pickNetsToUnpreserve(c));
      r.removeAll(onLeaf);
      return r;
    }

    static void run(Design d, RWRouteConfig c, Collection<SitePinInst> pins) { routeDesign(new Router(d, c, pins)); }
  }

  static int countBad(Design d) {
    int n = 0;
    for (Net net : d.getNets())
      if (!net.isStaticNet() && net.getPIPs().stream().anyMatch(Y219RWRoute::bad)) n++;
    return n;
  }

  public static void main(String[] a) throws IOException {
    row = a.length > 2 ? Integer.parseInt(a[2]) : 219;
    if (System.getProperty("pips") != null) stuck = new HashSet<>(Files.readAllLines(Paths.get(System.getProperty("pips"))));
    Design d = Design.readCheckpoint(a[0], a[0].replaceAll("\\.dcp$", ".edf"));
    List<SitePinInst> pins = new ArrayList<>();
    int nets = 0;
    for (Net net : d.getNets()) {
      if (net.isStaticNet() || net.isClockNet()) continue;
      if (net.getPIPs().stream().noneMatch(Y219RWRoute::bad)) {
        if (net.getPIPs().stream().anyMatch(Y219RWRoute::cut)) onLeaf.add(net);
        continue;
      }
      nets++;
      Map<Node, List<PIP>> down = new HashMap<>();
      for (PIP p : net.getPIPs()) down.computeIfAbsent(p.getStartNode(), k -> new ArrayList<>()).add(p);
      // the routing the source (and the alternate source, if routed from it) reaches without a PIP in the row
      Set<Node> tree = new HashSet<>();
      Deque<Node> q = new ArrayDeque<>();
      for (SitePinInst s : new SitePinInst[] {net.getSource(), net.getAlternateSource()})
        if (s != null && tree.add(s.getConnectedNode())) q.add(s.getConnectedNode());
      List<PIP> kept = new ArrayList<>();
      while (!q.isEmpty())
        for (PIP p : down.getOrDefault(q.poll(), Collections.emptyList()))
          if (!cut(p) && tree.add(p.getEndNode())) { kept.add(p); q.add(p.getEndNode()); }
      Set<Node> sinks = new HashSet<>();
      for (SitePinInst s : net.getSinkPins()) sinks.add(s.getConnectedNode());
      // drop the stubs that led only to the row
      for (boolean pruned = true; pruned; ) {
        Set<Node> starts = new HashSet<>();
        for (PIP p : kept) starts.add(p.getStartNode());
        pruned = kept.removeIf(p -> !sinks.contains(p.getEndNode()) && !starts.contains(p.getEndNode()));
      }
      int cut = 0;
      for (SitePinInst s : net.getSinkPins())
        if (!tree.contains(s.getConnectedNode())) { s.setRouted(false); pins.add(s); cut++; }
      System.out.println("CUT  " + net + " pips " + net.getPIPs().size() + " -> " + kept.size() + ", " + cut + " sink(s) to route");
      net.setPIPs(kept);
    }
    System.out.println("nets with a PIP in row " + row + ": " + nets + ", sinks to route: " + pins.size());
    RWRouteConfig c = new RWRouteConfig(new String[0]);
    c.setTimingDriven(true);
    c.setVerbose(true);
    Router.run(d, c, pins);
    int left = 0;
    for (SitePinInst s : pins) if (!s.isRouted()) left++;
    System.out.println("routed; sinks left unrouted " + left + ", nets with a PIP in row " + row + ": " + countBad(d));
    d.writeCheckpoint(a[1]);
  }
}
