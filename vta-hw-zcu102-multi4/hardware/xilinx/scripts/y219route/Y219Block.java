// #### JO added (2026-10-25): a routing blockage for one tile row, for Vivado (RapidWright 2025.2).
// The lab ZCU102 has stuck-at-0 configuration cells in tile row Y219 (log/BOARD_TODO.md 2026-10-23), and Vivado has
// no routing blockage. This makes one: it places dummy LUTs (vta_y219blk_*) in the empty slices of rows row-1..row+1
// and routes their outputs, as antenna trees, onto every free node a PIP in the row drives (that a tree reaches; what is
// left are site outputs and carry inputs of empty sites), so that Vivado's own
// timing-driven router cannot use any PIP there. The nets with a PIP in the row lose the routing below such a PIP
// (stubs left hanging are dropped); Vivado routes those sinks again. y219block.tcl then routes the design and removes
// the dummies before write_bitstream. Rerouting with RapidWright alone (Y219Route.java, Y219RWRoute.java) lost
// 1.6-1.9 ns: neither knows the design's clocks.
// Place the design with rows row-1..row+1 empty (no site pin there).
// usage: RAPIDWRIGHT_PATH=<data> java -Xmx100g -cp rapidwright-2025.2.2-standalone-lin64.jar Y219Block.java \
//          <in.dcp> <out.dcp> [row=219]       (needs <in>.edf next to the DCP: write_edif in Vivado)
import com.xilinx.rapidwright.design.Cell;
import com.xilinx.rapidwright.design.Design;
import com.xilinx.rapidwright.design.Net;
import com.xilinx.rapidwright.design.SitePinInst;
import com.xilinx.rapidwright.design.Unisim;
import com.xilinx.rapidwright.device.Node;
import com.xilinx.rapidwright.device.PIP;
import com.xilinx.rapidwright.device.Site;
import com.xilinx.rapidwright.device.SiteTypeEnum;
import com.xilinx.rapidwright.device.Tile;

import java.util.*;

public class Y219Block {
  static int row;

  static boolean bad(PIP p) { return p.getTile().getTileYCoordinate() == row; }

