// Shipped library node. Copy this file into your own scripts folder to fork and edit it.
// !ofs:signal discrete
// !ofs:input a
// !ofs:name Invert
// !ofs:description Mirrors all positions around the center value. Default center 50 turns a stroke into its complement.
// !ofs:param center 50 0 100 int
foreach (var p in a)
    outp.Add(p.At, 2 * center - p.Pos);
