// #### JO added (2026-10-25): test design for the routing wires that pass over tile row Y219 of the lab ZCU102.
// Registers whose nets crossed that row read wrong bits on the board (log/BOARD_TODO.md 2026-10-25) although the
// configuration read back as written, so some of these wires are bad. For every INT node that begins on one side of
// the row and ends on the other (NN2/4/12, SS2/4/12; none of its PIPs in the row), this makes one net that crosses
// the row on that node only: a source FF on the node's begin side (CE=0, so it keeps its INIT) drives it, and a
// sink SRL (SRLC32E in a SLICEM LUT, clocked) on the other side shifts it in. The two legs to the node are found by
// BFS on free nodes that stay on their own side and away from the row. WireTestBit.tcl clocks the sinks from
// STARTUPE3 CFGMCLK and writes one bitstream with the sources at 0 and one at 1 (sinks at the opposite INIT). After
// loading one (fpga_manager; the design has no PS connection), a JTAG readback (no capture: Vivado cannot capture
// FF states on this device, but LUT contents read back live) shows in wiretest.py the wires whose SRL did not fill
// with the source value. (Programming over JTAG froze the board's Linux, 2026-10-25.)
// With -Dspeed=1 (at-speed test, WireSpeed: wirespeed.py) the source is an SRL too, in a SLICEM LUT whose output feeds
// its own input (a 32-bit pattern circulates), and the sink SRL records what arrives; with the clock stopped, a
// readback shows per node whether the sink got the source's bits in the same cycle or later.
// With -Dinrow=1 -Dpips=stuck_int_pips.txt the tested nodes are those that begin in the row and leave it upward or
// downward; the source leg reaches them from the other side through PIPs in the row (none from the stuck list), so
// each net crosses the row on PIPs inside it (the question whether those PIPs work although they set no stuck cell).
// Writes <out>.dcp, <out>.txt ("net node src_cell dst_cell" per tested node) and <out>.badpin.txt (nets whose SRL
// the site router connected to another pin than the routed one: not a valid test). The SLICEMs near the row hold
// about 11700 sinks, so the 20328 nodes need two designs: the second skips the nodes of the first (<skip.txt>).
// usage: RAPIDWRIGHT_PATH=<data> java -Xmx16g -cp rapidwright-2025.2.2-standalone-lin64.jar WireTest.java \
//          <out> [row=219] [max nets=0 (all)] [x range=0-999] [skip.txt,...]
import com.xilinx.rapidwright.design.*;
import com.xilinx.rapidwright.device.*;

import java.io.*;
import java.util.*;
import java.util.regex.*;

public class WireTest {
  static final Pattern TILE = Pattern.compile("[A-Z_]+_X(\\d+)Y(\\d+)");
  static int row;
  static final boolean SPEED = !"0".equals(System.getProperty("speed", "0"));
  static final boolean INROW = !"0".equals(System.getProperty("inrow", "0"));
  static Set<String> stuck = new HashSet<>();   // "<from wire> <to wire>" of INT PIPs that set a stuck cell (-Dpips)
  static Map<String, Node> lutIn = new HashMap<>();   // "SITE X6LUT" -> its X_I node (speed: the source loop)
  static final int BAND = 16;    // legs stay within this many rows of the row
  static final int XWIN = 2;     // and this many INT columns of the tested node
  static Set<Node> used = new HashSet<>();
  static Set<Node> crossing = new HashSet<>();
  static Map<Node, int[]> span = new HashMap<>();
  // site pin node -> "SITE BEL" (sources: FF outputs; sinks: SRL data inputs X_I of SLICEM LUTs)
  static Map<Node, String> qPin = new HashMap<>(), dPin = new HashMap<>();
  static Map<String, Character> role = new HashMap<>();   // site -> 'S' (sources) / 'D' (sinks)
  static Set<String> usedBel = new HashSet<>();

