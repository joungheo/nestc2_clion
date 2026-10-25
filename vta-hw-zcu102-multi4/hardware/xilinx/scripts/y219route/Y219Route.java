// #### JO added (2026-10-24): reroute the nets of a routed design that use a PIP in one tile row (RapidWright 2020.1).
// The lab ZCU102 has stuck-at-0 configuration cells in tile row Y219 (log/BOARD_TODO.md 2026-10-23), and Vivado has no
// routing blockage: nets driven from outside every pblock (PS S_AXI_HP rdata, smartconnect exits) still turn in that row.
// For every net with a PIP in a tile of that row, this keeps the part of its routing that the source still reaches
// without such a PIP, reconnects each branch cut off there by a short detour (A* from the kept tree to the first nodes
// of that branch) so the branch keeps its old routing, and routes any sink still cut off from the tree; never through a
// PIP in the row (nodes that only pass over the row stay usable). Vivado's routing is timing-driven and this is not:
// ripping up whole nets turned the big reset nets into chains (WNS -2.8 ns), routing the cut-off sinks one by one
// still cost 1.6 ns on nets that cross the row (WNS -1.7 ns). A net may take a node of another net at a penalty (site pins, clock and static nets
// are never taken); the net that lost it has the branch below that node routed again the same way, with a growing
// history cost on contested nodes (PathFinder-like). Run it again on its own output if nets are left. Place the design
// with the rows next to it empty too (no site pin in rows row-1..row+1): half of the IMUX inputs there are fed from
// row-219 tiles, and with sinks there the nets kept fighting over the rest.
// usage: RAPIDWRIGHT_PATH=<data> java -Xmx64g -cp rapidwright-2020.1.7-standalone-lin64.jar Y219Route.java \
//          <in.dcp> <out.dcp> [row=219]       (needs <in>.edf next to the DCP: write_edif in Vivado)
//        -Dlimit=<A* pops per sink> (default 2M), -Dbudget=<net routes> (default 20000), -Donly=<net name regexp>
import com.xilinx.rapidwright.design.Design;
import com.xilinx.rapidwright.design.Net;
import com.xilinx.rapidwright.design.SitePinInst;
import com.xilinx.rapidwright.device.Node;
import com.xilinx.rapidwright.device.PIP;
import com.xilinx.rapidwright.device.Tile;
import com.xilinx.rapidwright.device.Wire;

import java.util.*;

public class Y219Route {
  static int row;
  static final Map<Node, Net> owner = new HashMap<>();
  static final Set<Node> locked = new HashSet<>();  // site pins, clock and static net nodes
  static final Map<Node, Double> hist = new HashMap<>();
  static final double PENALTY = 20;

  static boolean badPIP(PIP p) { return p.getTile().getTileYCoordinate() == row; }

  static int dist(Node a, Node b) {
    Tile x = a.getTile(), y = b.getTile();
    return Math.abs(x.getTileXCoordinate() - y.getTileXCoordinate()) + Math.abs(x.getTileYCoordinate() - y.getTileYCoordinate());
  }

  // node cost: one per hop plus a little per tile spanned (a long wire is slower than a short one, not 12x)
  static double cost(Node n) {
    Tile t = n.getTile();
    Wire[] w = n.getAllWiresInNode();
    int span = 0;
    if (w.length > 1) {
      Tile e = w[w.length - 1].getTile();
      span = Math.abs(t.getTileXCoordinate() - e.getTileXCoordinate()) + Math.abs(t.getTileYCoordinate() - e.getTileYCoordinate());
    }
    return 1.0 + 0.05 * span + hist.getOrDefault(n, 0.0);
  }

  static class Q implements Comparable<Q> {
    final Node n; final double g, f;
    Q(Node n, double g, double f) { this.n = n; this.g = g; this.f = f; }
    public int compareTo(Q o) { return Double.compare(f, o.f); }
  }

  static int dist(Node a, List<Node> targets) {
    int d = Integer.MAX_VALUE;
    for (Node t : targets) d = Math.min(d, dist(a, t));
    return d;
  }

