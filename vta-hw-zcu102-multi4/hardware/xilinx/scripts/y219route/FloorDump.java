import com.xilinx.rapidwright.design.*;
import com.xilinx.rapidwright.device.*;
import java.util.*;
import java.util.regex.*;
import java.io.*;
public class FloorDump { public static void main(String[] a) throws Exception {
  Design d = Design.readCheckpoint(a[0], a[0].replaceAll("\\.dcp$", ".edf"));
  Device dev = d.getDevice();
  PrintWriter w = new PrintWriter(a[1]);
  Pattern ev = Pattern.compile("^vta_i/[a-z_]+?_(\\d)(_\\d+)?/");
  for (Site s : dev.getAllSites()) {
    String n = s.getName(), ty;
    if (n.startsWith("SLICE_")) ty = "L"; else if (n.startsWith("RAMB36_")) ty = "B"; else if (n.startsWith("DSP48E2_")) ty = "D";
    else if (n.startsWith("PS8_") || n.startsWith("MMCM_")) ty = n.substring(0, 2); else continue;
    SiteInst si = d.getSiteInstFromSite(s);
    String use = "-";
    if (ty.equals("B") && si == null) {  // a RAMB36 site is also used when its RAMB18 halves are
      for (Site h : s.getTile().getSites()) if (h != s && d.getSiteInstFromSite(h) != null) { si = d.getSiteInstFromSite(h); break; }
    }
    if (si != null && !si.getCells().isEmpty()) {
      Map<String, Integer> c = new HashMap<>();
      for (Cell cell : si.getCells()) { Matcher m = ev.matcher(cell.getName()); c.merge(m.find() ? m.group(1) : "s", 1, Integer::sum); }
      use = Collections.max(c.entrySet(), Map.Entry.comparingByValue()).getKey();
    }
    w.println(ty + "," + n + "," + s.getTile().getColumn() + "," + s.getTile().getRow() + "," + use);
  }
  w.close();
} }
