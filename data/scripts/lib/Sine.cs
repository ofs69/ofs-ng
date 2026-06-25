// Shipped library node. Copy this file into your own scripts folder to fork and edit it.
// !ofs:signal functional
// !ofs:name Sine Wave
// !ofs:description Generates a smooth sinusoidal oscillation. Combine with Scale or Mix to place it in a specific range.
// !ofs:param amplitude 25 int
// !ofs:param period 1.0
// !ofs:param phase 0.0
// !ofs:param center 50 int
double per = period == 0f ? 1.0 : period;
double angle = 2.0 * Math.PI * ((t - ctx.RegionStart) / per + phase);
return center + amplitude * (float)Math.Sin(angle);
