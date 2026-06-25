// Shipped library node. Copy this file into your own scripts folder to fork and edit it.
// !ofs:signal functional
// !ofs:name Triangle Wave
// !ofs:description Generates a triangle wave with sharp linear peaks. More mechanical than Sine Wave.
// !ofs:param amplitude 25 int
// !ofs:param period 1.0
// !ofs:param phase 0.0
// !ofs:param center 50 int
double per = period == 0f ? 1.0 : period;
double pos = (t - ctx.RegionStart) / per + phase;
double frac = pos - Math.Floor(pos);
double tri;
if (frac < 0.25) tri = 4.0 * frac;
else if (frac < 0.75) tri = 2.0 - 4.0 * frac;
else tri = 4.0 * frac - 4.0;
return center + amplitude * (float)tri;