  // route from the tree of net to the nearest of targets (a sink, or nodes of a cut-off branch); returns the PIPs
  // of the new branch (the last one ends on the target reached) or null
  static List<PIP> routeSink(Net net, Set<Node> tree, List<Node> targets, int limit) {
    Set<Node> tset = new HashSet<>(targets);
    Map<Node, PIP> prev = new HashMap<>();
    Map<Node, Double> best = new HashMap<>();
    PriorityQueue<Q> pq = new PriorityQueue<>();
    for (Node n : tree) { best.put(n, 0.0); pq.add(new Q(n, 0, dist(n, targets) / 6.0)); }
    int pops = 0, near = Integer.MAX_VALUE;
    while (!pq.isEmpty() && pops++ < limit) {
      Q q = pq.poll();
      if (q.g > best.getOrDefault(q.n, Double.MAX_VALUE)) continue;
      near = Math.min(near, dist(q.n, targets));
      if (tset.contains(q.n)) {
        LinkedList<PIP> path = new LinkedList<>();
        for (Node n = q.n; !tree.contains(n); n = prev.get(n).getStartNode()) path.addFirst(prev.get(n));
        return path;
      }
      for (PIP p : q.n.getAllDownhillPIPs()) {
        if (badPIP(p) || p.isRouteThru() || p.isBidirectional()) continue;
        Node e = p.getEndNode();
        if (e == null || e.isInvalidNode() || e.isTied() || tree.contains(e)) continue;
        double c = cost(e);
        if (!tset.contains(e)) {
          if (locked.contains(e) || e.getSitePin() != null) continue;
          Net o = owner.get(e);
          if (o != null && o != net) c += PENALTY;
        }
        double g = q.g + c;
        if (g < best.getOrDefault(e, Double.MAX_VALUE)) {
          best.put(e, g); prev.put(e, p);
          pq.add(new Q(e, g, g + dist(e, targets) / 6.0));
        }
      }
    }
    System.out.println("  no path: " + pops + " pops, closest " + near + " tiles from " + targets.get(0));
    return null;
  }

  // add to tree the part of the net's old routing below from that is reachable without a PIP in the row or a node
  // another net holds (Vivado's timing-driven routing stays; only the cut-off branches are routed again)
  static void grow(Net net, Map<Node, List<PIP>> down, Set<Node> tree, List<PIP> kept, Node from) {
    tree.add(from);
    Deque<Node> q = new ArrayDeque<>(Collections.singleton(from));
    while (!q.isEmpty())
      for (PIP p : down.getOrDefault(q.poll(), Collections.emptyList())) {
        Node e = p.getEndNode();
        Net o = owner.get(e);
        if (badPIP(p) || (o != null && o != net) || !tree.add(e)) continue;
        kept.add(p);
        q.add(e);
      }
  }

  // the first nodes (breadth first, at most max) of the old branch below a cut PIP that are not in the tree
  static List<Node> branch(Net net, Map<Node, List<PIP>> down, Set<Node> tree, PIP cut, int max) {
    List<Node> r = new ArrayList<>();
    Set<Node> seen = new HashSet<>();
    Deque<Node> q = new ArrayDeque<>(Collections.singleton(cut.getEndNode()));
    while (!q.isEmpty() && r.size() < max) {
      Node n = q.poll();
      if (!seen.add(n) || tree.contains(n)) continue;
      Net o = owner.get(n);
      if (o == null || o == net) r.add(n);
      for (PIP p : down.getOrDefault(n, Collections.emptyList())) q.add(p.getEndNode());
    }
    return r;
  }

  static void release(Net net) {
    for (PIP p : net.getPIPs())
      for (Node n : new Node[] {p.getStartNode(), p.getEndNode()})
        if (owner.get(n) == net) owner.remove(n);
  }

