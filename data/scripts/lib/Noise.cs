// Shipped library node. Copy this file into your own scripts folder to fork and edit it.
// !ofs:signal functional
// !ofs:input a
// !ofs:name Noise
// !ofs:description Adds smooth random variation to the signal. Frequency controls how rapidly the noise changes; seed changes the pattern.
// !ofs:param amplitude 10.0 0 50
// !ofs:param frequency 1.0 0.01 10
// !ofs:param seed 0 int
static float Hash(uint n, uint s)
{
    unchecked
    {
        n = n * 1234567u + s * 7654321u;
        n ^= n >> 13;
        n = n * (n * n * 15731u + 789221u) + 1376312589u;
        return n / (float)uint.MaxValue;
    }
}
double scaled = (t - ctx.RegionStart) * frequency;
int i0 = (int)Math.Floor(scaled);
double f = scaled - i0;
double sm = f * f * (3.0 - 2.0 * f);
float v0 = Hash((uint)i0, (uint)seed) * 2f - 1f;
float v1 = Hash((uint)(i0 + 1), (uint)seed) * 2f - 1f;
float noise = v0 + (float)sm * (v1 - v0);
return a + amplitude * noise;