  static int[] tileXY(Tile t) {
    Matcher m = TILE.matcher(t.getName());
    return m.matches() ? new int[]{Integer.parseInt(m.group(1)), Integer.parseInt(m.group(2))} : null;
  }

  // {min row, max row, min x, max x} over the INT and CLE wires of a node
  static int[] spanOf(Node n) {
    return span.computeIfAbsent(n, k -> {
      int[] s = {Integer.MAX_VALUE, -1, Integer.MAX_VALUE, -1};
      for (Wire w : k.getAllWiresInNode()) {
        String tn = w.getTile().getName();
        if (!tn.startsWith("INT_X") && !tn.startsWith("CLE")) continue;
        int[] xy = tileXY(w.getTile());
        if (xy == null) continue;
        s[0] = Math.min(s[0], xy[1]); s[1] = Math.max(s[1], xy[1]);
        s[2] = Math.min(s[2], xy[0]); s[3] = Math.max(s[3], xy[0]);
      }
      return s;
    });
  }

  // a node a leg may use: free, on its side (rows lo..hi), within the column window, not passing over the row
  static boolean ok(Node n, int lo, int hi, int x0) {
    if (used.contains(n) || crossing.contains(n)) return false;
    // no site input pin (LUT inputs, CE/SR, bypass): the SRL address pins and FF controls are tied to VCC/GND there
    if (n.getSitePin() != null) return false;
    int[] s = spanOf(n);
    if (s[1] < 0) return false;
    return s[0] >= lo && s[1] <= hi && s[2] >= x0 - XWIN && s[3] <= x0 + XWIN;
  }

  // BFS from `from` (downhill if fwd, else uphill) to a node accepted by `goal`; returns the PIPs in signal order
  static List<PIP> bfs(Node from, boolean fwd, int lo, int hi, int x0, java.util.function.Predicate<Node> goal) {
    Map<Node, PIP> prev = new HashMap<>();
    ArrayDeque<Node> q = new ArrayDeque<>();
    q.add(from);
    prev.put(from, null);
    int expanded = 0;
    while (!q.isEmpty() && expanded < 20000) {
      Node n = q.poll();
      expanded++;
      for (PIP p : fwd ? n.getAllDownhillPIPs() : n.getAllUphillPIPs()) {
        Node m = fwd ? p.getEndNode() : p.getStartNode();
        if (m == null || prev.containsKey(m)) continue;
        if (p.getTile().getName().startsWith("INT_X") && tileXY(p.getTile())[1] == row
            && stuck.contains(p.getStartWireName() + " " + p.getEndWireName())) continue;
        if (goal.test(m)) {
          prev.put(m, p);
          List<PIP> path = new ArrayList<>();
          for (Node k = m; prev.get(k) != null; k = fwd ? prev.get(k).getStartNode() : prev.get(k).getEndNode())
            path.add(prev.get(k));
          if (fwd) Collections.reverse(path);
          lastGoal = m;
          return path;
        }
        if (!ok(m, lo, hi, x0)) continue;
        prev.put(m, p);
        q.add(m);
      }
    }
    return null;
  }
  static Node lastGoal;

  static boolean belFree(String siteBel, char r) {
    String site = siteBel.split(" ")[0];
    Character c = role.get(site);
    return (c == null || c == r) && !usedBel.contains(siteBel);
  }