  public static void main(String[] a) {
    row = a.length > 2 ? Integer.parseInt(a[2]) : 219;
    int limit = Integer.getInteger("limit", 2_000_000), budget = Integer.getInteger("budget", 20_000);
    String only = System.getProperty("only");
    Design d = Design.readCheckpoint(a[0]);
    Deque<Net> work = new ArrayDeque<>();
    for (Net net : d.getNets()) {
      boolean b = false, lock = net.isClockNet() || net.isStaticNet();
      for (PIP p : net.getPIPs()) {
        for (Node n : new Node[] {p.getStartNode(), p.getEndNode()}) { owner.put(n, net); if (lock) locked.add(n); }
        b |= badPIP(p);
      }
      for (SitePinInst sp : net.getPins()) { Node n = sp.getConnectedNode(); if (n != null) locked.add(n); }
      if (b && (only == null || net.getName().matches(only))) {
        if (lock) System.out.println("SKIP " + net + " (clock/static)"); else work.add(net);
      }
    }
    int initial = work.size(), routes = 0, ripped = 0;
    System.out.println("nets with a PIP in row " + row + ": " + initial);
    Set<Net> failed = new LinkedHashSet<>();
    while (!work.isEmpty() && routes < budget) {
      Net net = work.poll();
      routes++;
      int oldPips = net.getPIPs().size();
      release(net);
      Node src = net.getSource().getConnectedNode();
      Map<Node, List<PIP>> down = new HashMap<>();
      List<PIP> cuts = new ArrayList<>();
      for (PIP p : net.getPIPs()) {
        down.computeIfAbsent(p.getStartNode(), k -> new ArrayList<>()).add(p);
        Net o = owner.get(p.getEndNode());
        if (badPIP(p) || (o != null && o != net)) cuts.add(p);
      }
      List<PIP> pips = new ArrayList<>();
      Set<Node> tree = new HashSet<>();
      grow(net, down, tree, pips, src);
      // first reconnect each cut-off branch where it was cut (a short detour around the row), keeping its old routing
      int detours = 0;
      for (PIP cut : cuts) {
        List<Node> tg = branch(net, down, tree, cut, 64);
        if (tg.isEmpty()) continue;
        List<PIP> br = routeSink(net, tree, tg, limit);
        if (br == null) continue;
        for (PIP p : br) { pips.add(p); tree.add(p.getEndNode()); }
        grow(net, down, tree, pips, br.get(br.size() - 1).getEndNode());
        detours++;
      }
      // then any sink still cut off is routed from the tree
      List<SitePinInst> sinks = new ArrayList<>(net.getSinkPins());
      sinks.removeIf(s -> tree.contains(s.getConnectedNode()));
      sinks.sort(Comparator.comparingInt(s -> dist(src, s.getConnectedNode())));
      boolean ok = true;
      for (SitePinInst s : sinks) {
        List<PIP> br = routeSink(net, tree, Collections.singletonList(s.getConnectedNode()), limit);
        if (br == null) { ok = false; System.out.println("FAIL " + net + " sink " + s); break; }
        for (PIP p : br) { pips.add(p); tree.add(p.getEndNode()); }
      }
      if (!ok) { net.setPIPs(new ArrayList<>()); failed.add(net); continue; }
      int reroutedSinks = sinks.size();
      failed.remove(net);
      net.setPIPs(pips);
      Set<Net> victims = new LinkedHashSet<>();
      for (Node n : tree) {
        Net o = owner.put(n, net);
        if (o != null && o != net) { victims.add(o); hist.merge(n, 1.0, Double::sum); }
      }
      for (Net v : victims) {  // the net that lost a node is routed again: grow drops the taken nodes and below
        if (!work.contains(v)) work.add(v);
        ripped++;
      }
      System.out.println("OK   " + net + " pips " + oldPips + " -> " + pips.size() + ", " + detours + " detour(s), " + reroutedSinks + " sink(s) rerouted"
          + (victims.isEmpty() ? "" : ", took nodes of " + victims.size() + " net(s)"));
    }
    failed.addAll(work);
    System.out.println("routed " + routes + " times (" + initial + " nets in row " + row + ", " + ripped + " rip-ups of other nets); "
        + failed.size() + " nets left unrouted" + (failed.isEmpty() ? "" : ": " + failed));
    d.writeCheckpoint(a[1]);
  }
}
