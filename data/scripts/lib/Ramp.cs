// Shipped library node. Copy this file into your own scripts folder to fork and edit it.
// !ofs:signal functional
// !ofs:name Ramp
// !ofs:description Generates a linear ramp from a start position to an end position over the region.
// !ofs:param startPos 0 0 100 int
// !ofs:param endPos 100 0 100 int
if (ctx.RegionEnd <= ctx.RegionStart) return startPos;
double frac = Math.Clamp((t - ctx.RegionStart) / (ctx.RegionEnd - ctx.RegionStart), 0.0, 1.0);
return (float)(startPos + frac * (endPos - startPos));