  public static void main(String[] a) throws IOException {
    String out = a[0];
    row = a.length > 1 ? Integer.parseInt(a[1]) : 219;
    int max = a.length > 2 ? Integer.parseInt(a[2]) : 0;
    int xlo = 0, xhi = 999;
    if (a.length > 3) { String[] r = a[3].split("-"); xlo = Integer.parseInt(r[0]); xhi = Integer.parseInt(r[1]); }
    Set<String> skip = new HashSet<>();
    if (a.length > 4) for (String f : a[4].split(",")) for (String l : java.nio.file.Files.readAllLines(java.nio.file.Paths.get(f))) skip.add(l.split(" ")[1]);
    if (System.getProperty("pips") != null) stuck.addAll(java.nio.file.Files.readAllLines(java.nio.file.Paths.get(System.getProperty("pips"))));
    if (INROW && stuck.isEmpty()) throw new IllegalArgumentException("-Dinrow needs -Dpips=stuck_int_pips.txt");
    Design d = new Design("wiretest", "xczu9eg-ffvb1156-2-e");
    Device dev = d.getDevice();

    // the nodes that pass over the row, and the FF pins near it
    List<Node> todo = new ArrayList<>();
    String[] L = "ABCDEFGH".split("");
    for (Tile t : dev.getAllTiles()) {
      if (!t.getName().startsWith("INT_X")) continue;
      int[] xy = tileXY(t);
      if (Math.abs(xy[1] - row) > BAND) continue;
      for (int w = 0; w < t.getWireCount(); w++) {
        Node n = Node.getNode(t, w);
        if (n == null || !n.getTile().equals(t)) continue;
        int[] s = spanOf(n);
        if (s[0] < row && s[1] > row) crossing.add(n);
        boolean test = INROW ? xy[1] == row && (s[0] == row && s[1] > row || s[1] == row && s[0] < row) : s[0] < row && s[1] > row;
        if (test && xy[0] >= xlo && xy[0] <= xhi && !skip.contains(n.toString())) todo.add(n);
      }
      if (xy[1] == row) continue;   // no FF in the row itself
      for (int dx : new int[]{-1, 1}) {
        Tile c = t.getTileNeighbor(dx, 0);
        if (c == null) continue;
        for (Site s : c.getSites()) {
          SiteTypeEnum ty = s.getSiteTypeEnum();
          if (ty != SiteTypeEnum.SLICEL && ty != SiteTypeEnum.SLICEM) continue;
          for (String l : L) {
            if (!SPEED) {
              qPin.put(s.getConnectedNode(s.getPinIndex(l + "Q")), s.getName() + " " + l + "FF");
              qPin.put(s.getConnectedNode(s.getPinIndex(l + "Q2")), s.getName() + " " + l + "FF2");
            } else if (ty == SiteTypeEnum.SLICEM) {
              qPin.put(s.getConnectedNode(s.getPinIndex(l + "_O")), s.getName() + " " + l + "6LUT");
              lutIn.put(s.getName() + " " + l + "6LUT", s.getConnectedNode(s.getPinIndex(l + "_I")));
            }
            if (ty == SiteTypeEnum.SLICEM) dPin.put(s.getConnectedNode(s.getPinIndex(l + "_I")), s.getName() + " " + l + "6LUT");
          }
        }
      }
    }
    // long wires first (fewest begin/end choices), then by column
    todo.sort(Comparator.comparingInt((Node n) -> -(spanOf(n)[1] - spanOf(n)[0])).thenComparing(Node::toString));
    System.err.println("nodes passing over row " + row + ": " + crossing.size() + ", to test: " + todo.size());

    Net gnd = d.getGndNet(), vcc = d.getVccNet();
    PrintWriter txt = new PrintWriter(out + ".txt");
    int made = 0, failA = 0, failB = 0, failL = 0;
    for (Node n : todo) {
      if (max > 0 && made >= max) break;
      boolean up = INROW ? spanOf(n)[1] > row : tileXY(n.getTile())[1] < row;   // NN: begins below (inrow: leaves upward)
      int x0 = tileXY(n.getTile())[0];
      int sLo = up ? row - BAND : row + 1, sHi = up ? row - 1 : row + BAND;
      if (INROW) { if (up) sHi = row; else sLo = row; }   // the source leg ends in the row
      int dLo = up ? row + 1 : row - BAND, dHi = up ? row + BAND : row - 1;
      List<PIP> legA = bfs(n, false, sLo, sHi, x0, m -> qPin.containsKey(m) && !used.contains(m) && belFree(qPin.get(m), 'S'));
      if (legA == null) { failA++; continue; }
      Node src = lastGoal;
      List<PIP> legL = new ArrayList<>();
      if (SPEED) {
        // the source SRL's output back to its own input, on its side, off the nodes of leg A
        Node in = lutIn.get(qPin.get(src));
        Set<Node> tmp = new HashSet<>();
        for (PIP p : legA) { tmp.add(p.getStartNode()); tmp.add(p.getEndNode()); }
        tmp.removeAll(used);
        used.addAll(tmp);
        used.remove(src);
        legL = used.contains(in) ? null : bfs(src, true, up ? sLo : row + 1, up ? row - 1 : sHi, x0, m -> m.equals(in));
        used.removeAll(tmp);
        if (legL == null) { failL++; continue; }
      }
      List<PIP> legB = bfs(n, true, dLo, dHi, x0, m -> dPin.containsKey(m) && !used.contains(m) && belFree(dPin.get(m), 'D'));
      if (legB == null) { failB++; continue; }
      Node dst = lastGoal;
      String[] sb = qPin.get(src).split(" "), db = dPin.get(dst).split(" ");
      String id = "wt_" + made;
      Cell cs = d.createAndPlaceCell(id + "_s", SPEED ? Unisim.SRLC32E : Unisim.FDRE, sb[0] + "/" + sb[1]);
      Cell cd = d.createAndPlaceCell(id + "_d", Unisim.SRLC32E, db[0] + "/" + db[1]);
      Net net = d.createNet(id);
      net.connect(cs, "Q");
      net.connect(cd, "D");
      if (SPEED) {
        net.connect(cs, "D");
        vcc.connect(cs, "CE");
        for (int k = 0; k < 5; k++) vcc.connect(cs, "A[" + k + "]");
      } else {
        gnd.connect(cs, "D"); gnd.connect(cs, "CE"); gnd.connect(cs, "R"); gnd.connect(cs, "C");
      }
      vcc.connect(cd, "CE");   // CLK: WireTestBit.tcl
      for (int k = 0; k < 5; k++) vcc.connect(cd, "A[" + k + "]");
      for (PIP p : legA) { net.addPIP(p); used.add(p.getStartNode()); used.add(p.getEndNode()); }
      for (PIP p : legB) { net.addPIP(p); used.add(p.getStartNode()); used.add(p.getEndNode()); }
      for (PIP p : legL) { net.addPIP(p); used.add(p.getStartNode()); used.add(p.getEndNode()); }
      used.add(n);
      role.put(sb[0], 'S'); role.put(db[0], 'D');
      usedBel.add(qPin.get(src)); usedBel.add(dPin.get(dst));
      txt.println(id + " " + n + " " + cs.getName() + "@" + sb[0] + "/" + sb[1] + " " + cd.getName() + "@" + db[0] + "/" + db[1]);
      made++;
    }
    txt.close();
    System.err.println("nets made: " + made + ", no source leg: " + failA + ", no sink leg: " + failB + ", no source loop: " + failL);
    d.routeSites();
    // the site router picks the SRL input pin; it must be the one the sink leg ends on
    int bad = 0;
    PrintWriter badpin = new PrintWriter(out + ".badpin.txt");
    for (Net net : d.getNets()) {
      if (!net.getName().startsWith("wt_")) continue;
      Set<Node> pins = new HashSet<>();
      for (SitePinInst p : net.getPins()) pins.add(p.getConnectedNode());
      for (PIP p : net.getPIPs()) {
        if (dPin.containsKey(p.getEndNode()) && !pins.contains(p.getEndNode())) { bad++; badpin.println(net.getName()); }
      }
    }
    badpin.close();
    System.err.println("nets whose sink pin differs from the routed one: " + bad);
    d.writeCheckpoint(out + ".dcp");
  }
}