  public static void main(String[] a) {
    row = a.length > 2 ? Integer.parseInt(a[2]) : 219;
    Design d = Design.readCheckpoint(a[0], a[0].replaceAll("\\.dcp$", ".edf"));
    // cut the nets that use the row (as Y219RWRoute.java)
    int nets = 0, cutSinks = 0;
    for (Net net : d.getNets()) {
      if (net.isStaticNet() || net.isClockNet() || net.getPIPs().stream().noneMatch(Y219Block::bad)) continue;
      nets++;
      Map<Node, List<PIP>> down = new HashMap<>();
      for (PIP p : net.getPIPs()) down.computeIfAbsent(p.getStartNode(), k -> new ArrayList<>()).add(p);
      Set<Node> tree = new HashSet<>();
      Deque<Node> q = new ArrayDeque<>();
      for (SitePinInst s : new SitePinInst[] {net.getSource(), net.getAlternateSource()})
        if (s != null && tree.add(s.getConnectedNode())) q.add(s.getConnectedNode());
      List<PIP> kept = new ArrayList<>();
      while (!q.isEmpty())
        for (PIP p : down.getOrDefault(q.poll(), Collections.emptyList()))
          if (!bad(p) && tree.add(p.getEndNode())) { kept.add(p); q.add(p.getEndNode()); }
      Set<Node> sinks = new HashSet<>();
      for (SitePinInst s : net.getSinkPins()) sinks.add(s.getConnectedNode());
      for (boolean pruned = true; pruned; ) {
        Set<Node> starts = new HashSet<>();
        for (PIP p : kept) starts.add(p.getStartNode());
        pruned = kept.removeIf(p -> !sinks.contains(p.getEndNode()) && !starts.contains(p.getEndNode()));
      }
      for (SitePinInst s : net.getSinkPins())
        if (!tree.contains(s.getConnectedNode())) { s.setRouted(false); cutSinks++; }
      net.setPIPs(kept);
    }
    System.out.println("nets with a PIP in row " + row + ": " + nets + ", sinks cut off: " + cutSinks);
    // nodes still in use
    Set<Node> used = new HashSet<>();
    for (Net net : d.getNets()) {
      for (PIP p : net.getPIPs()) { used.add(p.getStartNode()); used.add(p.getEndNode()); }
      for (SitePinInst s : net.getPins()) used.add(s.getConnectedNode());
    }
    // the nodes a PIP in the row drives
    Set<Node> targets = new HashSet<>();
    int rt = 0;
    for (Tile t : d.getDevice().getAllTiles()) {
      if (t.getTileYCoordinate() != row) continue;
      for (PIP p : t.getPIPs()) {
        Node e = p.getEndNode();
        if (e == null || e.isInvalidNode() || e.isTied()) continue;
        if (p.isRouteThru()) { rt++; continue; }
        targets.add(e);
      }
    }
    int inUse = 0;
    for (Node n : targets) if (used.contains(n)) inUse++;
    System.out.println("nodes driven in row " + row + ": " + targets.size() + " (" + inUse + " in use by other nets), route-thru PIPs skipped: " + rt);
    // dummy LUT outputs in the empty slices of rows row-1..row+1; each target goes to the output that reaches it with
    // the fewest nodes outside the targets (at most HOPS, driven within 12 rows of the row: wires that end in the row),
    // and each output's tree is the union of the paths to its targets
    // A tree may reach at most XW tile columns from its LUT (-Dxw): with no bound, 0-cost steps along the row gave one
    // LUT a tree over most of the row (2026-10-25 blk_e: Vivado then routed real nets on that tree's nodes).
    final int HOPS = Integer.getInteger("hops", 5), XW = Integer.getInteger("xw", 3);
    Map<Node, String> src = new LinkedHashMap<>();  // output node -> site/LUT
    Map<Node, Node> root = new HashMap<>();
    Map<Node, PIP> parent = new HashMap<>();
    Map<Node, Integer> hops = new HashMap<>();
    Deque<Node> q = new ArrayDeque<>();
    for (Site s : d.getDevice().getAllSites()) {
      SiteTypeEnum ty = s.getSiteTypeEnum();
      if ((ty != SiteTypeEnum.SLICEL && ty != SiteTypeEnum.SLICEM) || Math.abs(s.getTile().getTileYCoordinate() - row) > 1) continue;
      if (d.getSiteInstFromSite(s) != null) { System.out.println("site in use, skipped: " + s); continue; }
      for (char l : "ABCDEFGH".toCharArray()) {
        Node n = s.getConnectedNode(l + "_O");
        if (n == null || used.contains(n)) continue;
        src.put(n, s.getName() + "/" + l);
        root.put(n, n);
        hops.put(n, 0);
        q.add(n);
      }
    }
    while (!q.isEmpty()) {  // 0-1 BFS: a target costs nothing, any other node one hop
      Node n = q.poll();
      int hn = hops.get(n);
      for (PIP p : n.getAllDownhillPIPs()) {
        Node e = p.getEndNode();
        if (e == null || p.isRouteThru() || used.contains(e) || src.containsKey(e)) continue;
        boolean t = targets.contains(e);
        int h = hn + (t ? 0 : 1);
        if (h >= hops.getOrDefault(e, Integer.MAX_VALUE)) continue;
        if (Math.abs(p.getTile().getTileXCoordinate() - root.get(n).getTile().getTileXCoordinate()) > XW) continue;
        if (!t && (h > HOPS || e.isInvalidNode() || e.isTied() || Math.abs(p.getTile().getTileYCoordinate() - row) > 12)) continue;
        root.put(e, root.get(n));
        parent.put(e, p);
        hops.put(e, h);
        if (t) q.addFirst(e); else q.addLast(e);
      }
    }
    Map<Node, Set<PIP>> tree = new LinkedHashMap<>();
    for (Node n : targets)
      for (Node m = n; parent.containsKey(m) && tree.computeIfAbsent(root.get(m), k -> new LinkedHashSet<>()).add(parent.get(m)); )
        m = parent.get(m).getStartNode();
    Map<Net, List<PIP>> pips = new LinkedHashMap<>();
    int cells = 0;
    for (Map.Entry<Node, Set<PIP>> e : tree.entrySet()) {
      String[] sl = src.get(e.getKey()).split("/");
      String name = "vta_y219blk_" + sl[0] + "_" + sl[1];
      Cell c = d.createAndPlaceCell(name, Unisim.LUT1, sl[0] + "/" + sl[1] + "6LUT");
      Net net = d.createNet(name + "_o");
      net.connect(c, "O");
      pips.put(net, new ArrayList<>(e.getValue()));
      cells++;
    }
    Map<Node, Net> owner = new HashMap<>();
    int extra = 0;
    for (Map.Entry<Net, List<PIP>> e : pips.entrySet())
      for (PIP p : e.getValue()) { owner.put(p.getEndNode(), e.getKey()); if (!targets.contains(p.getEndNode())) extra++; }
    int covered = 0;
    for (Node n : targets) if (owner.containsKey(n)) covered++;
    for (Map.Entry<Net, List<PIP>> e : pips.entrySet()) e.getKey().setPIPs(e.getValue());
    pips.entrySet().stream().sorted((x, y) -> y.getValue().size() - x.getValue().size()).limit(5)
        .forEach(e -> System.out.println("  largest: " + e.getKey() + " " + e.getValue().size() + " PIPs"));
    System.out.println(cells + " dummy LUTs, covering " + covered + " of " + targets.size() + " nodes (and " + extra + " nodes next to the row); "
        + (targets.size() - inUse - covered) + " free nodes left uncovered");
    Map<String, Integer> left = new TreeMap<>();
    for (Node n : targets)
      if (!owner.containsKey(n) && !used.contains(n)) left.merge(n.getTile().getTileTypeEnum() + " " + n.getWireName().replaceAll("[0-9]+", "#"), 1, Integer::sum);
    Map<Integer, Integer> byX = new TreeMap<>();
    for (Node n : targets)
      if (!owner.containsKey(n) && !used.contains(n) && n.getTile().getName().startsWith("INT_X")) byX.merge(n.getTile().getTileXCoordinate(), 1, Integer::sum);
    System.out.println("  uncovered INT nodes by tile column: " + byX);
    int shown = 0;
    for (Node n : targets) {
      if (owner.containsKey(n) || used.contains(n) || !n.getTile().getName().startsWith("INT_X") || shown++ >= Integer.getInteger("show", 0)) continue;
      StringBuilder sb = new StringBuilder("  e.g. " + n + " <-");
      for (Node u : n.getAllUphillNodes())
        sb.append(" ").append(u).append(used.contains(u) ? "(used)" : owner.containsKey(u) ? "(blk)" : root.containsKey(u) ? "(reached)" : "");
      System.out.println(sb);
    }
    left.entrySet().stream().sorted((x, y) -> y.getValue() - x.getValue()).limit(30).forEach(e -> System.out.println("  uncovered " + e.getValue() + " " + e.getKey()));
    d.writeCheckpoint(a[1]);
  }
}
