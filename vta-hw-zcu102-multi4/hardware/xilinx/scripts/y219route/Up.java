// #### JO added (2026-10-27, from ~/tools/rapidwright/t/Up.java): uphill PIPs of a site pin and how many of their own uphill PIPs sit in row Y219.
// usage: RAPIDWRIGHT_PATH=~/tools/rapidwright/d2020.1 java -Xmx16g -cp ~/tools/rapidwright/rapidwright-2020.1.7-standalone-lin64.jar Up.java <site> <pin>   e.g. SLICE_X47Y218 F4
import com.xilinx.rapidwright.device.*;
import java.util.*;
public class Up { public static void main(String[] a){
  Device d = Device.getDevice("xczu9eg-ffvb1156-2-e");
  Site s = d.getSite(a[0]); Node n = s.getConnectedNode(a[1]);
  System.out.println("pin node " + n);
  for (PIP p : n.getAllUphillPIPs()) { Node st = p.getStartNode();
    int ok = 0, bad = 0;
    for (PIP q : st.getAllUphillPIPs()) { if (q.getTile().getTileYCoordinate()==219) bad++; else ok++; }
    System.out.println(" " + p + " (pip row " + p.getTile().getTileYCoordinate() + ") start=" + st + " : its uphill pips ok=" + ok + " inRow219=" + bad);
  }
}}
