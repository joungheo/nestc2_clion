// #### JO added (2026-10-27): which tile a stuckchk.py "extra" cell belongs to (RapidWright 2025.2 bitstream API).
// usage: RAPIDWRIGHT_PATH=~/tools/rapidwright/d2025.2 java -Xmx16g -cp ~/tools/rapidwright/rapidwright-2025.2.2-standalone-lin64.jar StuckLoc.java <design.bit> <base.bit>
// then match the frame to the PIP with build/wiretest/uray segbits_int.db (docs/evta_systolic.md §8 6)).
import com.xilinx.rapidwright.bitstream.*;
// frames where <bit> sets a stuck cell (word 61 b18, word 62 b0-3,12-14) that <base> does not; prints block tiles
public class StuckLoc { public static void main(String[] a) {
  ConfigArray d = Bitstream.readBitstream(a[0]).configureArray(), b = Bitstream.readBitstream(a[1]).configureArray();
  int[][] st = {{61,18},{62,0},{62,1},{62,2},{62,3},{62,12},{62,13},{62,14}};
  for (int r = 0; r < d.getNumOfConfigRows(); r++) {
    ConfigRow cd = d.getConfigRows().get(r), cb = b.getConfigRows().get(r);
    for (int k = 0; k < cd.numOfBlocks(); k++) {
      Block bd = cd.getBlock(k), bb = cb.getBlock(k);
      for (int f = 0; f < bd.getFrameCount(); f++) {
        Frame fd = bd.getFrame(f), fb = bb.getFrame(f); if (fd == null || fb == null) continue;
        int[] wd = fd.getWords(), wb = fb.getWords();
        for (int[] s : st) if ((wd[s[0]] >> s[1] & 1) == 1 && (wb[s[0]] >> s[1] & 1) == 0)
          System.out.printf("row %d blk %d type %d col %d minor %d w%d b%d tiles %s..%s cr %s%n", cd.getRowIndex(), k,
            bd.getBlockType(d.getDevice().getSeries()), bd.getTileColumn(), f, s[0], s[1], bd.getBottomTile(), bd.getTopTile(), bd.getClockRegion());
      } } } } }
